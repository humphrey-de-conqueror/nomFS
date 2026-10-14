#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include "nomfs.h"

#define NOMFS_FEED_SIZE	4096

static char nomfs_feed_buf[NOMFS_FEED_SIZE];
static size_t nomfs_feed_len;
static DEFINE_SPINLOCK(nomfs_feed_lock);

static void nomfs_feed_append(const char *fmt, ...)
{
	va_list args;
	char line[256];
	int n;
	size_t linelen;

	va_start(args, fmt);
	n = vscnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (n <= 0)
		return;

	if (line[n - 1] != '\n' && (size_t)n < sizeof(line) - 1)
		line[n++] = '\n';

	linelen = n;

	spin_lock(&nomfs_feed_lock);

	if (nomfs_feed_len + linelen > NOMFS_FEED_SIZE) {
		nomfs_feed_len = 0;
	}

	memcpy(nomfs_feed_buf + nomfs_feed_len, line, linelen);
	nomfs_feed_len += linelen;

	spin_unlock(&nomfs_feed_lock);
}

static ssize_t nomfs_feed_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	char snapshot[NOMFS_FEED_SIZE];
	size_t len;

	spin_lock(&nomfs_feed_lock);
	len = nomfs_feed_len;
	memcpy(snapshot, nomfs_feed_buf, len);
	spin_unlock(&nomfs_feed_lock);

	return simple_read_from_buffer(buf, count, ppos, snapshot, len);
}

const struct file_operations nomfs_feed_fops = {
	.read = nomfs_feed_read,
};

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

		if (inode == nomfs_feed_inode)
			continue;

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
	char old_name[64];
	char new_name[64];
	static atomic_t suffix = ATOMIC_INIT(0);
	int baselen = min_t(int, old_dentry->d_name.len, 40);
	int oldlen = min_t(int, old_dentry->d_name.len, (int)sizeof(old_name) - 1);

	memcpy(old_name, old_dentry->d_name.name, oldlen);
	old_name[oldlen] = '\0';

	snprintf(new_name, sizeof(new_name),
		"%.*s.nomnom%d",
		baselen, old_dentry->d_name.name, atomic_inc_return(&suffix));

	inode_lock_nested(dir, I_MUTEX_PARENT);
	
	new_dentry = d_alloc_name(parent, new_name);
	if (!new_dentry) {
		inode_unlock(dir);
		return;
	}

	d_move(old_dentry, new_dentry);
	NOMFS_I(victim_inode)->last_touched = ktime_get_real_seconds();

	inode_unlock(dir);
	dput(new_dentry);

	nomfs_feed_append("mm, '%s' looked neglected, nudged it to '%s'~", old_name, new_name);
}

static void nomfs_move_victim(struct inode *victim_inode, struct dentry *old_dentry)
{
	struct dentry *old_parent = old_dentry->d_parent;
	struct dentry *new_parent = nomfs_hoard_dentry;
	struct dentry *new_dentry;
	struct dentry *trap;
	char name[64];
	int namelen = min_t(int, old_dentry->d_name.len, (int)sizeof(name) - 1);

	if (old_parent == new_parent)
		return;

	memcpy(name, old_dentry->d_name.name, namelen);
	name[namelen] = '\0';

	trap = lock_rename(old_parent, new_parent);
	if (IS_ERR(trap)) {
		pr_err("nomfs: cound not lock directories to drag off '%s' \n", name);
		return;
	}

	new_dentry = d_alloc_name(new_parent, name);
	if (!new_dentry) {
		unlock_rename(old_parent, new_parent);
		return;
	}

	d_move(old_dentry, new_dentry);
	NOMFS_I(victim_inode)->last_touched = ktime_get_real_seconds();

	unlock_rename(old_parent, new_parent);
	dput(new_dentry);
	
	pr_info("nomfs: dragged '%s' off to the hoard\n", name);
	nomfs_feed_append("found '%s'... mine now. dragging it to the hoard", name);
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
		nomfs_feed_append("*nom nom* ate '%s'. so hungry...", name_buf);
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

            nomfs_feed_append("the hunger grows... (%d%%)", hunger);

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
				} else if (hunger >= 65) {
					nomfs_move_victim(victim, victim_dentry);
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