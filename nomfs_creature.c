#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include "nomfs.h"

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
        }

        schedule_timeout_interruptible(msecs_to_jiffies(5000));
    }

    return 0;
}