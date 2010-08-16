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

/*
 * ZFS control directory (a.k.a. ".zfs")
 *
 * This directory provides a common location for all ZFS meta-objects.
 * Currently, this is only the 'snapshot' directory, but this may expand in the
 * future.  The elements are built using the GFS primitives, as the hierarchy
 * does not actually exist on disk.
 *
 * For 'snapshot', we don't want to have all snapshots always mounted, because
 * this would take up a huge amount of space in /etc/mnttab.  We have three
 * types of objects:
 *
 * 	ctldir ------> snapshotdir -------> snapshot
 *                                             |
 *                                             |
 *                                             V
 *                                         mounted fs
 *
 * The 'snapshot' node contains just enough information to lookup '..' and act
 * as a mountpoint for the snapshot.  Whenever we lookup a specific snapshot, we
 * perform an automount of the underlying filesystem and return the
 * corresponding vnode.
 *
 * All mounts are handled automatically by the kernel, but unmounts are
 * (currently) handled from user land.  The main reason is that there is no
 * reliable way to auto-unmount the filesystem when it's "no longer in use".
 * When the user unmounts a filesystem, we call zfsctl_unmount(), which
 * unmounts any snapshots within the snapshot directory.
 *
 * The '.zfs', '.zfs/snapshot', and all directories created under
 * '.zfs/snapshot' (ie: '.zfs/snapshot/<snapname>') are all GFS nodes and
 * share the same vfs_t as the head filesystem (what '.zfs' lives under).
 *
 * File systems mounted ontop of the GFS nodes '.zfs/snapshot/<snapname>'
 * (ie: snapshots) are ZFS nodes and have their own unique vfs_t.
 * However, vnodes within these mounted on file systems have their v_vfsp
 * fields set to the head filesystem to make NFS happy (see
 * zfsctl_snapdir_lookup()). We VFS_HOLD the head filesystem's vfs_t
 * so that it cannot be freed until all snapshots have been unmounted.
 */

#include <fs/fs_subr.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>
#include <sys/vfs_opreg.h>
// #include <sys/gfs.h>
#include <sys/stat.h>
#include <sys/dmu.h>
#include <sys/dsl_deleg.h>
#include <sys/mount.h>
#include <sys/sunddi.h>
#define FUSE_USE_VERSION 26

#include <fuse/fuse_lowlevel.h>

#include "zfs_namecheck.h"
#include "util.h"
#include "fuse_listener.h"
#include "zfs_operations.h"

#define ctldir_str "snapshot"
static const char *ctldir_name = "snapshot";

static char **temp_snap;
static int used_snap,alloc_snap,destroy_snaps;

static void conv_spaces(char *buff) {
    char *s;
    // decode spaces
    while (s = strstr(buff,"\\040")) {
	*s = ' ';
	strcpy(&s[1],&s[4]);
    }
}

static int get_mtpoint(zfsvfs_t *zfsvfs, char *mt,int size) {
    // libfuse has the equivalent of this function, but it can't be called
    // from "kernel" mode (aka zfs-fuse). And I don't find how to access the
    // mountpoint property.
    // So here is my version (usefull when changing snapdir after the dataset
    // has been mounted)
    FILE *f = fopen("/proc/mounts","r");
    char osname[MAXNAMELEN];
    char snap[MAXNAMELEN+6];
    char buff[2048];

    dmu_objset_name(zfsvfs->z_os, osname);
    
    char *s;
    // Spaces conversion
    while (s = strchr(osname,' ')) {
	memmove(&s[4],&s[1],strlen(s));
	strncpy(s,"\\040",4);
    }
    sprintf(snap,"%s/snap_",osname);
    int len;
    strcat(osname," ");
    len = strlen(osname);
    while (!feof(f)) {
	fgets(buff,2048,f);
	if (!strncmp(buff,osname,len)) { // found it
	    char *s = &buff[len];
	    char *e = strchr(s,' ');
	    *e = 0;
	    conv_spaces(s);
	    strncpy(mt,s,size);
	    fclose(f);
	    return 0;
	}
    }
    fclose(f);
    return 1; // not found
}

/* This is taken from the hello_ll.c example code from fuse ! */
static int ctldir_stat(fuse_ino_t ino, struct stat *stbuf)
{
	stbuf->st_ino = ino;
	// stbuf->st_mode = S_IFREG | 0444;
	stbuf->st_mode = S_IFDIR | 0555;
	stbuf->st_nlink = 1;
	stbuf->st_size = strlen(ctldir_str);

	return 0;
}

static void ctldir_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	struct stat stbuf;

	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	if (ctldir_stat(ino, &stbuf) == -1)
		fuse_reply_err(req, ENOENT);
	else
		fuse_reply_attr(req, &stbuf, 1.0);
}

static void mount_snap(zfsvfs_t *zfsvfs, char *osname, const char *snapname) {
    // find the snapshot for this inode...
    if (destroy_snaps)
	return;
    char cmd[4096];
    char mt[PATH_MAX];
    sprintf(cmd,"%s/snap_%s",osname,snapname);
    for (int n=0; n<used_snap; n++)
	if (!strcmp(temp_snap[n],cmd)) {
	    printf("mount_snap: %s already mounted\n",cmd);
	    return;
	}


    if (used_snap == alloc_snap) {
	alloc_snap += 10;
	temp_snap = realloc(temp_snap, (alloc_snap*sizeof(char*)));
    }
    temp_snap[used_snap++] = strdup(cmd);

    if (get_mtpoint(zfsvfs,mt,PATH_MAX) == 0) {
	printf("got mt %s\n",mt);
	snprintf(cmd,4096,"zfs clone -o mountpoint=\"%s/.zfs/snapshot/%s\" -o readonly=on %s@%s %s/snap_%s",mt,snapname,osname,snapname,osname,snapname);
	printf("running cmd...\n");
	int ret = system(cmd);
	printf("cmd %s returned %d\n",cmd,ret);
	return;
    } else
	printf("didn't find mountpoint ?\n");
}

static void ctldir_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse_entry_param e;
    zfsvfs_t *zfsvfs = (zfsvfs_t *) fuse_req_userdata(req);
    char osname[MAXNAMELEN];
    char snapname[MAXNAMELEN];

    dmu_objset_name(zfsvfs->z_os, osname);

    // Everything is a directory in .zfs, so initialise for a directory
    memset(&e, 0, sizeof(e));
    if (!strcmp(name,"."))
	e.ino = 1;
    else if (!strcmp(name,ctldir_name))
	e.ino = 2;

    // the root directory (.zfs) can be cached for ever, it will never change
    e.attr_timeout = 86400;
    e.entry_timeout = 86400;
    ctldir_stat(e.ino, &e.attr);
    if (parent != 1) { // lookup in snapshot
	e.attr_timeout = 1;
	e.entry_timeout = 1;
	// Then we must return the correct inode.
	// for now I ask the info about the snapshot, it should be fast
	// enough and easier than maintain a list of names / inodes.
	objset_t *snap;
	sprintf(snapname,"%s@%s",osname,name);
	if (dmu_objset_hold(snapname, FTAG, &snap) == 0) {
	    e.ino = dmu_objset_id(snap);
	    TIMESTRUC_TO_TIME(dmu_objset_snap_cmtime(snap), &e.attr.st_atime);
	    e.attr.st_mtime = e.attr.st_ctime = e.attr.st_atime;
	    dmu_objset_rele(snap, FTAG);
	    fuse_reply_entry(req, &e);
	    /* the snapshot is mounted on a lookup call and not in readdir
	     * because it's easier to reply correctly to lookup before the
	     * mount */
	    mount_snap(zfsvfs,osname,name);
	    return;

	} else { // not found
	    fuse_reply_err(req, ENOENT);
	    return;
	}
    }

    fuse_reply_entry(req, &e);
}

struct dirbuf {
	char *p;
	size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
		       fuse_ino_t ino)
{
	struct stat stbuf;
	size_t oldsize = b->size;
	b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
	b->p = (char *) realloc(b->p, b->size);
	memset(&stbuf, 0, sizeof(stbuf));
	stbuf.st_ino = ino;
	fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
			  b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
			     off_t off, size_t maxsize)
{
	if (off < bufsize)
		return fuse_reply_buf(req, buf + off,
				      min(bufsize - off, maxsize));
	else
		return fuse_reply_buf(req, NULL, 0);
}

/*
 * .zfs inode namespace
 *
 * We need to generate unique inode numbers for all files and directories
 * within the .zfs pseudo-filesystem.  We use the following scheme:
 *
 * 	ENTRY			ZFSCTL_INODE
 * 	.zfs			1
 * 	.zfs/snapshot		2
 * 	.zfs/snapshot/<snap>	objectid(snap)
 */

#define	ZFSCTL_INO_SNAP(id)	(id)

static void ctldir_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			     off_t off, struct fuse_file_info *fi)
{
    (void) fi;
    struct dirbuf b;
    zfsvfs_t *zfsvfs = (zfsvfs_t *) fuse_req_userdata(req);
    char snapname[MAXNAMELEN];
    uint64_t id, cookie;
    boolean_t case_conflict;
    int error;

    switch(ino) {
	case 1: /* / */
	    memset(&b, 0, sizeof(b));
	    dirbuf_add(req, &b, ".", 1);
	    dirbuf_add(req, &b, "..", 1);
	    dirbuf_add(req, &b, ctldir_name, 2);
	    reply_buf_limited(req, b.p, b.size, off, size);
	    free(b.p);
	    break;
	case 2: // snapshot

	    memset(&b, 0, sizeof(b));
	    dirbuf_add(req, &b, ".", 2);
	    dirbuf_add(req, &b, "..", 1);

	    ZFS_VOID_ENTER(zfsvfs);

	    cookie = 0;
	    do {
		error = dmu_snapshot_list_next(zfsvfs->z_os, MAXNAMELEN, snapname, &id,
			&cookie, &case_conflict);
		if (error) {
		    ZFS_EXIT(zfsvfs);
		    reply_buf_limited(req, b.p, b.size, off, size);
		    free(b.p);
		    return;
		}
		dirbuf_add(req, &b, snapname, ZFSCTL_INO_SNAP(id));
	    } while (error == 0);

	default:
	    // snapshot directory
	    fuse_reply_err(req,ENOTDIR);
    }
}

static void
// zfsctl_rename_snap(fsctl_snapdir_t *sdp, zfs_snapentry_t *sep, const char *nm)
zfsctl_rename_snap(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname)
{
	if (parent != 2 || newparent != 2) {
		fuse_reply_err(req,EINVAL);
		return;
	}

	if (strcmp(name, newname) == 0) {
	    fuse_reply_err(req,0);
	    return;
	}
	char osname[MAXNAMELEN];
	char snapname[MAXNAMELEN];
	char snapname2[MAXNAMELEN];
	char command[2048];
	zfsvfs_t *zfsvfs = (zfsvfs_t *) fuse_req_userdata(req);

	dmu_objset_name(zfsvfs->z_os, osname);
	cred_t cr;
	zfsfuse_getcred(req,&cr);
	int err = zfs_secpolicy_snapshot_perms(osname, &cr);
	if (err) {
	    fuse_reply_err(req,err);
	    return;
	}
	sprintf(snapname,"%s@%s",osname,name);
	sprintf(snapname2,"%s@%s",osname,newname);
	snprintf(command, 2048, "zfs rename %s %s",snapname,snapname2);
	int ret = system(command);
#ifdef DEBUG
	printf("%s returned %d\n",command,ret);
#endif
	fuse_reply_err(req,ret & 0xff);
}

static void
// zfsctl_snapdir_mkdir(vnode_t *dvp, char *dirname, vattr_t *vap, vnode_t  **vpp,
zfsctl_snapdir_mkdir(fuse_req_t req, fuse_ino_t parent, const char *dirname, mode_t mode)
{
    if (parent != 2) {
	fuse_reply_err(req,EINVAL);
	return;
    }

    zfsvfs_t *zfsvfs = (zfsvfs_t *) fuse_req_userdata(req);
    char name[MAXNAMELEN];
    int err;
    static enum symfollow follow = NO_FOLLOW;
    static enum uio_seg seg = UIO_SYSSPACE;
    char snapname[MAXNAMELEN];
    struct fuse_entry_param e = { 0 };
    cred_t cr;
    zfsfuse_getcred(req,&cr);

    e.attr_timeout = fuse_attr_timeout;
    e.entry_timeout = fuse_entry_timeout;

    e.generation = 1; // ???

    if (snapshot_namecheck(dirname, NULL, NULL) != 0) {
	fuse_reply_err (req,EILSEQ);
	return;
    }

    dmu_objset_name(zfsvfs->z_os, name);

    err = zfs_secpolicy_snapshot_perms(name, &cr);
    if (err) {
	fuse_reply_err(req,err);
	return;
    }

    if (err == 0) {
	err = dmu_objset_snapshot(name, (char*)dirname, NULL, B_FALSE);
	if (err) {
	    fuse_reply_err(req,err);
	    return;
	}
	// err = lookupnameat(dirname, seg, follow, NULL, vpp, dvp);
	sprintf(snapname,"%s@%s",name,dirname);
	objset_t *snap;
	if (dmu_objset_hold(snapname, FTAG, &snap) == 0) {
	    e.ino = dmu_objset_id(snap);
	    ctldir_stat(e.ino, &e.attr);

	    dmu_objset_rele(snap, FTAG);
	} else { // not found
	    fuse_reply_err(req, ENOENT);
	    return;
	}
	fuse_reply_entry(req,&e);

    }

}

static struct fuse_lowlevel_ops ctldir_oper = {
	.lookup		= ctldir_lookup,
	.getattr	= ctldir_getattr,
	.readdir	= ctldir_readdir,
	.rename		= zfsctl_rename_snap,
	.mkdir		= zfsctl_snapdir_mkdir,
};

typedef struct zfsctl_node {
	// gfs_dir_t	zc_gfs_private;
	uint64_t	zc_id;
	timestruc_t	zc_cmtime;	/* ctime and mtime, always the same */
} zfsctl_node_t;

typedef struct zfsctl_snapdir {
	zfsctl_node_t	sd_node;
	kmutex_t	sd_lock;
	avl_tree_t	sd_snaps;
} zfsctl_snapdir_t;

typedef struct {
	char		*se_name;
	vnode_t		*se_root;
	avl_node_t	se_node;
} zfs_snapentry_t;

static int
snapentry_compare(const void *a, const void *b)
{
	const zfs_snapentry_t *sa = a;
	const zfs_snapentry_t *sb = b;
	int ret = strcmp(sa->se_name, sb->se_name);

	if (ret < 0)
		return (-1);
	else if (ret > 0)
		return (1);
	else
		return (0);
}

vnodeops_t *zfsctl_ops_root;
vnodeops_t *zfsctl_ops_snapdir;
vnodeops_t *zfsctl_ops_snapshot;
vnodeops_t *zfsctl_ops_shares;
vnodeops_t *zfsctl_ops_shares_dir;

static const fs_operation_def_t zfsctl_tops_root[];
static const fs_operation_def_t zfsctl_tops_snapdir[];
static const fs_operation_def_t zfsctl_tops_snapshot[];
static const fs_operation_def_t zfsctl_tops_shares[];

static vnode_t *zfsctl_mknode_snapdir(vnode_t *);
static vnode_t *zfsctl_mknode_shares(vnode_t *);
static vnode_t *zfsctl_snapshot_mknode(vnode_t *, uint64_t objset);
static int zfsctl_unmount_snap(zfs_snapentry_t *, int, cred_t *);

#if 0
static gfs_opsvec_t zfsctl_opsvec[] = {
	{ ".zfs", zfsctl_tops_root, &zfsctl_ops_root },
	{ ".zfs/snapshot", zfsctl_tops_snapdir, &zfsctl_ops_snapdir },
	{ ".zfs/snapshot/vnode", zfsctl_tops_snapshot, &zfsctl_ops_snapshot },
	{ ".zfs/shares", zfsctl_tops_shares, &zfsctl_ops_shares_dir },
	{ ".zfs/shares/vnode", zfsctl_tops_shares, &zfsctl_ops_shares },
	{ NULL }
};

/*
 * Root directory elements.  We only have two entries
 * snapshot and shares.
 */
static gfs_dirent_t zfsctl_root_entries[] = {
	{ "snapshot", zfsctl_mknode_snapdir, GFS_CACHE_VNODE },
	{ "shares", zfsctl_mknode_shares, GFS_CACHE_VNODE },
	{ NULL }
};

/* include . and .. in the calculation */
#define	NROOT_ENTRIES	((sizeof (zfsctl_root_entries) / \
    sizeof (gfs_dirent_t)) + 1)
#endif


/*
 * Initialize the various GFS pieces we'll need to create and manipulate .zfs
 * directories.  This is called from the ZFS init routine, and initializes the
 * vnode ops vectors that we'll be using.
 */
void
zfsctl_init(void)
{
	// VERIFY(gfs_make_opsvec(zfsctl_opsvec) == 0);
}

void
zfsctl_fini(void)
{
	/*
	 * Remove vfsctl vnode ops
	 */
#if 0
	if (zfsctl_ops_root)
		vn_freevnodeops(zfsctl_ops_root);
	if (zfsctl_ops_snapdir)
		vn_freevnodeops(zfsctl_ops_snapdir);
	if (zfsctl_ops_snapshot)
		vn_freevnodeops(zfsctl_ops_snapshot);
	if (zfsctl_ops_shares)
		vn_freevnodeops(zfsctl_ops_shares);
	if (zfsctl_ops_shares_dir)
		vn_freevnodeops(zfsctl_ops_shares_dir);

	zfsctl_ops_root = NULL;
	zfsctl_ops_snapdir = NULL;
	zfsctl_ops_snapshot = NULL;
	zfsctl_ops_shares = NULL;
	zfsctl_ops_shares_dir = NULL;
#endif
}

/*
 * Return the inode number associated with the 'snapshot' or
 * 'shares' directory.
 */
/* ARGSUSED */
#if 0
static ino64_t
zfsctl_root_inode_cb(vnode_t *vp, int index)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;

	ASSERT(index <= 2);

	if (index == 0)
		return (ZFSCTL_INO_SNAPDIR);

	return (zfsvfs->z_shares_dir);
}
#endif

void fuse_setup_ctldir(zfsvfs_t *zfsvfs, const char *dir) {
    if (zfsvfs->z_show_ctldir) {
	char mntdir[PATH_MAX];
	sprintf(mntdir,"%s/.zfs",dir);
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	int err = -1;
	char my_arg[512];
	char osname[MAXNAMELEN];

	dmu_objset_name(zfsvfs->z_os, osname);
	sprintf(my_arg,"fsname=%s/.zfs,nonempty,allow_other",osname);
	if(fuse_opt_add_arg(&args, "") == -1 ||
	   fuse_opt_add_arg(&args, "-o") == -1 ||
	   fuse_opt_add_arg(&args, my_arg) == -1) {
		fuse_opt_free_args(&args);
		return;
	}

	struct fuse_chan *ch = fuse_mount(mntdir, &args);
	if (ch != NULL) {
		struct fuse_session *se;

		se = fuse_lowlevel_new(&args, &ctldir_oper,
				       sizeof(ctldir_oper), zfsvfs);
		if (se != NULL) {
		    fuse_session_add_chan(se, ch);
		    if(zfsfuse_newfs(mntdir, ch) != 0) {
			fuse_session_destroy(se);
			fuse_unmount(dir,ch);
			fuse_opt_free_args(&args);
			return;
		    }
		}
	}
	fuse_opt_free_args(&args);

	return;
    }
}

/*
 * Create the '.zfs' directory.  This directory is cached as part of the VFS
 * structure.  This results in a hold on the vfs_t.  The code in zfs_umount()
 * therefore checks against a vfs_count of 2 instead of 1.  This reference
 * is removed when the ctldir is destroyed in the unmount.
 */
void
zfsctl_create(zfsvfs_t *zfsvfs)
{
    char osname[MAXNAMELEN];
    dmu_objset_name(zfsvfs->z_os, osname);
    vnode_t *vp = NULL;
    zfsctl_node_t *zcp;

    if (!zfsvfs->z_ctldir) {
	/* We will create .zfs *ALWAYS* when the dataset is mounted and keep
	 * it locked (so that even if you run an rmdir on it, the vnode will
	 * still be there). The reason for that is that if we create it when
	 * z_show_snapdir changes, and if a thread is busy with a buffered
	 * write operation, then it triggers a assertion failure !
	 * So the safest solution is to create it here and keep it available
	 * for later */

	ZFS_VOID_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, 3, &znode, B_FALSE);
	if(error) {
	    ZFS_EXIT(zfsvfs);
	    return;
	}

	ASSERT(znode != NULL);
	vnode_t *dvp = ZTOV(znode);
	ASSERT(dvp != NULL);

	error = VOP_LOOKUP(dvp, ".zfs", &vp, NULL, 0, NULL, kcred, NULL, NULL, NULL);
	if (vp == NULL) {
	    // create it
	    vattr_t vattr = { 0 };
	    vattr.va_type = VDIR;
	    vattr.va_mode = 0555 & PERMMASK;
	    vattr.va_mask = AT_TYPE | AT_MODE;

	    error = VOP_MKDIR(dvp, ".zfs", &vattr, &vp, kcred, NULL, 0, NULL);
	    if (error) {
		zfsvfs->z_show_ctldir = B_FALSE;
	    }
	}
	zfsvfs->z_ctldir = vp;
	// vp can't be released here
	// if (vp) VN_RELE(vp);
	VN_RELE(dvp);
	ZFS_EXIT(zfsvfs);
    }

    char mt[PATH_MAX];
    if (get_mtpoint(zfsvfs,mt,PATH_MAX) == 0) 
	fuse_setup_ctldir(zfsvfs,mt);
}
	
/*
 * Destroy the '.zfs' directory.  Only called when the filesystem is unmounted.
 * There might still be more references if we were force unmounted, but only
 * new zfs_inactive() calls can occur and they don't reference .zfs
 */
void
zfsctl_destroy(zfsvfs_t *zfsvfs)
{
    printf("zfsctl_destroy\n");
    VN_RELE(zfsvfs->z_ctldir);
    zfsvfs->z_ctldir = NULL;
}

/*
 * Given a root znode, retrieve the associated .zfs directory.
 * Add a hold to the vnode and return it.
 */
vnode_t *
zfsctl_root(znode_t *zp)
{
    printf("in root\n");
	ASSERT(zfs_has_ctldir(zp));
	VN_HOLD(zp->z_zfsvfs->z_ctldir);
	return (zp->z_zfsvfs->z_ctldir);
}
#if 0

/*
 * Common open routine.  Disallow any write access.
 */
/* ARGSUSED */
static int
zfsctl_common_open(vnode_t **vpp, int flags, cred_t *cr, caller_context_t *ct)
{
	if (flags & FWRITE)
		return (EACCES);

	return (0);
}

/*
 * Common close routine.  Nothing to do here.
 */
/* ARGSUSED */
static int
zfsctl_common_close(vnode_t *vpp, int flags, int count, offset_t off,
    cred_t *cr, caller_context_t *ct)
{
	return (0);
}

/*
 * Common access routine.  Disallow writes.
 */
/* ARGSUSED */
static int
zfsctl_common_access(vnode_t *vp, int mode, int flags, cred_t *cr,
    caller_context_t *ct)
{
	if (flags & V_ACE_MASK) {
		if (mode & ACE_ALL_WRITE_PERMS)
			return (EACCES);
	} else {
		if (mode & VWRITE)
			return (EACCES);
	}

	return (0);
}

/*
 * Common getattr function.  Fill in basic information.
 */
static void
zfsctl_common_getattr(vnode_t *vp, vattr_t *vap)
{
	timestruc_t	now;

	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_rdev = 0;
	/*
	 * We are a purely virtual object, so we have no
	 * blocksize or allocated blocks.
	 */
	vap->va_blksize = 0;
	vap->va_nblocks = 0;
	vap->va_seq = 0;
	vap->va_fsid = vp->v_vfsp->vfs_dev;
	vap->va_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP |
	    S_IROTH | S_IXOTH;
	vap->va_type = VDIR;
	/*
	 * We live in the now (for atime).
	 */
	gethrestime(&now);
	vap->va_atime = now;
}

/*ARGSUSED*/
static int
zfsctl_common_fid(vnode_t *vp, fid_t *fidp, caller_context_t *ct)
{
	zfsvfs_t	*zfsvfs = vp->v_vfsp->vfs_data;
	zfsctl_node_t	*zcp = vp->v_data;
	uint64_t	object = zcp->zc_id;
	zfid_short_t	*zfid;
	int		i;

	ZFS_ENTER(zfsvfs);

	if (fidp->fid_len < SHORT_FID_LEN) {
		fidp->fid_len = SHORT_FID_LEN;
		ZFS_EXIT(zfsvfs);
		return (ENOSPC);
	}

	zfid = (zfid_short_t *)fidp;

	zfid->zf_len = SHORT_FID_LEN;

	for (i = 0; i < sizeof (zfid->zf_object); i++)
		zfid->zf_object[i] = (uint8_t)(object >> (8 * i));

	/* .zfs znodes always have a generation number of 0 */
	for (i = 0; i < sizeof (zfid->zf_gen); i++)
		zfid->zf_gen[i] = 0;

	ZFS_EXIT(zfsvfs);
	return (0);
}


/*ARGSUSED*/
static int
zfsctl_shares_fid(vnode_t *vp, fid_t *fidp, caller_context_t *ct)
{
	zfsvfs_t	*zfsvfs = vp->v_vfsp->vfs_data;
	znode_t		*dzp;
	int		error;

	ZFS_ENTER(zfsvfs);

	if (zfsvfs->z_shares_dir == 0) {
		ZFS_EXIT(zfsvfs);
		return (ENOTSUP);
	}

	if ((error = zfs_zget(zfsvfs, zfsvfs->z_shares_dir, &dzp)) == 0) {
		error = VOP_FID(ZTOV(dzp), fidp, ct);
		VN_RELE(ZTOV(dzp));
	}

	ZFS_EXIT(zfsvfs);
	return (error);
}

/*
 * Get root directory attributes.
 */
/* ARGSUSED */
static int
zfsctl_root_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr,
    caller_context_t *ct)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	zfsctl_node_t *zcp = vp->v_data;

	ZFS_ENTER(zfsvfs);
	vap->va_nodeid = ZFSCTL_INO_ROOT;
	vap->va_nlink = vap->va_size = NROOT_ENTRIES;
	vap->va_mtime = vap->va_ctime = zcp->zc_cmtime;

	zfsctl_common_getattr(vp, vap);
	ZFS_EXIT(zfsvfs);

	return (0);
}

/*
 * Special case the handling of "..".
 */
/* ARGSUSED */
int
zfsctl_root_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, pathname_t *pnp,
    int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
    int *direntflags, pathname_t *realpnp)
{
	zfsvfs_t *zfsvfs = dvp->v_vfsp->vfs_data;
	int err;

	/*
	 * No extended attributes allowed under .zfs
	 */
	if (flags & LOOKUP_XATTR)
		return (EINVAL);

	ZFS_ENTER(zfsvfs);

	if (strcmp(nm, "..") == 0) {
		err = VFS_ROOT(dvp->v_vfsp, vpp);
	} else {
		err = gfs_vop_lookup(dvp, nm, vpp, pnp, flags, rdir,
		    cr, ct, direntflags, realpnp);
	}

	ZFS_EXIT(zfsvfs);

	return (err);
}

static int
zfsctl_pathconf(vnode_t *vp, int cmd, ulong_t *valp, cred_t *cr,
    caller_context_t *ct)
{
	/*
	 * We only care about ACL_ENABLED so that libsec can
	 * display ACL correctly and not default to POSIX draft.
	 */
	if (cmd == _PC_ACL_ENABLED) {
		*valp = _ACL_ACE_ENABLED;
		return (0);
	}

	return (fs_pathconf(vp, cmd, valp, cr, ct));
}

static const fs_operation_def_t zfsctl_tops_root[] = {
	{ VOPNAME_OPEN,		{ .vop_open = zfsctl_common_open }	},
	{ VOPNAME_CLOSE,	{ .vop_close = zfsctl_common_close }	},
	{ VOPNAME_IOCTL,	{ .error = fs_inval }			},
	{ VOPNAME_GETATTR,	{ .vop_getattr = zfsctl_root_getattr }	},
	{ VOPNAME_ACCESS,	{ .vop_access = zfsctl_common_access }	},
	{ VOPNAME_READDIR,	{ .vop_readdir = gfs_vop_readdir } 	},
	{ VOPNAME_LOOKUP,	{ .vop_lookup = zfsctl_root_lookup }	},
	{ VOPNAME_SEEK,		{ .vop_seek = fs_seek }			},
	{ VOPNAME_INACTIVE,	{ .vop_inactive = gfs_vop_inactive }	},
	{ VOPNAME_PATHCONF,	{ .vop_pathconf = zfsctl_pathconf }	},
	{ VOPNAME_FID,		{ .vop_fid = zfsctl_common_fid	}	},
	{ NULL }
};

static int
zfsctl_snapshot_zname(vnode_t *vp, const char *name, int len, char *zname)
{
	objset_t *os = ((zfsvfs_t *)((vp)->v_vfsp->vfs_data))->z_os;

	if (snapshot_namecheck(name, NULL, NULL) != 0)
		return (EILSEQ);
	dmu_objset_name(os, zname);
	if (strlen(zname) + 1 + strlen(name) >= len)
		return (ENAMETOOLONG);
	(void) strcat(zname, "@");
	(void) strcat(zname, name);
	return (0);
}

static int
zfsctl_unmount_snap(zfs_snapentry_t *sep, int fflags, cred_t *cr)
{
	vnode_t *svp = sep->se_root;
	int error;

	ASSERT(vn_ismntpt(svp));

	/* this will be dropped by dounmount() */
	if ((error = vn_vfswlock(svp)) != 0)
		return (error);

	VN_HOLD(svp);
	error = dounmount(vn_mountedvfs(svp), fflags, cr);
	if (error) {
		VN_RELE(svp);
		return (error);
	}

	/*
	 * We can't use VN_RELE(), as that will try to invoke
	 * zfsctl_snapdir_inactive(), which would cause us to destroy
	 * the sd_lock mutex held by our caller.
	 */
	ASSERT(svp->v_count == 1);
	gfs_vop_inactive(svp, cr, NULL);

	kmem_free(sep->se_name, strlen(sep->se_name) + 1);
	kmem_free(sep, sizeof (zfs_snapentry_t));

	return (0);
}

static void
/*ARGSUSED*/
static int
zfsctl_snapdir_rename(vnode_t *sdvp, char *snm, vnode_t *tdvp, char *tnm,
    cred_t *cr, caller_context_t *ct, int flags)
{
	zfsctl_snapdir_t *sdp = sdvp->v_data;
	zfs_snapentry_t search, *sep;
	zfsvfs_t *zfsvfs;
	avl_index_t where;
	char from[MAXNAMELEN], to[MAXNAMELEN];
	char real[MAXNAMELEN];
	int err;

	zfsvfs = sdvp->v_vfsp->vfs_data;
	ZFS_ENTER(zfsvfs);

	if ((flags & FIGNORECASE) || zfsvfs->z_case == ZFS_CASE_INSENSITIVE) {
		err = dmu_snapshot_realname(zfsvfs->z_os, snm, real,
		    MAXNAMELEN, NULL);
		if (err == 0) {
			snm = real;
		} else if (err != ENOTSUP) {
			ZFS_EXIT(zfsvfs);
			return (err);
		}
	}

	ZFS_EXIT(zfsvfs);

	err = zfsctl_snapshot_zname(sdvp, snm, MAXNAMELEN, from);
	if (!err)
		err = zfsctl_snapshot_zname(tdvp, tnm, MAXNAMELEN, to);
	if (!err)
		err = zfs_secpolicy_rename_perms(from, to, cr);
	if (err)
		return (err);

	/*
	 * Cannot move snapshots out of the snapdir.
	 */
	if (sdvp != tdvp)
		return (EINVAL);

	if (strcmp(snm, tnm) == 0)
		return (0);

	mutex_enter(&sdp->sd_lock);

	search.se_name = (char *)snm;
	if ((sep = avl_find(&sdp->sd_snaps, &search, &where)) == NULL) {
		mutex_exit(&sdp->sd_lock);
		return (ENOENT);
	}

	err = dmu_objset_rename(from, to, B_FALSE);
	if (err == 0)
		zfsctl_rename_snap(sdp, sep, tnm);

	mutex_exit(&sdp->sd_lock);

	return (err);
}

/* ARGSUSED */
static int
zfsctl_snapdir_remove(vnode_t *dvp, char *name, vnode_t *cwd, cred_t *cr,
    caller_context_t *ct, int flags)
{
	zfsctl_snapdir_t *sdp = dvp->v_data;
	zfs_snapentry_t *sep;
	zfs_snapentry_t search;
	zfsvfs_t *zfsvfs;
	char snapname[MAXNAMELEN];
	char real[MAXNAMELEN];
	int err;

	zfsvfs = dvp->v_vfsp->vfs_data;
	ZFS_ENTER(zfsvfs);

	if ((flags & FIGNORECASE) || zfsvfs->z_case == ZFS_CASE_INSENSITIVE) {

		err = dmu_snapshot_realname(zfsvfs->z_os, name, real,
		    MAXNAMELEN, NULL);
		if (err == 0) {
			name = real;
		} else if (err != ENOTSUP) {
			ZFS_EXIT(zfsvfs);
			return (err);
		}
	}

	ZFS_EXIT(zfsvfs);

	err = zfsctl_snapshot_zname(dvp, name, MAXNAMELEN, snapname);
	if (!err)
		err = zfs_secpolicy_destroy_perms(snapname, cr);
	if (err)
		return (err);

	mutex_enter(&sdp->sd_lock);

	search.se_name = name;
	sep = avl_find(&sdp->sd_snaps, &search, NULL);
	if (sep) {
		avl_remove(&sdp->sd_snaps, sep);
		err = zfsctl_unmount_snap(sep, MS_FORCE, cr);
		if (err)
			avl_add(&sdp->sd_snaps, sep);
		else
			err = dmu_objset_destroy(snapname, B_FALSE);
	} else {
		err = ENOENT;
	}

	mutex_exit(&sdp->sd_lock);

	return (err);
}

/*
 * This creates a snapshot under '.zfs/snapshot'.
 */
/* ARGSUSED */
static void
zfsctl_snapdir_mkdir(vnode_t *dvp, const char *dirname, vattr_t *vap, vnode_t  **vpp,
    cred_t *cr, caller_context_t *cc, int flags, vsecattr_t *vsecp)
{
	zfsvfs_t *zfsvfs = dvp->v_vfsp->vfs_data;
	char name[MAXNAMELEN];
	int err;
	static enum symfollow follow = NO_FOLLOW;
	static enum uio_seg seg = UIO_SYSSPACE;

	if (snapshot_namecheck(dirname, NULL, NULL) != 0)
		return (EILSEQ);

	dmu_objset_name(zfsvfs->z_os, name);

	*vpp = NULL;

	err = zfs_secpolicy_snapshot_perms(name, cr);
	if (err)
		return (err);

	if (err == 0) {
		err = dmu_objset_snapshot(name, dirname, NULL, B_FALSE);
		if (err)
			return (err);
		err = lookupnameat(dirname, seg, follow, NULL, vpp, dvp);
	}

	return (err);
}

/*
 * Lookup entry point for the 'snapshot' directory.  Try to open the
 * snapshot if it exist, creating the pseudo filesystem vnode as necessary.
 * Perform a mount of the associated dataset on top of the vnode.
 */
/* ARGSUSED */
static int
zfsctl_snapdir_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, pathname_t *pnp,
    int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
    int *direntflags, pathname_t *realpnp)
{
	zfsctl_snapdir_t *sdp = dvp->v_data;
	objset_t *snap;
	char snapname[MAXNAMELEN];
	char real[MAXNAMELEN];
	char *mountpoint;
	zfs_snapentry_t *sep, search;
	struct mounta margs;
	vfs_t *vfsp;
	size_t mountpoint_len;
	avl_index_t where;
	zfsvfs_t *zfsvfs = dvp->v_vfsp->vfs_data;
	int err;

	/*
	 * No extended attributes allowed under .zfs
	 */
	if (flags & LOOKUP_XATTR)
		return (EINVAL);

	ASSERT(dvp->v_type == VDIR);

	/*
	 * If we get a recursive call, that means we got called
	 * from the domount() code while it was trying to look up the
	 * spec (which looks like a local path for zfs).  We need to
	 * add some flag to domount() to tell it not to do this lookup.
	 */
	if (MUTEX_HELD(&sdp->sd_lock))
		return (ENOENT);

	ZFS_ENTER(zfsvfs);

	if (gfs_lookup_dot(vpp, dvp, zfsvfs->z_ctldir, nm) == 0) {
		ZFS_EXIT(zfsvfs);
		return (0);
	}

	if (flags & FIGNORECASE) {
		boolean_t conflict = B_FALSE;

		err = dmu_snapshot_realname(zfsvfs->z_os, nm, real,
		    MAXNAMELEN, &conflict);
		if (err == 0) {
			nm = real;
		} else if (err != ENOTSUP) {
			ZFS_EXIT(zfsvfs);
			return (err);
		}
		if (realpnp)
			(void) strlcpy(realpnp->pn_buf, nm,
			    realpnp->pn_bufsize);
		if (conflict && direntflags)
			*direntflags = ED_CASE_CONFLICT;
	}

	mutex_enter(&sdp->sd_lock);
	search.se_name = (char *)nm;
	if ((sep = avl_find(&sdp->sd_snaps, &search, &where)) != NULL) {
		*vpp = sep->se_root;
		VN_HOLD(*vpp);
		err = traverse(vpp);
		if (err) {
			VN_RELE(*vpp);
			*vpp = NULL;
		} else if (*vpp == sep->se_root) {
			/*
			 * The snapshot was unmounted behind our backs,
			 * try to remount it.
			 */
			goto domount;
		} else {
			/*
			 * VROOT was set during the traverse call.  We need
			 * to clear it since we're pretending to be part
			 * of our parent's vfs.
			 */
			(*vpp)->v_flag &= ~VROOT;
		}
		mutex_exit(&sdp->sd_lock);
		ZFS_EXIT(zfsvfs);
		return (err);
	}

	/*
	 * The requested snapshot is not currently mounted, look it up.
	 */
	err = zfsctl_snapshot_zname(dvp, nm, MAXNAMELEN, snapname);
	if (err) {
		mutex_exit(&sdp->sd_lock);
		ZFS_EXIT(zfsvfs);
		/*
		 * handle "ls *" or "?" in a graceful manner,
		 * forcing EILSEQ to ENOENT.
		 * Since shell ultimately passes "*" or "?" as name to lookup
		 */
		return (err == EILSEQ ? ENOENT : err);
	}
	if (dmu_objset_hold(snapname, FTAG, &snap) != 0) {
		mutex_exit(&sdp->sd_lock);
		ZFS_EXIT(zfsvfs);
		return (ENOENT);
	}

	sep = kmem_alloc(sizeof (zfs_snapentry_t), KM_SLEEP);
	sep->se_name = kmem_alloc(strlen(nm) + 1, KM_SLEEP);
	(void) strcpy(sep->se_name, nm);
	*vpp = sep->se_root = zfsctl_snapshot_mknode(dvp, dmu_objset_id(snap));
	avl_insert(&sdp->sd_snaps, sep, where);

	dmu_objset_rele(snap, FTAG);
domount:
	mountpoint_len = strlen(refstr_value(dvp->v_vfsp->vfs_mntpt)) +
	    strlen("/.zfs/snapshot/") + strlen(nm) + 1;
	mountpoint = kmem_alloc(mountpoint_len, KM_SLEEP);
	(void) snprintf(mountpoint, mountpoint_len, "%s/.zfs/snapshot/%s",
	    refstr_value(dvp->v_vfsp->vfs_mntpt), nm);

	margs.spec = snapname;
	margs.dir = mountpoint;
	margs.flags = MS_SYSSPACE | MS_NOMNTTAB;
	margs.fstype = "zfs";
	margs.dataptr = NULL;
	margs.datalen = 0;
	margs.optptr = NULL;
	margs.optlen = 0;

	err = domount("zfs", &margs, *vpp, kcred, &vfsp);
	kmem_free(mountpoint, mountpoint_len);

	if (err == 0) {
		/*
		 * Return the mounted root rather than the covered mount point.
		 * Takes the GFS vnode at .zfs/snapshot/<snapname> and returns
		 * the ZFS vnode mounted on top of the GFS node.  This ZFS
		 * vnode is the root of the newly created vfsp.
		 */
		VFS_RELE(vfsp);
		err = traverse(vpp);
	}

	if (err == 0) {
		/*
		 * Fix up the root vnode mounted on .zfs/snapshot/<snapname>.
		 *
		 * This is where we lie about our v_vfsp in order to
		 * make .zfs/snapshot/<snapname> accessible over NFS
		 * without requiring manual mounts of <snapname>.
		 */
		ASSERT(VTOZ(*vpp)->z_zfsvfs != zfsvfs);
		VTOZ(*vpp)->z_zfsvfs->z_parent = zfsvfs;
		(*vpp)->v_vfsp = zfsvfs->z_vfs;
		(*vpp)->v_flag &= ~VROOT;
	}
	mutex_exit(&sdp->sd_lock);
	ZFS_EXIT(zfsvfs);

	/*
	 * If we had an error, drop our hold on the vnode and
	 * zfsctl_snapshot_inactive() will clean up.
	 */
	if (err) {
		VN_RELE(*vpp);
		*vpp = NULL;
	}
	return (err);
}

/* ARGSUSED */
static int
zfsctl_shares_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, pathname_t *pnp,
    int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
    int *direntflags, pathname_t *realpnp)
{
	zfsvfs_t *zfsvfs = dvp->v_vfsp->vfs_data;
	znode_t *dzp;
	int error;

	ZFS_ENTER(zfsvfs);

	if (gfs_lookup_dot(vpp, dvp, zfsvfs->z_ctldir, nm) == 0) {
		ZFS_EXIT(zfsvfs);
		return (0);
	}

	if (zfsvfs->z_shares_dir == 0) {
		ZFS_EXIT(zfsvfs);
		return (ENOTSUP);
	}
	if ((error = zfs_zget(zfsvfs, zfsvfs->z_shares_dir, &dzp)) == 0)
		error = VOP_LOOKUP(ZTOV(dzp), nm, vpp, pnp,
		    flags, rdir, cr, ct, direntflags, realpnp);

	VN_RELE(ZTOV(dzp));
	ZFS_EXIT(zfsvfs);

	return (error);
}

/* ARGSUSED */
static int
zfsctl_snapdir_readdir_cb(vnode_t *vp, void *dp, int *eofp,
    offset_t *offp, offset_t *nextp, void *data, int flags)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	char snapname[MAXNAMELEN];
	uint64_t id, cookie;
	boolean_t case_conflict;
	int error;

	ZFS_ENTER(zfsvfs);

	cookie = *offp;
	error = dmu_snapshot_list_next(zfsvfs->z_os, MAXNAMELEN, snapname, &id,
	    &cookie, &case_conflict);
	if (error) {
		ZFS_EXIT(zfsvfs);
		if (error == ENOENT) {
			*eofp = 1;
			return (0);
		}
		return (error);
	}

	if (flags & V_RDDIR_ENTFLAGS) {
		edirent_t *eodp = dp;

		(void) strcpy(eodp->ed_name, snapname);
		eodp->ed_ino = ZFSCTL_INO_SNAP(id);
		eodp->ed_eflags = case_conflict ? ED_CASE_CONFLICT : 0;
	} else {
		struct dirent64 *odp = dp;

		(void) strcpy(odp->d_name, snapname);
		odp->d_ino = ZFSCTL_INO_SNAP(id);
	}
	*nextp = cookie;

	ZFS_EXIT(zfsvfs);

	return (0);
}

/* ARGSUSED */
static int
zfsctl_shares_readdir(vnode_t *vp, uio_t *uiop, cred_t *cr, int *eofp,
    caller_context_t *ct, int flags)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	znode_t *dzp;
	int error;

	ZFS_ENTER(zfsvfs);

	if (zfsvfs->z_shares_dir == 0) {
		ZFS_EXIT(zfsvfs);
		return (ENOTSUP);
	}
	if ((error = zfs_zget(zfsvfs, zfsvfs->z_shares_dir, &dzp)) == 0) {
		error = VOP_READDIR(ZTOV(dzp), uiop, cr, eofp, ct, flags);
		VN_RELE(ZTOV(dzp));
	} else {
		*eofp = 1;
		error = ENOENT;
	}

	ZFS_EXIT(zfsvfs);
	return (error);
}

/*
 * pvp is the '.zfs' directory (zfsctl_node_t).
 * Creates vp, which is '.zfs/snapshot' (zfsctl_snapdir_t).
 *
 * This function is the callback to create a GFS vnode for '.zfs/snapshot'
 * when a lookup is performed on .zfs for "snapshot".
 */
vnode_t *
zfsctl_mknode_snapdir(vnode_t *pvp)
{
	vnode_t *vp;
	zfsctl_snapdir_t *sdp;

	vp = gfs_dir_create(sizeof (zfsctl_snapdir_t), pvp,
	    zfsctl_ops_snapdir, NULL, NULL, MAXNAMELEN,
	    zfsctl_snapdir_readdir_cb, NULL);
	sdp = vp->v_data;
	sdp->sd_node.zc_id = ZFSCTL_INO_SNAPDIR;
	sdp->sd_node.zc_cmtime = ((zfsctl_node_t *)pvp->v_data)->zc_cmtime;
	mutex_init(&sdp->sd_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&sdp->sd_snaps, snapentry_compare,
	    sizeof (zfs_snapentry_t), offsetof(zfs_snapentry_t, se_node));
	return (vp);
}

vnode_t *
zfsctl_mknode_shares(vnode_t *pvp)
{
	vnode_t *vp;
	zfsctl_node_t *sdp;

	vp = gfs_dir_create(sizeof (zfsctl_node_t), pvp,
	    zfsctl_ops_shares, NULL, NULL, MAXNAMELEN,
	    NULL, NULL);
	sdp = vp->v_data;
	sdp->zc_cmtime = ((zfsctl_node_t *)pvp->v_data)->zc_cmtime;
	return (vp);

}

/* ARGSUSED */
static int
zfsctl_shares_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr,
    caller_context_t *ct)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	znode_t *dzp;
	int error;

	ZFS_ENTER(zfsvfs);
	if (zfsvfs->z_shares_dir == 0) {
		ZFS_EXIT(zfsvfs);
		return (ENOTSUP);
	}
	if ((error = zfs_zget(zfsvfs, zfsvfs->z_shares_dir, &dzp)) == 0) {
		error = VOP_GETATTR(ZTOV(dzp), vap, flags, cr, ct);
		VN_RELE(ZTOV(dzp));
	}
	ZFS_EXIT(zfsvfs);
	return (error);


}

/* ARGSUSED */
static int
zfsctl_snapdir_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr,
    caller_context_t *ct)
{
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	zfsctl_snapdir_t *sdp = vp->v_data;

	ZFS_ENTER(zfsvfs);
	zfsctl_common_getattr(vp, vap);
	vap->va_nodeid = gfs_file_inode(vp);
	vap->va_nlink = vap->va_size = avl_numnodes(&sdp->sd_snaps) + 2;
	vap->va_ctime = vap->va_mtime = dmu_objset_snap_cmtime(zfsvfs->z_os);
	ZFS_EXIT(zfsvfs);

	return (0);
}

/* ARGSUSED */
static void
zfsctl_snapdir_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	zfsctl_snapdir_t *sdp = vp->v_data;
	void *private;

	private = gfs_dir_inactive(vp);
	if (private != NULL) {
		ASSERT(avl_numnodes(&sdp->sd_snaps) == 0);
		mutex_destroy(&sdp->sd_lock);
		avl_destroy(&sdp->sd_snaps);
		kmem_free(private, sizeof (zfsctl_snapdir_t));
	}
}

static const fs_operation_def_t zfsctl_tops_snapdir[] = {
	{ VOPNAME_OPEN,		{ .vop_open = zfsctl_common_open }	},
	{ VOPNAME_CLOSE,	{ .vop_close = zfsctl_common_close }	},
	{ VOPNAME_IOCTL,	{ .error = fs_inval }			},
	{ VOPNAME_GETATTR,	{ .vop_getattr = zfsctl_snapdir_getattr } },
	{ VOPNAME_ACCESS,	{ .vop_access = zfsctl_common_access }	},
	{ VOPNAME_RENAME,	{ .vop_rename = zfsctl_snapdir_rename }	},
	{ VOPNAME_RMDIR,	{ .vop_rmdir = zfsctl_snapdir_remove }	},
	{ VOPNAME_MKDIR,	{ .vop_mkdir = zfsctl_snapdir_mkdir }	},
	{ VOPNAME_READDIR,	{ .vop_readdir = gfs_vop_readdir }	},
	{ VOPNAME_LOOKUP,	{ .vop_lookup = zfsctl_snapdir_lookup }	},
	{ VOPNAME_SEEK,		{ .vop_seek = fs_seek }			},
	{ VOPNAME_INACTIVE,	{ .vop_inactive = zfsctl_snapdir_inactive } },
	{ VOPNAME_FID,		{ .vop_fid = zfsctl_common_fid }	},
	{ NULL }
};

static const fs_operation_def_t zfsctl_tops_shares[] = {
	{ VOPNAME_OPEN,		{ .vop_open = zfsctl_common_open }	},
	{ VOPNAME_CLOSE,	{ .vop_close = zfsctl_common_close }	},
	{ VOPNAME_IOCTL,	{ .error = fs_inval }			},
	{ VOPNAME_GETATTR,	{ .vop_getattr = zfsctl_shares_getattr } },
	{ VOPNAME_ACCESS,	{ .vop_access = zfsctl_common_access }	},
	{ VOPNAME_READDIR,	{ .vop_readdir = zfsctl_shares_readdir } },
	{ VOPNAME_LOOKUP,	{ .vop_lookup = zfsctl_shares_lookup }	},
	{ VOPNAME_SEEK,		{ .vop_seek = fs_seek }			},
	{ VOPNAME_INACTIVE,	{ .vop_inactive = gfs_vop_inactive } },
	{ VOPNAME_FID,		{ .vop_fid = zfsctl_shares_fid } },
	{ NULL }
};

/*
 * pvp is the GFS vnode '.zfs/snapshot'.
 *
 * This creates a GFS node under '.zfs/snapshot' representing each
 * snapshot.  This newly created GFS node is what we mount snapshot
 * vfs_t's ontop of.
 */
static vnode_t *
zfsctl_snapshot_mknode(vnode_t *pvp, uint64_t objset)
{
	vnode_t *vp;
	zfsctl_node_t *zcp;

	vp = gfs_dir_create(sizeof (zfsctl_node_t), pvp,
	    zfsctl_ops_snapshot, NULL, NULL, MAXNAMELEN, NULL, NULL);
	zcp = vp->v_data;
	zcp->zc_id = objset;

	return (vp);
}

static void
zfsctl_snapshot_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	zfsctl_snapdir_t *sdp;
	zfs_snapentry_t *sep, *next;
	vnode_t *dvp;

	VERIFY(gfs_dir_lookup(vp, "..", &dvp, cr, 0, NULL, NULL) == 0);
	sdp = dvp->v_data;

	mutex_enter(&sdp->sd_lock);

	if (vp->v_count > 1) {
		mutex_exit(&sdp->sd_lock);
		return;
	}
	ASSERT(!vn_ismntpt(vp));

	sep = avl_first(&sdp->sd_snaps);
	while (sep != NULL) {
		next = AVL_NEXT(&sdp->sd_snaps, sep);

		if (sep->se_root == vp) {
			avl_remove(&sdp->sd_snaps, sep);
			kmem_free(sep->se_name, strlen(sep->se_name) + 1);
			kmem_free(sep, sizeof (zfs_snapentry_t));
			break;
		}
		sep = next;
	}
	ASSERT(sep != NULL);

	mutex_exit(&sdp->sd_lock);
	VN_RELE(dvp);

	/*
	 * Dispose of the vnode for the snapshot mount point.
	 * This is safe to do because once this entry has been removed
	 * from the AVL tree, it can't be found again, so cannot become
	 * "active".  If we lookup the same name again we will end up
	 * creating a new vnode.
	 */
	gfs_vop_inactive(vp, cr, ct);
}


/*
 * These VP's should never see the light of day.  They should always
 * be covered.
 */
static const fs_operation_def_t zfsctl_tops_snapshot[] = {
	VOPNAME_INACTIVE, { .vop_inactive =  zfsctl_snapshot_inactive },
	NULL, NULL
};

int
zfsctl_lookup_objset(vfs_t *vfsp, uint64_t objsetid, zfsvfs_t **zfsvfsp)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;
	vnode_t *dvp, *vp;
	zfsctl_snapdir_t *sdp;
	zfsctl_node_t *zcp;
	zfs_snapentry_t *sep;
	int error;

	ASSERT(zfsvfs->z_ctldir != NULL);
	error = zfsctl_root_lookup(zfsvfs->z_ctldir, "snapshot", &dvp,
	    NULL, 0, NULL, kcred, NULL, NULL, NULL);
	if (error != 0)
		return (error);
	sdp = dvp->v_data;

	mutex_enter(&sdp->sd_lock);
	sep = avl_first(&sdp->sd_snaps);
	while (sep != NULL) {
		vp = sep->se_root;
		zcp = vp->v_data;
		if (zcp->zc_id == objsetid)
			break;

		sep = AVL_NEXT(&sdp->sd_snaps, sep);
	}

	if (sep != NULL) {
		VN_HOLD(vp);
		/*
		 * Return the mounted root rather than the covered mount point.
		 * Takes the GFS vnode at .zfs/snapshot/<snapshot objsetid>
		 * and returns the ZFS vnode mounted on top of the GFS node.
		 * This ZFS vnode is the root of the vfs for objset 'objsetid'.
		 */
		error = traverse(&vp);
		if (error == 0) {
			if (vp == sep->se_root)
				error = EINVAL;
			else
				*zfsvfsp = VTOZ(vp)->z_zfsvfs;
		}
		mutex_exit(&sdp->sd_lock);
		VN_RELE(vp);
	} else {
		error = EINVAL;
		mutex_exit(&sdp->sd_lock);
	}

	VN_RELE(dvp);

	return (error);
}
#endif

/*
 * Unmount any snapshots for the given filesystem.  This is called from
 * zfs_umount() - if we have a ctldir, then go through and unmount all the
 * snapshots.
 */
int
zfsctl_umount_snapshots(vfs_t *vfsp, int fflags, cred_t *cr)
{
    /* Just unmount .zfs for now (no mounting handled yet) */
    char dir[PATH_MAX+7];
    zfsvfs_t *zfsvfs = vfsp->vfs_data;

    /* Problem : with the umount or umount2 system call, the call is successful
     * but the filesystem still shows in /proc/mounts even if you can't access
     * it anymore.
     * There are 2 solutions : calling the command umount, not the function
     * in this case you can pass the source instead of the target and it works
     * or try to address fuse directly... 
     * Well calling fuse functions directly create a thread contention problem
     * because we are executing a fuse command here, so the only solution left
     * is to do like the zfs command, call the command umount... */
    printf("umount snapshots\n");
    destroy_snaps = 1;
    char osname[MAXNAMELEN];

    dmu_objset_name(zfsvfs->z_os, osname);
    char base[4096];
    sprintf(base,"%s/snap_",osname);
    int len = strlen(base);
    for (int n=0; n<used_snap; n++) {
	if (!strncmp(temp_snap[n],base,len)) {
	    char cmd[4096];
	    snprintf(cmd,4096,"zfs destroy %s",temp_snap[n]);
	    printf("cmd to destroy snap : %s\n",cmd);
	    int ret = system(cmd);
	    printf("cmd %s returned %d\n",cmd,ret);
	    free(temp_snap[n]);
	    if (n < used_snap-1)
		memmove(&temp_snap[n],&temp_snap[n+1],(used_snap-n-1)*sizeof(char *));
	    used_snap--;
	    n--;
	}
    }
    if (used_snap == 0 && temp_snap) {
	free(temp_snap);
	temp_snap = NULL;
    }
    destroy_snaps = 0;
    return 0;
}

