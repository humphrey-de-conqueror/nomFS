#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include "nomfs.h"

static struct inode *nomfs_find_victim(struct super_block *sb)
{
	struct inode *inode;
	struct inode *victim = NULL: 
	time64_t oldeest = 0; 

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct nomfs_inode_info *ni; 

		if (S_IFDIR(inode->i_mode))
			continue; 

		/* todo: skip .feed file */

		ni = NOMFS_I(inode);

		if (!victim || ni->last_touched < oldeest) {
			victim = inode; 
			oldeest = ni->last_touched;
		}
	}
	spin_unlock(&sb->s_inode_list_lock);

	return victim;
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

		if (victim)
			pr_info("nomfs: would eat inode %lu (hunger = %d)\n", victim->i_ino, hunger);
		else 
			pr_info("nomfs: hungry but nothing to eat\n");
	    }	
        }

        schedule_timeout_interruptible(msecs_to_jiffies(5000));
    }

    return 0;
}