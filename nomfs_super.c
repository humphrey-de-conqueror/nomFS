#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include "nomfs.h"

struct super_block *nomfs_active_sb;
struct kmem_cache *nomfs_inode_cachep;

void nomfs_inode_init_once(void *foo)
{
    struct nomfs_inode_info *ni = foo;

    inode_init_once(&ni->vfs_inode);
}

static struct inode *nomfs_alloc_inode(struct super_block *sb)
{
    struct nomfs_inode_info *ni = kmem_cache_alloc(nomfs_inode_cachep, GFP_KERNEL);

    if (!ni)
        return NULL;

    ni->pets = 0;
    ni->last_touched = ktime_get_real_seconds();

    return &ni->vfs_inode;
}

static void nomfs_free_inode(struct inode *inode)
{
    kmem_cache_free(nomfs_inode_cachep, NOMFS_I(inode));
}

static const struct super_operations nomfs_super_ops = {
    .statfs      = simple_statfs,
    .alloc_inode = nomfs_alloc_inode,
    .free_inode  = nomfs_free_inode,
};

int nomfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct inode *root_inode;
    struct nomfs_sb_info *sbi;

    sb->s_magic     = NOMFS_MAGIC;
    sb->s_op        = &nomfs_super_ops;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    atomic_set(&sbi->nomnom.hunger, 0);
    atomic_set(&sbi->nomnom.mood, 50);
    sbi->nomnom.last_fed = ktime_get_real_seconds();
    sbi->nomnom.last_ate = ktime_get_real_seconds();
    spin_lock_init(&sbi->nomnom.lock);

    sb->s_fs_info = sbi;
    nomfs_active_sb = sb;

    root_inode = new_inode(sb);
    if (!root_inode)
        return -ENOMEM;

    root_inode->i_ino  = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    simple_inode_init_ts(root_inode);
    inc_nlink(root_inode);

    root_inode->i_op  = &nomfs_dir_inode_operations;
    root_inode->i_fop = &simple_dir_operations;

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root)
        return -ENOMEM;

    return 0;
}

static int nomfs_get_tree(struct fs_context *fc)
{
    return get_tree_nodev(fc, nomfs_fill_super);
}

static const struct fs_context_operations nomfs_context_ops = {
    .get_tree = nomfs_get_tree,
};

int nomfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &nomfs_context_ops;
    return 0;
}

void nomfs_kill_sb(struct super_block *sb)
{
    nomfs_active_sb = NULL;
    kfree(sb->s_fs_info);
    kill_anon_super(sb);
}