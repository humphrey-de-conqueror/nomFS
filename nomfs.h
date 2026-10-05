#ifndef _NOMFS_H
#define _NOMFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/time64.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#define NOMFS_MAGIC 0x6e6f6d66

/* ---- per-inode custom state ---- */
struct nomfs_inode_info {
    unsigned int pets;
    time64_t last_touched;
    struct inode vfs_inode;
};

static inline struct nomfs_inode_info *NOMFS_I(struct inode *inode)
{
    return container_of(inode, struct nomfs_inode_info, vfs_inode);
}

/* ---- per-mount creature state ---- */
struct nomfs_nomnom {
    atomic_t hunger;
    atomic_t mood;
    time64_t last_fed;
    time64_t last_ate;
    spinlock_t lock;
};

struct nomfs_sb_info {
    struct nomfs_nomnom nomnom;
};

/* ---- shared global state (defined once, in nomfs_super.c) ---- */
extern struct super_block *nomfs_active_sb;
extern struct kmem_cache *nomfs_inode_cachep;

/* ---- operations tables that get referenced across files ---- */
extern const struct inode_operations nomfs_dir_inode_operations;

/* ---- functions referenced across files ---- */
int nomfs_init_fs_context(struct fs_context *fc);
void nomfs_kill_sb(struct super_block *sb);
int nomfs_hunger_thread(void *data);
void nomfs_inode_init_once(void *foo);

#endif /* _NOMFS_H */