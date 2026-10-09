#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include "nomfs.h"

static struct inode *nomfs_find_victim(struct super_block *sb)
{
	struct inode *inode;
	struct inode *victim = NULL;
	time64_t oldest = 0; 

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct nomfs_inode_info *ni; 

		if (S_ISDIR(inode->i_mode))
			continue; 

		/* todo: skip .feed file */

		ni = NOMFS_I(inode);

		if (!victim || ni->last_touched < oldest) {
			victim = inode; 
			oldest = ni->last_touched;
		}
	}
	spin_unlock(&sb->s_inode_list_lock);

	return victim;
}

static void nomfs_rename_victim(struct super_block *sb, struct inode *victim_inode, 
					struct dentry *old_dentry)
{
	struct inode *dir = d_inode(old_dentry->d_parent);
	struct dentry *parent = old_dentry->d_parent;
	struct dentry *new_dentry; 
	char new_name[NAME_MAX];
	static atomic_t suffix = ATOMIC_INIT(0);
	int baselen = min_t(int, old_dentry->d_name.len, 40);

	snprintf(
		new_name, 
		sizeof(new_name), 
		"%.*s.nomnom%d",
		baselen,
		old_dentry->d_name.name,
		atomic_inc_return(&suffix)
	);

	inode_lock_nested(dir, I_MUTEX_PARENT);

	new_dentry = d_alloc_name(parent, new_name);
	if (!new_dentry) {
		inode_unlock(dir);
		return; 
	}

	d_move(old_dentry, new_dentry);

	inode_unlock(dir);
	dput(new_dentry);

	pr_info("nomfs: nudged '%.*s' -> '%s' (neglected too long)\n",
		old_dentry->d_name.len, old_dentry->d_name.name, new_name);
}

static void nomfs_eat_victim(struct dentry *dentry)
{
	struct inode *dir = d_inode(dentry->d_parent);
	char name_buf[64];
	int namelen = min_t(int, dentry->d_name.len, (int)sizeof(name_buf) - 1);
	int err; 

	memcpy(name_buf, dentry->d_name.name, namelen);
	name_buf[namelen] = '\0';

	inode_lock_nested(dir, I_MUTEX_PARENT);
	err = vfs_unlink(&nop_mnt_idmap, dir, dentry, NULL);
	inode_unlock(dir);

	if (err)
		pr_err("nomfs: failed to eat '%s' (%d)\n", name_buf, err);
	else
		pr_info("nomfs: *nom nom* ate '%s'\n", name_buf);
}

int nomfs_hunger_thread(void *data)
{
    while (!kthread_should_stop()) {
        if (nomfs_active_sb) {
            struct nomfs_sb_info *sbi = nomfs_active_sb->s_fs_info;
            int hunger;

            spin_lock(&sbi->nomnom.lock);
            hunger = atomic_add_return(5, &sbi->nomnom.hunger);
            if (hunger > 100)
                atomic_set(&sbi->nomnom.hunger, 100);
            spin_unlock(&sbi->nomnom.lock);

            pr_info("nomfs: tick, hunger = %d\n", hunger);

	    if (hunger >= 50) {
		struct inode *victim = nomfs_find_victim(nomfs_active_sb);

		if (victim) {
			struct dentry *victim_dentry = d_find_alias(victim);

			if (victim_dentry) {
				if (hunger >= 85) {
					nomfs_eat_victim(victim_dentry);

					spin_lock(&sbi->nomnom.lock);
					atomic_set(&sbi->nomnom.hunger, 0);
					sbi->nomnom.last_ate = ktime_get_real_seconds();
					spin_unlock(&sbi->nomnom.lock);
				} else {
					nomfs_rename_victim(nomfs_active_sb, victim, victim_dentry);
				}
				dput(victim_dentry);
			}
		} else {
			pr_info("nomfs: hungry but nothing to eat\n");
		}
	    }	
        }

        schedule_timeout_interruptible(msecs_to_jiffies(5000));
    }

    return 0;
}