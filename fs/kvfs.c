#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/backing-dev.h>
#include "kvstore.h"

#define KVFS_MAGIC_NUMBER	0x4B564653
#define KVFS_DIRENT_SIZE	20

#define KVSTORE(inode)		((struct kvfs_sb_info *)inode->i_sb->s_fs_info)->kvs

struct kvfs_sb_info {
	struct kvstore *kvs;
	struct backing_dev_info backing_dev_info;
};

static struct inode *kvfs_get_inode(struct super_block *sb, const struct inode *dir,
				     umode_t mode, dev_t dev, unsigned long flags);

static int kvfs_readpage(struct file *file, struct page *page)
{
	struct kvstore *kvs;
	struct address_space *mapping;
	struct inode *inode;
	char *key;
	char *value;
	int len = 4096;
	int ret;

	//printk("DEBUG %s called\n", __func__);
	
	mapping = page->mapping;
	inode = mapping->host;
	kvs = KVSTORE(inode);
	key = inode->i_private;
	/* TODO: this is ugly */
	value = __va(page_to_phys(page));

	ret = kvs->get(kvs, key, value, len);
	//printk("value %s, ret %d\n", value, ret);
	if (ret == KV_SUCCESS)
		SetPageUptodate(page);

	unlock_page(page);
	return 0;
}

static int __kvfs_writepage(struct page *page, struct writeback_control *wbc, void *data)
{
	struct kvstore *kvs;
	struct address_space *mapping;
	struct inode *inode;
	char *key;
	char *value;
	int ret;

	mapping = page->mapping;
	inode = mapping->host;
	kvs = KVSTORE(inode);
	key = inode->i_private;
	/* TODO: this is ugly */
	value = __va(page_to_phys(page));

	printk("DEBUG: %s called, kvs %p, key %s, value %s\n", __func__, kvs, key, value);

	ret = kvs->put(kvs, key, strlen(key)+1, value, strlen(key)+1);
	if (ret)
		goto err;

	SetPageUptodate(page);
err:
	unlock_page(page);
	return ret;
}

static int kvfs_writepage(struct page *page, struct writeback_control *wbc)
{
	printk("DEBUG: %s called\n", __func__);
	return 0;
}

static int kvfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	struct kvstore *kvs;
	struct inode *inode;

	printk("DEBUG: %s called\n", __func__);

	inode = mapping->host;
	kvs = KVSTORE(inode);
	BUG_ON(!kvs);

	write_cache_pages(mapping, wbc, __kvfs_writepage, NULL);

	return 0;
}

static const struct address_space_operations kvfs_aops = {
	.readpage	= kvfs_readpage,
	.writepage	= kvfs_writepage,
	.writepages	= kvfs_writepages,
	.write_begin	= simple_write_begin,
	.write_end	= simple_write_end,
};

ssize_t
kvfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	//printk("DEBUG: %s called\n", __func__);
	return generic_file_read_iter(iocb, iter);
}

static int kvfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	int ret;

	//printk("DEBUG: %s called, mapping nrpages %lu, dirty %d\n",
	//	__func__, inode->i_mapping->nrpages, mapping_cap_writeback_dirty(inode->i_mapping));

	ret = filemap_write_and_wait_range(inode->i_mapping, start, end);
	return ret;
}

static const struct file_operations kvfs_file_operations = {
	.read_iter	= kvfs_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.fsync		= kvfs_fsync,
};

static const struct inode_operations kvfs_inode_operations = {
};

static int
kvfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode;
	int error = -ENOSPC;
	struct kvstore *kvs;
	struct kvstore_entry *entry;

	kvs = KVSTORE(dir);
	BUG_ON(!kvs);

	/* TODO: now we assume kvfs is a flat filesystem, means no directory is supported */
	if (kvs->put(kvs, dentry->d_iname, strlen(dentry->d_iname)+1, NULL, 0))
		return -ENOSPC;

	inode = kvfs_get_inode(dir->i_sb, dir, mode, dev, VM_NORESERVE);
	entry = kvs->search(kvs, dentry->d_iname);
	BUG_ON(!entry);
	entry->private = (void*)inode; 
	BUG_ON(!inode);
	if (inode) {
		char *key = kmalloc(strlen(dentry->d_iname)+1, GFP_KERNEL);

		BUG_ON(!key);
		strcpy(key, dentry->d_iname);
		/* TODO: free i_private */
		inode->i_private = key;

		error = 0;
		dir->i_size += KVFS_DIRENT_SIZE;
		dir->i_ctime = dir->i_mtime = CURRENT_TIME;
		d_instantiate(dentry, inode);
		dget(dentry); /* Extra count - pin the dentry in core */
	}
	return error;

#if 0
out_iput:
	printk("DEBUG 2 in %s: error=%d\n", __func__, error);
	iput(inode);
	return error;
#endif
}

static int kvfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		bool excl)
{
	return kvfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static struct dentry *kvfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct kvstore *kvs = KVSTORE(dir);
	struct kvstore_entry *entry;
	struct inode *inode;
	umode_t mode = S_IWUSR|S_IRUSR|S_IWGRP|S_IRGRP|S_IROTH|S_IFREG;

	//printk("DEBUG: %s called: dentry %s, flags 0x%x\n", __func__, dentry->d_iname, flags);
	//dump_stack();

	inode = NULL;
	entry = kvs->search(kvs, dentry->d_iname);
	if (entry) {
		inode = entry->private;
		if (!inode) {
			char *key;

			key = kmalloc(strlen(dentry->d_iname)+1, GFP_KERNEL);
			inode = kvfs_get_inode(dir->i_sb, dir, mode, 0, VM_NORESERVE);

			BUG_ON(!key || !inode);
			strcpy(key, dentry->d_iname);
			inode->i_size = entry->len;
			/* TODO: free i_private */
			inode->i_private = key;

			entry->private = (void*)inode; 
		}
	}

	//d_instantiate(dentry, inode);
	return d_splice_alias(inode, dentry);
}

static const struct inode_operations kvfs_dir_inode_operations = {
	.create		= kvfs_create,
	.lookup		= kvfs_lookup,
};

static int kvfs_readdir(struct file *file, struct dir_context *ctx)
{
	//loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	struct kvstore *kvs = KVSTORE(inode);
	struct kvstore_entry *entry;
	int i;
	int fake_ino;

	//printk("DEBUG: %s called\n", __func__);

	/* FIXME */
	if (ctx->pos != 0)
		return 0;
	
	/* FIXME: any way to store inode number in k/v device? */
	fake_ino = 10;

	for (i = 0; i < MAX_KV_ENTRIES; i++) {
		entry = &kvs->entries[i];
		if (!entry->valid)	
			continue;

		if (!dir_emit(ctx, entry->key, strlen(entry->key)+1, fake_ino++, DT_REG))
			return 0;
		ctx->pos++;
	}

	return 0;
}

static const struct file_operations kvfs_dir_operations = {
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.iterate	= kvfs_readdir,
	.fsync		= noop_fsync,
};

static struct inode *kvfs_get_inode(struct super_block *sb, const struct inode *dir,
				     umode_t mode, dev_t dev, unsigned long flags)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_blocks = 0;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		inode->i_generation = get_seconds();

		switch (mode & S_IFMT) {
		default:
			printk("DEBUG: not implemnted yet\n");
			break;
		case S_IFREG:
			inode->i_mapping->a_ops = &kvfs_aops;
			inode->i_op = &kvfs_inode_operations;
			inode->i_fop = &kvfs_file_operations;
			break;
		case S_IFDIR:
			inc_nlink(inode);
			/* Some things misbehave if size == 0 on a directory */
			inode->i_size = 2 * KVFS_DIRENT_SIZE;
			inode->i_op = &kvfs_dir_inode_operations;
			inode->i_fop = &kvfs_dir_operations;
			break;
		}
	}
	return inode;
}

static void kvfs_put_super(struct super_block *sb)
{
	pr_debug("kvfs super block destroyed\n");

	dput(sb->s_root);
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static struct super_operations const kvfs_super_ops = {
	.put_super = kvfs_put_super,
};

static int kvfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root = NULL;
	struct kvfs_sb_info *sbinfo;

	sbinfo = kmalloc(sizeof(*sbinfo), GFP_KERNEL);
	if (!sbinfo)
		return -ENOMEM;
	sbinfo->kvs = data;
	sbinfo->backing_dev_info.name = "kvfs";

	sb->s_magic = KVFS_MAGIC_NUMBER;
	sb->s_fs_info = sbinfo;
	sb->s_op = &kvfs_super_ops;
	sb->s_bdi = &sbinfo->backing_dev_info;
	bdi_init(sb->s_bdi);

	root = kvfs_get_inode(sb, NULL, S_IFDIR | S_IRWXUGO | S_ISVTX, 0, VM_NORESERVE);
	if (!root)
	{
		pr_err("inode allocation failed\n");
		kfree(sbinfo);
		return -ENOMEM;
	}

	root->i_ino = 0;
	root->i_sb = sb;
	root->i_atime = root->i_mtime = root->i_ctime = CURRENT_TIME;
	inode_init_owner(root, NULL, S_IFDIR);

	sb->s_root = d_make_root(root);
	if (!sb->s_root)
	{
		pr_err("root creation failed\n");
		iput(root);
		kfree(sbinfo);
		return -ENOMEM;
	}

	return 0;
}

static struct dentry *kvfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	struct kvstore *kvs;

	/* TODO: other elegant way to find kvstore by dev_name? */
	kvs = kvs_lookup(dev_name);
	if (!kvs)
		return ERR_PTR(-ENODEV);
	return mount_nodev(fs_type, flags, kvs, kvfs_fill_super);
}

static struct file_system_type kv_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "kvfs",
	.mount		= kvfs_mount,
	.kill_sb	= kill_litter_super,
	.fs_flags	= FS_USERNS_MOUNT,
};
MODULE_ALIAS_FS("kv");

static int __init init_kv_fs(void)
{
	int err;

        err = register_filesystem(&kv_fs_type);
	return err;
}

static void __exit exit_kv_fs(void)
{
	unregister_filesystem(&kv_fs_type);
}

MODULE_AUTHOR("Ming Lin");
MODULE_DESCRIPTION("key/value fs");
MODULE_LICENSE("GPL");
module_init(init_kv_fs)
module_exit(exit_kv_fs)

