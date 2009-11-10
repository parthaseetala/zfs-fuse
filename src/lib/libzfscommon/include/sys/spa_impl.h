/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_SPA_IMPL_H
#define	_SYS_SPA_IMPL_H

#include <sys/spa.h>
#include <sys/vdev.h>
#include <sys/metaslab.h>
#include <sys/dmu.h>
#include <sys/dsl_pool.h>
#include <sys/uberblock_impl.h>
#include <sys/zfs_context.h>
#include <sys/avl.h>
#include <sys/refcount.h>
#include <sys/bplist.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct zio_aio_ctx;

typedef struct spa_error_entry {
	zbookmark_t	se_bookmark;
	char		*se_name;
	avl_node_t	se_avl;
} spa_error_entry_t;

typedef struct spa_history_phys {
	uint64_t sh_pool_create_len;	/* ending offset of zpool create */
	uint64_t sh_phys_max_off;	/* physical EOF */
	uint64_t sh_bof;		/* logical BOF */
	uint64_t sh_eof;		/* logical EOF */
	uint64_t sh_records_lost;	/* num of records overwritten */
} spa_history_phys_t;

struct spa_aux_vdev {
	uint64_t	sav_object;		/* MOS object for device list */
	nvlist_t	*sav_config;		/* cached device config */
	vdev_t		**sav_vdevs;		/* devices */
	int		sav_count;		/* number devices */
	boolean_t	sav_sync;		/* sync the device list */
	nvlist_t	**sav_pending;		/* pending device additions */
	uint_t		sav_npending;		/* # pending devices */
};

typedef struct spa_config_lock {
	kmutex_t	scl_lock;
	kthread_t	*scl_writer;
	int		scl_write_wanted;
	kcondvar_t	scl_cv;
	refcount_t	scl_count;
} spa_config_lock_t;

typedef struct spa_config_dirent {
	list_node_t	scd_link;
	char		*scd_path;
} spa_config_dirent_t;

enum zio_taskq_type {
	ZIO_TASKQ_ISSUE = 0,
	ZIO_TASKQ_INTERRUPT,
	ZIO_TASKQ_TYPES
};

struct spa {
	/*
	 * Fields protected by spa_namespace_lock.
	 */
	char		spa_name[MAXNAMELEN];	/* pool name */
	avl_node_t	spa_avl;		/* node in spa_namespace_avl */
	nvlist_t	*spa_config;		/* last synced config */
	nvlist_t	*spa_config_syncing;	/* currently syncing config */
	uint64_t	spa_config_txg;		/* txg of last config change */
	int		spa_sync_pass;		/* iterate-to-convergence */
	pool_state_t	spa_state;		/* pool state */
	int		spa_inject_ref;		/* injection references */
	uint8_t		spa_sync_on;		/* sync threads are running */
	spa_load_state_t spa_load_state;	/* current load operation */
	boolean_t	spa_load_verbatim;	/* load the given config? */
	taskq_t		*spa_zio_taskq[ZIO_TYPES][ZIO_TASKQ_TYPES];
	dsl_pool_t	*spa_dsl_pool;
	metaslab_class_t *spa_normal_class;	/* normal data class */
	metaslab_class_t *spa_log_class;	/* intent log data class */
	uint64_t	spa_first_txg;		/* first txg after spa_open() */
	uint64_t	spa_final_txg;		/* txg of export/destroy */
	uint64_t	spa_freeze_txg;		/* freeze pool at this txg */
	uint64_t	spa_load_max_txg;	/* best initial ub_txg */
	uint64_t	spa_claim_max_txg;	/* highest claimed birth txg */
	objset_t	*spa_meta_objset;	/* copy of dp->dp_meta_objset */
	txg_list_t	spa_vdev_txg_list;	/* per-txg dirty vdev list */
	vdev_t		*spa_root_vdev;		/* top-level vdev container */
	uint64_t	spa_load_guid;		/* initial guid for spa_load */
	list_t		spa_config_dirty_list;	/* vdevs with dirty config */
	list_t		spa_state_dirty_list;	/* vdevs with dirty state */
	spa_aux_vdev_t	spa_spares;		/* hot spares */
	spa_aux_vdev_t	spa_l2cache;		/* L2ARC cache devices */
	uint64_t	spa_config_object;	/* MOS object for pool config */
	uint64_t	spa_config_generation;	/* config generation number */
	uint64_t	spa_syncing_txg;	/* txg currently syncing */
	uint64_t	spa_deferred_bplist_obj; /* object for deferred frees */
	bplist_t	spa_deferred_bplist;	/* deferred-free bplist */
	bplist_t	spa_free_bplist[TXG_SIZE]; /* bplist of stuff to free */
	uberblock_t	spa_ubsync;		/* last synced uberblock */
	uberblock_t	spa_uberblock;		/* current uberblock */
	boolean_t	spa_extreme_rewind;	/* rewind past deferred frees */
	kmutex_t	spa_scrub_lock;		/* resilver/scrub lock */
	uint64_t	spa_scrub_inflight;	/* in-flight scrub I/Os */
	uint64_t	spa_scrub_maxinflight;	/* max in-flight scrub I/Os */
	uint64_t	spa_scrub_errors;	/* scrub I/O error count */
	kcondvar_t	spa_scrub_io_cv;	/* scrub I/O completion */
	uint8_t		spa_scrub_active;	/* active or suspended? */
	uint8_t		spa_scrub_type;		/* type of scrub we're doing */
	uint8_t		spa_scrub_finished;	/* indicator to rotate logs */
	uint8_t		spa_scrub_started;	/* started since last boot */
	uint8_t		spa_scrub_reopen;	/* scrub doing vdev_reopen */
	kmutex_t	spa_async_lock;		/* protect async state */
	kthread_t	*spa_async_thread;	/* thread doing async task */
	int		spa_async_suspended;	/* async tasks suspended */
	kcondvar_t	spa_async_cv;		/* wait for thread_exit() */
	uint16_t	spa_async_tasks;	/* async task mask */
	char		*spa_root;		/* alternate root directory */
	uint64_t	spa_ena;		/* spa-wide ereport ENA */
	int		spa_last_open_failed;	/* error if last open failed */
	nvlist_t	*spa_failed_open_cfg;	/* cached config nvlist */
	uint64_t	spa_last_ubsync_txg;	/* "best" uberblock txg */
	uint64_t	spa_last_ubsync_txg_ts;	/* timestamp from that ub */
	uint64_t	spa_load_txg;		/* ub txg that loaded */
	uint64_t	spa_load_txg_ts;	/* timestamp from that ub */
	uint64_t	spa_load_meta_errors;	/* verify metadata err count */
	uint64_t	spa_load_data_errors;	/* verify data err count */
	uint64_t	spa_verify_min_txg;	/* start txg of verify scrub */
	kmutex_t	spa_errlog_lock;	/* error log lock */
	uint64_t	spa_errlog_last;	/* last error log object */
	uint64_t	spa_errlog_scrub;	/* scrub error log object */
	kmutex_t	spa_errlist_lock;	/* error list/ereport lock */
	avl_tree_t	spa_errlist_last;	/* last error list */
	avl_tree_t	spa_errlist_scrub;	/* scrub error list */
	uint64_t	spa_deflate;		/* should we deflate? */
	uint64_t	spa_history;		/* history object */
	kmutex_t	spa_history_lock;	/* history lock */
	vdev_t		*spa_pending_vdev;	/* pending vdev additions */
	kmutex_t	spa_props_lock;		/* property lock */
	uint64_t	spa_pool_props_object;	/* object for properties */
	uint64_t	spa_bootfs;		/* default boot filesystem */
	uint64_t	spa_failmode;		/* failure mode for the pool */
	struct zio_aio_ctx *spa_aio_ctx;	/* asynchronous I/O context */
	uint64_t	spa_delegation;		/* delegation on/off */
	list_t		spa_config_list;	/* previous cache file(s) */
	zio_t		*spa_async_zio_root;	/* root of all async I/O */
	zio_t		*spa_suspend_zio_root;	/* root of all suspended I/O */
	kmutex_t	spa_suspend_lock;	/* protects suspend_zio_root */
	kcondvar_t	spa_suspend_cv;		/* notification of resume */
	uint8_t		spa_suspended;		/* pool is suspended */
	uint8_t		spa_claiming;		/* pool is doing zil_claim() */
	boolean_t	spa_is_root;		/* pool is root */
	int		spa_minref;		/* num refs when first opened */
	int		spa_mode;		/* FREAD | FWRITE */
	spa_log_state_t spa_log_state;		/* log state */
	uint64_t	spa_autoexpand;		/* lun expansion on/off */
	ddt_t		*spa_ddt[ZIO_CHECKSUM_FUNCTIONS]; /* in-core DDTs */
	uint64_t	spa_ddt_stat_object;	/* DDT statistics */
	uint64_t	spa_dedup_ditto;	/* dedup ditto threshold */
	uint64_t	spa_dedup_checksum;	/* default dedup checksum */
	boolean_t	spa_autoreplace;	/* autoreplace set in open */
	int		spa_vdev_locks;		/* locks grabbed */
	/*
	 * spa_refcnt & spa_config_lock must be the last elements
	 * because refcount_t changes size based on compilation options.
	 * In order for the MDB module to function correctly, the other
	 * fields must remain in the same location.
	 */
	spa_config_lock_t spa_config_lock[SCL_LOCKS]; /* config changes */
	refcount_t	spa_refcount;		/* number of opens */
};

extern const char *spa_config_path;

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SPA_IMPL_H */
