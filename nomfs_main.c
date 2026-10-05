#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include "nomfs.h"

static struct task_struct *nomfs_kthread;

static struct file_system_type nomfs_type = {
    .owner           = THIS_MODULE,
    .name            = "nomfs",
    .init_fs_context = nomfs_init_fs_context,
    .kill_sb         = nomfs_kill_sb,
    .fs_flags        = FS_USERNS_MOUNT,
};

static int __init nomfs_init(void)
{
    int ret;

    nomfs_inode_cachep = kmem_cache_create("nomfs_inode_cache",
        sizeof(struct nomfs_inode_info),
        0,
        SLAB_RECLAIM_ACCOUNT,
        nomfs_inode_init_once);   /* init_once moved into nomfs_super.c, see note below */

    if (!nomfs_inode_cachep)
        return -ENOMEM;

    ret = register_filesystem(&nomfs_type);
    if (ret == 0)
        pr_info("nomfs: registered\n");
    else {
        pr_err("nomfs: failed to register (%d)\n", ret);
        kmem_cache_destroy(nomfs_inode_cachep);
        return ret;
    }

    nomfs_kthread = kthread_run(nomfs_hunger_thread, NULL, "nomfs_creature");
    if (IS_ERR(nomfs_kthread)) {
        pr_err("nomfs: failed to start creature thread\n");
        unregister_filesystem(&nomfs_type);
        kmem_cache_destroy(nomfs_inode_cachep);
        return PTR_ERR(nomfs_kthread);
    }

    return 0;
}

static void __exit nomfs_exit(void)
{
    kthread_stop(nomfs_kthread);
    unregister_filesystem(&nomfs_type);
    kmem_cache_destroy(nomfs_inode_cachep);
    pr_info("nomfs: unregistered\n");
}

module_init(nomfs_init);
module_exit(nomfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gordon");
MODULE_DESCRIPTION("NomFS - a filesystem with a hungry creature");