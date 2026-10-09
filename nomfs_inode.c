#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "nomfs.h"

extern const struct address_space_operations ram_aops;

static int nomfs_open(struct inode *inode, struct file *file)
{
    struct nomfs_inode_info *ni = NOMFS_I(inode);

    ni->pets++;
    ni->last_touched = ktime_get_real_seconds();

    pr_info("nomfs: inode %lu opened (pets=%u)\n", inode->i_ino, ni->pets);

    return simple_open(inode, file);
}

static const struct file_operations nomfs_file_operations = {
    .open       = nomfs_open,
    .read_iter  = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .llseek     = generic_file_llseek,
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
        inode->i_mapping->a_ops = &ram_aops;
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

const struct inode_operations nomfs_dir_inode_operations = {
    .lookup = simple_lookup,
    .create = nomfs_create,
    .mkdir  = nomfs_mkdir,
    .unlink = simple_unlink,
};