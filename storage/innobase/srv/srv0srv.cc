/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, 2009 Google Inc.
Copyright (c) 2009, Percona Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file srv/srv0srv.cc
The database server main program

Created 10/8/1995 Heikki Tuuri
*******************************************************/

/* Dummy comment */
#include "srv0srv.h"

#include "ut0mem.h"
#include "ut0ut.h"
#include "os0proc.h"
#include "mem0mem.h"
#include "mem0pool.h"
#include "sync0sync.h"
#include "que0que.h"
#include "log0recv.h"
#include "pars0pars.h"
#include "usr0sess.h"
#include "lock0lock.h"
#include "trx0purge.h"
#include "ibuf0ibuf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "btr0defragment.h"
#include "btr0sea.h"
#include "dict0load.h"
#include "dict0boot.h"
#include "dict0stats_bg.h" /* dict_stats_event */
#include "srv0start.h"
#include "row0mysql.h"
#include "ha_prototypes.h"
#include "trx0i_s.h"
#include "os0sync.h" /* for HAVE_ATOMIC_BUILTINS */
#include "srv0mon.h"
#include "ut0crc32.h"

#include "mysqld.h"

#include "mysql/plugin.h"
#include "mysql/service_thd_wait.h"


/* How much data manipulation language (DML) statements need to be delayed,
in microseconds, in order to reduce the lagging of the purge thread. */
UNIV_INTERN ulint	srv_dml_needed_delay = 0;

UNIV_INTERN ibool	srv_monitor_active = FALSE;
UNIV_INTERN ibool	srv_error_monitor_active = FALSE;

UNIV_INTERN ibool	srv_buf_dump_thread_active = FALSE;

bool	srv_buf_resize_thread_active = false;

UNIV_INTERN ibool	srv_dict_stats_thread_active = FALSE;

UNIV_INTERN const char*	srv_main_thread_op_info = "";

/** Prefix used by MySQL to indicate pre-5.1 table name encoding */
const char		srv_mysql50_table_name_prefix[10] = "#mysql50#";

/* Server parameters which are read from the initfile */

/* The following three are dir paths which are catenated before file
names, where the file name itself may also contain a path */

UNIV_INTERN char*	srv_data_home	= NULL;

/** Rollback files directory, can be absolute. */
UNIV_INTERN char*	srv_undo_dir = NULL;

/** The number of tablespaces to use for rollback segments. */
UNIV_INTERN ulong	srv_undo_tablespaces = 8;

/** The number of UNDO tablespaces that are open and ready to use. */
UNIV_INTERN ulint	srv_undo_tablespaces_open = 8;

/* The number of rollback segments to use */
UNIV_INTERN ulong	srv_undo_logs = 1;

#ifdef UNIV_LOG_ARCHIVE
UNIV_INTERN char*	srv_arch_dir	= NULL;
#endif /* UNIV_LOG_ARCHIVE */

/** Set if InnoDB must operate in read-only mode. We don't do any
recovery and open all tables in RO mode instead of RW mode. We don't
sync the max trx id to disk either. */
UNIV_INTERN my_bool	srv_read_only_mode;
/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
UNIV_INTERN my_bool	srv_file_per_table;
/** The file format to use on new *.ibd files. */
UNIV_INTERN ulint	srv_file_format = 0;
/** Whether to check file format during startup.  A value of
UNIV_FORMAT_MAX + 1 means no checking ie. FALSE.  The default is to
set it to the highest format we support. */
UNIV_INTERN ulint	srv_max_file_format_at_startup = UNIV_FORMAT_MAX;
/** Set if InnoDB operates in read-only mode or innodb-force-recovery
is greater than SRV_FORCE_NO_TRX_UNDO. */
UNIV_INTERN my_bool	high_level_read_only;

#if UNIV_FORMAT_A
# error "UNIV_FORMAT_A must be 0!"
#endif

/** Place locks to records only i.e. do not use next-key locking except
on duplicate key checking and foreign key checking */
UNIV_INTERN ibool	srv_locks_unsafe_for_binlog = FALSE;
/** Should we call thd_report_row_lock_wait() when a lock request is queued? */
UNIV_INTERN ibool	srv_enable_row_lock_wait_callback = FALSE;
/** Sort buffer size in index creation */
UNIV_INTERN ulong	srv_sort_buf_size = 1048576;
/** Maximum modification log file size for online index creation */
UNIV_INTERN unsigned long long	srv_online_max_size;

/* If this flag is TRUE, then we will use the native aio of the
OS (provided we compiled Innobase with it in), otherwise we will
use simulated aio we build below with threads.
Currently we support native aio on windows and linux */
UNIV_INTERN my_bool	srv_use_native_aio = TRUE;
UNIV_INTERN my_bool	srv_numa_interleave = FALSE;

#ifdef __WIN__
/* Windows native condition variables. We use runtime loading / function
pointers, because they are not available on Windows Server 2003 and
Windows XP/2000.

We use condition for events on Windows if possible, even if os_event
resembles Windows kernel event object well API-wise. The reason is
performance, kernel objects are heavyweights and WaitForSingleObject() is a
performance killer causing calling thread to context switch. Besides, Innodb
is preallocating large number (often millions) of os_events. With kernel event
objects it takes a big chunk out of non-paged pool, which is better suited
for tasks like IO than for storing idle event objects. */
UNIV_INTERN ibool	srv_use_native_conditions = FALSE;
#endif /* __WIN__ */

UNIV_INTERN ulint	srv_n_data_files = 0;
UNIV_INTERN char**	srv_data_file_names = NULL;
/* size in database pages */
UNIV_INTERN ulint*	srv_data_file_sizes = NULL;

/* if TRUE, then we auto-extend the last data file */
UNIV_INTERN ibool	srv_auto_extend_last_data_file	= FALSE;
/* if != 0, this tells the max size auto-extending may increase the
last data file size */
UNIV_INTERN ulint	srv_last_file_size_max	= 0;
/* If the last data file is auto-extended, we add this
many pages to it at a time */
UNIV_INTERN ulong	srv_auto_extend_increment = 8;
UNIV_INTERN ulint*	srv_data_file_is_raw_partition = NULL;

/* If the following is TRUE we do not allow inserts etc. This protects
the user from forgetting the 'newraw' keyword to my.cnf */

UNIV_INTERN ibool	srv_created_new_raw	= FALSE;

UNIV_INTERN char*	srv_log_group_home_dir	= NULL;

UNIV_INTERN ulong	srv_n_log_files		= SRV_N_LOG_FILES_MAX;
/* size in database pages */
UNIV_INTERN ib_uint64_t	srv_log_file_size	= IB_UINT64_MAX;
UNIV_INTERN ib_uint64_t	srv_log_file_size_requested;
/* size in database pages */
UNIV_INTERN ulint	srv_log_buffer_size	= ULINT_MAX;
UNIV_INTERN ulong	srv_flush_log_at_trx_commit = 1;
UNIV_INTERN uint	srv_flush_log_at_timeout = 1;
UNIV_INTERN ulong	srv_page_size		= UNIV_PAGE_SIZE_DEF;
UNIV_INTERN ulong	srv_page_size_shift	= UNIV_PAGE_SIZE_SHIFT_DEF;

/* Try to flush dirty pages so as to avoid IO bursts at
the checkpoints. */
UNIV_INTERN char	srv_adaptive_flushing	= TRUE;

/** Maximum number of times allowed to conditionally acquire
mutex before switching to blocking wait on the mutex */
#define MAX_MUTEX_NOWAIT	20

/** Check whether the number of failed nonblocking mutex
acquisition attempts exceeds maximum allowed value. If so,
srv_printf_innodb_monitor() will request mutex acquisition
with mutex_enter(), which will wait until it gets the mutex. */
#define MUTEX_NOWAIT(mutex_skipped)	((mutex_skipped) < MAX_MUTEX_NOWAIT)

/** The sort order table of the MySQL latin1_swedish_ci character set
collation */
UNIV_INTERN const byte*	srv_latin1_ordering;

/* use os/external memory allocator */
UNIV_INTERN my_bool	srv_use_sys_malloc	= TRUE;
/* requested size in bytes */
UNIV_INTERN ulint	srv_buf_pool_size	= ULINT_MAX;
/* requested unit size of chunks in bytes */
ulong	srv_buf_pool_chunk_unit = 0;
/* force virtual page preallocation (prefault) */
UNIV_INTERN my_bool	srv_buf_pool_populate	= FALSE;
/* requested number of buffer pool instances */
UNIV_INTERN ulint       srv_buf_pool_instances  = 1;
/* number of locks to protect buf_pool->page_hash */
UNIV_INTERN ulong	srv_n_page_hash_locks = 16;
/** Scan depth for LRU flush batch i.e.: number of blocks scanned*/
UNIV_INTERN ulong	srv_LRU_scan_depth	= 1024;
/** whether or not to flush neighbors of a block */
UNIV_INTERN ulong	srv_flush_neighbors	= 1;
/* previously requested size */
ulint	srv_buf_pool_old_size	= 0;
/* current size as scaling factor for the other components */
ulint	srv_buf_pool_base_size	= 0;
/* current size in kilobytes */
UNIV_INTERN ulint	srv_buf_pool_curr_size	= 0;
/* dump that may % of each buffer pool during BP dump */
UNIV_INTERN ulong srv_buf_pool_dump_pct;
/* requested size (number) */
UNIV_INTERN ulint	srv_sync_pool_size	= 1024;
/* size in bytes */
UNIV_INTERN ulint	srv_mem_pool_size	= ULINT_MAX;
UNIV_INTERN ulint	srv_lock_table_size	= ULINT_MAX;

/* Defragmentation */
UNIV_INTERN my_bool	srv_defragment = TRUE;
UNIV_INTERN my_bool	srv_defragment_pause = FALSE;
UNIV_INTERN uint	srv_defragment_n_pages = 7;
UNIV_INTERN uint	srv_defragment_stats_accuracy = 0;
UNIV_INTERN uint	srv_defragment_fill_factor_n_recs = 20;
UNIV_INTERN double	srv_defragment_fill_factor = 0.9;
UNIV_INTERN uint	srv_defragment_frequency =
	SRV_DEFRAGMENT_FREQUENCY_DEFAULT;
UNIV_INTERN ulonglong	srv_defragment_interval = 0;
UNIV_INTERN uint srv_defragment_max_runtime_pct = 50;

UNIV_INTERN ulint	srv_idle_flush_pct = 100;

/* This parameter is deprecated. Use srv_n_io_[read|write]_threads
instead. */
UNIV_INTERN ulint	srv_n_file_io_threads	= ULINT_MAX;
UNIV_INTERN ulint	srv_n_read_io_threads	= ULINT_MAX;
UNIV_INTERN ulint	srv_n_write_io_threads	= ULINT_MAX;

/* Switch to enable random read ahead. */
UNIV_INTERN my_bool	srv_random_read_ahead	= FALSE;
/* User settable value of the number of pages that must be present
in the buffer cache and accessed sequentially for InnoDB to trigger a
readahead request. */
UNIV_INTERN ulong	srv_read_ahead_threshold	= 56;

/** Maximum on-disk size of change buffer in terms of percentage
of the buffer pool. */
uint	srv_change_buffer_max_size = CHANGE_BUFFER_DEFAULT_SIZE;

UNIV_INTERN ulong	srv_trx_log_write_block_size	= 4096;

#ifdef UNIV_LOG_ARCHIVE
UNIV_INTERN ibool		srv_log_archive_on	= FALSE;
UNIV_INTERN ibool		srv_archive_recovery	= 0;
UNIV_INTERN ib_uint64_t	srv_archive_recovery_limit_lsn;
#endif /* UNIV_LOG_ARCHIVE */

/* This parameter is used to throttle the number of insert buffers that are
merged in a batch. By increasing this parameter on a faster disk you can
possibly reduce the number of I/O operations performed to complete the
merge operation. The value of this parameter is used as is by the
background loop when the system is idle (low load), on a busy system
the parameter is scaled down by a factor of 4, this is to avoid putting
a heavier load on the I/O sub system. */

UNIV_INTERN ulong	srv_insert_buffer_batch_size = 20;

UNIV_INTERN char*	srv_file_flush_method_str = NULL;
UNIV_INTERN ulint	srv_unix_file_flush_method = SRV_UNIX_FSYNC;
UNIV_INTERN ulint	srv_win_file_flush_method = SRV_WIN_IO_UNBUFFERED;
UNIV_INTERN my_bool	srv_use_fdatasync = FALSE;

UNIV_INTERN ulint	srv_max_n_open_files	  = 300;

/* Number of IO operations per second the server can do */
UNIV_INTERN ulong	srv_io_capacity         = 200;
UNIV_INTERN ulong	srv_max_io_capacity     = 400;

/* The InnoDB main thread tries to keep the ratio of modified pages
in the buffer pool to all database pages in the buffer pool smaller than
the following number. But it is not guaranteed that the value stays below
that during a time of heavy update/insert activity. */

UNIV_INTERN double	srv_max_buf_pool_modified_pct	= 75.0;
UNIV_INTERN double	srv_max_dirty_pages_pct_lwm	= 50.0;

/* This is the percentage of log capacity at which adaptive flushing,
if enabled, will kick in. */
UNIV_INTERN double	srv_adaptive_flushing_lwm	= 10.0;

/* Number of iterations over which adaptive flushing is averaged. */
UNIV_INTERN ulong	srv_flushing_avg_loops		= 30;

/* The number of purge threads to use.*/
UNIV_INTERN ulong	srv_n_purge_threads = 1;

/* the number of pages to purge in one batch */
UNIV_INTERN ulong	srv_purge_batch_size = 20;

/* Internal setting for "innodb_stats_method". Decides how InnoDB treats
NULL value when collecting statistics. By default, it is set to
SRV_STATS_NULLS_EQUAL(0), ie. all NULL value are treated equal */
UNIV_INTERN ulong srv_innodb_stats_method = SRV_STATS_NULLS_EQUAL;

/* Enable adaptive sleep time calculation for page cleaner thread if enabled. */
UNIV_INTERN my_bool	srv_pc_adaptive_sleep;

/** The maximum time limit for a single LRU tail flush iteration by the page
cleaner thread */
UNIV_INTERN ulint	srv_cleaner_max_lru_time = 1000;

UNIV_INTERN srv_stats_t	srv_stats;

/* structure to pass status variables to MySQL */
UNIV_INTERN export_var_t export_vars;

/** Time doing a checkpoint */
UNIV_INTERN ulonglong   srv_checkpoint_time	= 0;

/** Time in insert buffer */
UNIV_INTERN ulonglong   srv_ibuf_contract_time	= 0;

/** Time flushing logs */
UNIV_INTERN ulonglong   srv_log_flush_time	= 0;

/** Time enforcing dict cache limit */
UNIV_INTERN ulonglong   srv_cache_limit_time	= 0;

/** Time checking log freespace */
UNIV_INTERN ulonglong   srv_free_log_time	= 0;

/** Time doing background table drop */
UNIV_INTERN ulonglong   srv_drop_table_time	= 0;

/** Time in trx_purge */
UNIV_INTERN ulonglong   srv_purge_time		= 0;

/** Normally 0. When nonzero, skip some phases of crash recovery,
starting from SRV_FORCE_IGNORE_CORRUPT, so that data can be recovered
by SELECT or mysqldump. When this is nonzero, we do not allow any user
modifications to the data. */
UNIV_INTERN ulong	srv_force_recovery;
#ifdef XTRABACKUP
/** Special XTRABACKUP command line switch to disable background
tasks. The effect is that of srv_force_recovery=SRV_FORCE_NO_BACKGROUND
without srv_force_recovery=SRV_FORCE_IGNORE_CORRUPT. */
extern my_bool		xtrabackup_no_background;
#endif // ifdef XTRABACKUP
#ifndef DBUG_OFF
/** Inject a crash at different steps of the recovery process.
This is for testing and debugging only. */
UNIV_INTERN ulong	srv_force_recovery_crash;
#endif /* !DBUG_OFF */

/** Number of deadlocks */
UNIV_INTERN ulint	srv_lock_deadlocks	= 0;

/** Number of row lock wait timeouts */
UNIV_INTERN ulint	srv_lock_wait_timeouts	= 0;

/** Number times purge skipped a row because the table had been dropped */
UNIV_INTERN ulint srv_drop_purge_skip_row = 0;

/** Number times ibuf merges skipped a row becaue the table had been dropped */
UNIV_INTERN ulint srv_drop_ibuf_skip_row = 0;

/** Count as "slow" file read, write and fsync requests that take this long */
UNIV_INTERN ulint	srv_io_slow_usecs	= 0;

/** Count as "old" file read and write requests that wait this long in bg arrays
before getting scheduled */
UNIV_INTERN ulint	srv_io_old_usecs	= 0;

/** Maximum number of outstanding aio requests. We stop submitting batched
aio read requests when the number of outstanding aio requests exceed this
number. But the maximum is not strictly enforced. There could be a short period
of time the number is exceeded. */
UNIV_INTERN ulong	srv_io_outstanding_requests = 0;

#ifdef UNIV_DEBUG
/** Support diabling insert buffer merges during testing */
UNIV_INTERN my_bool srv_allow_ibuf_merges = TRUE;
#endif

/** Number of extra writes done in buf_flush_try_neighbors from LRU list */
UNIV_INTERN ulint	srv_neighbors_flushed_lru	= 0;

/** Number of extra writes done in buf_flush_try_neighbors from flush list */
UNIV_INTERN ulint	srv_neighbors_flushed_list	= 0;

/** Print all user-level transactions deadlocks to mysqld stderr */

UNIV_INTERN my_bool	srv_print_all_deadlocks = FALSE;

/** Perform deadlock detection check. */
UNIV_INTERN my_bool	srv_deadlock_detect = TRUE;

/** Maximum number of deadlock detection steps allowed. */
UNIV_INTERN uint	srv_max_deadlock_detection_steps = 0;

/** Enable INFORMATION_SCHEMA.innodb_cmp_per_index */
UNIV_INTERN my_bool	srv_cmp_per_index_enabled = FALSE;

/* If the following is set to 1 then we do not run purge and insert buffer
merge to completion before shutdown. If it is set to 2, do not even flush the
buffer pool to data files at the shutdown: we effectively 'crash'
InnoDB (but lose no committed transactions). */
UNIV_INTERN ulint	srv_fast_shutdown	= 0;

/* Generate a innodb_status.<pid> file */
UNIV_INTERN ibool	srv_innodb_status	= FALSE;

/* Log query that use gap lock if this flag is set */
UNIV_INTERN my_bool srv_monitor_gaplock_query = FALSE;
/* Log verbose information along with the gaplock query */
UNIV_INTERN my_bool srv_monitor_gaplock_query_print_verbose = FALSE;
/* Filename to log the query that use gap locking */
UNIV_INTERN char* srv_monitor_gaplock_query_filename = NULL;
/* Mutex to synchronize co-ordination for logging gap queries */
UNIV_INTERN ib_mutex_t srv_monitor_gaplock_query_mutex;
/* File handle to log the query that use gap lock */
UNIV_INTERN FILE* srv_monitor_gaplock_query_file = NULL;

UNIV_INTERN my_bool srv_recv_ibuf_operations = FALSE;

/* Optimize prefix index queries to skip cluster index lookup when possible */
/* Enables or disables this prefix optimization.  Disabled by default. */
UNIV_INTERN my_bool	srv_prefix_index_cluster_optimization = 0;

/* When estimating number of different key values in an index, sample
this many index pages, there are 2 ways to calculate statistics:
* persistent stats that are calculated by ANALYZE TABLE and saved
  in the innodb database.
* quick transient stats, that are used if persistent stats for the given
  table/index are not found in the innodb database */
UNIV_INTERN unsigned long long	srv_stats_transient_sample_pages = 8;
UNIV_INTERN my_bool		srv_stats_persistent = TRUE;
UNIV_INTERN my_bool		srv_stats_include_delete_marked = FALSE;
UNIV_INTERN unsigned long long	srv_stats_persistent_sample_pages = 20;
UNIV_INTERN my_bool		srv_stats_auto_recalc = TRUE;
UNIV_INTERN double		srv_stats_recalc_threshold = 0.1;

UNIV_INTERN ulong	srv_use_doublewrite_buf	= 1;
UNIV_INTERN my_bool	srv_doublewrite_reset = FALSE;

/** doublewrite buffer is 1MB is size i.e.: it can hold 128 16K pages.
The following parameter is the size of the buffer that is used for
batch flushing i.e.: LRU flushing and flush_list flushing. The rest
of the pages are used for single page flushing. */
UNIV_INTERN ulong	srv_doublewrite_batch_size	= 120;

UNIV_INTERN ulong	srv_replication_delay		= 0;

#ifdef XTRABACKUP
UNIV_INTERN ibool	srv_apply_log_only	= FALSE;
#endif /* XTRABACKUP */

/*-------------------------------------------*/
UNIV_INTERN ulong	srv_n_spin_wait_rounds	= 30;
UNIV_INTERN ulong	srv_spin_wait_delay	= 6;
UNIV_INTERN ibool	srv_priority_boost	= TRUE;

#ifdef UNIV_DEBUG
UNIV_INTERN ibool	srv_print_thread_releases	= FALSE;
UNIV_INTERN ibool	srv_print_lock_waits		= FALSE;
UNIV_INTERN ibool	srv_print_buf_io		= FALSE;
UNIV_INTERN ibool	srv_print_log_io		= FALSE;
UNIV_INTERN ibool	srv_print_latch_waits		= FALSE;
#endif /* UNIV_DEBUG */

static ulint		srv_n_rows_inserted_old		= 0;
static ulint		srv_n_rows_updated_old		= 0;
static ulint		srv_n_rows_deleted_old		= 0;
static ulint		srv_n_rows_read_old		= 0;
static ulint		srv_n_system_rows_inserted_old	= 0;
static ulint		srv_n_system_rows_updated_old	= 0;
static ulint		srv_n_system_rows_deleted_old	= 0;
static ulint		srv_n_system_rows_read_old	= 0;

UNIV_INTERN ulint	srv_truncated_status_writes	= 0;
UNIV_INTERN ulint	srv_available_undo_logs         = 0;

/* Set the following to 0 if you want InnoDB to write messages on
stderr on startup/shutdown. */
UNIV_INTERN ibool	srv_print_verbose_log		= TRUE;
UNIV_INTERN my_bool	srv_print_innodb_monitor	= FALSE;
UNIV_INTERN my_bool	srv_print_innodb_lock_monitor	= FALSE;
UNIV_INTERN ibool	srv_print_innodb_tablespace_monitor = FALSE;
UNIV_INTERN ibool	srv_print_innodb_table_monitor = FALSE;

/* Array of English strings describing the current state of an
i/o handler thread */

UNIV_INTERN const char* srv_io_thread_op_info[SRV_MAX_N_IO_THREADS];
UNIV_INTERN const char* srv_io_thread_function[SRV_MAX_N_IO_THREADS];

UNIV_INTERN time_t	srv_last_monitor_time;

UNIV_INTERN ib_mutex_t	srv_innodb_monitor_mutex;

/* Mutex for locking srv_monitor_file. Not created if srv_read_only_mode */
UNIV_INTERN ib_mutex_t	srv_monitor_file_mutex;

#ifdef UNIV_PFS_MUTEX
# ifndef HAVE_ATOMIC_BUILTINS
/* Key to register server_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	server_mutex_key;
# endif /* !HAVE_ATOMIC_BUILTINS */
/** Key to register srv_innodb_monitor_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_innodb_monitor_mutex_key;
/** Key to register srv_monitor_file_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_monitor_file_mutex_key;
/** Key to register srv_monitor_gaplock_query_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_monitor_gaplock_query_mutex_key;
/** Key to register longest_buf_resizing_trx_stall_time_mutex */
UNIV_INTERN mysql_pfs_key_t	longest_buf_resizing_trx_stall_time_mutex_key;
/** Key to register srv_dict_tmpfile_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_dict_tmpfile_mutex_key;
/** Key to register the mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_misc_tmpfile_mutex_key;
/** Key to register srv_sys_t::mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_sys_mutex_key;
/** Key to register srv_sys_t::tasks_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_sys_tasks_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/** Temporary file for innodb monitor output */
UNIV_INTERN FILE*	srv_monitor_file;
/** Mutex for locking srv_dict_tmpfile. Not created if srv_read_only_mode.
This mutex has a very high rank; threads reserving it should not
be holding any InnoDB latches. */
UNIV_INTERN ib_mutex_t	srv_dict_tmpfile_mutex;
/** Temporary file for output from the data dictionary */
UNIV_INTERN FILE*	srv_dict_tmpfile;
/** Mutex for locking srv_misc_tmpfile. Not created if srv_read_only_mode.
This mutex has a very low rank; threads reserving it should not
acquire any further latches or sleep before releasing this one. */
UNIV_INTERN ib_mutex_t	srv_misc_tmpfile_mutex;
/** Temporary file for miscellanous diagnostic output */
UNIV_INTERN FILE*	srv_misc_tmpfile;

/* Simulate compression failures. */
UNIV_INTERN uint srv_simulate_comp_failures = 0;

UNIV_INTERN ulint	srv_main_thread_process_no	= 0;
UNIV_INTERN ulint	srv_main_thread_id		= 0;

/** See the sync_checkpoint_limit user variable declaration in ha_innodb.cc */
UNIV_INTERN ulint	srv_sync_checkpoint_limit	= 0;

/** If false, there will be no table stats update from the replication
slave thread. */
UNIV_INTERN my_bool	srv_enable_slave_update_table_stats = FALSE;

/** Number of pages processed by trx_purge */
UNIV_INTERN ulint	srv_purged_pages	= 0;

/* The following counts are used by the srv_master_thread. */

/** Iterations of the loop bounded by 'srv_active' label. */
static ulint		srv_main_active_loops		= 0;
/** Iterations of the loop bounded by the 'srv_idle' label. */
static ulint		srv_main_idle_loops		= 0;
/** Iterations of the loop bounded by the 'srv_shutdown' label. */
static ulint		srv_main_shutdown_loops		= 0;
/** Log writes involving flush. */
static ulint		srv_log_writes_and_flush	= 0;

ulint	srv_n_commit_all	= 0;
ulint	srv_n_commit_with_undo	= 0;
ulint	srv_n_rollback_total	= 0;
ulint	srv_n_rollback_partial	= 0;

// big_file_slow_removal directory
UNIV_INTERN ulong srv_slowrm_speed_mbps = 0;

/** Number of times secondary index lookup triggered cluster lookup */
atomic_stat<ulint>	srv_sec_rec_cluster_reads;

/** Number of times prefix optimization avoided triggering cluster lookup */
atomic_stat<ulint>	srv_sec_rec_cluster_reads_avoided;


/* This is only ever touched by the master thread. It records the
time when the last flush of log file has happened. The master
thread ensures that we flush the log files at least once per
second. */
static time_t	srv_last_log_flush_time;

/* Interval in seconds at which various tasks are performed by the
master thread when server is active. In order to balance the workload,
we should try to keep intervals such that they are not multiple of
each other. For example, if we have intervals for various tasks
defined as 5, 10, 15, 60 then all tasks will be performed when
current_time % 60 == 0 and no tasks will be performed when
current_time % 5 != 0. */

# define	SRV_MASTER_CHECKPOINT_INTERVAL		(7)
# define	SRV_MASTER_PURGE_INTERVAL		(10)
#ifdef MEM_PERIODIC_CHECK
# define	SRV_MASTER_MEM_VALIDATE_INTERVAL	(13)
#endif /* MEM_PERIODIC_CHECK */
# define	SRV_MASTER_DICT_LRU_INTERVAL		(47)

/** Acquire the system_mutex. */
#define srv_sys_mutex_enter() do {			\
	mutex_enter(&srv_sys->mutex);			\
} while (0)

/** Test if the system mutex is owned. */
#define srv_sys_mutex_own() (mutex_own(&srv_sys->mutex)	\
			     && !srv_read_only_mode)

/** Release the system mutex. */
#define srv_sys_mutex_exit() do {			\
	mutex_exit(&srv_sys->mutex);			\
} while (0)

#define fetch_lock_wait_timeout(trx)			\
	((trx)->lock.allowed_to_wait			\
	 ? thd_lock_wait_timeout((trx)->mysql_thd)	\
	 : 0)

/*
	IMPLEMENTATION OF THE SERVER MAIN PROGRAM
	=========================================

There is the following analogue between this database
server and an operating system kernel:

DB concept			equivalent OS concept
----------			---------------------
transaction		--	process;

query thread		--	thread;

lock			--	semaphore;

kernel			--	kernel;

query thread execution:
(a) without lock mutex
reserved		--	process executing in user mode;
(b) with lock mutex reserved
			--	process executing in kernel mode;

The server has several backgroind threads all running at the same
priority as user threads. It periodically checks if here is anything
happening in the server which requires intervention of the master
thread. Such situations may be, for example, when flushing of dirty
blocks is needed in the buffer pool or old version of database rows
have to be cleaned away (purged). The user can configure a separate
dedicated purge thread(s) too, in which case the master thread does not
do any purging.

The threads which we call user threads serve the queries of the MySQL
server. They run at normal priority.

When there is no activity in the system, also the master thread
suspends itself to wait for an event making the server totally silent.

There is still one complication in our server design. If a
background utility thread obtains a resource (e.g., mutex) needed by a user
thread, and there is also some other user activity in the system,
the user thread may have to wait indefinitely long for the
resource, as the OS does not schedule a background thread if
there is some other runnable user thread. This problem is called
priority inversion in real-time programming.

One solution to the priority inversion problem would be to keep record
of which thread owns which resource and in the above case boost the
priority of the background thread so that it will be scheduled and it
can release the resource.  This solution is called priority inheritance
in real-time programming.  A drawback of this solution is that the overhead
of acquiring a mutex increases slightly, maybe 0.2 microseconds on a 100
MHz Pentium, because the thread has to call os_thread_get_curr_id.  This may
be compared to 0.5 microsecond overhead for a mutex lock-unlock pair. Note
that the thread cannot store the information in the resource , say mutex,
itself, because competing threads could wipe out the information if it is
stored before acquiring the mutex, and if it stored afterwards, the
information is outdated for the time of one machine instruction, at least.
(To be precise, the information could be stored to lock_word in mutex if
the machine supports atomic swap.)

The above solution with priority inheritance may become actual in the
future, currently we do not implement any priority twiddling solution.
Our general aim is to reduce the contention of all mutexes by making
them more fine grained.

The thread table contains information of the current status of each
thread existing in the system, and also the event semaphores used in
suspending the master thread and utility threads when they have nothing
to do.  The thread table can be seen as an analogue to the process table
in a traditional Unix implementation. */

/** The server system struct */
struct srv_sys_t{
	ib_mutex_t	tasks_mutex;		/*!< variable protecting the
						tasks queue */
	UT_LIST_BASE_NODE_T(que_thr_t)
			tasks;			/*!< task queue */

	ib_mutex_t	mutex;			/*!< variable protecting the
						fields below. */
	ulint		n_sys_threads;		/*!< size of the sys_threads
						array */

	srv_slot_t*	sys_threads;		/*!< server thread table */

	ulint		n_threads_active[SRV_MASTER + 1];
						/*!< number of threads active
						in a thread class */

	srv_stats_t::ulint_ctr_1_t
			activity_count;		/*!< For tracking server
						activity */
};

#ifndef HAVE_ATOMIC_BUILTINS
/** Mutex protecting some server global variables. */
UNIV_INTERN ib_mutex_t	server_mutex;
#endif /* !HAVE_ATOMIC_BUILTINS */

static srv_sys_t*	srv_sys	= NULL;

/** Event to signal the monitor thread. */
UNIV_INTERN os_event_t	srv_monitor_event;

/** Event to signal the error thread */
UNIV_INTERN os_event_t	srv_error_event;

/** Event to signal the buffer pool dump/load thread */
UNIV_INTERN os_event_t	srv_buf_dump_event;

/** Event to signal the buffer pool resize thread */
os_event_t	srv_buf_resize_event;

/** The buffer pool dump/load file name */
UNIV_INTERN char*	srv_buf_dump_filename;

/** Boolean config knobs that tell InnoDB to dump the buffer pool at shutdown
and/or load it during startup. */
UNIV_INTERN char	srv_buffer_pool_dump_at_shutdown = FALSE;
UNIV_INTERN char	srv_buffer_pool_load_at_startup = FALSE;

/** Slot index in the srv_sys->sys_threads array for the purge thread. */
static const ulint	SRV_PURGE_SLOT	= 1;

/** Slot index in the srv_sys->sys_threads array for the master thread. */
static const ulint	SRV_MASTER_SLOT = 0;

/*********************************************************************//**
Prints counters for work done by srv_master_thread. */
static
void
srv_print_master_thread_info(
/*=========================*/
	FILE  *file)    /* in: output stream */
{
	fprintf(file, "srv_master_thread loops: %lu srv_active, "
		"%lu srv_shutdown, %lu srv_idle\n",
		srv_main_active_loops,
		srv_main_shutdown_loops,
		srv_main_idle_loops);

	fprintf(file,
		"Seconds in background IO: %.2f insert buffer, "
		"%.2f log flush, %.2f purge, %.2f cache limit, "
		"%.2f free log, %.2f drop table, %.2f checkpoint\n",
		my_timer_to_seconds(srv_ibuf_contract_time),
		my_timer_to_seconds(srv_log_flush_time),
		my_timer_to_seconds(srv_cache_limit_time),
		my_timer_to_seconds(srv_free_log_time),
		my_timer_to_seconds(srv_drop_table_time),
		my_timer_to_seconds(srv_purge_time),
		my_timer_to_seconds(srv_checkpoint_time));

	fprintf(file, "srv_master_thread log flush and writes: %lu\n",
		srv_log_writes_and_flush);
}

/*********************************************************************//**
Sets the info describing an i/o thread current state. */
UNIV_INTERN
void
srv_set_io_thread_op_info(
/*======================*/
	ulint		i,	/*!< in: the 'segment' of the i/o thread */
	const char*	str)	/*!< in: constant char string describing the
				state */
{
	ut_a(i < SRV_MAX_N_IO_THREADS);

	srv_io_thread_op_info[i] = str;
}

/*********************************************************************//**
Resets the info describing an i/o thread current state. */
UNIV_INTERN
void
srv_reset_io_thread_op_info()
/*=========================*/
{
	for (ulint i = 0; i < UT_ARR_SIZE(srv_io_thread_op_info); ++i) {
		srv_io_thread_op_info[i] = "not started yet";
	}
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Validates the type of a thread table slot.
@return TRUE if ok */
static
ibool
srv_thread_type_validate(
/*=====================*/
	srv_thread_type	type)	/*!< in: thread type */
{
	switch (type) {
	case SRV_NONE:
		break;
	case SRV_WORKER:
	case SRV_PURGE:
	case SRV_MASTER:
		return(TRUE);
	}
	ut_error;
	return(FALSE);
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Gets the type of a thread table slot.
@return thread type */
static
srv_thread_type
srv_slot_get_type(
/*==============*/
	const srv_slot_t*	slot)	/*!< in: thread slot */
{
	srv_thread_type	type = slot->type;
	ut_ad(srv_thread_type_validate(type));
	return(type);
}

/*********************************************************************//**
Reserves a slot in the thread table for the current thread.
@return	reserved slot */
static
srv_slot_t*
srv_reserve_slot(
/*=============*/
	srv_thread_type	type)	/*!< in: type of the thread */
{
	srv_slot_t*	slot = 0;

	srv_sys_mutex_enter();

	ut_ad(srv_thread_type_validate(type));

	switch (type) {
	case SRV_MASTER:
		slot = &srv_sys->sys_threads[SRV_MASTER_SLOT];
		break;

	case SRV_PURGE:
		slot = &srv_sys->sys_threads[SRV_PURGE_SLOT];
		break;

	case SRV_WORKER:
		/* Find an empty slot, skip the master and purge slots. */
		for (slot = &srv_sys->sys_threads[2];
		     slot->in_use;
		     ++slot) {

			ut_a(slot < &srv_sys->sys_threads[
			     srv_sys->n_sys_threads]);
		}
		break;

	case SRV_NONE:
		ut_error;
	}

	ut_a(!slot->in_use);

	slot->in_use = TRUE;
	slot->suspended = FALSE;
	slot->type = type;

	ut_ad(srv_slot_get_type(slot) == type);

	++srv_sys->n_threads_active[type];

	srv_sys_mutex_exit();

	return(slot);
}

/*********************************************************************//**
Suspends the calling thread to wait for the event in its thread slot.
@return the current signal count of the event. */
static
ib_int64_t
srv_suspend_thread_low(
/*===================*/
	srv_slot_t*	slot)	/*!< in/out: thread slot */
{

	ut_ad(!srv_read_only_mode);
	ut_ad(srv_sys_mutex_own());

	ut_ad(slot->in_use);

	srv_thread_type	type = srv_slot_get_type(slot);

	switch (type) {
	case SRV_NONE:
		ut_error;

	case SRV_MASTER:
		/* We have only one master thread and it
		should be the first entry always. */
		ut_a(srv_sys->n_threads_active[type] == 1);
		break;

	case SRV_PURGE:
		/* We have only one purge coordinator thread
		and it should be the second entry always. */
		ut_a(srv_sys->n_threads_active[type] == 1);
		break;

	case SRV_WORKER:
		ut_a(srv_n_purge_threads > 1);
		ut_a(srv_sys->n_threads_active[type] > 0);
		break;
	}

	ut_a(!slot->suspended);
	slot->suspended = TRUE;

	ut_a(srv_sys->n_threads_active[type] > 0);

	srv_sys->n_threads_active[type]--;

	return(os_event_reset(slot->event));
}

/*********************************************************************//**
Suspends the calling thread to wait for the event in its thread slot.
@return the current signal count of the event. */
static
ib_int64_t
srv_suspend_thread(
/*===============*/
	srv_slot_t*	slot)	/*!< in/out: thread slot */
{
	srv_sys_mutex_enter();

	ib_int64_t	sig_count = srv_suspend_thread_low(slot);

	srv_sys_mutex_exit();

	return(sig_count);
}

/*********************************************************************//**
Releases threads of the type given from suspension in the thread table.
NOTE! The server mutex has to be reserved by the caller!
@return number of threads released: this may be less than n if not
        enough threads were suspended at the moment. */
UNIV_INTERN
ulint
srv_release_threads(
/*================*/
	srv_thread_type	type,	/*!< in: thread type */
	ulint		n)	/*!< in: number of threads to release */
{
	ulint		i;
	ulint		count	= 0;

	ut_ad(srv_thread_type_validate(type));
	ut_ad(n > 0);

	srv_sys_mutex_enter();

	for (i = 0; i < srv_sys->n_sys_threads; i++) {
		srv_slot_t*	slot;

		slot = &srv_sys->sys_threads[i];

		if (slot->in_use
		    && srv_slot_get_type(slot) == type
		    && slot->suspended) {

			switch (type) {
			case SRV_NONE:
				ut_error;

			case SRV_MASTER:
				/* We have only one master thread and it
				should be the first entry always. */
				ut_a(n == 1);
				ut_a(i == SRV_MASTER_SLOT);
				ut_a(srv_sys->n_threads_active[type] == 0);
				break;

			case SRV_PURGE:
				/* We have only one purge coordinator thread
				and it should be the second entry always. */
				ut_a(n == 1);
				ut_a(i == SRV_PURGE_SLOT);
				ut_a(srv_n_purge_threads > 0);
				ut_a(srv_sys->n_threads_active[type] == 0);
				break;

			case SRV_WORKER:
				ut_a(srv_n_purge_threads > 1);
				ut_a(srv_sys->n_threads_active[type]
				     < srv_n_purge_threads - 1);
				break;
			}

			slot->suspended = FALSE;

			++srv_sys->n_threads_active[type];

			os_event_set(slot->event);

			if (++count == n) {
				break;
			}
		}
	}

	srv_sys_mutex_exit();

	return(count);
}

/*********************************************************************//**
Release a thread's slot. */
static
void
srv_free_slot(
/*==========*/
	srv_slot_t*	slot)	/*!< in/out: thread slot */
{
	srv_sys_mutex_enter();

	if (!slot->suspended) {
		/* Mark the thread as inactive. */
		srv_suspend_thread_low(slot);
	}

	/* Free the slot for reuse. */
	ut_ad(slot->in_use);
	slot->in_use = FALSE;

	srv_sys_mutex_exit();
}

/*********************************************************************//**
Initializes the server. */
UNIV_INTERN
void
srv_init(void)
/*==========*/
{
	ulint	n_sys_threads = 0;
	ulint	srv_sys_sz = sizeof(*srv_sys);

#ifndef HAVE_ATOMIC_BUILTINS
	mutex_create(server_mutex_key, &server_mutex, SYNC_ANY_LATCH);
#endif /* !HAVE_ATOMIC_BUILTINS */

	mutex_create(srv_innodb_monitor_mutex_key,
		     &srv_innodb_monitor_mutex, SYNC_NO_ORDER_CHECK);

	if (!srv_read_only_mode) {

		/* Number of purge threads + master thread */
		n_sys_threads = srv_n_purge_threads + 1;

		srv_sys_sz += n_sys_threads * sizeof(*srv_sys->sys_threads);
	}

	srv_sys = static_cast<srv_sys_t*>(mem_zalloc(srv_sys_sz));

	srv_sys->n_sys_threads = n_sys_threads;

	if (!srv_read_only_mode) {

		mutex_create(srv_sys_mutex_key, &srv_sys->mutex, SYNC_THREADS);

		mutex_create(srv_sys_tasks_mutex_key,
			     &srv_sys->tasks_mutex, SYNC_ANY_LATCH);

		srv_sys->sys_threads = (srv_slot_t*) &srv_sys[1];

		for (ulint i = 0; i < srv_sys->n_sys_threads; ++i) {
			srv_slot_t*	slot = &srv_sys->sys_threads[i];

			slot->event = os_event_create();

			ut_a(slot->event);
		}

		srv_error_event = os_event_create();

		srv_buf_dump_event = os_event_create();

		UT_LIST_INIT(srv_sys->tasks);
	}

	srv_monitor_event = os_event_create();

	srv_buf_resize_event = os_event_create();

	/* page_zip_stat_per_index_mutex is acquired from:
	1. page_zip_compress() (after SYNC_FSP)
	2. page_zip_decompress()
	3. i_s_cmp_per_index_fill_low() (where SYNC_DICT is acquired)
	4. innodb_cmp_per_index_update(), no other latches
	since we do not acquire any other latches while holding this mutex,
	it can have very low level. We pick SYNC_ANY_LATCH for it. */

	mutex_create(
		page_zip_stat_per_index_mutex_key,
		&page_zip_stat_per_index_mutex, SYNC_ANY_LATCH);

	/* Create dummy indexes for infimum and supremum records */

	dict_ind_init();

	srv_conc_init();

	/* Initialize some INFORMATION SCHEMA internal structures */
	trx_i_s_cache_init(trx_i_s_cache);

	ut_crc32_init();

	dict_mem_init();
}

/*********************************************************************//**
Frees the data structures created in srv_init(). */
UNIV_INTERN
void
srv_free(void)
/*==========*/
{
	srv_conc_free();

	/* The mutexes srv_sys->mutex and srv_sys->tasks_mutex should have
	been freed by sync_close() already. */
	mem_free(srv_sys);
	srv_sys = NULL;

	trx_i_s_cache_free(trx_i_s_cache);

	if (!srv_read_only_mode) {
		os_event_free(srv_buf_dump_event);
		srv_buf_dump_event = NULL;
	}

	os_event_free(srv_buf_resize_event);
	srv_buf_resize_event = NULL;
}

/*********************************************************************//**
Initializes the synchronization primitives, memory system, and the thread
local storage. */
UNIV_INTERN
void
srv_general_init(void)
/*==================*/
{
	ut_mem_init();
	/* Reset the system variables in the recovery module. */
	recv_sys_var_init();
	os_sync_init();
	sync_init();
	mem_init(srv_mem_pool_size);
	que_init();
	row_mysql_init();
}

/*********************************************************************//**
Normalizes init parameter values to use units we use inside InnoDB. */
static
void
srv_normalize_init_values(void)
/*===========================*/
{
	ulint	n;
	ulint	i;

	n = srv_n_data_files;

	for (i = 0; i < n; i++) {
		srv_data_file_sizes[i] = srv_data_file_sizes[i]
			* ((1024 * 1024) / UNIV_PAGE_SIZE);
	}

	srv_last_file_size_max = srv_last_file_size_max
		* ((1024 * 1024) / UNIV_PAGE_SIZE);

	srv_log_file_size = srv_log_file_size / UNIV_PAGE_SIZE;

	srv_log_buffer_size = srv_log_buffer_size / UNIV_PAGE_SIZE;

	srv_lock_table_size = 5 * (srv_buf_pool_size / UNIV_PAGE_SIZE);
}

/*********************************************************************//**
Boots the InnoDB server. */
UNIV_INTERN
void
srv_boot(void)
/*==========*/
{
	/* Transform the init parameter values given by MySQL to
	use units we use inside InnoDB: */

	srv_normalize_init_values();

	/* Initialize synchronization primitives, memory management, and thread
	local storage */

	srv_general_init();

	/* Initialize this module */

	srv_init();
	srv_mon_create();
}

/******************************************************************//**
Refreshes the values used to calculate per-second averages. */
static
void
srv_refresh_innodb_monitor_stats(void)
/*==================================*/
{
	mutex_enter(&srv_innodb_monitor_mutex);

	srv_last_monitor_time = time(NULL);

	os_aio_refresh_stats();

	btr_cur_n_sea_old = btr_cur_n_sea;
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;

	log_refresh_stats();

	buf_refresh_io_stats_all();

	srv_n_rows_inserted_old = srv_stats.n_rows_inserted;
	srv_n_rows_updated_old = srv_stats.n_rows_updated;
	srv_n_rows_deleted_old = srv_stats.n_rows_deleted;
	srv_n_rows_read_old = srv_stats.n_rows_read;

	srv_n_system_rows_inserted_old = srv_stats.n_system_rows_inserted;
	srv_n_system_rows_updated_old = srv_stats.n_system_rows_updated;
	srv_n_system_rows_deleted_old = srv_stats.n_system_rows_deleted;
	srv_n_system_rows_read_old = srv_stats.n_system_rows_read;

	mutex_exit(&srv_innodb_monitor_mutex);
}

/******************************************************************//**
Output for SHOW INNODB TRANSACTION STATUS */
UNIV_INTERN
void
srv_printf_innodb_transaction(
/*======================*/
	FILE*	file)		/* in: output stream */
{
	mutex_enter(&srv_innodb_monitor_mutex);

	fputs("\n=================================================\n", file);
	ut_print_timestamp(file);
	fprintf(file,
		" INNODB TRANSACTION MONITOR OUTPUT\n"
		"=================================================\n");

	if (lock_print_info_summary(file, TRUE)) {
		lock_print_info_all_transactions(file);
	}

	fputs("----------------------------------------\n"
		"END OF INNODB TRANSACTION MONITOR OUTPUT\n"
		"========================================\n", file);
	mutex_exit(&srv_innodb_monitor_mutex);
	fflush(file);
}

/******************************************************************//**
Outputs to a file the output of the InnoDB Monitor.
@return FALSE if not all information printed
due to failure to obtain necessary mutex */
UNIV_INTERN
ibool
srv_printf_innodb_monitor(
/*======================*/
	FILE*	file,		/*!< in: output stream */
	ibool	nowait,		/*!< in: whether to wait for the
				lock_sys_t:: mutex */
	ibool	include_trxs,	/*!< in: include per-transaction output */
	srv_monitor_stats_types	status_type)	/*!< in: types of status info
						to include */
{
	double	time_elapsed;
	time_t	current_time;
	ulint	n_reserved;
	ibool	ret = TRUE;

	mutex_enter(&srv_innodb_monitor_mutex);

	current_time = time(NULL);

	/* We add 0.001 seconds to time_elapsed to prevent division
	by zero if two users happen to call SHOW ENGINE INNODB STATUS at the
	same time */

	time_elapsed = difftime(current_time, srv_last_monitor_time)
		+ 0.001;

	srv_last_monitor_time = time(NULL);

	fputs("\n=====================================\n", file);

	ut_print_timestamp(file);
	fprintf(file,
		" INNODB MONITOR OUTPUT\n"
		"=====================================\n"
		"Per second averages calculated from the last %lu seconds\n",
		(ulong) time_elapsed);

	if (status_type.background_thread) {
		fputs("-----------------\n"
		      "BACKGROUND THREAD\n"
		      "-----------------\n", file);
		srv_print_master_thread_info(file);
	}

	if (status_type.semaphores) {
		fputs("----------\n"
		      "SEMAPHORES\n"
		      "----------\n", file);
		sync_print(file);
	}

	if (status_type.fk) {
		/* Conceptually, srv_innodb_monitor_mutex has a very high
		latching order level in sync0sync.h, while
		dict_foreign_err_mutex has a very low level 135. Therefore we
		can reserve the latter mutex here without a danger of a deadlock
		of threads. */

		mutex_enter(&dict_foreign_err_mutex);

		if (!srv_read_only_mode && ftell(dict_foreign_err_file) != 0L) {
			fputs("------------------------\n"
			      "LATEST FOREIGN KEY ERROR\n"
			      "------------------------\n", file);
			ut_copy_file(file, dict_foreign_err_file);
		}

		mutex_exit(&dict_foreign_err_mutex);
	}

	if (status_type.trx) {
		/* Only if lock_print_info_summary proceeds correctly,
		before we call the lock_print_info_all_transactions
		to print all the lock information. IMPORTANT NOTE: This
		function acquires the lock mutex on success. */
		ret = lock_print_info_summary(file, nowait);

		if (include_trxs && ret) {
			/* NOTE: If we get here then we have the lock mutex.
			This function will release the lock mutex that we
			acquired when we called the lock_print_info_summary()
			function earlier. */

			lock_print_info_all_transactions(file);
		} else if (ret) {
			lock_mutex_exit();
		}
	}

	if (status_type.file) {
		fputs("--------\n"
		      "FILE I/O\n"
		      "--------\n", file);
		os_aio_print(file);
	}

	if (status_type.tablespace) {
		fputs("--------------\n"
		      "TABLESPACE I/O\n"
		      "--------------\n", file);
		fil_print(file);
	}

	if (status_type.insbuffer) {
		fputs("-------------------------------------\n"
		      "INSERT BUFFER AND ADAPTIVE HASH INDEX\n"
		      "-------------------------------------\n", file);
		ibuf_print(file);

		ha_print_info(file, btr_search_sys->hash_index);

		fprintf(file,
			"%.2f hash searches/s, %.2f non-hash searches/s\n",
			(btr_cur_n_sea - btr_cur_n_sea_old)
			/ time_elapsed,
			(btr_cur_n_non_sea - btr_cur_n_non_sea_old)
			/ time_elapsed);
	}
	/* Resetting counters regardless of whether we were printing them.
	Otherwise the stats will go crazy due to srv_last_monitor_time reset */
	btr_cur_n_sea_old = btr_cur_n_sea;
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;

	if (status_type.log) {
		fputs("---\n"
		      "LOG\n"
		      "---\n", file);
		log_print(file);
	}

	if (status_type.memory) {
		fputs("----------------------\n"
		      "BUFFER POOL AND MEMORY\n"
		      "----------------------\n", file);
		fprintf(file,
			"Total memory allocated " ULINTPF
			"; in additional pool allocated " ULINTPF "\n",
			ut_total_allocated_memory,
			mem_pool_get_reserved(mem_comm_pool));
		fprintf(file, "Dictionary memory allocated " ULINTPF "\n",
			dict_sys->size);

		buf_print_io(file);
	}

	if (status_type.row_operations) {
		fputs("--------------\n"
		      "ROW OPERATIONS\n"
		      "--------------\n", file);
		fprintf(file, "%ld queries inside InnoDB, %lu queries"
				"in queue\n",
				(long) srv_conc_get_active_threads(),
				srv_conc_get_waiting_threads());

		/* This is a dirty read, without holding trx_sys->mutex. */
		fprintf(file, "%lu read views open inside InnoDB\n",
			UT_LIST_GET_LEN(trx_sys->view_list));

		n_reserved = fil_space_get_n_reserved_extents(0);
		if (n_reserved > 0) {
			fprintf(file,
				"%lu tablespace extents now reserved for"
				" B-tree split operations\n",
				(ulong) n_reserved);
		}

#ifdef UNIV_LINUX
		fprintf(file, "Main thread process no. %lu, id %lu,"
			" state: %s\n",
			(ulong) srv_main_thread_process_no,
			(ulong) srv_main_thread_id,
			srv_main_thread_op_info);
#else
		fprintf(file, "Main thread id %lu, state: %s\n",
			(ulong) srv_main_thread_id,
			srv_main_thread_op_info);
#endif
		fprintf(file,
			"Number of rows inserted " ULINTPF
			", updated " ULINTPF ", deleted " ULINTPF
			", read " ULINTPF "\n",
			(ulint) srv_stats.n_rows_inserted,
			(ulint) srv_stats.n_rows_updated,
			(ulint) srv_stats.n_rows_deleted,
			(ulint) srv_stats.n_rows_read);
		fprintf(file,
			"%.2f inserts/s, %.2f updates/s,"
			" %.2f deletes/s, %.2f reads/s\n",
			((ulint) srv_stats.n_rows_inserted -
			srv_n_rows_inserted_old)
			/ time_elapsed,
			((ulint) srv_stats.n_rows_updated -
			srv_n_rows_updated_old)
			/ time_elapsed,
			((ulint) srv_stats.n_rows_deleted -
			srv_n_rows_deleted_old)
			/ time_elapsed,
			((ulint) srv_stats.n_rows_read - srv_n_rows_read_old)
			/ time_elapsed);
		fprintf(file,
			"Number of system rows inserted " ULINTPF
			", updated " ULINTPF ", deleted " ULINTPF
			", read " ULINTPF "\n",
			(ulint) srv_stats.n_system_rows_inserted,
			(ulint) srv_stats.n_system_rows_updated,
			(ulint) srv_stats.n_system_rows_deleted,
			(ulint) srv_stats.n_system_rows_read);
		fprintf(file,
			"%.2f inserts/s, %.2f updates/s,"
			" %.2f deletes/s, %.2f reads/s\n",
			((ulint) srv_stats.n_system_rows_inserted
			 - srv_n_system_rows_inserted_old) / time_elapsed,
			((ulint) srv_stats.n_system_rows_updated
			 - srv_n_system_rows_updated_old) / time_elapsed,
			((ulint) srv_stats.n_system_rows_deleted
			 - srv_n_system_rows_deleted_old) / time_elapsed,
			((ulint) srv_stats.n_system_rows_read
			 - srv_n_system_rows_read_old) / time_elapsed);
	}
	/* Resetting counters regardless of whether we were printing them.
	Otherwise the stats will go crazy due to srv_last_monitor_time reset */
	srv_n_rows_inserted_old = srv_stats.n_rows_inserted;
	srv_n_rows_updated_old = srv_stats.n_rows_updated;
	srv_n_rows_deleted_old = srv_stats.n_rows_deleted;
	srv_n_rows_read_old = srv_stats.n_rows_read;
	srv_n_system_rows_inserted_old = srv_stats.n_system_rows_inserted;
	srv_n_system_rows_updated_old = srv_stats.n_system_rows_updated;
	srv_n_system_rows_deleted_old = srv_stats.n_system_rows_deleted;
	srv_n_system_rows_read_old = srv_stats.n_system_rows_read;

	fputs("----------------------------\n"
	      "END OF INNODB MONITOR OUTPUT\n"
	      "============================\n", file);
	mutex_exit(&srv_innodb_monitor_mutex);
	fflush(file);

	return(ret);
}

/******************************************************************//**
Set compression related counters */
static void
export_zip(
/*=======*/
	ulint*			compressed,
	ulint*			compressed_ok,
	ulonglong*		compressed_time,
	ulonglong*		compressed_ok_time,
	ulint*			compressed_primary,
	ulint*			compressed_primary_ok,
	ulonglong*		compressed_primary_time,
	ulonglong*		compressed_primary_ok_time,
	ulint*			compressed_secondary,
	ulint*			compressed_secondary_ok,
	ulonglong*		compressed_secondary_time,
	ulonglong*		compressed_secondary_ok_time,
	ulint*			decompressed,
	ulonglong*		decompressed_time,
	ulint*			decompressed_primary,
	ulonglong*		decompressed_primary_time,
	ulint*			decompressed_secondary,
	ulonglong*		decompressed_secondary_time,
	page_zip_stat_t*	zip_stat)
{
	*compressed		= zip_stat->compressed;
	*compressed_ok		= zip_stat->compressed_ok;
	*compressed_time	= zip_stat->compressed_time;
	*compressed_ok_time	= zip_stat->compressed_ok_time;
	*compressed_primary	= zip_stat->compressed_primary;
	*compressed_primary_ok = zip_stat->compressed_primary_ok;
	*compressed_primary_time = zip_stat->compressed_primary_time;
	*compressed_primary_ok_time	= zip_stat->compressed_primary_ok_time;
	*compressed_secondary	= zip_stat->compressed_secondary;
	*compressed_secondary_ok = zip_stat->compressed_secondary_ok;
	*compressed_secondary_time = zip_stat->compressed_secondary_time;
	*compressed_secondary_ok_time	= zip_stat->compressed_secondary_ok_time;
	*decompressed		= zip_stat->decompressed;
	*decompressed_time	= zip_stat->decompressed_time;
	*decompressed_primary	= zip_stat->decompressed_primary;
	*decompressed_primary_time = zip_stat->decompressed_primary_time;
	*decompressed_secondary	= zip_stat->decompressed_secondary;
	*decompressed_secondary_time = zip_stat->decompressed_secondary_time;
}

#ifdef UNIV_DEBUG
extern ullint row_ins_optimistic_insert_calls_in_pessimistic_descent;
#endif /* UNIV_DEBUG */

/******************************************************************//**
Function to pass InnoDB status variables to MySQL */
UNIV_INTERN
void
srv_export_innodb_status(void)
/*==========================*/
{
	buf_pool_stat_t		stat;
	buf_pools_list_size_t	buf_pools_list_size;
	ulint			LRU_len;
	ulint			old_LRU_len;
	ulint			free_len;
	ulint			flush_list_len;
	ulint			unzip_LRU_len;
	uint i;
	ib_uint64_t lsn_oldest = buf_pool_get_oldest_modification();
	ib_uint64_t lsn_current = log_sys->lsn;
	ib_uint64_t lsn_gap = lsn_current - lsn_oldest;
	ib_uint64_t lsn_checkpoint = log_sys->last_checkpoint_lsn;

	buf_get_total_stat(&stat);
	buf_get_total_list_len(
		&LRU_len, &old_LRU_len, &free_len, &flush_list_len,
		&unzip_LRU_len);
	buf_get_total_list_size_in_bytes(&buf_pools_list_size);

	mutex_enter(&srv_innodb_monitor_mutex);

	export_vars.innodb_data_pending_reads =
		os_n_pending_reads;

	export_vars.innodb_data_pending_writes =
		os_n_pending_writes;

	export_vars.innodb_data_pending_fsyncs =
		fil_n_pending_log_flushes
		+ fil_n_pending_tablespace_flushes;

	export_vars.innodb_data_fsyncs = os_n_fsyncs;

	export_vars.innodb_data_fsync_time = os_file_flush_time;

	export_vars.innodb_data_fsync_slow = os_fsync_too_slow;

	export_vars.innodb_data_fsync_max_time = os_fsync_max_time;

	export_vars.innodb_data_read = srv_stats.data_read;

	export_vars.innodb_data_reads = os_n_file_reads;

	export_vars.innodb_data_writes = os_n_file_writes;

	export_vars.innodb_data_written = srv_stats.data_written;

	export_vars.innodb_data_async_read_bytes = os_async_read_perf.bytes;

	export_vars.innodb_data_async_read_requests =
		os_async_read_perf.requests;

	export_vars.innodb_data_async_read_svc_time =
		os_async_read_perf.svc_time ;

	export_vars.innodb_data_async_read_old_ios =
		os_async_read_old_ios;

	export_vars.innodb_data_async_read_slow_ios =
		os_async_read_perf.slow_ios;

	export_vars.innodb_data_sync_read_bytes = os_sync_read_perf.bytes;

	export_vars.innodb_data_sync_read_requests =
		os_sync_read_perf.requests;

	export_vars.innodb_data_sync_read_svc_time =
		os_sync_read_perf.svc_time;

	export_vars.innodb_data_sync_read_slow_ios =
		os_sync_read_perf.slow_ios;

	export_vars.innodb_data_async_write_bytes = os_async_write_perf.bytes;

	export_vars.innodb_data_async_write_requests =
		os_async_write_perf.requests;

	export_vars.innodb_data_async_write_svc_time =
		os_async_write_perf.svc_time;

	export_vars.innodb_data_async_write_old_ios =
		os_async_write_old_ios;

	export_vars.innodb_data_async_write_slow_ios =
		os_async_write_perf.slow_ios;

	export_vars.innodb_data_sync_write_bytes = os_sync_write_perf.bytes;

	export_vars.innodb_data_sync_write_requests =
		os_sync_write_perf.requests;

	export_vars.innodb_data_sync_write_svc_time =
		os_sync_write_perf.svc_time;

	export_vars.innodb_data_sync_write_slow_ios =
		os_sync_write_perf.slow_ios;

	export_vars.innodb_data_log_write_bytes = os_log_write_perf.bytes;

	export_vars.innodb_data_log_write_requests = os_log_write_perf.requests;

	export_vars.innodb_data_log_write_svc_time = os_log_write_perf.svc_time;

	export_vars.innodb_data_log_write_slow_ios = os_log_write_perf.slow_ios;

	export_vars.innodb_data_double_write_bytes = os_double_write_perf.bytes;

	export_vars.innodb_data_double_write_requests =
		os_double_write_perf.requests;

	export_vars.innodb_data_double_write_svc_time =
		os_double_write_perf.svc_time;

	export_vars.innodb_data_double_write_slow_ios=
		os_double_write_perf.slow_ios;

	export_vars.innodb_buffer_pool_flushed_lru = stat.n_flushed_lru;

	export_vars.innodb_buffer_pool_flushed_list = stat.n_flushed_list;

	export_vars.innodb_buffer_pool_flushed_page =
		stat.n_flushed_single_page;

	export_vars.innodb_buffer_pool_pct_dirty =
		(((double) flush_list_len) / (LRU_len + 1.0)) * 100.0;

	export_vars.innodb_buffer_pool_read_requests = stat.n_page_gets;

	export_vars.innodb_buffer_pool_write_requests =
		srv_stats.buf_pool_write_requests;

	export_vars.innodb_buffer_pool_wait_free =
		srv_stats.buf_pool_wait_free;

	export_vars.innodb_buffer_pool_pages_flushed =
		srv_stats.buf_pool_flushed;

	export_vars.innodb_buffer_pool_reads = srv_stats.buf_pool_reads;

	export_vars.innodb_buffer_pool_read_ahead_rnd =
		stat.n_ra_pages_read_rnd;

	export_vars.innodb_buffer_pool_read_ahead =
		stat.n_ra_pages_read;

	export_vars.innodb_buffer_pool_read_ahead_evicted =
		stat.n_ra_pages_evicted;

	export_vars.innodb_buffer_pool_pages_data = LRU_len;

	export_vars.innodb_buffer_pool_bytes_data =
		buf_pools_list_size.LRU_bytes
		+ buf_pools_list_size.unzip_LRU_bytes;

	export_vars.innodb_buffer_pool_pages_dirty = flush_list_len;

	export_vars.innodb_buffer_pool_bytes_dirty =
		buf_pools_list_size.flush_list_bytes;

	export_vars.innodb_buffer_pool_pages_unzip = unzip_LRU_len;

	export_vars.innodb_buffer_pool_pages_free = free_len;

#ifdef UNIV_DEBUG
	export_vars.innodb_buffer_pool_pages_latched =
		buf_get_latched_pages_number();
#endif /* UNIV_DEBUG */
	export_vars.innodb_buffer_pool_pages_total = buf_pool_get_n_pages();

	export_vars.innodb_buffer_pool_pages_lru_old = old_LRU_len;

	export_vars.innodb_buffer_pool_pages_misc =
		buf_pool_get_n_pages() - LRU_len - free_len;

	export_vars.innodb_buffer_pool_neighbors_flushed_list=
		srv_neighbors_flushed_list;
	export_vars.innodb_buffer_pool_neighbors_flushed_lru=
		srv_neighbors_flushed_lru;

	export_vars.innodb_hash_searches = btr_cur_n_sea;
	export_vars.innodb_hash_nonsearches = btr_cur_n_non_sea;

#ifdef HAVE_ATOMIC_BUILTINS
	export_vars.innodb_have_atomic_builtins = 1;
#else
	export_vars.innodb_have_atomic_builtins = 0;
#endif

	export_vars.innodb_ibuf_inserts =
		ibuf->n_merged_ops[IBUF_OP_INSERT];
	export_vars.innodb_ibuf_delete_marks =
		ibuf->n_merged_ops[IBUF_OP_DELETE_MARK];
	export_vars.innodb_ibuf_deletes =
		ibuf->n_merged_ops[IBUF_OP_DELETE];
	export_vars.innodb_ibuf_merges = ibuf->n_merges;
	export_vars.innodb_ibuf_size = ibuf->size;

	export_vars.innodb_page_size = UNIV_PAGE_SIZE;

	export_vars.innodb_lock_deadlocks = srv_lock_deadlocks;

	export_vars.innodb_lock_wait_timeouts = srv_lock_wait_timeouts;

	export_vars.innodb_log_waits = srv_stats.log_waits;

	export_vars.innodb_log_checkpoints= log_sys->n_checkpoints;

	export_vars.innodb_log_syncs= log_sys->n_syncs;

	export_vars.innodb_log_write_archive=
		log_sys->log_sync_callers[LOG_WRITE_FROM_LOG_ARCHIVE];
	export_vars.innodb_log_write_background_async=
		log_sys->log_sync_callers[LOG_WRITE_FROM_BACKGROUND_ASYNC];
	export_vars.innodb_log_write_background_sync=
		log_sys->log_sync_callers[LOG_WRITE_FROM_BACKGROUND_SYNC];
	export_vars.innodb_log_write_checkpoint_async=
		log_sys->log_sync_callers[LOG_WRITE_FROM_CHECKPOINT_ASYNC];
	export_vars.innodb_log_write_checkpoint_sync=
		log_sys->log_sync_callers[LOG_WRITE_FROM_CHECKPOINT_SYNC];
	export_vars.innodb_log_write_commit_async=
		log_sys->log_sync_callers[LOG_WRITE_FROM_COMMIT_ASYNC];
	export_vars.innodb_log_write_commit_sync=
		log_sys->log_sync_callers[LOG_WRITE_FROM_COMMIT_SYNC];
	export_vars.innodb_log_write_flush_dirty=
		log_sys->log_sync_callers[LOG_WRITE_FROM_DIRTY_BUFFER];
	export_vars.innodb_log_write_other=
		log_sys->log_sync_callers[LOG_WRITE_FROM_INTERNAL];

	export_vars.innodb_log_sync_archive=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_LOG_ARCHIVE];
	export_vars.innodb_log_sync_background_async=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_BACKGROUND_ASYNC];
	export_vars.innodb_log_sync_background_sync=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_BACKGROUND_SYNC];
	export_vars.innodb_log_sync_checkpoint_async=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_CHECKPOINT_ASYNC];
	export_vars.innodb_log_sync_checkpoint_sync=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_CHECKPOINT_SYNC];
	export_vars.innodb_log_sync_commit_async=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_COMMIT_ASYNC];
	export_vars.innodb_log_sync_commit_sync=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_COMMIT_SYNC];
	export_vars.innodb_log_sync_flush_dirty=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_DIRTY_BUFFER];
	export_vars.innodb_log_sync_other=
		log_sys->log_sync_syncers[LOG_WRITE_FROM_INTERNAL];
	export_vars.innodb_log_write_padding = log_sys->log_write_padding;
	export_vars.innodb_log_physical_write_bytes =
		log_sys->log_physical_write_bytes;
	export_vars.innodb_log_logical_write_bytes =
		log_sys->log_logical_write_bytes;

	export_vars.innodb_os_log_written = srv_stats.os_log_written;

	export_vars.innodb_os_log_fsyncs = fil_n_log_flushes;

	export_vars.innodb_os_log_pending_fsyncs = fil_n_pending_log_flushes;

	export_vars.innodb_os_log_pending_writes =
		srv_stats.os_log_pending_writes;

	export_vars.innodb_log_write_requests = srv_stats.log_write_requests;

	export_vars.innodb_log_writes = srv_stats.log_writes;

	export_vars.innodb_dblwr_pages_written =
		srv_stats.dblwr_pages_written;

	export_vars.innodb_dblwr_writes = srv_stats.dblwr_writes;

	export_vars.innodb_pages_created = stat.n_pages_created;

	export_vars.innodb_pages_read = stat.n_pages_read;
	export_vars.innodb_pages_read_index = stat.n_pages_read_index;
	export_vars.innodb_pages_read_undo_log = stat.n_pages_read_undo_log;
	export_vars.innodb_pages_read_inode = stat.n_pages_read_inode;
	export_vars.innodb_pages_read_ibuf_free_list =
		stat.n_pages_read_ibuf_free_list;
	export_vars.innodb_pages_read_allocated = stat.n_pages_read_allocated;
	export_vars.innodb_pages_read_ibuf_bitmap =
		stat.n_pages_read_ibuf_bitmap;
	export_vars.innodb_pages_read_sys = stat.n_pages_read_sys;
	export_vars.innodb_pages_read_trx_sys = stat.n_pages_read_trx_sys;
	export_vars.innodb_pages_read_fsp_hdr = stat.n_pages_read_fsp_hdr;
	export_vars.innodb_pages_read_xdes = stat.n_pages_read_xdes;
	export_vars.innodb_pages_read_blob = stat.n_pages_read_blob;

	export_vars.innodb_pages_written = stat.n_pages_written;
	export_vars.innodb_pages_written_index = stat.n_pages_written_index;
	export_vars.innodb_pages_written_undo_log =
		stat.n_pages_written_undo_log;
	export_vars.innodb_pages_written_inode = stat.n_pages_written_inode;
	export_vars.innodb_pages_written_ibuf_free_list =
		stat.n_pages_written_ibuf_free_list;
	export_vars.innodb_pages_written_allocated =
		stat.n_pages_written_allocated;
	export_vars.innodb_pages_written_ibuf_bitmap =
		stat.n_pages_written_ibuf_bitmap;
	export_vars.innodb_pages_written_sys = stat.n_pages_written_sys;
	export_vars.innodb_pages_written_trx_sys = stat.n_pages_written_trx_sys;
	export_vars.innodb_pages_written_fsp_hdr = stat.n_pages_written_fsp_hdr;
	export_vars.innodb_pages_written_xdes = stat.n_pages_written_xdes;
	export_vars.innodb_pages_written_blob = stat.n_pages_written_blob;

	export_vars.innodb_purge_pending = trx_sys->rseg_history_len;
	export_vars.innodb_purged_pages= srv_purged_pages;

	export_vars.innodb_row_lock_waits = srv_stats.n_lock_wait_count;

	export_vars.innodb_row_lock_current_waits =
		srv_stats.n_lock_wait_current_count;

	export_vars.innodb_row_lock_time = srv_stats.n_lock_wait_time / 1000;

	if (srv_stats.n_lock_wait_count > 0) {

		export_vars.innodb_row_lock_time_avg = (ulint)
			(srv_stats.n_lock_wait_time
			 / 1000 / srv_stats.n_lock_wait_count);

	} else {
		export_vars.innodb_row_lock_time_avg = 0;
	}

	export_vars.innodb_row_lock_time_max =
		lock_sys->n_lock_max_wait_time / 1000;

  export_vars.innodb_row_recreations = srv_stats.row_recreations;
  export_vars.innodb_row_recreation_steps = srv_stats.row_recreation_steps;

	export_vars.innodb_rows_read = srv_stats.n_rows_read;

	export_vars.innodb_rows_inserted = srv_stats.n_rows_inserted;

	export_vars.innodb_rows_updated = srv_stats.n_rows_updated;

	export_vars.innodb_rows_deleted = srv_stats.n_rows_deleted;

	export_vars.innodb_system_rows_read = srv_stats.n_system_rows_read;

	export_vars.innodb_system_rows_inserted =
		srv_stats.n_system_rows_inserted;

	export_vars.innodb_system_rows_updated =
		srv_stats.n_system_rows_updated;

	export_vars.innodb_system_rows_deleted =
		srv_stats.n_system_rows_deleted;

	export_vars.innodb_num_open_files = fil_n_file_opened;

	export_vars.innodb_truncated_status_writes =
		srv_truncated_status_writes;

	export_vars.innodb_available_undo_logs = srv_available_undo_logs;

	export_vars.innodb_buf_dblwr_page_no =
		buf_dblwr ? buf_dblwr->block1 : 0;

#ifdef UNIV_DEBUG
	rw_lock_s_lock(&purge_sys->latch);
	trx_id_t	done_trx_no	= purge_sys->done.trx_no;
	trx_id_t	up_limit_id	= purge_sys->view
		? purge_sys->view->up_limit_id
		: 0;
	rw_lock_s_unlock(&purge_sys->latch);

	mutex_enter(&trx_sys->mutex);
	trx_id_t	max_trx_id	= trx_sys->rw_max_trx_id;
	mutex_exit(&trx_sys->mutex);

	if (!done_trx_no || max_trx_id < done_trx_no - 1) {
		export_vars.innodb_purge_trx_id_age = 0;
	} else {
		export_vars.innodb_purge_trx_id_age =
			(ulint) (max_trx_id - done_trx_no + 1);
	}

	if (!up_limit_id
	    || max_trx_id < up_limit_id) {
		export_vars.innodb_purge_view_trx_id_age = 0;
	} else {
		export_vars.innodb_purge_view_trx_id_age =
			(ulint) (max_trx_id - up_limit_id);
	}
#endif /* UNIV_DEBUG */

	export_vars.innodb_mutex_os_waits = mutex_os_wait_count;
	export_vars.innodb_mutex_spin_rounds = mutex_spin_round_count;
	export_vars.innodb_mutex_spin_waits = mutex_spin_wait_count;

	export_vars.innodb_rwlock_s_os_waits = rw_lock_stats.rw_s_os_wait_count;
	export_vars.innodb_rwlock_s_spin_rounds = rw_lock_stats.rw_s_spin_round_count;
	export_vars.innodb_rwlock_s_spin_waits = rw_lock_stats.rw_s_spin_wait_count;

	export_vars.innodb_rwlock_x_os_waits = rw_lock_stats.rw_x_os_wait_count;
	export_vars.innodb_rwlock_x_spin_rounds = rw_lock_stats.rw_x_spin_round_count;
	export_vars.innodb_rwlock_x_spin_waits = rw_lock_stats.rw_x_spin_wait_count;

	export_vars.innodb_srv_checkpoint_time= srv_checkpoint_time;
	export_vars.innodb_srv_ibuf_contract_time= srv_ibuf_contract_time;
	export_vars.innodb_srv_log_flush_time= srv_log_flush_time;
	export_vars.innodb_srv_cache_limit_time= srv_cache_limit_time;
	export_vars.innodb_srv_free_log_time= srv_free_log_time;
	export_vars.innodb_srv_drop_table_time= srv_drop_table_time;
	export_vars.innodb_srv_purge_time= srv_purge_time;

	export_vars.innodb_trx_n_commit_all= srv_n_commit_all;
	export_vars.innodb_trx_n_commit_with_undo= srv_n_commit_with_undo;
	export_vars.innodb_trx_n_rollback_partial= srv_n_rollback_partial;
	export_vars.innodb_trx_n_rollback_total= srv_n_rollback_total;

	export_vars.innodb_sec_rec_cluster_reads =
		srv_sec_rec_cluster_reads.load();
	export_vars.innodb_sec_rec_cluster_reads_avoided =
		srv_sec_rec_cluster_reads_avoided.load();

	export_vars.innodb_preflush_async_limit = log_sys->max_modified_age_async;
	export_vars.innodb_preflush_sync_limit = log_sys->max_modified_age_sync;
	if (!lsn_oldest) {
		export_vars.innodb_preflush_async_margin =
			log_sys->max_modified_age_async;
		export_vars.innodb_preflush_sync_margin =
			log_sys->max_modified_age_sync;
	} else {
		export_vars.innodb_preflush_async_margin =
			(log_sys->max_modified_age_async >= lsn_gap) ?
			(log_sys->max_modified_age_async - lsn_gap) : 0;
		export_vars.innodb_preflush_sync_margin =
			(log_sys->max_modified_age_sync >= lsn_gap) ?
			(log_sys->max_modified_age_sync - lsn_gap) : 0;
	}
	export_vars.innodb_checkpoint_lsn = lsn_checkpoint;
	export_vars.innodb_checkpoint_diff = lsn_current - lsn_checkpoint;
	export_vars.innodb_lsn_current = lsn_current;
	export_vars.innodb_lsn_oldest = lsn_oldest;
	export_vars.innodb_lsn_diff = lsn_gap;

	export_vars.innodb_defragment_compression_failures =
		btr_defragment_compression_failures;
	export_vars.innodb_defragment_failures = btr_defragment_failures;
	export_vars.innodb_defragment_count = btr_defragment_count;
  export_vars.innodb_defragment_runtime_pct = btr_defragment_runtime_pct;
  export_vars.innodb_defragment_avg_runtime = btr_defragment_avg_runtime;
  export_vars.innodb_defragment_avg_idletime = btr_defragment_avg_idletime;

	export_vars.innodb_buffered_aio_submitted =
		srv_stats.n_aio_submitted;
	export_vars.innodb_outstanding_aio_requests =
		os_aio_n_outstanding;
#ifdef UNIV_DEBUG
	export_vars.innodb_max_outstanding_aio_requests =
		os_aio_max_outstanding;
#endif /* UNIV_DEBUG */
	export_vars.innodb_logical_read_ahead_misses =
		srv_stats.n_logical_read_ahead_misses;
	export_vars.innodb_logical_read_ahead_prefetched =
		srv_stats.n_logical_read_ahead_prefetched;
	export_vars.innodb_logical_read_ahead_in_buf_pool =
		srv_stats.n_logical_read_ahead_in_buf_pool;

	for (i = 0; i < PAGE_ZIP_SSIZE_MAX - 1; i++) {
		page_zip_stat_t*        zip_stat = &page_zip_stat[i];

		ulint page_size = UNIV_ZIP_SIZE_MIN << i;

		switch (page_size) {
		case 1024:
			export_zip(&export_vars.zip1024_compressed,
				&export_vars.zip1024_compressed_ok,
				&export_vars.zip1024_compressed_time,
				&export_vars.zip1024_compressed_ok_time,
				&export_vars.zip1024_compressed_primary,
				&export_vars.zip1024_compressed_primary_ok,
				&export_vars.zip1024_compressed_primary_time,
				&export_vars.zip1024_compressed_primary_ok_time,
				&export_vars.zip1024_compressed_secondary,
				&export_vars.zip1024_compressed_secondary_ok,
				&export_vars.zip1024_compressed_secondary_time,
				&export_vars.zip1024_compressed_secondary_ok_time,
				&export_vars.zip1024_decompressed,
				&export_vars.zip1024_decompressed_time,
				&export_vars.zip1024_decompressed_primary,
				&export_vars.zip1024_decompressed_primary_time,
				&export_vars.zip1024_decompressed_secondary,
				&export_vars.zip1024_decompressed_secondary_time,
				zip_stat);
			break;
		case 2048:
			export_zip(&export_vars.zip2048_compressed,
				&export_vars.zip2048_compressed_ok,
				&export_vars.zip2048_compressed_time,
				&export_vars.zip2048_compressed_ok_time,
				&export_vars.zip2048_compressed_primary,
				&export_vars.zip2048_compressed_primary_ok,
				&export_vars.zip2048_compressed_primary_time,
				&export_vars.zip2048_compressed_primary_ok_time,
				&export_vars.zip2048_compressed_secondary,
				&export_vars.zip2048_compressed_secondary_ok,
				&export_vars.zip2048_compressed_secondary_time,
				&export_vars.zip2048_compressed_secondary_ok_time,
				&export_vars.zip2048_decompressed,
				&export_vars.zip2048_decompressed_time,
				&export_vars.zip2048_decompressed_primary,
				&export_vars.zip2048_decompressed_primary_time,
				&export_vars.zip2048_decompressed_secondary,
				&export_vars.zip2048_decompressed_secondary_time,
				zip_stat);
			break;
		case 4096:
			export_zip(&export_vars.zip4096_compressed,
				&export_vars.zip4096_compressed_ok,
				&export_vars.zip4096_compressed_time,
				&export_vars.zip4096_compressed_ok_time,
				&export_vars.zip4096_compressed_primary,
				&export_vars.zip4096_compressed_primary_ok,
				&export_vars.zip4096_compressed_primary_time,
				&export_vars.zip4096_compressed_primary_ok_time,
				&export_vars.zip4096_compressed_secondary,
				&export_vars.zip4096_compressed_secondary_ok,
				&export_vars.zip4096_compressed_secondary_time,
				&export_vars.zip4096_compressed_secondary_ok_time,
				&export_vars.zip4096_decompressed,
				&export_vars.zip4096_decompressed_time,
				&export_vars.zip4096_decompressed_primary,
				&export_vars.zip4096_decompressed_primary_time,
				&export_vars.zip4096_decompressed_secondary,
				&export_vars.zip4096_decompressed_secondary_time,
				zip_stat);
			break;
		case 8192:
			export_zip(&export_vars.zip8192_compressed,
				&export_vars.zip8192_compressed_ok,
				&export_vars.zip8192_compressed_time,
				&export_vars.zip8192_compressed_ok_time,
				&export_vars.zip8192_compressed_primary,
				&export_vars.zip8192_compressed_primary_ok,
				&export_vars.zip8192_compressed_primary_time,
				&export_vars.zip8192_compressed_primary_ok_time,
				&export_vars.zip8192_compressed_secondary,
				&export_vars.zip8192_compressed_secondary_ok,
				&export_vars.zip8192_compressed_secondary_time,
				&export_vars.zip8192_compressed_secondary_ok_time,
				&export_vars.zip8192_decompressed,
				&export_vars.zip8192_decompressed_time,
				&export_vars.zip8192_decompressed_primary,
				&export_vars.zip8192_decompressed_primary_time,
				&export_vars.zip8192_decompressed_secondary,
				&export_vars.zip8192_decompressed_secondary_time,
				zip_stat);
			break;
		case 16384:
			export_zip(&export_vars.zip16384_compressed,
				&export_vars.zip16384_compressed_ok,
				&export_vars.zip16384_compressed_time,
				&export_vars.zip16384_compressed_ok_time,
				&export_vars.zip16384_compressed_primary,
				&export_vars.zip16384_compressed_primary_ok,
				&export_vars.zip16384_compressed_primary_time,
				&export_vars.zip16384_compressed_primary_ok_time,
				&export_vars.zip16384_compressed_secondary,
				&export_vars.zip16384_compressed_secondary_ok,
				&export_vars.zip16384_compressed_secondary_time,
				&export_vars.zip16384_compressed_secondary_ok_time,
				&export_vars.zip16384_decompressed,
				&export_vars.zip16384_decompressed_time,
				&export_vars.zip16384_decompressed_primary,
				&export_vars.zip16384_decompressed_primary_time,
				&export_vars.zip16384_decompressed_secondary,
				&export_vars.zip16384_decompressed_secondary_time,
				zip_stat);
			break;
		default:
			break;
		}
	}

#ifdef UNIV_DEBUG
	export_vars.num_optimistic_insert_calls_in_pessimistic_descent =
		row_ins_optimistic_insert_calls_in_pessimistic_descent;
#endif /* UNIV_DEBUG */

	export_vars.innodb_drop_purge_skip_row = srv_drop_purge_skip_row;
	export_vars.innodb_drop_ibuf_skip_row = srv_drop_ibuf_skip_row;

	size_t i_bins;
	for (i_bins = 0; i_bins < NUMBER_OF_HISTOGRAM_BINS; ++i_bins) {
		export_vars.histogram_async_read_values[i_bins] =
				latency_histogram_get_count(
					&histogram_async_read, i_bins);
		export_vars.histogram_async_write_values[i_bins] =
				latency_histogram_get_count(
					&histogram_async_write, i_bins);

		export_vars.histogram_sync_read_values[i_bins] =
				latency_histogram_get_count(
					&histogram_sync_read, i_bins);
		export_vars.histogram_sync_write_values[i_bins] =
				latency_histogram_get_count(
					&histogram_sync_write, i_bins);

		export_vars.histogram_log_write_values[i_bins] =
				latency_histogram_get_count(
					&histogram_log_write, i_bins);
		export_vars.histogram_double_write_values[i_bins] =
				latency_histogram_get_count(
					&histogram_double_write, i_bins);

		export_vars.histogram_file_flush_time_values[i_bins] =
				latency_histogram_get_count(
					&histogram_file_flush_time, i_bins);
		export_vars.histogram_fsync_values[i_bins] =
				latency_histogram_get_count(
					&histogram_fsync, i_bins);
	}

	mutex_exit(&srv_innodb_monitor_mutex);
}

/*********************************************************************//**
A thread which prints the info output by various InnoDB monitors.
@return	a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_monitor_thread)(
/*===============================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ib_int64_t	sig_count;
	double		time_elapsed;
	time_t		current_time;
	time_t		last_table_monitor_time;
	time_t		last_tablespace_monitor_time;
	time_t		last_monitor_time;
	ulint		mutex_skipped;
	ibool		last_srv_print_monitor;
	srv_monitor_stats_types	types;

	ut_ad(!srv_read_only_mode);

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Lock timeout thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_monitor_thread_key);
#endif /* UNIV_PFS_THREAD */
	srv_monitor_active = TRUE;

	UT_NOT_USED(arg);
	srv_last_monitor_time = ut_time();
	last_table_monitor_time = ut_time();
	last_tablespace_monitor_time = ut_time();
	last_monitor_time = ut_time();
	mutex_skipped = 0;
	last_srv_print_monitor = srv_print_innodb_monitor;
loop:
	/* Wake up every 5 seconds to see if we need to print
	monitor information or if signalled at shutdown. */

	sig_count = os_event_reset(srv_monitor_event);

	os_event_wait_time_low(srv_monitor_event, 5000000, sig_count);

	current_time = ut_time();

	time_elapsed = difftime(current_time, last_monitor_time);

	if (time_elapsed > 15) {
		last_monitor_time = ut_time();

		if (srv_print_innodb_monitor) {
			/* Reset mutex_skipped counter everytime
			srv_print_innodb_monitor changes. This is to
			ensure we will not be blocked by lock_sys->mutex
			for short duration information printing,
			such as requested by sync_array_print_long_waits() */
			if (!last_srv_print_monitor) {
				mutex_skipped = 0;
				last_srv_print_monitor = TRUE;
			}

			if (!srv_printf_innodb_monitor(
				stderr, MUTEX_NOWAIT(mutex_skipped),
				TRUE, types.set_all())) {
				mutex_skipped++;
			} else {
				/* Reset the counter */
				mutex_skipped = 0;
			}
		} else {
			last_srv_print_monitor = FALSE;
		}


		/* We don't create the temp files or associated
		mutexes in read-only-mode */

		if (!srv_read_only_mode && srv_innodb_status) {
			mutex_enter(&srv_monitor_file_mutex);
			rewind(srv_monitor_file);
			if (!srv_printf_innodb_monitor(
				srv_monitor_file, MUTEX_NOWAIT(mutex_skipped),
				TRUE, types.set_all())) {
				mutex_skipped++;
			} else {
				mutex_skipped = 0;
			}

			os_file_set_eof(srv_monitor_file);
			mutex_exit(&srv_monitor_file_mutex);
		}

		if (srv_print_innodb_tablespace_monitor
		    && difftime(current_time,
				last_tablespace_monitor_time) > 60) {
			last_tablespace_monitor_time = ut_time();

			fputs("========================"
			      "========================\n",
			      stderr);

			ut_print_timestamp(stderr);

			fputs(" INNODB TABLESPACE MONITOR OUTPUT\n"
			      "========================"
			      "========================\n",
			      stderr);

			fsp_print(0);
			fputs("Validating tablespace\n", stderr);
			fsp_validate(0);
			fputs("Validation ok\n"
			      "---------------------------------------\n"
			      "END OF INNODB TABLESPACE MONITOR OUTPUT\n"
			      "=======================================\n",
			      stderr);
		}

		if (srv_print_innodb_table_monitor
		    && difftime(current_time, last_table_monitor_time) > 60) {

			last_table_monitor_time = ut_time();

			fprintf(stderr, "Warning: %s\n",
				DEPRECATED_MSG_INNODB_TABLE_MONITOR);

			fputs("===========================================\n",
			      stderr);

			ut_print_timestamp(stderr);

			fputs(" INNODB TABLE MONITOR OUTPUT\n"
			      "===========================================\n",
			      stderr);
			dict_print();

			fputs("-----------------------------------\n"
			      "END OF INNODB TABLE MONITOR OUTPUT\n"
			      "==================================\n",
			      stderr);

			fprintf(stderr, "Warning: %s\n",
				DEPRECATED_MSG_INNODB_TABLE_MONITOR);
		}
	}

	if (srv_shutdown_state >= SRV_SHUTDOWN_CLEANUP) {
		goto exit_func;
	}

	if (srv_print_innodb_monitor
	    || srv_print_innodb_lock_monitor
	    || srv_print_innodb_tablespace_monitor
	    || srv_print_innodb_table_monitor) {
		goto loop;
	}

	goto loop;

exit_func:
	srv_monitor_active = FALSE;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************//**
A thread which prints warnings about semaphore waits which have lasted
too long. These can be used to track bugs which cause hangs.
Note: In order to make sync_arr_wake_threads_if_sema_free work as expected,
we should avoid waiting any mutexes in this function!
@return	a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_error_monitor_thread)(
/*=====================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	/* number of successive fatal timeouts observed */
	ulint		fatal_cnt	= 0;
	lsn_t		old_lsn;
	lsn_t		new_lsn;
	ib_int64_t	sig_count;
	/* longest waiting thread for a semaphore */
	os_thread_id_t	waiter		= os_thread_get_curr_id();
	os_thread_id_t	old_waiter	= waiter;
	/* the semaphore that is being waited for */
	const void*	sema		= NULL;
	const void*	old_sema	= NULL;

	ut_ad(!srv_read_only_mode);

	old_lsn = srv_start_lsn;

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Error monitor thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_error_monitor_thread_key);
#endif /* UNIV_PFS_THREAD */
	srv_error_monitor_active = TRUE;

loop:
	/* Try to track a strange bug reported by Harald Fuchs and others,
	where the lsn seems to decrease at times */

	if (log_peek_lsn(&new_lsn)) {
		if (new_lsn < old_lsn) {
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Error: old log sequence number " LSN_PF
				" was greater\n"
				"InnoDB: than the new log sequence number " LSN_PF "!\n"
				"InnoDB: Please submit a bug report"
				" to http://bugs.mysql.com\n",
				old_lsn, new_lsn);
			ut_ad(0);
		}

		old_lsn = new_lsn;
	}

	if (difftime(time(NULL), srv_last_monitor_time) > 60) {
		/* We referesh InnoDB Monitor values so that averages are
		printed from at most 60 last seconds */

		srv_refresh_innodb_monitor_stats();
	}

	/* Update the statistics collected for deciding LRU
	eviction policy. */
	buf_LRU_stat_update();

	/* In case mutex_exit is not a memory barrier, it is
	theoretically possible some threads are left waiting though
	the semaphore is already released. Wake up those threads: */

	sync_arr_wake_threads_if_sema_free();

	if (sync_array_print_long_waits(&waiter, &sema)
	    && sema == old_sema && os_thread_eq(waiter, old_waiter)) {
		fatal_cnt++;
		if (fatal_cnt > 10) {

			fprintf(stderr,
				"InnoDB: Error: semaphore wait has lasted"
				" > %lu seconds\n"
				"InnoDB: We intentionally crash the server,"
				" because it appears to be hung.\n",
				opt_srv_fatal_semaphore_timeout);

			ut_error;
		}
	} else {
		fatal_cnt = 0;
		old_waiter = waiter;
		old_sema = sema;
	}

	/* Flush stderr so that a database user gets the output
	to possible MySQL error file */

	fflush(stderr);

	sig_count = os_event_reset(srv_error_event);

	os_event_wait_time_low(srv_error_event, 1000000, sig_count);

	if (srv_shutdown_state < SRV_SHUTDOWN_CLEANUP) {

		goto loop;
	}

	srv_error_monitor_active = FALSE;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/******************************************************************//**
Increment the server activity count. */
UNIV_INTERN
void
srv_inc_activity_count(void)
/*========================*/
{
	srv_sys->activity_count.inc();
}

/**********************************************************************//**
Check whether any background thread is active. If so return the thread
type.
@return SRV_NONE if all are suspended or have exited, thread
type if any are still active. */
UNIV_INTERN
srv_thread_type
srv_get_active_thread_type(void)
/*============================*/
{
	srv_thread_type ret = SRV_NONE;

	if (srv_read_only_mode) {
		return(SRV_NONE);
	}

	srv_sys_mutex_enter();

	for (ulint i = SRV_WORKER; i <= SRV_MASTER; ++i) {
		if (srv_sys->n_threads_active[i] != 0) {
			ret = static_cast<srv_thread_type>(i);
			break;
		}
	}

	srv_sys_mutex_exit();

	/* Check only on shutdown. */
	if (ret == SRV_NONE
	    && srv_shutdown_state != SRV_SHUTDOWN_NONE
	    && trx_purge_state() != PURGE_STATE_DISABLED
#ifdef XTRABACKUP
	    && trx_purge_state() != PURGE_STATE_INIT
#endif /* XTRABACKUP */
	    && trx_purge_state() != PURGE_STATE_EXIT) {

		ret = SRV_PURGE;
	}

	return(ret);
}

/**********************************************************************//**
Check whether any background thread are active. If so print which thread
is active. Send the threads wakeup signal.
@return name of thread that is active or NULL */
UNIV_INTERN
const char*
srv_any_background_threads_are_active(void)
/*=======================================*/
{
	const char*	thread_active = NULL;

	if (srv_read_only_mode) {
		if (srv_buf_resize_thread_active) {
			thread_active = "buf_resize_thread";
		}
		os_event_set(srv_buf_resize_event);
		return(thread_active);
	} else if (srv_error_monitor_active) {
		thread_active = "srv_error_monitor_thread";
	} else if (lock_sys->timeout_thread_active) {
		thread_active = "srv_lock_timeout thread";
	} else if (srv_monitor_active) {
		thread_active = "srv_monitor_thread";
	} else if (srv_buf_dump_thread_active) {
		thread_active = "buf_dump_thread";
	} else if (srv_dict_stats_thread_active) {
		thread_active = "dict_stats_thread";
	}

	os_event_set(srv_error_event);
	os_event_set(srv_monitor_event);
	os_event_set(srv_buf_dump_event);
	os_event_set(srv_buf_resize_event);
	os_event_set(lock_sys->timeout_event);
	os_event_set(dict_stats_event);

	return(thread_active);
}

/*******************************************************************//**
Tells the InnoDB server that there has been activity in the database
and wakes up the master thread if it is suspended (not sleeping). Used
in the MySQL interface. Note that there is a small chance that the master
thread stays suspended (we do not protect our operation with the
srv_sys_t->mutex, for performance reasons). */
UNIV_INTERN
void
srv_active_wake_master_thread(void)
/*===============================*/
{
	if (srv_read_only_mode) {
		return;
	}

	ut_ad(!srv_sys_mutex_own());

	srv_inc_activity_count();

	if (srv_sys->n_threads_active[SRV_MASTER] == 0) {
		srv_slot_t*	slot;

		srv_sys_mutex_enter();

		slot = &srv_sys->sys_threads[SRV_MASTER_SLOT];

		/* Only if the master thread has been started. */

		if (slot->in_use) {
			ut_a(srv_slot_get_type(slot) == SRV_MASTER);

			if (slot->suspended) {

				slot->suspended = FALSE;

				++srv_sys->n_threads_active[SRV_MASTER];

				os_event_set(slot->event);
			}
		}

		srv_sys_mutex_exit();
	}
}

/*******************************************************************//**
Tells the purge thread that there has been activity in the database
and wakes up the purge thread if it is suspended (not sleeping).  Note
that there is a small chance that the purge thread stays suspended
(we do not protect our check with the srv_sys_t:mutex and the
purge_sys->latch, for performance reasons). */
UNIV_INTERN
void
srv_wake_purge_thread_if_not_active(void)
/*=====================================*/
{
	ut_ad(!srv_sys_mutex_own());

	if (purge_sys->state == PURGE_STATE_RUN
	    && srv_sys->n_threads_active[SRV_PURGE] == 0) {

		srv_release_threads(SRV_PURGE, 1);
	}
}

/*******************************************************************//**
Wakes up the master thread if it is suspended or being suspended. */
UNIV_INTERN
void
srv_wake_master_thread(void)
/*========================*/
{
	ut_ad(!srv_sys_mutex_own());

	srv_inc_activity_count();

	srv_release_threads(SRV_MASTER, 1);
}

/*******************************************************************//**
Get current server activity count. We don't hold srv_sys::mutex while
reading this value as it is only used in heuristics.
@return activity count. */
UNIV_INTERN
ulint
srv_get_activity_count(void)
/*========================*/
{
	return(srv_sys->activity_count);
}

/*******************************************************************//**
Check if there has been any activity.
@return FALSE if no change in activity counter. */
UNIV_INTERN
ibool
srv_check_activity(
/*===============*/
	ulint		old_activity_count)	/*!< in: old activity count */
{
	return(srv_sys->activity_count != old_activity_count);
}

/********************************************************************//**
The master thread is tasked to ensure that flush of log file happens
once every second in the background. This is to ensure that not more
than one second of trxs are lost in case of crash when
innodb_flush_logs_at_trx_commit != 1 */
static
void
srv_sync_log_buffer_in_background(void)
/*===================================*/
{
	time_t	current_time = time(NULL);

	srv_main_thread_op_info = "flushing log";
	if (difftime(current_time, srv_last_log_flush_time)
	    >= srv_flush_log_at_timeout) {
		log_buffer_sync_in_background(TRUE);
		srv_last_log_flush_time = current_time;
		srv_log_writes_and_flush++;
	}
}

/********************************************************************//**
Make room in the table cache by evicting an unused table.
@return number of tables evicted. */
static
ulint
srv_master_evict_from_table_cache(
/*==============================*/
	ulint	pct_check)	/*!< in: max percent to check */
{
	ulint	n_tables_evicted = 0;

	rw_lock_x_lock(&dict_operation_lock);

	dict_mutex_enter_for_mysql();

	n_tables_evicted = dict_make_room_in_cache(
		innobase_get_table_cache_size(), pct_check);

	dict_mutex_exit_for_mysql();

	rw_lock_x_unlock(&dict_operation_lock);

	return(n_tables_evicted);
}

/*********************************************************************//**
This function prints progress message every 60 seconds during server
shutdown, for any activities that master thread is pending on. */
static
void
srv_shutdown_print_master_pending(
/*==============================*/
	ib_time_t*	last_print_time,	/*!< last time the function
						print the message */
	ulint		n_tables_to_drop,	/*!< number of tables to
						be dropped */
	ulint		n_bytes_merged)		/*!< number of change buffer
						just merged */
{
	ib_time_t	current_time;
	double		time_elapsed;

	current_time = ut_time();
	time_elapsed = ut_difftime(current_time, *last_print_time);

	if (time_elapsed > 60) {
		*last_print_time = ut_time();

		if (n_tables_to_drop) {
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: Waiting for "
				"%lu table(s) to be dropped\n",
				(ulong) n_tables_to_drop);
		}

		/* Check change buffer merge, we only wait for change buffer
		merge if it is a slow shutdown */
		if (!srv_fast_shutdown && n_bytes_merged) {
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: Waiting for change "
				"buffer merge to complete\n"
				"  InnoDB: number of bytes of change buffer "
				"just merged:  %lu\n",
				n_bytes_merged);
		}
	}
}

/*********************************************************************//**
Perform the tasks that the master thread is supposed to do when the
server is active. There are two types of tasks. The first category is
of such tasks which are performed at each inovcation of this function.
We assume that this function is called roughly every second when the
server is active. The second category is of such tasks which are
performed at some interval e.g.: purge, dict_LRU cleanup etc. */
static
void
srv_master_do_active_tasks(void)
/*============================*/
{
	ib_time_t	cur_time = ut_time();
	ullint		counter_time = ut_time_us(NULL);
	ulonglong	start_time;

	/* First do the tasks that we are suppose to do at each
	invocation of this function. */

	++srv_main_active_loops;

	MONITOR_INC(MONITOR_MASTER_ACTIVE_LOOPS);

	/* ALTER TABLE in MySQL requires on Unix that the table handler
	can drop tables lazily after there no longer are SELECT
	queries to them. */
	start_time = my_timer_now();
	srv_main_thread_op_info = "doing background drop tables";
	row_drop_tables_for_mysql_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_BACKGROUND_DROP_TABLE_MICROSECOND, counter_time);
	srv_drop_table_time += my_timer_since(start_time);

	if (srv_shutdown_state > 0) {
		return;
	}

	/* make sure that there is enough reusable space in the redo
	log files */
	start_time = my_timer_now();
	srv_main_thread_op_info = "checking free log space";
	log_free_check();
	srv_free_log_time += my_timer_since(start_time);

	/* Do an ibuf merge */
	start_time = my_timer_now();
	srv_main_thread_op_info = "doing insert buffer merge";
	counter_time = ut_time_us(NULL);
	ibuf_merge_in_background(false);
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_IBUF_MERGE_MICROSECOND, counter_time);
	srv_ibuf_contract_time += my_timer_since(start_time);

	/* Flush logs if needed */
	start_time = my_timer_now();
	srv_main_thread_op_info = "flushing log";
	srv_sync_log_buffer_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_LOG_FLUSH_MICROSECOND, counter_time);
	srv_log_flush_time += my_timer_since(start_time);

	/* Now see if various tasks that are performed at defined
	intervals need to be performed. */

#ifdef MEM_PERIODIC_CHECK
	/* Check magic numbers of every allocated mem block once in
	SRV_MASTER_MEM_VALIDATE_INTERVAL seconds */
	if (cur_time % SRV_MASTER_MEM_VALIDATE_INTERVAL == 0) {
		mem_validate_all_blocks();
		MONITOR_INC_TIME_IN_MICRO_SECS(
			MONITOR_SRV_MEM_VALIDATE_MICROSECOND, counter_time);
	}
#endif
	if (srv_shutdown_state > 0) {
		return;
	}

	if (srv_shutdown_state > 0) {
		return;
	}

	start_time = my_timer_now();
	if (cur_time % SRV_MASTER_DICT_LRU_INTERVAL == 0) {
		srv_main_thread_op_info = "enforcing dict cache limit";
		srv_master_evict_from_table_cache(50);
		MONITOR_INC_TIME_IN_MICRO_SECS(
			MONITOR_SRV_DICT_LRU_MICROSECOND, counter_time);
	}
	srv_cache_limit_time += my_timer_since(start_time);

	if (srv_shutdown_state > 0) {
		return;
	}

	/* Make a new checkpoint */
	start_time = my_timer_now();
	if (cur_time % SRV_MASTER_CHECKPOINT_INTERVAL == 0) {
		srv_main_thread_op_info = "making checkpoint";
		log_checkpoint(TRUE, FALSE);
		MONITOR_INC_TIME_IN_MICRO_SECS(
			MONITOR_SRV_CHECKPOINT_MICROSECOND, counter_time);
	}
	srv_checkpoint_time += my_timer_since(start_time);
}

/*********************************************************************//**
Perform the tasks that the master thread is supposed to do whenever the
server is idle. We do check for the server state during this function
and if the server has entered the shutdown phase we may return from
the function without completing the required tasks.
Note that the server can move to active state when we are executing this
function but we don't check for that as we are suppose to perform more
or less same tasks when server is active. */
static
void
srv_master_do_idle_tasks(void)
/*==========================*/
{
	ullint	counter_time;

	++srv_main_idle_loops;

	MONITOR_INC(MONITOR_MASTER_IDLE_LOOPS);


	/* ALTER TABLE in MySQL requires on Unix that the table handler
	can drop tables lazily after there no longer are SELECT
	queries to them. */
	counter_time = ut_time_us(NULL);
	srv_main_thread_op_info = "doing background drop tables";
	row_drop_tables_for_mysql_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_BACKGROUND_DROP_TABLE_MICROSECOND,
			 counter_time);

	if (srv_shutdown_state > 0) {
		return;
	}

	/* make sure that there is enough reusable space in the redo
	log files */
	srv_main_thread_op_info = "checking free log space";
	log_free_check();

	/* Do an ibuf merge */
	counter_time = ut_time_us(NULL);
	srv_main_thread_op_info = "doing insert buffer merge";
	ibuf_merge_in_background(true);
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_IBUF_MERGE_MICROSECOND, counter_time);

	if (srv_shutdown_state > 0) {
		return;
	}

	srv_main_thread_op_info = "enforcing dict cache limit";
	srv_master_evict_from_table_cache(100);
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_DICT_LRU_MICROSECOND, counter_time);

	/* Flush logs if needed */
	srv_sync_log_buffer_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_LOG_FLUSH_MICROSECOND, counter_time);

	if (srv_shutdown_state > 0) {
		return;
	}

	/* Make a new checkpoint */
	srv_main_thread_op_info = "making checkpoint";
	log_checkpoint(TRUE, FALSE);
	MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_SRV_CHECKPOINT_MICROSECOND,
				       counter_time);
}

/*********************************************************************//**
Perform the tasks during shutdown. The tasks that we do at shutdown
depend on srv_fast_shutdown:
2 => very fast shutdown => do no book keeping
1 => normal shutdown => clear drop table queue and make checkpoint
0 => slow shutdown => in addition to above do complete purge and ibuf
merge
@return TRUE if some work was done. FALSE otherwise */
static
ibool
srv_master_do_shutdown_tasks(
/*=========================*/
	ib_time_t*	last_print_time)/*!< last time the function
					print the message */
{
	ulint		n_bytes_merged = 0;
	ulint		n_tables_to_drop = 0;

	ut_ad(!srv_read_only_mode);

	++srv_main_shutdown_loops;

	ut_a(srv_shutdown_state > 0);

	/* In very fast shutdown none of the following is necessary */
	if (srv_fast_shutdown == 2) {
		return(FALSE);
	}

	/* ALTER TABLE in MySQL requires on Unix that the table handler
	can drop tables lazily after there no longer are SELECT
	queries to them. */
	srv_main_thread_op_info = "doing background drop tables";
	n_tables_to_drop = row_drop_tables_for_mysql_in_background();

	/* make sure that there is enough reusable space in the redo
	log files */
	srv_main_thread_op_info = "checking free log space";
	log_free_check();

	/* In case of normal shutdown we don't do ibuf merge or purge */
	if (srv_fast_shutdown == 1) {
		goto func_exit;
	}

	/* Do an ibuf merge */
	srv_main_thread_op_info = "doing insert buffer merge";
	n_bytes_merged = ibuf_merge_in_background(true);

	/* Flush logs if needed */
	srv_sync_log_buffer_in_background();

func_exit:
	/* Make a new checkpoint about once in 10 seconds */
	srv_main_thread_op_info = "making checkpoint";
	log_checkpoint(TRUE, FALSE);

	/* Print progress message every 60 seconds during shutdown */
	if (srv_shutdown_state > 0 && srv_print_verbose_log) {
		srv_shutdown_print_master_pending(
			last_print_time, n_tables_to_drop, n_bytes_merged);
	}

	return(n_bytes_merged || n_tables_to_drop);
}

/*********************************************************************//**
Puts master thread to sleep. At this point we are using polling to
service various activities. Master thread sleeps for one second before
checking the state of the server again */
static
void
srv_master_sleep(void)
/*==================*/
{
	srv_main_thread_op_info = "sleeping";
	os_thread_sleep(1000000);
	srv_main_thread_op_info = "";
}

/*********************************************************************//**
The master thread controlling the server.
@return	a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_master_thread)(
/*==============================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	srv_slot_t*	slot;
	ulint		old_activity_count = srv_get_activity_count();
	ib_time_t	last_print_time;

	ut_ad(!srv_read_only_mode);

	buf_pool_resizable_master = false;

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Master thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_master_thread_key);
#endif /* UNIV_PFS_THREAD */

	srv_main_thread_process_no = os_proc_get_number();
	srv_main_thread_id = os_thread_pf(os_thread_get_curr_id());

	slot = srv_reserve_slot(SRV_MASTER);
	ut_a(slot == srv_sys->sys_threads);

	last_print_time = ut_time();
loop:
	if (srv_force_recovery >= SRV_FORCE_NO_BACKGROUND
#ifdef XTRABACKUP
	    || xtrabackup_no_background
#endif // ifdef XTRABACKUP
	) {
		goto suspend_thread;
	}

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		buf_pool_resizable_master = true;
		srv_master_sleep();

		if (buf_pool_resizing_bg) {
			os_event_wait(buf_pool_resized_event);
		}
		buf_pool_resizable_master = false;

		MONITOR_INC(MONITOR_MASTER_THREAD_SLEEP);

		if (srv_check_activity(old_activity_count)) {
			old_activity_count = srv_get_activity_count();
			srv_master_do_active_tasks();
		} else {
			srv_master_do_idle_tasks();
		}
	}

	while (srv_master_do_shutdown_tasks(&last_print_time)) {

		/* Shouldn't loop here in case of very fast shutdown */
		ut_ad(srv_fast_shutdown < 2);
	}

suspend_thread:
	srv_main_thread_op_info = "suspending";

	srv_suspend_thread(slot);

	/* DO NOT CHANGE THIS STRING. innobase_start_or_create_for_mysql()
	waits for database activity to die down when converting < 4.1.x
	databases, and relies on this string being exactly as it is. InnoDB
	manual also mentions this string in several places. */
	srv_main_thread_op_info = "waiting for server activity";

	os_event_wait(slot->event);

	if (srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS) {
		os_thread_exit(NULL);
	}

	goto loop;

	buf_pool_resizable_master = true;

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/*********************************************************************//**
Check if purge should stop.
@return true if it should shutdown. */
static
bool
srv_purge_should_exit(
/*==============*/
	ulint		n_purged)	/*!< in: pages purged in last batch */
{
	switch (srv_shutdown_state) {
	case SRV_SHUTDOWN_NONE:
		/* Normal operation. */
		break;

	case SRV_SHUTDOWN_CLEANUP:
	case SRV_SHUTDOWN_EXIT_THREADS:
		/* Exit unless slow shutdown requested or all done. */
		return(srv_fast_shutdown != 0 || n_purged == 0);

	case SRV_SHUTDOWN_LAST_PHASE:
	case SRV_SHUTDOWN_FLUSH_PHASE:
		ut_error;
	}

	return(false);
}

/*********************************************************************//**
Fetch and execute a task from the work queue.
@return	true if a task was executed */
static
bool
srv_task_execute(void)
/*==================*/
{
	que_thr_t*	thr = NULL;

	ut_ad(!srv_read_only_mode);
	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

	mutex_enter(&srv_sys->tasks_mutex);

	if (UT_LIST_GET_LEN(srv_sys->tasks) > 0) {

		thr = UT_LIST_GET_FIRST(srv_sys->tasks);

		ut_a(que_node_get_type(thr->child) == QUE_NODE_PURGE);

		UT_LIST_REMOVE(queue, srv_sys->tasks, thr);
	}

	mutex_exit(&srv_sys->tasks_mutex);

	if (thr != NULL) {

		que_run_threads(thr);

		os_atomic_inc_ulint(
			&purge_sys->bh_mutex, &purge_sys->n_completed, 1);
	}

	return(thr != NULL);
}

/*********************************************************************//**
Worker thread that reads tasks from the work queue and executes them.
@return	a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_worker_thread)(
/*==============================*/
	void*	arg MY_ATTRIBUTE((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	srv_slot_t*	slot;

	ut_ad(!srv_read_only_mode);
	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ut_print_timestamp(stderr);
	fprintf(stderr, " InnoDB: worker thread starting, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	slot = srv_reserve_slot(SRV_WORKER);

	ut_a(srv_n_purge_threads > 1);

	srv_sys_mutex_enter();

	ut_a(srv_sys->n_threads_active[SRV_WORKER] < srv_n_purge_threads);

	srv_sys_mutex_exit();

	/* We need to ensure that the worker threads exit after the
	purge coordinator thread. Otherwise the purge coordinaor can
	end up waiting forever in trx_purge_wait_for_workers_to_complete() */

	do {
		srv_suspend_thread(slot);

		os_event_wait(slot->event);

		if (srv_task_execute()) {

			/* If there are tasks in the queue, wakeup
			the purge coordinator thread. */

			srv_wake_purge_thread_if_not_active();
		}

		/* Note: we are checking the state without holding the
		purge_sys->latch here. */
	} while (purge_sys->state != PURGE_STATE_EXIT);

	srv_free_slot(slot);

	rw_lock_x_lock(&purge_sys->latch);

	ut_a(!purge_sys->running);
	ut_a(purge_sys->state == PURGE_STATE_EXIT);
	ut_a(srv_shutdown_state > SRV_SHUTDOWN_NONE);

	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ut_print_timestamp(stderr);
	fprintf(stderr, " InnoDB: Purge worker thread exiting, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/*********************************************************************//**
Do the actual purge operation.
@return length of history list before the last purge batch. */
static
ulint
srv_do_purge(
/*=========*/
	ulint		n_threads,	/*!< in: number of threads to use */
	ulint*		n_total_purged)	/*!< in/out: total pages purged */
{
	ulint		n_pages_purged;

	static ulint	count = 0;
	static ulint	n_use_threads = 0;
	static ulint	rseg_history_len = 0;
	ulint		old_activity_count = srv_get_activity_count();

	ut_a(n_threads > 0);
	ut_ad(!srv_read_only_mode);

	/* Purge until there are no more records to purge and there is
	no change in configuration or server state. If the user has
	configured more than one purge thread then we treat that as a
	pool of threads and only use the extra threads if purge can't
	keep up with updates. */

	if (n_use_threads == 0) {
		n_use_threads = n_threads;
	}

	do {
		if (trx_sys->rseg_history_len > rseg_history_len
		    || (srv_max_purge_lag > 0
			&& rseg_history_len > srv_max_purge_lag)) {

			/* History length is now longer than what it was
			when we took the last snapshot. Use more threads. */

			if (n_use_threads < n_threads) {
				++n_use_threads;
			}

		} else if (srv_check_activity(old_activity_count)
			   && n_use_threads > 1) {

			/* History length same or smaller since last snapshot,
			use fewer threads. */

			--n_use_threads;

			old_activity_count = srv_get_activity_count();
		}

		/* Ensure that the purge threads are less than what
		was configured. */

		ut_a(n_use_threads > 0);
		ut_a(n_use_threads <= n_threads);

		/* Take a snapshot of the history list before purge. */
		if ((rseg_history_len = trx_sys->rseg_history_len) == 0) {
			break;
		}

		ulonglong	start_time;
		start_time = my_timer_now();

		n_pages_purged = trx_purge(
			n_use_threads, srv_purge_batch_size,
			(++count % TRX_SYS_N_RSEGS) == 0);

		srv_purge_time += my_timer_since(start_time);

		*n_total_purged += n_pages_purged;

	} while (!srv_purge_should_exit(n_pages_purged)
		 && n_pages_purged > 0
		 && purge_sys->state == PURGE_STATE_RUN);

	return(rseg_history_len);
}

/*********************************************************************//**
Suspend the purge coordinator thread. */
static
void
srv_purge_coordinator_suspend(
/*==========================*/
	srv_slot_t*	slot,			/*!< in/out: Purge coordinator
						thread slot */
	ulint		rseg_history_len)	/*!< in: history list length
						before last purge */
{
	ut_ad(!srv_read_only_mode);
	ut_a(slot->type == SRV_PURGE);

	bool		stop = false;

	/** Maximum wait time on the purge event, in micro-seconds. */
	static const ulint SRV_PURGE_MAX_TIMEOUT = 10000;

	ib_int64_t	sig_count = srv_suspend_thread(slot);

	do {
		ulint		ret;

		rw_lock_x_lock(&purge_sys->latch);

		purge_sys->running = false;

		rw_lock_x_unlock(&purge_sys->latch);

		/* We don't wait right away on the the non-timed wait because
		we want to signal the thread that wants to suspend purge. */

		if (stop) {
			os_event_wait_low(slot->event, sig_count);
			ret = 0;
		} else if (rseg_history_len <= trx_sys->rseg_history_len) {
			ret = os_event_wait_time_low(
				slot->event, SRV_PURGE_MAX_TIMEOUT, sig_count);
		} else {
			/* We don't want to waste time waiting, if the
			history list increased by the time we got here,
			unless purge has been stopped. */
			ret = 0;
		}

		srv_sys_mutex_enter();

		/* The thread can be in state !suspended after the timeout
		but before this check if another thread sent a wakeup signal. */

		if (slot->suspended) {
			slot->suspended = FALSE;
			++srv_sys->n_threads_active[slot->type];
			ut_a(srv_sys->n_threads_active[slot->type] == 1);
		}

		srv_sys_mutex_exit();

		sig_count = srv_suspend_thread(slot);

		rw_lock_x_lock(&purge_sys->latch);

		stop = (srv_shutdown_state == SRV_SHUTDOWN_NONE
			&& purge_sys->state == PURGE_STATE_STOP);

		if (!stop) {
			ut_a(purge_sys->n_stop == 0);
			purge_sys->running = true;
		} else {
			ut_a(purge_sys->n_stop > 0);

			/* Signal that we are suspended. */
			os_event_set(purge_sys->event);
		}

		rw_lock_x_unlock(&purge_sys->latch);

		if (ret == OS_SYNC_TIME_EXCEEDED) {

			/* No new records added since wait started then simply
			wait for new records. The magic number 5000 is an
			approximation for the case where we have cached UNDO
			log records which prevent truncate of the UNDO
			segments. */

			if (rseg_history_len == trx_sys->rseg_history_len
			    && trx_sys->rseg_history_len < 5000) {

				stop = true;
			}
		}

	} while (stop);

	srv_sys_mutex_enter();

	if (slot->suspended) {
		slot->suspended = FALSE;
		++srv_sys->n_threads_active[slot->type];
		ut_a(srv_sys->n_threads_active[slot->type] == 1);
	}

	srv_sys_mutex_exit();
}

/*********************************************************************//**
Purge coordinator thread that schedules the purge tasks.
@return	a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_purge_coordinator_thread)(
/*=========================================*/
	void*	arg MY_ATTRIBUTE((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	my_thread_init();

	srv_slot_t*	slot;
	ulint           n_total_purged = ULINT_UNDEFINED;

	ut_ad(!srv_read_only_mode);
	ut_a(srv_n_purge_threads >= 1);
	ut_a(trx_purge_state() == PURGE_STATE_INIT);
	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

	buf_pool_resizable_purge = false;

	rw_lock_x_lock(&purge_sys->latch);

	purge_sys->running = true;
	purge_sys->state = PURGE_STATE_RUN;

	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_purge_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ut_print_timestamp(stderr);
	fprintf(stderr, " InnoDB: Purge coordinator thread created, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	slot = srv_reserve_slot(SRV_PURGE);

	ulint	rseg_history_len = trx_sys->rseg_history_len;

	do {
		buf_pool_resizable_purge = true;

		/* If there are no records to purge or the last
		purge didn't purge any records then wait for activity. */

		if (srv_shutdown_state == SRV_SHUTDOWN_NONE
		    && (purge_sys->state == PURGE_STATE_STOP
			|| n_total_purged == 0)) {

			srv_purge_coordinator_suspend(slot, rseg_history_len);
		}

		if (buf_pool_resizing) {
			os_event_wait(buf_pool_resized_event);
		}

		buf_pool_resizable_purge = false;

		if (srv_purge_should_exit(n_total_purged)) {
			ut_a(!slot->suspended);
			break;
		}

		n_total_purged = 0;

		rseg_history_len = srv_do_purge(
			srv_n_purge_threads, &n_total_purged);

	} while (!srv_purge_should_exit(n_total_purged));

	/* Ensure that we don't jump out of the loop unless the
	exit condition is satisfied. */

	ut_a(srv_purge_should_exit(n_total_purged));

	ulint	n_pages_purged = ULINT_MAX;

	/* Ensure that all records are purged if it is not a fast shutdown.
	This covers the case where a record can be added after we exit the
	loop above. */
	while (srv_fast_shutdown == 0 && n_pages_purged > 0) {
		n_pages_purged = trx_purge(1, srv_purge_batch_size, false);
	}

	/* This trx_purge is called to remove any undo records (added by
	background threads) after completion of the above loop. When
	srv_fast_shutdown != 0, a large batch size can cause significant
	delay in shutdown ,so reducing the batch size to magic number 20
	(which was default in 5.5), which we hope will be sufficient to
	remove all the undo records */
	const	uint temp_batch_size = 20;

	n_pages_purged = trx_purge(1, srv_purge_batch_size <= temp_batch_size
				      ? srv_purge_batch_size : temp_batch_size,
				   true);
	ut_a(n_pages_purged == 0 || srv_fast_shutdown != 0);

	/* The task queue should always be empty, independent of fast
	shutdown state. */
	ut_a(srv_get_task_queue_length() == 0);

	srv_free_slot(slot);

	/* Note that we are shutting down. */
	rw_lock_x_lock(&purge_sys->latch);

	purge_sys->state = PURGE_STATE_EXIT;

	purge_sys->running = false;

	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ut_print_timestamp(stderr);
	fprintf(stderr, " InnoDB: Purge coordinator exiting, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	/* Ensure that all the worker threads quit. */
	if (srv_n_purge_threads > 1) {
		srv_release_threads(SRV_WORKER, srv_n_purge_threads - 1);
	}

	buf_pool_resizable_purge = true;

	my_thread_end();

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/**********************************************************************//**
Enqueues a task to server task queue and releases a worker thread, if there
is a suspended one. */
UNIV_INTERN
void
srv_que_task_enqueue_low(
/*=====================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	ut_ad(!srv_read_only_mode);
	mutex_enter(&srv_sys->tasks_mutex);

	UT_LIST_ADD_LAST(queue, srv_sys->tasks, thr);

	mutex_exit(&srv_sys->tasks_mutex);

	srv_release_threads(SRV_WORKER, 1);
}

/**********************************************************************//**
Get count of tasks in the queue.
@return number of tasks in queue  */
UNIV_INTERN
ulint
srv_get_task_queue_length(void)
/*===========================*/
{
	ulint	n_tasks;

	ut_ad(!srv_read_only_mode);

	mutex_enter(&srv_sys->tasks_mutex);

	n_tasks = UT_LIST_GET_LEN(srv_sys->tasks);

	mutex_exit(&srv_sys->tasks_mutex);

	return(n_tasks);
}

/**********************************************************************//**
Wakeup the purge threads. */
UNIV_INTERN
void
srv_purge_wakeup(void)
/*==================*/
{
	ut_ad(!srv_read_only_mode);

	if (srv_force_recovery < SRV_FORCE_NO_BACKGROUND
#ifdef XTRABACKUP
	    && !xtrabackup_no_background
#endif // ifdef XTRABACKUP
	) {

		srv_release_threads(SRV_PURGE, 1);

		if (srv_n_purge_threads > 1) {
			ulint	n_workers = srv_n_purge_threads - 1;

			srv_release_threads(SRV_WORKER, n_workers);
		}
	}
}
