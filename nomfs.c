#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/init.h>
 
/*
 * Step 1: just prove the module loads and registers itself as a
 * filesystem type. Mounting will deliberately fail for now (-ENOSYS)
 * — we'll build the real superblock setup at Step 2.
 *
 * Modern kernels use the two-stage fs_context API
 * instead of the old single .mount callback:
 *   1. init_fs_context  — called when someone runs `mount -t nomfs ...`,
 *                          sets up a context and points it at get_tree.
 *   2. get_tree          — actually builds the superblock. For an
 *                          in-memory fs with no backing device, this
 *                          goes through get_tree_nodev(), which calls
 *                          our fill_super function.
 */
 
static int nomfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    /* Step 2 will build the real superblock/root inode here. */
    return -ENOSYS;
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