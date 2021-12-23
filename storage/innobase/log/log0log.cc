/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2014, 2021, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file log/log0log.cc
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <debug_sync.h>
#include <my_service_manager.h>

#include "log0log.h"
#include "log0crypt.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0stats_bg.h"
#include "btr0defragment.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0mon.h"
#include "buf0dump.h"
#include "log0sync.h"
#include "log.h"

/*
General philosophy of InnoDB redo-logs:

Every change to a contents of a data page must be done
through mtr_t, and mtr_t::commit() will write log records
to the InnoDB redo log. */

/** Redo log system */
log_t	log_sys;

/* Margins for free space in the log buffer after a log entry is catenated */
#define LOG_BUF_FLUSH_RATIO	2
#define LOG_BUF_FLUSH_MARGIN	((4 * 4096) /* cf. log_t::append_prepare() */ \
				 + (4U << srv_page_size_shift))

/** Calculate the recommended highest values for lsn - last_checkpoint_lsn
and lsn - buf_pool.get_oldest_modification().
@param[in]	file_size	requested innodb_log_file_size
@retval true on success
@retval false if the smallest log group is too small to
accommodate the number of OS threads in the database server */
bool
log_set_capacity(ulonglong file_size)
{
	/* Margin for the free space in the smallest log, before a new query
	step which modifies the database, is started */
	const size_t LOG_CHECKPOINT_FREE_PER_THREAD = 4U
						      << srv_page_size_shift;
	const size_t LOG_CHECKPOINT_EXTRA_FREE = 8U << srv_page_size_shift;

	lsn_t		margin;
	ulint		free;

	lsn_t smallest_capacity = file_size - log_t::START_OFFSET;
	/* Add extra safety */
	smallest_capacity -= smallest_capacity / 10;

	/* For each OS thread we must reserve so much free space in the
	smallest log group that it can accommodate the log entries produced
	by single query steps: running out of free log space is a serious
	system error which requires rebooting the database. */

	free = LOG_CHECKPOINT_FREE_PER_THREAD * 10
		+ LOG_CHECKPOINT_EXTRA_FREE;
	if (free >= smallest_capacity / 2) {
		sql_print_error("InnoDB: innodb_log_file_size is too small."
				" %s", INNODB_PARAMETERS_MSG);
		return false;
	}

	margin = smallest_capacity - free;
	margin = margin - margin / 10;	/* Add still some extra safety */

	mysql_mutex_lock(&log_sys.mutex);

	log_sys.log_capacity = smallest_capacity;

	log_sys.max_modified_age_async = margin - margin / 8;
	log_sys.max_checkpoint_age = margin;

	mysql_mutex_unlock(&log_sys.mutex);

	return(true);
}

/** Initialize the redo log subsystem. */
void log_t::create()
{
  ut_ad(this == &log_sys);
  ut_ad(!is_initialised());

#if defined(__aarch64__)
  mysql_mutex_init(log_sys_mutex_key, &mutex, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(
    log_flush_order_mutex_key, &flush_order_mutex, MY_MUTEX_INIT_FAST);
#else
  mysql_mutex_init(log_sys_mutex_key, &mutex, nullptr);
  mysql_mutex_init(log_flush_order_mutex_key, &flush_order_mutex, nullptr);
#endif

  /* Start the lsn from one log block from zero: this way every
  log record has a non-zero start lsn, a fact which we will use */

  set_lsn(FIRST_LSN);
  set_flushed_lsn(FIRST_LSN);

#ifndef HAVE_PMEM
  buf= static_cast<byte*>(ut_malloc_dontdump(buf_size, PSI_INSTRUMENT_ME));
  TRASH_ALLOC(buf, buf_size);
  flush_buf= static_cast<byte*>(ut_malloc_dontdump(buf_size,
                                                   PSI_INSTRUMENT_ME));
  TRASH_ALLOC(flush_buf, buf_size);
  checkpoint_buf= static_cast<byte*>(aligned_malloc(4096, 4096));
  memset_aligned<4096>(checkpoint_buf, 0, 4096);
#else
  ut_ad(!checkpoint_buf);
  ut_ad(!buf);
  ut_ad(!flush_buf);
#endif

  max_buf_free= buf_size / LOG_BUF_FLUSH_RATIO - LOG_BUF_FLUSH_MARGIN;
  set_check_flush_or_checkpoint();

  n_log_ios_old= n_log_ios;
  last_printout_time= time(NULL);

  last_checkpoint_lsn= FIRST_LSN;
  n_log_ios= 0;
  n_log_ios_old= 0;
  log_capacity= 0;
  max_modified_age_async= 0;
  max_checkpoint_age= 0;
  next_checkpoint_lsn= 0;
  n_pending_checkpoint_writes= 0;

  buf_free= 0;

  ut_ad(is_initialised());
}

void log_file_t::open(bool read_only) noexcept
{
  bool success;
  ut_a(!is_opened());
  m_file= os_file_create(innodb_log_file_key, m_path.c_str(),
                       OS_FILE_OPEN, OS_FILE_NORMAL, OS_LOG_FILE,
                       read_only, &success);
  ut_ad(success);
  ut_a(m_file != OS_FILE_CLOSED);
}

dberr_t log_file_t::close() noexcept
{
  ut_a(is_opened());

  if (!os_file_close(m_file))
    return DB_ERROR;

  m_file= OS_FILE_CLOSED;
  return DB_SUCCESS;
}

dberr_t log_file_t::rename(std::string new_path) noexcept
{
  if (IF_WIN(!MoveFileEx(m_path.c_str(), new_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING),
             ::rename(m_path.c_str(), new_path.c_str())))
    return DB_ERROR;
  m_path= std::move(new_path);
  return DB_SUCCESS;
}

dberr_t log_file_t::read(os_offset_t offset, span<byte> buf) noexcept
{
  ut_ad(is_opened());
  return os_file_read(IORequestRead, m_file, buf.data(), offset, buf.size());
}

dberr_t log_file_t::write(os_offset_t offset, span<const byte> buf) noexcept
{
  ut_ad(is_opened());
  return os_file_write(IORequestWrite, m_path.c_str(), m_file,
                       buf.data(), offset, buf.size());
}

#ifdef HAVE_PMEM
# include <libpmem.h>
#endif

void log_t::file::open_file(std::string path)
{
  const bool read_only(srv_read_only_mode);
  fd= log_file_t(std::move(path));
  fd.open(read_only);
#ifdef HAVE_PMEM
  ut_ad(!log_sys.buf);
  ut_ad(!log_sys.flush_buf);
  struct stat st;
  ut_a(!fstat(fd.m_file, &st));
  log_sys.file_size= st.st_size;
  if (!(st.st_size & 4095))
  {
    const size_t size(st.st_size);
    void *ptr=
      my_mmap(0, size, read_only ? PROT_READ : PROT_READ | PROT_WRITE,
              MAP_SHARED_VALIDATE | MAP_SYNC, fd.m_file, 0);
#if 0 && defined __linux__
    if (ptr == MAP_FAILED)
    {
      const auto st_dev= st.st_dev;
      if (!stat("/dev/shm", &st) && st.st_dev == st_dev)
        ptr= my_mmap(0, size, read_only ? PROT_READ : PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd.m_file, 0);
    }
#endif /* __linux__ */
    if (ptr != MAP_FAILED)
    {
      fd.close();
      log_sys.buf= static_cast<byte*>(ptr);
      return;
    }
  }
  log_sys.buf= static_cast<byte*>(ut_malloc_dontdump(log_sys.buf_size,
                                                     PSI_INSTRUMENT_ME));
  TRASH_ALLOC(log_sys.buf, log_sys.buf_size);
  log_sys.flush_buf= static_cast<byte*>(ut_malloc_dontdump(log_sys.buf_size,
                                                           PSI_INSTRUMENT_ME));
  TRASH_ALLOC(log_sys.flush_buf, log_sys.buf_size);
  const size_t bs{log_sys.get_block_size()};
  log_sys.checkpoint_buf= static_cast<byte*>(aligned_malloc(bs, bs));
  memset_aligned<64>(log_sys.checkpoint_buf, 0, bs);
#else
  log_sys.file_size= os_file_get_size(fd.m_file);
#endif
}

void log_t::create(lsn_t lsn) noexcept
{
  mysql_mutex_assert_owner(&mutex);
  ut_ad(!recv_no_log_write);
  ut_ad(is_latest());
  ut_ad(this == &log_sys);

  set_lsn(lsn);
  set_first_lsn(lsn);

  last_checkpoint_lsn= 0;

#ifdef HAVE_PMEM
  if (is_pmem())
  {
    memset_aligned<4096>(buf, 0, 4096);
    buf_free= START_OFFSET;
  }
  else
#endif
  {
    buf_free= 0;
    memset_aligned<4096>(flush_buf, 0, buf_size);
    memset_aligned<4096>(buf, 0, buf_size);
  }

  mach_write_to_4(buf + LOG_HEADER_FORMAT, FORMAT_10_8);
  mach_write_to_8(buf + LOG_HEADER_START_LSN, lsn);
  static constexpr const char LOG_HEADER_CREATOR_CURRENT[]=
    "MariaDB "
    IB_TO_STR(MYSQL_VERSION_MAJOR) "."
    IB_TO_STR(MYSQL_VERSION_MINOR) "."
    IB_TO_STR(MYSQL_VERSION_PATCH);

  strcpy(reinterpret_cast<char*>(buf) + LOG_HEADER_CREATOR,
         LOG_HEADER_CREATOR_CURRENT);
  static_assert(LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR >=
                sizeof LOG_HEADER_CREATOR_CURRENT, "compatibility");
  if (is_encrypted())
    log_crypt_write_header(buf + LOG_HEADER_CREATOR_END);
  mach_write_to_4(my_assume_aligned<4>(508 + buf), my_crc32c(0, buf, 508));

  DBUG_PRINT("ib_log", ("write header " LSN_PF, lsn));

#ifdef HAVE_PMEM
  if (is_pmem())
    pmem_deep_persist(buf, 512);
  else
#endif
  {
    log.write(0, {buf, 4096});
    memset_aligned<512>(buf, 0, 512);
  }
}

void log_t::file::read(os_offset_t offset, span<byte> buf)
{
  ut_ad(!(offset & (log_sys.get_block_size() - 1)));
  if (const dberr_t err= fd.read(offset, buf))
    ib::fatal() << "read(" << fd.get_path() << ") returned "<< err;
}

void log_t::file::write(os_offset_t offset, span<const byte> buf)
{
  srv_stats.os_log_pending_writes.inc();
  if (const dberr_t err= fd.write(offset, buf))
    ib::fatal() << "write(" << fd.get_path() << ") returned " << err;
  srv_stats.os_log_pending_writes.dec();
  srv_stats.os_log_written.add(buf.size());
  srv_stats.log_writes.inc();
  log_sys.n_log_ios++;
}

void log_t::file::close_file()
{
#ifdef HAVE_PMEM
  if (log_sys.is_pmem())
  {
    ut_ad(!fd.is_opened());
    ut_ad(!log_sys.checkpoint_buf);
    if (log_sys.buf)
    {
      my_munmap(log_sys.buf, log_sys.file_size);
      log_sys.buf= nullptr;
    }
    return;
  }

  ut_free_dodump(log_sys.buf, log_sys.buf_size);
  log_sys.buf= nullptr;
  ut_free_dodump(log_sys.flush_buf, log_sys.buf_size);
  log_sys.flush_buf= nullptr;
  aligned_free(log_sys.checkpoint_buf);
  log_sys.checkpoint_buf= nullptr;
#endif
  if (fd.is_opened())
    if (const dberr_t err= fd.close())
      ib::fatal() << "close(" << fd.get_path() << ") returned " << err;
  fd.free();                                    // Free path
}

static group_commit_lock write_lock;

/** Write an aligned buffer to ib_logfile0.
@param buf    buffer to be written
@param len    length of data to be written
@param offset log file offset */
static void log_write_buf(const byte *buf, size_t len, lsn_t offset)
{
  ut_ad(write_lock.is_owner());
  ut_ad(!recv_no_log_write);
  ut_d(const size_t block_size_1= log_sys.get_block_size() - 1);
  ut_ad(!(offset & block_size_1));
  ut_ad(!(len & block_size_1));
  ut_ad(!(size_t(buf) & block_size_1));
  ut_ad(len);

  if (UNIV_LIKELY(offset + len <= log_sys.file_size))
  {
write:
    log_sys.log.write(offset, {buf, len});
    return;
  }

  const size_t write_len= size_t(log_sys.file_size - offset);
  log_sys.log.write(offset, {buf, write_len});
  len-= write_len;
  buf+= write_len;
  ut_ad(log_sys.START_OFFSET + len < offset);
  offset= log_sys.START_OFFSET;
  goto write;
}

/** Invoke commit_checkpoint_notify_ha() to notify that outstanding
log writes have been completed. */
void log_flush_notify(lsn_t flush_lsn);

#if 0 // Currently we overwrite the last log block until it is complete.
/** CRC-32C of pad messages using between 1 and 15 bytes of NUL bytes
in the payload */
static const unsigned char pad_crc[15][4]= {
  {0xA6,0x59,0xC1,0xDB}, {0xF2,0xAF,0x80,0x73}, {0xED,0x02,0xF1,0x90},
  {0x68,0x4E,0xA3,0xF3}, {0x5D,0x1B,0xEA,0x6A}, {0xE0,0x01,0x86,0xB9},
  {0xD1,0x06,0x86,0xF5}, {0xEB,0x20,0x12,0x33}, {0xBA,0x73,0xB2,0xA3},
  {0x5F,0xA2,0x08,0x03}, {0x70,0x03,0xD6,0x9D}, {0xED,0xB3,0x49,0x78},
  {0xFD,0xD6,0xB9,0x9C}, {0x25,0xF8,0xB1,0x2C}, {0xCD,0xAA,0xE7,0x10}
};

/** Pad the log with some dummy bytes
@param lsn    desired log sequence number
@param pad    number of bytes to append to the log
@param begin  buffer to write 'pad' bytes to
@param extra  buffer for additional pad bytes (up to 15 bytes)
@return additional bytes used in extra[] */
ATTRIBUTE_NOINLINE
static size_t log_pad(lsn_t lsn, size_t pad, byte *begin, byte *extra)
{
  ut_ad(!(size_t(begin + pad) & (log_sys.get_block_size() - 1)));
  byte *b= begin;
  const byte seq{log_sys.get_sequence_bit(lsn)};
  /* The caller should never request padding such that the
  file would wrap around to the beginning. That is, the sequence
  bit must be the same for all records. */
  ut_ad(seq == log_sys.get_sequence_bit(lsn + pad));

  if (log_sys.is_encrypted())
  {
    /* The lengths of our pad messages vary between 15 and 29 bytes
    (FILE_CHECKPOINT byte, 1 to 15 NUL bytes, sequence byte,
    4 bytes checksum, 8 NUL bytes nonce). */
    if (pad < 15)
    {
      extra[0]= FILE_CHECKPOINT | 1;
      extra[1]= 0;
      extra[2]= seq;
      memcpy(extra + 3, pad_crc[0], 4);
      memset(extra + 7, 0, 8);
      memcpy(b, extra, pad);
      memmove(extra, extra + pad, 15 - pad);
      return 15 - pad;
    }

    /* Pad first with 29-byte messages until the remaining size is
    less than 29+15 bytes, and then write 1 or 2 shorter messages. */
    const byte *const end= begin + pad;
    for (; b + (29 + 15) < end; b+= 29)
    {
      b[0]= FILE_CHECKPOINT | 15;
      memset(b + 1, 0, 15);
      b[16]= seq;
      memcpy(b + 17, pad_crc[14], 4);
      memset(b + 21, 0, 8);
    }
    if (b + 29 < end)
    {
      b[0]= FILE_CHECKPOINT | 1;
      b[1]= 0;
      b[2]= seq;
      memcpy(b + 3, pad_crc[0], 4);
      memset(b + 7, 0, 8);
      b+= 15;
    }
    const size_t last_pad(end - b);
    ut_ad(last_pad >= 15);
    ut_ad(last_pad <= 29);
    b[0]= FILE_CHECKPOINT | byte(last_pad - 14);
    memset(b + 1, 0, last_pad - 14);
    b[last_pad - 13]= seq;
    memcpy(b + last_pad - 12, pad_crc[last_pad - 15], 4);
    memset(b + last_pad - 8, 0, 8);
  }
  else
  {
    /* The lengths of our pad messages vary between 7 and 21 bytes
    (FILE_CHECKPOINT byte, 1 to 15 NUL bytes, sequence byte,
    4 bytes checksum). */
    if (pad < 7)
    {
      extra[0]= FILE_CHECKPOINT | 1;
      extra[1]= 0;
      extra[2]= seq;
      memcpy(extra + 3, pad_crc[0], 4);
      memcpy(b, extra, pad);
      memmove(extra, extra + pad, 7 - pad);
      return 7 - pad;
    }

    /* Pad first with 21-byte messages until the remaining size is
    less than 21+7 bytes, and then write 1 or 2 shorter messages. */
    const byte *const end= begin + pad;
    for (; b + (21 + 7) < end; b+= 21)
    {
      b[0]= FILE_CHECKPOINT | 15;
      memset(b + 1, 0, 15);
      b[16]= seq;
      memcpy(b + 17, pad_crc[14], 4);
    }
    if (b + 21 < end)
    {
      b[0]= FILE_CHECKPOINT | 1;
      b[1]= 0;
      b[2]= seq;
      memcpy(b + 3, pad_crc[0], 4);
      b+= 7;
    }
    const size_t last_pad(end - b);
    ut_ad(last_pad >= 7);
    ut_ad(last_pad <= 21);
    b[0]= FILE_CHECKPOINT | byte(last_pad - 6);
    memset(b + 1, 0, last_pad - 6);
    b[last_pad - 5]= seq;
    memcpy(b + last_pad - 4, pad_crc[last_pad - 7], 4);
  }

  return 0;
}
#endif

/** Write the log buffer to the file and release the mutex.
@param lsn  the current log sequence number */
inline void log_t::write(lsn_t lsn) noexcept
{
  mysql_mutex_assert_owner(&mutex);
  ut_ad(lsn == get_lsn());
  ut_ad(!recv_no_log_write);

  const lsn_t write_lsn{flushed_to_disk_lsn.load(std::memory_order_relaxed)};

  if (lsn == write_lsn)
  {
    /* Nothing to write */
    mysql_mutex_unlock(&mutex);
    return;
  }

#ifdef HAVE_PMEM
  if (is_pmem())
  {
    mysql_mutex_unlock(&mutex);

    const size_t old_offset(calc_lsn_offset(write_lsn));
    const size_t new_offset(calc_lsn_offset(lsn));
    if (UNIV_UNLIKELY(old_offset > new_offset))
    {
      pmem_deep_persist(log_sys.buf + old_offset, file_size - old_offset);
      pmem_deep_persist(log_sys.buf + START_OFFSET, new_offset - START_OFFSET);
    }
    else
      pmem_deep_persist(log_sys.buf + old_offset, new_offset - old_offset);
  }
  else
#endif
  {
    const size_t block_size_1{get_block_size() - 1};
    const lsn_t offset{calc_lsn_offset(write_lsn) & ~block_size_1};
    DBUG_PRINT("ib_log", ("write " LSN_PF " to " LSN_PF " at " LSN_PF,
                          write_lsn, lsn, offset));
    const byte *write_buf{buf};
    size_t length{buf_free};
    ut_ad(length >= (calc_lsn_offset(write_lsn) & block_size_1));
    buf_free&= block_size_1;
    ut_ad(buf_free == ((lsn - first_lsn) & block_size_1));

    if (buf_free)
    {
#if 0 /* TODO: Pad the last log block with dummy records. */
      buf_free= log_pad(lsn, get_block_size() - buf_free,
                        buf + buf_free, flush_buf);
      ... /* TODO: Update the LSN and adjust other code. */
#else
      /* The rest of the block will be written as garbage.
      (We want to avoid memset() while holding log_sys.mutex.)
      This block will be overwritten later, once records beyond
      the current LSN are generated. */
      MEM_MAKE_DEFINED(buf + length, get_block_size() - buf_free);
      buf[length]= 0; /* allow recovery to catch EOF faster */
      length&= ~block_size_1;
      memcpy_aligned<16>(flush_buf, buf + length, (buf_free + 15) & ~15);
      length+= get_block_size();
#endif
    }

    std::swap(buf, flush_buf);
    mysql_mutex_unlock(&mutex);

    if (UNIV_UNLIKELY(srv_shutdown_state > SRV_SHUTDOWN_INITIATED))
    {
      service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                     "InnoDB log write: " LSN_PF, write_lsn);
    }

    /* Do the write to the log file */
    log_write_buf(write_buf, length, offset);
  }

  set_flushed_lsn(lsn);

  log_flush_notify(lsn);
  DBUG_EXECUTE_IF("crash_after_log_write_upto", DBUG_SUICIDE(););
}

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param lsn      log sequence number that should be included in the file write
@param callback log write completion callback */
void log_write_up_to(lsn_t lsn, const completion_callback *callback)
{
  ut_ad(!srv_read_only_mode);
  ut_ad(lsn != LSN_MAX);

  if (UNIV_UNLIKELY(recv_no_ibuf_operations))
  {
    /* A non-final batch of recovery is active no writes to the log
    are allowed yet. */
    ut_a(!callback);
    return;
  }

  ut_ad(lsn <= log_sys.get_lsn());
  if (lsn < log_sys.get_flushed_lsn(std::memory_order_relaxed))
  {
    if (callback)
      callback->m_callback(callback->m_param);
    return;
  }

  while (write_lock.acquire(lsn, callback) == group_commit_lock::ACQUIRED)
  {
    mysql_mutex_lock(&log_sys.mutex);
    const lsn_t wrote_lsn{log_sys.get_flushed_lsn(std::memory_order_relaxed)};
    const lsn_t write_lsn{log_sys.get_lsn(std::memory_order_relaxed)};

    if (wrote_lsn >= write_lsn)
      mysql_mutex_unlock(&log_sys.mutex);
    else
    {
      write_lock.set_pending(write_lsn);
      log_sys.write(write_lsn);
    }

    lsn= write_lock.release(write_lsn);
    if (!lsn)
      break;
    /*
    There is no new group commit lead, some async waiters could stall.
    Rerun log_write_up_to(), to prevent that.
    */
    static const completion_callback dummy{[](void *) {},nullptr};
    callback= &dummy;
  }
}

/** Write to the log file up to the last log entry. */
void log_buffer_flush_to_disk()
{
  ut_ad(!srv_read_only_mode);
  log_write_up_to(log_sys.get_lsn(std::memory_order_acquire));
}

/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.mutex. */
ATTRIBUTE_COLD void log_write_and_flush_prepare()
{
  mysql_mutex_assert_not_owner(&log_sys.mutex);

  while (write_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
}

/** Durably write the log */
ATTRIBUTE_COLD void log_write_and_flush()
{
  ut_ad(!srv_read_only_mode);
  auto lsn= log_sys.get_lsn();
  write_lock.set_pending(lsn);
  log_sys.write(lsn);
  ut_ad(log_sys.get_flushed_lsn() == lsn);
  write_lock.release(lsn);
}

/********************************************************************

Tries to establish a big enough margin of free space in the log buffer, such
that a new log entry can be catenated without an immediate need for a flush. */
ATTRIBUTE_COLD static void log_flush_margin()
{
  mysql_mutex_lock(&log_sys.mutex);
  const bool flush{log_sys.buf_free > log_sys.max_buf_free};
  mysql_mutex_unlock(&log_sys.mutex);

  if (flush)
    log_buffer_flush_to_disk();
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log, such
that a new log entry can be catenated without an immediate need for a
checkpoint. NOTE: this function may only be called if the calling thread
owns no synchronization objects! */
ATTRIBUTE_COLD static void log_checkpoint_margin()
{
  while (log_sys.check_flush_or_checkpoint())
  {
    mysql_mutex_lock(&log_sys.mutex);
    ut_ad(!recv_no_log_write);

    if (!log_sys.check_flush_or_checkpoint())
    {
func_exit:
      mysql_mutex_unlock(&log_sys.mutex);
      return;
    }

    const lsn_t lsn= log_sys.get_lsn();
    const lsn_t checkpoint= log_sys.last_checkpoint_lsn;
    const lsn_t sync_lsn= checkpoint + log_sys.max_checkpoint_age;
    if (lsn <= sync_lsn)
    {
      log_sys.set_check_flush_or_checkpoint(false);
      goto func_exit;
    }

    mysql_mutex_unlock(&log_sys.mutex);

    /* We must wait to prevent the tail of the log overwriting the head. */
    buf_flush_wait_flushed(std::min(sync_lsn, checkpoint + (1U << 20)));
    /* Sleep to avoid a thundering herd */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
ATTRIBUTE_COLD void log_check_margins()
{
  do
  {
    log_flush_margin();
    log_checkpoint_margin();
    ut_ad(!recv_no_log_write);
  }
  while (log_sys.check_flush_or_checkpoint());
}

extern void buf_resize_shutdown();

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown()
{
	lsn_t			lsn;
	ulint			count = 0;

	ib::info() << "Starting shutdown...";

	/* Wait until the master thread and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */
	bool do_srv_shutdown = false;
	if (srv_master_timer) {
		do_srv_shutdown = srv_fast_shutdown < 2;
		srv_master_timer.reset();
	}

	/* Wait for the end of the buffer resize task.*/
	buf_resize_shutdown();
	dict_stats_shutdown();
	btr_defragment_shutdown();

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;

	if (srv_buffer_pool_dump_at_shutdown &&
		!srv_read_only_mode && srv_fast_shutdown < 2) {
		buf_dump_start();
	}
	srv_monitor_timer.reset();

	if (do_srv_shutdown) {
		srv_shutdown(srv_fast_shutdown == 0);
	}


loop:
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);
	ut_ad(fil_system.is_initialised() || !srv_was_started);

#define COUNT_INTERVAL 600U
#define CHECK_INTERVAL 100000U
	std::this_thread::sleep_for(std::chrono::microseconds(CHECK_INTERVAL));

	count++;

	/* Check that there are no longer transactions, except for
	PREPARED ones. We need this wait even for the 'very fast'
	shutdown, because the InnoDB layer may have committed or
	prepared transactions and we don't want to lose them. */

	if (ulint total_trx = srv_was_started && !srv_read_only_mode
	    && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    ? trx_sys.any_active_transactions() : 0) {

		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			service_manager_extend_timeout(
				COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
				"Waiting for %lu active transactions to finish",
				(ulong) total_trx);
			ib::info() << "Waiting for " << total_trx << " active"
				<< " transactions to finish";

			count = 0;
		}

		goto loop;
	}

	/* We need these threads to stop early in shutdown. */
	const char* thread_name;

   if (srv_fast_shutdown != 2 && trx_rollback_is_active) {
		thread_name = "rollback of recovered transactions";
	} else {
		thread_name = NULL;
	}

	if (thread_name) {
		ut_ad(!srv_read_only_mode);
wait_suspend_loop:
		service_manager_extend_timeout(
			COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
			"Waiting for %s to exit", thread_name);
		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			ib::info() << "Waiting for " << thread_name
				   << " to exit";
			count = 0;
		}
		goto loop;
	}

	/* Check that the background threads are suspended */

	ut_ad(!srv_any_background_activity());
	if (srv_n_fil_crypt_threads_started) {
		fil_crypt_threads_signal(true);
		thread_name = "fil_crypt_thread";
		goto wait_suspend_loop;
	}

	if (buf_page_cleaner_is_active) {
		thread_name = "page cleaner thread";
		pthread_cond_signal(&buf_pool.do_flush_list);
		goto wait_suspend_loop;
	}

	buf_load_dump_end();

	if (!buf_pool.is_initialised()) {
		ut_ad(!srv_was_started);
	} else if (ulint pending_io = buf_pool.io_pending()) {
		if (srv_print_verbose_log && count > 600) {
			ib::info() << "Waiting for " << pending_io << " buffer"
				" page I/Os to complete";
			count = 0;
		}

		goto loop;
	} else {
		buf_flush_buffer_pool();
	}

	if (log_sys.is_initialised()) {
		mysql_mutex_lock(&log_sys.mutex);
		const ulint	n_write	= log_sys.n_pending_checkpoint_writes;
		mysql_mutex_unlock(&log_sys.mutex);

		if (n_write) {
			if (srv_print_verbose_log && count > 600) {
				sql_print_information(
					"InnoDB: Pending checkpoint writes: "
					ULINTPF, n_write);
				count = 0;
			}
			goto loop;
		}
	}

	if (srv_fast_shutdown == 2 || !srv_was_started) {
		if (!srv_read_only_mode && srv_was_started) {
			ib::info() << "Executing innodb_fast_shutdown=2."
				" Next startup will execute crash recovery!";

			/* In this fastest shutdown we do not flush the
			buffer pool:

			it is essentially a 'crash' of the InnoDB server.
			Make sure that the log is all flushed to disk, so
			that we can recover all committed transactions in
			a crash recovery. */
			log_buffer_flush_to_disk();
		}

		srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;
		return;
	}

	if (!srv_read_only_mode) {
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
			"ensuring dirty buffer pool are written to log");
		log_make_checkpoint();

                const auto sizeof_cp = log_sys.is_encrypted()
			? SIZE_OF_FILE_CHECKPOINT + 8
			: SIZE_OF_FILE_CHECKPOINT;

		mysql_mutex_lock(&log_sys.mutex);

		lsn = log_sys.get_lsn();

		const bool lsn_changed = lsn != log_sys.last_checkpoint_lsn
			&& lsn != log_sys.last_checkpoint_lsn + sizeof_cp;
		ut_ad(lsn >= log_sys.last_checkpoint_lsn);

		mysql_mutex_unlock(&log_sys.mutex);

		if (lsn_changed) {
			goto loop;
		}
	} else {
		lsn = recv_sys.recovered_lsn;
	}

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
				       "Free innodb buffer pool");
	ut_d(buf_pool.assert_all_freed());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);

	if (UNIV_UNLIKELY(lsn < recv_sys.recovered_lsn)) {
		ib::error() << "Shutdown LSN=" << lsn
			    << " is less than start LSN="
			    << recv_sys.recovered_lsn;
	}

	srv_shutdown_lsn = lsn;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
}

/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file)	/*!< in: file where to print */
{
	double	time_elapsed;
	time_t	current_time;

	mysql_mutex_lock(&log_sys.mutex);

	const lsn_t lsn= log_sys.get_lsn();
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	const lsn_t pages_flushed = buf_pool.get_oldest_modification(lsn);
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

	fprintf(file,
		"Log sequence number " LSN_PF "\n"
		"Log flushed up to   " LSN_PF "\n"
		"Pages flushed up to " LSN_PF "\n"
		"Last checkpoint at  " LSN_PF "\n",
		lsn,
		log_sys.get_flushed_lsn(),
		pages_flushed,
		lsn_t{log_sys.last_checkpoint_lsn});

	current_time = time(NULL);

	time_elapsed = difftime(current_time,
				log_sys.last_printout_time);

	if (time_elapsed <= 0) {
		time_elapsed = 1;
	}

	fprintf(file,
		ULINTPF " pending chkp writes\n"
		ULINTPF " log i/o's done, %.2f log i/o's/second\n",
		log_sys.n_pending_checkpoint_writes,
		log_sys.n_log_ios,
		static_cast<double>(
			log_sys.n_log_ios - log_sys.n_log_ios_old)
		/ time_elapsed);

	log_sys.n_log_ios_old = log_sys.n_log_ios;
	log_sys.last_printout_time = current_time;

	mysql_mutex_unlock(&log_sys.mutex);
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
void
log_refresh_stats(void)
/*===================*/
{
	log_sys.n_log_ios_old = log_sys.n_log_ios;
	log_sys.last_printout_time = time(NULL);
}

/** Shut down the redo log subsystem. */
void log_t::close()
{
  ut_ad(this == &log_sys);
  if (!is_initialised()) return;
  log.close();

#ifndef HAVE_PMEM
  ut_free_dodump(buf, buf_size);
  buf= nullptr;
  ut_free_dodump(flush_buf, buf_size);
  flush_buf= nullptr;
  aligned_free(checkpoint_buf);
  checkpoint_buf= nullptr;
#else
  ut_ad(!checkpoint_buf);
  ut_ad(!buf);
  ut_ad(!flush_buf);
#endif

  mysql_mutex_destroy(&mutex);
  mysql_mutex_destroy(&flush_order_mutex);

  recv_sys.close();

  max_buf_free= 0;
}

std::string get_log_file_path(const char *filename)
{
  const size_t size= strlen(srv_log_group_home_dir) + /* path separator */ 1 +
                     strlen(filename) + /* longest suffix */ 3;
  std::string path;
  path.reserve(size);
  path.assign(srv_log_group_home_dir);

  switch (path.back()) {
#ifdef _WIN32
  case '\\':
#endif
  case '/':
    break;
  default:
    path.push_back('/');
  }
  path.append(filename);

  return path;
}

std::vector<std::string> get_existing_log_files_paths() {
  std::vector<std::string> result;

  for (int i= 0; i < 101; i++) {
    auto path= get_log_file_path(LOG_FILE_NAME_PREFIX)
                                 .append(std::to_string(i));
    os_file_stat_t stat;
    dberr_t err= os_file_get_status(path.c_str(), &stat, false, true);
    if (err)
      break;

    if (stat.type != OS_FILE_TYPE_FILE)
      break;

    result.push_back(std::move(path));
  }

  return result;
}
