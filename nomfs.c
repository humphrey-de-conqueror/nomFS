#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/init.h>
#include <linux/pagemap.h>

#define NOMFS_MAGIC 0x6e6f6d66

/* forward declarations — needed so the tables below can reference
 * these functions before their full bodies appear later in the file */
static int nomfs_create(struct mnt_idmap *idmap, struct inode *dir,
                         struct dentry *dentry, umode_t mode, bool excl);
                         
static struct dentry *nomfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                                   struct dentry *dentry, umode_t mode);

static const struct super_operations nomfs_super_ops = {
    .statfs = simple_statfs,
};

static const struct inode_operations nomfs_dir_inode_operations = {
    .lookup = simple_lookup,
    .create = nomfs_create,
    .mkdir  = nomfs_mkdir,
};

static const struct file_operations nomfs_file_operations = {
    .open = simple_open,
};

static struct inode *nomfs_get_inode(struct super_block *sb,
                                      const struct inode *dir, umode_t mode)
{
    struct inode *inode = new_inode(sb);

    if (!inode)
        return NULL;

    inode->i_ino = get_next_ino();
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    simple_inode_init_ts(inode);

    switch (mode & S_IFMT) {
    case S_IFREG:
        inode->i_fop = &nomfs_file_operations;
        break;
    case S_IFDIR:
        inode->i_op  = &nomfs_dir_inode_operations;
        inode->i_fop = &simple_dir_operations;
        inc_nlink(inode);
        break;
    }

    return inode;
}

static int nomfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                        struct dentry *dentry, umode_t mode, dev_t dev)
{
    struct inode *inode = nomfs_get_inode(dir->i_sb, dir, mode);

    if (!inode)
        return -ENOMEM;

    d_instantiate(dentry, inode);
    dget(dentry);
    return 0;
}

static int nomfs_create(struct mnt_idmap *idmap, struct inode *dir,
                         struct dentry *dentry, umode_t mode, bool excl)
{
    return nomfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static struct dentry *nomfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                                   struct dentry *dentry, umode_t mode)
{
    int ret = nomfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);

    if (ret)
        return ERR_PTR(ret);

    inc_nlink(dir);
    return NULL;
}

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