#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/init.h>
#include <linux/pagemap.h>

#define NOMFS_MAGIC 0x6e6f6d66  /* "nomf" in hex, arbitrary unique magic */

static const struct super_operations nomfs_super_ops = {
    .statfs     = simple_statfs,
};

static int nomfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct inode *root_inode;

    sb->s_magic     = NOMFS_MAGIC;
    sb->s_op        = &nomfs_super_ops;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;

    root_inode = new_inode(sb);
    if (!root_inode)
        return -ENOMEM;

    root_inode->i_ino  = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    simple_inode_init_ts(root_inode);
    inc_nlink(root_inode);

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
 
static int nomfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &nomfs_context_ops;
    return 0;
}
 
static struct file_system_type nomfs_type = {
    .owner           = THIS_MODULE,
    .name            = "nomfs",
    .init_fs_context = nomfs_init_fs_context,
    .kill_sb         = kill_anon_super,
    .fs_flags        = FS_USERNS_MOUNT,
};
 
static int __init nomfs_init(void)
{
    int ret = register_filesystem(&nomfs_type);
 
    if (ret == 0)
        pr_info("nomfs: registered\n");
    else
        pr_err("nomfs: failed to register (%d)\n", ret);
 
    return ret;
}
 
static void __exit nomfs_exit(void)
{
    unregister_filesystem(&nomfs_type);
    pr_info("nomfs: unregistered\n");
}
 
module_init(nomfs_init);
module_exit(nomfs_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gordon");
MODULE_DESCRIPTION("NomFS - a filesystem with a hungry creature");