/*
 * Copyright (c) 1998-2014 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2014 Stony Brook University
 * Copyright (c) 2003-2014 The Research Foundation of SUNY
 * Copyright (C) 2013-2014 Motorola Mobility, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "esdfs.h"

#ifndef LOOKUP_NOCASE
#define LOOKUP_NOCASE	0
#endif

struct esdfs_ci_getdents_callback {
	struct dir_context ctx;
	const char *name;
	char match_name[NAME_MAX+1];
	int found; /*-1: not found, 0: found*/
	int count;
};

/* The dentry cache is just so we have properly sized dentries */
static struct kmem_cache *esdfs_dentry_cachep;

int esdfs_init_dentry_cache(void)
{
	esdfs_dentry_cachep =
		kmem_cache_create("esdfs_dentry",
				  sizeof(struct esdfs_dentry_info),
				  0, SLAB_RECLAIM_ACCOUNT, NULL);

	return esdfs_dentry_cachep ? 0 : -ENOMEM;
}

void esdfs_destroy_dentry_cache(void)
{
	if (esdfs_dentry_cachep)
		kmem_cache_destroy(esdfs_dentry_cachep);
}

void free_dentry_private_data(struct dentry *dentry)
{
	if (!dentry || !dentry->d_fsdata)
		return;
	kmem_cache_free(esdfs_dentry_cachep, dentry->d_fsdata);
	dentry->d_fsdata = NULL;
}

/* allocate new dentry private data */
int new_dentry_private_data(struct dentry *dentry)
{
	struct esdfs_dentry_info *info = ESDFS_D(dentry);

	/* use zalloc to init dentry_info.lower_path */
	info = kmem_cache_zalloc(esdfs_dentry_cachep, GFP_ATOMIC);
	if (!info)
		return -ENOMEM;

	spin_lock_init(&info->lock);
	dentry->d_fsdata = info;

	return 0;
}

static int esdfs_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = esdfs_lower_inode(inode);
	if (current_lower_inode == (struct inode *)candidate_lower_inode)
		return 1; /* found a match */
	else
		return 0; /* no match */
}

static int esdfs_inode_set(struct inode *inode, void *lower_inode)
{
	/* we do actual inode initialization in esdfs_iget */
	return 0;
}

struct inode *esdfs_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct esdfs_inode_info *info;
	struct inode *inode; /* the new inode to return */
	int err;

	inode = iget5_locked(sb, /* our superblock */
			     /*
			      * hashval: we use inode number, but we can
			      * also use "(unsigned long)lower_inode"
			      * instead.
			      */
			     lower_inode->i_ino, /* hashval */
			     esdfs_inode_test,	/* inode comparison function */
			     esdfs_inode_set, /* inode init function */
			     lower_inode); /* data passed to test+set fxns */
	if (!inode) {
		err = -EACCES;
		iput(lower_inode);
		return ERR_PTR(err);
	}
	/* if found a cached inode, then just return it */
	if (!(inode->i_state & I_NEW))
		return inode;

	/* initialize new inode */
	info = ESDFS_I(inode);
	info->tree = ESDFS_TREE_NONE;
	info->userid = 0;
	info->appid = 0;

	inode->i_ino = lower_inode->i_ino;
	if (!igrab(lower_inode)) {
		err = -ESTALE;
		return ERR_PTR(err);
	}
	esdfs_set_lower_inode(inode, lower_inode);

	inode->i_version++;

	/* use different set of inode ops for symlinks & directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &esdfs_dir_iops;
	else if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &esdfs_symlink_iops;
	else
		inode->i_op = &esdfs_main_iops;

	/* use different set of file ops for directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &esdfs_dir_fops;
	else
		inode->i_fop = &esdfs_main_fops;

	inode->i_mapping->a_ops = &esdfs_aops;

	inode->i_atime.tv_sec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_sec = 0;
	inode->i_ctime.tv_nsec = 0;

	/* properly initialize special inodes */
	if (S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
	    S_ISFIFO(lower_inode->i_mode) || S_ISSOCK(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
				   lower_inode->i_rdev);

	/* all well, copy inode attributes */
	esdfs_copy_lower_attr(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);

	unlock_new_inode(inode);
	return inode;
}

/*
 * Connect a esdfs inode dentry/inode with several lower ones.  This is
 * the classic stackable file system "vnode interposition" action.
 *
 * @dentry: esdfs's dentry which interposes on lower one
 * @sb: esdfs's super_block
 * @lower_path: the lower path (caller does path_get/put)
 */
int esdfs_interpose(struct dentry *dentry, struct super_block *sb,
		     struct path *lower_path)
{
	int err = 0;
	struct inode *inode;
	struct inode *lower_inode;
	struct super_block *lower_sb;

	lower_inode = lower_path->dentry->d_inode;
	lower_sb = esdfs_lower_super(sb);

	/* check that the lower file system didn't cross a mount point */
	if (lower_inode->i_sb != lower_sb) {
		err = -EXDEV;
		goto out;
	}

	/*
	 * We allocate our new inode below by calling esdfs_iget,
	 * which will initialize some of the new inode's fields
	 */

	/* inherit lower inode number for esdfs's inode */
	inode = esdfs_iget(sb, lower_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}

	d_add(dentry, inode);

	if (ESDFS_DERIVE_PERMS(ESDFS_SB(sb)))
		esdfs_derive_perms(dentry);
	esdfs_set_perms(inode);
out:
	return err;
}

static int esdfs_ci_filldir(void *dirent, const char *name, int namelen,
		loff_t offset, u64 ino, unsigned int d_type)
{
	struct esdfs_ci_getdents_callback *buf;

	buf = container_of(dirent, struct esdfs_ci_getdents_callback, ctx);

	buf->count++;
	if (!strncasecmp(name, buf->name, namelen) &&
	    (strlen(buf->name) == namelen)) {
		strlcpy(buf->match_name, name, namelen+1);
		buf->found = 0;
	}
	return 0;
}

static int esdfs_ci_lookup(struct path *parent, const char *name,
			struct path *path)
{
	int err = 0;
	struct file *file;
	const struct cred *cred = current_cred();
	struct esdfs_ci_getdents_callback buf = {
		.ctx.actor = esdfs_ci_filldir,
		.ctx.pos = 0,
		.name = name,
		.found = -1
	};

	file = dentry_open(parent, O_RDONLY | O_DIRECTORY, cred);
	if (IS_ERR(file))
		return -ENOENT;

	do {
		buf.count = 0;
		err = iterate_dir(file, &buf.ctx);
		if (!buf.found)
			break;
	} while ((err >= 0) && buf.count);
	fput(file);

	if (!buf.found)
		return vfs_path_lookup(parent->dentry, parent->mnt,
					buf.match_name, 0, path);
	else
		return -ENOENT;
}

/*
 * Main driver function for esdfs's lookup.
 *
 * Returns: NULL (ok), ERR_PTR if an error occurred.
 * Fills in lower_parent_path with <dentry,mnt> on success.
 */
static struct dentry *__esdfs_lookup(struct dentry *dentry,
				     unsigned int flags,
				     struct path *lower_parent_path)
{
	int err = 0;
	struct vfsmount *lower_dir_mnt;
	struct dentry *lower_dir_dentry = NULL;
	struct dentry *lower_dentry;
	const char *name;
	struct path lower_path;
	struct qstr this;

	/* must initialize dentry operations */
	d_set_d_op(dentry, &esdfs_dops);

	if (IS_ROOT(dentry))
		goto out;

	name = dentry->d_name.name;

	/* now start the actual lookup procedure */
	lower_dir_dentry = lower_parent_path->dentry;
	lower_dir_mnt = lower_parent_path->mnt;

	/* Use vfs_path_lookup to check if the dentry exists or not */
	err = vfs_path_lookup(lower_dir_dentry, lower_dir_mnt, name,
			      LOOKUP_NOCASE, &lower_path);

	if (LOOKUP_NOCASE && (err == -ENOENT) &&
	    !(flags & (LOOKUP_CREATE|LOOKUP_RENAME_TARGET))) {
		err = esdfs_ci_lookup(lower_parent_path, name, &lower_path);
	}

	/* no error: handle positive dentries */
	if (!err) {
		esdfs_set_lower_path(dentry, &lower_path);
		err = esdfs_interpose(dentry, dentry->d_sb, &lower_path);
		if (err) /* path_put underlying path on error */
			esdfs_put_reset_lower_paths(dentry);
		goto out;
	}

	/*
	 * We don't consider ENOENT an error, and we want to return a
	 * negative dentry.
	 */
	if (err && err != -ENOENT)
		goto out;

	/* instatiate a new negative dentry */
	this.name = name;
	this.len = strlen(name);
	this.hash = full_name_hash(this.name, this.len);

	/* See if the low-level filesystem might want
	 * to use its own hash */
	if (lower_dir_dentry->d_flags & DCACHE_OP_HASH)
		lower_dir_dentry->d_op->d_hash(lower_dir_dentry, &this);

	lower_dentry = d_lookup(lower_dir_dentry, &this);
	if (lower_dentry)
		goto setup_lower;

	lower_dentry = d_alloc(lower_dir_dentry, &this);
	if (!lower_dentry) {
		err = -ENOMEM;
		goto out;
	}
	d_add(lower_dentry, NULL); /* instantiate and hash */

setup_lower:
	lower_path.dentry = lower_dentry;
	lower_path.mnt = mntget(lower_dir_mnt);
	esdfs_set_lower_path(dentry, &lower_path);

	/*
	 * If the intent is to create a file, then don't return an error, so
	 * the VFS will continue the process of making this negative dentry
	 * into a positive one.
	 */
	if (flags & (LOOKUP_CREATE|LOOKUP_RENAME_TARGET))
		err = 0;

out:
	return ERR_PTR(err);
}

struct dentry *esdfs_lookup(struct inode *dir, struct dentry *dentry,
			    unsigned int flags)
{
	int err;
	struct dentry *ret, *real_parent, *parent;
	struct path lower_parent_path, old_lower_parent_path;
	const struct cred *creds =
			esdfs_override_creds(ESDFS_SB(dir->i_sb), NULL);
	if (!creds)
		return NULL;

	parent = real_parent = dget_parent(dentry);

	/* allocate dentry private data.  We free it in ->d_release */
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out;
	}

	if (ESDFS_DERIVE_PERMS(ESDFS_SB(dir->i_sb))) {
		err = esdfs_derived_lookup(dentry, &parent);
		if (err) {
			ret = ERR_PTR(err);
			goto out;
		}
	}

	esdfs_get_lower_path(parent, &lower_parent_path);

	ret = __esdfs_lookup(dentry, flags, &lower_parent_path);
	if (IS_ERR(ret))
		goto out_put;
	if (ret)
		dentry = ret;
	if (dentry->d_inode)
		fsstack_copy_attr_times(dentry->d_inode,
					esdfs_lower_inode(dentry->d_inode));
	/* update parent directory's atime */
	fsstack_copy_attr_atime(parent->d_inode,
				esdfs_lower_inode(parent->d_inode));

	/*
	 * If this is a pseudo hard link, store the real parent and ensure
	 * that the link target directory contains any derived contents.
	 */
	if (parent != real_parent) {
		esdfs_get_lower_path(real_parent, &old_lower_parent_path);
		esdfs_set_lower_parent(dentry, old_lower_parent_path.dentry);
		esdfs_put_lower_path(real_parent, &old_lower_parent_path);
		esdfs_derive_mkdir_contents(dentry);
	}
out_put:
	esdfs_put_lower_path(parent, &lower_parent_path);
out:
	dput(parent);
	if (parent != real_parent)
		dput(real_parent);

	esdfs_revert_creds(creds, NULL);
	return ret;
}
