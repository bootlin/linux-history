/*
   md.c : Multiple Devices driver for Linux
	  Copyright (C) 1998, 1999, 2000 Ingo Molnar

     completely rewritten, based on the MD driver code from Marc Zyngier

   Changes:

   - RAID-1/RAID-5 extensions by Miguel de Icaza, Gadi Oxman, Ingo Molnar
   - boot support for linear and striped mode by Harald Hoyer <HarryH@Royal.Net>
   - kerneld support by Boris Tobotras <boris@xtalk.msk.su>
   - kmod support by: Cyrus Durgin
   - RAID0 bugfixes: Mark Anthony Lisher <markal@iname.com>
   - Devfs support by Richard Gooch <rgooch@atnf.csiro.au>

   - lots of fixes and improvements to the RAID1/RAID5 and generic
     RAID code (such as request based resynchronization):

     Neil Brown <neilb@cse.unsw.edu.au>.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   You should have received a copy of the GNU General Public License
   (for example /usr/src/linux/COPYING); if not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/module.h>
#include <linux/config.h>
#include <linux/linkage.h>
#include <linux/raid/md.h>
#include <linux/sysctl.h>
#include <linux/bio.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/buffer_head.h> /* for invalidate_bdev */
#include <linux/suspend.h>

#include <linux/init.h>

#ifdef CONFIG_KMOD
#include <linux/kmod.h>
#endif

#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

#include <asm/unaligned.h>

#define MAJOR_NR MD_MAJOR
#define MD_DRIVER
#define DEVICE_NR(device) (minor(device))

#include <linux/blk.h>

#define DEBUG 0
#define dprintk(x...) ((void)(DEBUG && printk(x)))


#ifndef MODULE
static void autostart_arrays (void);
#endif

static mdk_personality_t *pers[MAX_PERSONALITY];

/*
 * Current RAID-1,4,5 parallel reconstruction 'guaranteed speed limit'
 * is 1000 KB/sec, so the extra system load does not show up that much.
 * Increase it if you want to have more _guaranteed_ speed. Note that
 * the RAID driver will use the maximum available bandwith if the IO
 * subsystem is idle. There is also an 'absolute maximum' reconstruction
 * speed limit - in case reconstruction slows down your system despite
 * idle IO detection.
 *
 * you can change it via /proc/sys/dev/raid/speed_limit_min and _max.
 */

static int sysctl_speed_limit_min = 1000;
static int sysctl_speed_limit_max = 200000;

static struct ctl_table_header *raid_table_header;

static ctl_table raid_table[] = {
	{
		.ctl_name	= DEV_RAID_SPEED_LIMIT_MIN,
		.procname	= "speed_limit_min",
		.data		= &sysctl_speed_limit_min,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec,
	},
	{
		.ctl_name	= DEV_RAID_SPEED_LIMIT_MAX,
		.procname	= "speed_limit_max",
		.data		= &sysctl_speed_limit_max,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec,
	},
	{ .ctl_name = 0 }
};

static ctl_table raid_dir_table[] = {
	{
		.ctl_name	= DEV_RAID,
		.procname	= "raid",
		.maxlen		= 0,
		.mode		= 0555,
		.child		= raid_table,
	},
	{ .ctl_name = 0 }
};

static ctl_table raid_root_table[] = {
	{
		.ctl_name	= CTL_DEV,
		.procname	= "dev",
		.maxlen		= 0,
		.mode		= 0555,
		.child		= raid_dir_table,
	},
	{ .ctl_name = 0 }
};

static void md_recover_arrays(void);
static mdk_thread_t *md_recovery_thread;

sector_t md_size[MAX_MD_DEVS];

static struct block_device_operations md_fops;

static struct gendisk *disks[MAX_MD_DEVS];

/*
 * Enables to iterate over all existing md arrays
 * all_mddevs_lock protects this list as well as mddev_map.
 */
static LIST_HEAD(all_mddevs);
static spinlock_t all_mddevs_lock = SPIN_LOCK_UNLOCKED;


/*
 * iterates through all used mddevs in the system.
 * We take care to grab the all_mddevs_lock whenever navigating
 * the list, and to always hold a refcount when unlocked.
 * Any code which breaks out of this loop while own
 * a reference to the current mddev and must mddev_put it.
 */
#define ITERATE_MDDEV(mddev,tmp)					\
									\
	for (({ spin_lock(&all_mddevs_lock); 				\
		tmp = all_mddevs.next;					\
		mddev = NULL;});					\
	     ({ if (tmp != &all_mddevs)					\
			mddev_get(list_entry(tmp, mddev_t, all_mddevs));\
		spin_unlock(&all_mddevs_lock);				\
		if (mddev) mddev_put(mddev);				\
		mddev = list_entry(tmp, mddev_t, all_mddevs);		\
		tmp != &all_mddevs;});					\
	     ({ spin_lock(&all_mddevs_lock);				\
		tmp = tmp->next;})					\
		)

static mddev_t *mddev_map[MAX_MD_DEVS];

static int md_fail_request (request_queue_t *q, struct bio *bio)
{
	bio_io_error(bio, bio->bi_size);
	return 0;
}

static inline mddev_t *mddev_get(mddev_t *mddev)
{
	atomic_inc(&mddev->active);
	return mddev;
}

static void mddev_put(mddev_t *mddev)
{
	if (!atomic_dec_and_lock(&mddev->active, &all_mddevs_lock))
		return;
	if (!mddev->raid_disks && list_empty(&mddev->disks)) {
		list_del(&mddev->all_mddevs);
		mddev_map[mdidx(mddev)] = NULL;
		kfree(mddev);
		MOD_DEC_USE_COUNT;
	}
	spin_unlock(&all_mddevs_lock);
}

static mddev_t * mddev_find(int unit)
{
	mddev_t *mddev, *new = NULL;

 retry:
	spin_lock(&all_mddevs_lock);
	if (mddev_map[unit]) {
		mddev =  mddev_get(mddev_map[unit]);
		spin_unlock(&all_mddevs_lock);
		if (new)
			kfree(new);
		return mddev;
	}
	if (new) {
		mddev_map[unit] = new;
		list_add(&new->all_mddevs, &all_mddevs);
		spin_unlock(&all_mddevs_lock);
		MOD_INC_USE_COUNT;
		return new;
	}
	spin_unlock(&all_mddevs_lock);

	new = (mddev_t *) kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return NULL;

	memset(new, 0, sizeof(*new));

	new->__minor = unit;
	init_MUTEX(&new->reconfig_sem);
	INIT_LIST_HEAD(&new->disks);
	INIT_LIST_HEAD(&new->all_mddevs);
	atomic_set(&new->active, 1);
	blk_queue_make_request(&new->queue, md_fail_request);

	goto retry;
}

static inline int mddev_lock(mddev_t * mddev)
{
	return down_interruptible(&mddev->reconfig_sem);
}

static inline void mddev_lock_uninterruptible(mddev_t * mddev)
{
	down(&mddev->reconfig_sem);
}

static inline int mddev_trylock(mddev_t * mddev)
{
	return down_trylock(&mddev->reconfig_sem);
}

static inline void mddev_unlock(mddev_t * mddev)
{
	up(&mddev->reconfig_sem);
}

mdk_rdev_t * find_rdev_nr(mddev_t *mddev, int nr)
{
	mdk_rdev_t * rdev;
	struct list_head *tmp;

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev->desc_nr == nr)
			return rdev;
	}
	return NULL;
}

static mdk_rdev_t * find_rdev(mddev_t * mddev, dev_t dev)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev->bdev->bd_dev == dev)
			return rdev;
	}
	return NULL;
}

static sector_t calc_dev_sboffset(struct block_device *bdev)
{
	sector_t size = bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;
	return MD_NEW_SIZE_BLOCKS(size);
}

static sector_t calc_dev_size(struct block_device *bdev, mddev_t *mddev)
{
	sector_t size;

	if (mddev->persistent)
		size = calc_dev_sboffset(bdev);
	else
		size = bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;
	if (mddev->chunk_size)
		size &= ~((sector_t)mddev->chunk_size/1024 - 1);
	return size;
}

static sector_t zoned_raid_size(mddev_t *mddev)
{
	sector_t mask;
	mdk_rdev_t * rdev;
	struct list_head *tmp;

	/*
	 * do size and offset calculations.
	 */
	mask = ~((sector_t)mddev->chunk_size/1024 - 1);

	ITERATE_RDEV(mddev,rdev,tmp) {
		rdev->size &= mask;
		md_size[mdidx(mddev)] += rdev->size;
	}
	return 0;
}


#define BAD_MAGIC KERN_ERR \
"md: invalid raid superblock magic on %s\n"

#define BAD_MINOR KERN_ERR \
"md: %s: invalid raid minor (%x)\n"

#define OUT_OF_MEM KERN_ALERT \
"md: out of memory.\n"

#define NO_SB KERN_ERR \
"md: disabled device %s, could not read superblock.\n"

#define BAD_CSUM KERN_WARNING \
"md: invalid superblock checksum on %s\n"

static int alloc_disk_sb(mdk_rdev_t * rdev)
{
	if (rdev->sb_page)
		MD_BUG();

	rdev->sb_page = alloc_page(GFP_KERNEL);
	if (!rdev->sb_page) {
		printk(OUT_OF_MEM);
		return -EINVAL;
	}

	return 0;
}

static void free_disk_sb(mdk_rdev_t * rdev)
{
	if (rdev->sb_page) {
		page_cache_release(rdev->sb_page);
		rdev->sb_loaded = 0;
		rdev->sb_page = NULL;
		rdev->sb_offset = 0;
		rdev->size = 0;
	}
}


static int bi_complete(struct bio *bio, unsigned int bytes_done, int error)
{
	if (bio->bi_size)
		return 1;

	complete((struct completion*)bio->bi_private);
	return 0;
}

static int sync_page_io(struct block_device *bdev, sector_t sector, int size,
		   struct page *page, int rw)
{
	struct bio bio;
	struct bio_vec vec;
	struct completion event;

	bio_init(&bio);
	bio.bi_io_vec = &vec;
	vec.bv_page = page;
	vec.bv_len = size;
	vec.bv_offset = 0;
	bio.bi_vcnt = 1;
	bio.bi_idx = 0;
	bio.bi_size = size;
	bio.bi_bdev = bdev;
	bio.bi_sector = sector;
	init_completion(&event);
	bio.bi_private = &event;
	bio.bi_end_io = bi_complete;
	submit_bio(rw, &bio);
	blk_run_queues();
	wait_for_completion(&event);

	return test_bit(BIO_UPTODATE, &bio.bi_flags);
}

static int read_disk_sb(mdk_rdev_t * rdev)
{
	sector_t sb_offset;

	if (!rdev->sb_page) {
		MD_BUG();
		return -EINVAL;
	}
	if (rdev->sb_loaded)
		return 0;

	/*
	 * Calculate the position of the superblock,
	 * it's at the end of the disk.
	 *
	 * It also happens to be a multiple of 4Kb.
	 */
	sb_offset = calc_dev_sboffset(rdev->bdev);
	rdev->sb_offset = sb_offset;

	if (!sync_page_io(rdev->bdev, sb_offset<<1, MD_SB_BYTES, rdev->sb_page, READ))
		goto fail;
	rdev->sb_loaded = 1;
	return 0;

fail:
	printk(NO_SB,bdev_partition_name(rdev->bdev));
	return -EINVAL;
}

static int uuid_equal(mdp_super_t *sb1, mdp_super_t *sb2)
{
	if (	(sb1->set_uuid0 == sb2->set_uuid0) &&
		(sb1->set_uuid1 == sb2->set_uuid1) &&
		(sb1->set_uuid2 == sb2->set_uuid2) &&
		(sb1->set_uuid3 == sb2->set_uuid3))

		return 1;

	return 0;
}


static int sb_equal(mdp_super_t *sb1, mdp_super_t *sb2)
{
	int ret;
	mdp_super_t *tmp1, *tmp2;

	tmp1 = kmalloc(sizeof(*tmp1),GFP_KERNEL);
	tmp2 = kmalloc(sizeof(*tmp2),GFP_KERNEL);

	if (!tmp1 || !tmp2) {
		ret = 0;
		printk(KERN_INFO "md.c: sb1 is not equal to sb2!\n");
		goto abort;
	}

	*tmp1 = *sb1;
	*tmp2 = *sb2;

	/*
	 * nr_disks is not constant
	 */
	tmp1->nr_disks = 0;
	tmp2->nr_disks = 0;

	if (memcmp(tmp1, tmp2, MD_SB_GENERIC_CONSTANT_WORDS * 4))
		ret = 0;
	else
		ret = 1;

abort:
	if (tmp1)
		kfree(tmp1);
	if (tmp2)
		kfree(tmp2);

	return ret;
}

static unsigned int calc_sb_csum(mdp_super_t * sb)
{
	unsigned int disk_csum, csum;

	disk_csum = sb->sb_csum;
	sb->sb_csum = 0;
	csum = csum_partial((void *)sb, MD_SB_BYTES, 0);
	sb->sb_csum = disk_csum;
	return csum;
}

/*
 * Handle superblock details.
 * We want to be able to handle multiple superblock formats
 * so we have a common interface to them all, and an array of
 * different handlers.
 * We rely on user-space to write the initial superblock, and support
 * reading and updating of superblocks.
 * Interface methods are:
 *   int load_super(mdk_rdev_t *dev, mdk_rdev_t *refdev)
 *      loads and validates a superblock on dev.
 *      if refdev != NULL, compare superblocks on both devices
 *    Return:
 *      0 - dev has a superblock that is compatible with refdev
 *      1 - dev has a superblock that is compatible and newer than refdev
 *          so dev should be used as the refdev in future
 *     -EINVAL superblock incompatible or invalid
 *     -othererror e.g. -EIO
 *
 *   int validate_super(mddev_t *mddev, mdk_rdev_t *dev)
 *      Verify that dev is acceptable into mddev.
 *       The first time, mddev->raid_disks will be 0, and data from
 *       dev should be merged in.  Subsequent calls check that dev
 *       is new enough.  Return 0 or -EINVAL
 *
 *   void sync_super(mddev_t *mddev, mdk_rdev_t *dev)
 *     Update the superblock for rdev with data in mddev
 *     This does not write to disc.
 *
 */

struct super_type  {
	char 		*name;
	struct module	*owner;
	int		(*load_super)(mdk_rdev_t *rdev, mdk_rdev_t *refdev);
	int		(*validate_super)(mddev_t *mddev, mdk_rdev_t *rdev);
	void		(*sync_super)(mddev_t *mddev, mdk_rdev_t *rdev);
};

/*
 * load_super for 0.90.0 
 */
static int super_90_load(mdk_rdev_t *rdev, mdk_rdev_t *refdev)
{
	mdp_super_t *sb;
	int ret;

	ret = read_disk_sb(rdev);
	if (ret) return ret;

	ret = -EINVAL;

	sb = (mdp_super_t*)page_address(rdev->sb_page);

	if (sb->md_magic != MD_SB_MAGIC) {
		printk(BAD_MAGIC, bdev_partition_name(rdev->bdev));
		goto abort;
	}

	if (sb->major_version != 0 ||
	    sb->minor_version != 90) {
		printk(KERN_WARNING "Bad version number %d.%d on %s\n",
		       sb->major_version, sb->minor_version,
		       bdev_partition_name(rdev->bdev));
		goto abort;
	}

	if (sb->md_minor >= MAX_MD_DEVS) {
		printk(BAD_MINOR, bdev_partition_name(rdev->bdev), sb->md_minor);
		goto abort;
	}
	if (sb->raid_disks <= 0)
		goto abort;

	if (calc_sb_csum(sb) != sb->sb_csum) {
		printk(BAD_CSUM, bdev_partition_name(rdev->bdev));
		goto abort;
	}

	rdev->preferred_minor = sb->md_minor;

	if (refdev == 0)
		ret = 1;
	else {
		__u64 ev1, ev2;
		mdp_super_t *refsb = (mdp_super_t*)page_address(refdev->sb_page);
		if (!uuid_equal(refsb, sb)) {
			printk(KERN_WARNING "md: %s has different UUID to %s\n",
			       bdev_partition_name(rdev->bdev),
			       bdev_partition_name(refdev->bdev));
			goto abort;
		}
		if (!sb_equal(refsb, sb)) {
			printk(KERN_WARNING "md: %s has same UUID but different superblock to %s\n",
			       bdev_partition_name(rdev->bdev),
			       bdev_partition_name(refdev->bdev));
			goto abort;
		}
		ev1 = md_event(sb);
		ev2 = md_event(refsb);
		if (ev1 > ev2)
			ret = 1;
		else 
			ret = 0;
	}


 abort:
	return ret;
}

/*
 * validate_super for 0.90.0
 */
static int super_90_validate(mddev_t *mddev, mdk_rdev_t *rdev)
{
	mdp_disk_t *desc;
	mdp_super_t *sb = (mdp_super_t *)page_address(rdev->sb_page);

	if (mddev->raid_disks == 0) {
		mddev->major_version = sb->major_version;
		mddev->minor_version = sb->minor_version;
		mddev->patch_version = sb->patch_version;
		mddev->persistent = ! sb->not_persistent;
		mddev->chunk_size = sb->chunk_size;
		mddev->ctime = sb->ctime;
		mddev->utime = sb->utime;
		mddev->level = sb->level;
		mddev->layout = sb->layout;
		mddev->raid_disks = sb->raid_disks;
		mddev->size = sb->size;
		mddev->events = md_event(sb);

		if (sb->state & (1<<MD_SB_CLEAN))
			mddev->recovery_cp = MaxSector;
		else {
			if (sb->events_hi == sb->cp_events_hi && 
				sb->events_lo == sb->cp_events_lo) {
				mddev->recovery_cp = sb->recovery_cp;
			} else
				mddev->recovery_cp = 0;
		}

		memcpy(mddev->uuid+0, &sb->set_uuid0, 4);
		memcpy(mddev->uuid+4, &sb->set_uuid1, 4);
		memcpy(mddev->uuid+8, &sb->set_uuid2, 4);
		memcpy(mddev->uuid+12,&sb->set_uuid3, 4);

		mddev->max_disks = MD_SB_DISKS;
	} else {
		__u64 ev1;
		ev1 = md_event(sb);
		++ev1;
		if (ev1 < mddev->events) 
			return -EINVAL;
	}
	if (mddev->level != LEVEL_MULTIPATH) {
		rdev->desc_nr = sb->this_disk.number;
		rdev->raid_disk = -1;
		rdev->in_sync = rdev->faulty = 0;
		desc = sb->disks + rdev->desc_nr;

		if (desc->state & (1<<MD_DISK_FAULTY))
			rdev->faulty = 1;
		else if (desc->state & (1<<MD_DISK_SYNC) &&
			 desc->raid_disk < mddev->raid_disks) {
			rdev->in_sync = 1;
			rdev->raid_disk = desc->raid_disk;
		}
	}
	return 0;
}

/*
 * sync_super for 0.90.0
 */
static void super_90_sync(mddev_t *mddev, mdk_rdev_t *rdev)
{
	mdp_super_t *sb;
	struct list_head *tmp;
	mdk_rdev_t *rdev2;
	int next_spare = mddev->raid_disks;

	/* make rdev->sb match mddev data..
	 *
	 * 1/ zero out disks
	 * 2/ Add info for each disk, keeping track of highest desc_nr
	 * 3/ any empty disks < highest become removed
	 *
	 * disks[0] gets initialised to REMOVED because
	 * we cannot be sure from other fields if it has
	 * been initialised or not.
	 */
	int highest = 0;
	int i;
	int active=0, working=0,failed=0,spare=0,nr_disks=0;

	sb = (mdp_super_t*)page_address(rdev->sb_page);

	memset(sb, 0, sizeof(*sb));

	sb->md_magic = MD_SB_MAGIC;
	sb->major_version = mddev->major_version;
	sb->minor_version = mddev->minor_version;
	sb->patch_version = mddev->patch_version;
	sb->gvalid_words  = 0; /* ignored */
	memcpy(&sb->set_uuid0, mddev->uuid+0, 4);
	memcpy(&sb->set_uuid1, mddev->uuid+4, 4);
	memcpy(&sb->set_uuid2, mddev->uuid+8, 4);
	memcpy(&sb->set_uuid3, mddev->uuid+12,4);

	sb->ctime = mddev->ctime;
	sb->level = mddev->level;
	sb->size  = mddev->size;
	sb->raid_disks = mddev->raid_disks;
	sb->md_minor = mddev->__minor;
	sb->not_persistent = !mddev->persistent;
	sb->utime = mddev->utime;
	sb->state = 0;
	sb->events_hi = (mddev->events>>32);
	sb->events_lo = (u32)mddev->events;

	if (mddev->in_sync)
	{
		sb->recovery_cp = mddev->recovery_cp;
		sb->cp_events_hi = (mddev->events>>32);
		sb->cp_events_lo = (u32)mddev->events;
		if (mddev->recovery_cp == MaxSector) {
			printk(KERN_INFO "md: marking sb clean...\n");
			sb->state = (1<< MD_SB_CLEAN);
		}
	} else
		sb->recovery_cp = 0;

	sb->layout = mddev->layout;
	sb->chunk_size = mddev->chunk_size;

	sb->disks[0].state = (1<<MD_DISK_REMOVED);
	ITERATE_RDEV(mddev,rdev2,tmp) {
		mdp_disk_t *d;
		if (rdev2->raid_disk >= 0)
			rdev2->desc_nr = rdev2->raid_disk;
		else
			rdev2->desc_nr = next_spare++;
		d = &sb->disks[rdev2->desc_nr];
		nr_disks++;
		d->number = rdev2->desc_nr;
		d->major = MAJOR(rdev2->bdev->bd_dev);
		d->minor = MINOR(rdev2->bdev->bd_dev);
		if (rdev2->raid_disk >= 0)
			d->raid_disk = rdev2->raid_disk;
		else
			d->raid_disk = rdev2->desc_nr; /* compatibility */
		if (rdev2->faulty) {
			d->state = (1<<MD_DISK_FAULTY);
			failed++;
		} else if (rdev2->in_sync) {
			d->state = (1<<MD_DISK_ACTIVE);
			d->state |= (1<<MD_DISK_SYNC);
			active++;
			working++;
		} else {
			d->state = 0;
			spare++;
			working++;
		}
		if (rdev2->desc_nr > highest)
			highest = rdev2->desc_nr;
	}
	
	/* now set the "removed" bit on any non-trailing holes */
	for (i=0; i<highest; i++) {
		mdp_disk_t *d = &sb->disks[i];
		if (d->state == 0 && d->number == 0) {
			d->number = i;
			d->raid_disk = i;
			d->state = (1<<MD_DISK_REMOVED);
		}
	}
	sb->nr_disks = nr_disks;
	sb->active_disks = active;
	sb->working_disks = working;
	sb->failed_disks = failed;
	sb->spare_disks = spare;

	sb->this_disk = sb->disks[rdev->desc_nr];
	sb->sb_csum = calc_sb_csum(sb);
}

struct super_type super_types[] = {
	[0] = {
		.name	= "0.90.0",
		.owner	= THIS_MODULE,
		.load_super	= super_90_load,
		.validate_super	= super_90_validate,
		.sync_super	= super_90_sync,
	},
};


	
static mdk_rdev_t * match_dev_unit(mddev_t *mddev, mdk_rdev_t *dev)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev,rdev,tmp)
		if (rdev->bdev->bd_contains == dev->bdev->bd_contains)
			return rdev;

	return NULL;
}

static int match_mddev_units(mddev_t *mddev1, mddev_t *mddev2)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev1,rdev,tmp)
		if (match_dev_unit(mddev2, rdev))
			return 1;

	return 0;
}

static LIST_HEAD(pending_raid_disks);

static void bind_rdev_to_array(mdk_rdev_t * rdev, mddev_t * mddev)
{
	mdk_rdev_t *same_pdev;

	if (rdev->mddev) {
		MD_BUG();
		return;
	}
	same_pdev = match_dev_unit(mddev, rdev);
	if (same_pdev)
		printk( KERN_WARNING
"md%d: WARNING: %s appears to be on the same physical disk as %s. True\n"
"     protection against single-disk failure might be compromised.\n",
			mdidx(mddev), bdev_partition_name(rdev->bdev),
				bdev_partition_name(same_pdev->bdev));

	list_add(&rdev->same_set, &mddev->disks);
	rdev->mddev = mddev;
	printk(KERN_INFO "md: bind<%s>\n", bdev_partition_name(rdev->bdev));
}

static void unbind_rdev_from_array(mdk_rdev_t * rdev)
{
	if (!rdev->mddev) {
		MD_BUG();
		return;
	}
	list_del_init(&rdev->same_set);
	printk(KERN_INFO "md: unbind<%s>\n", bdev_partition_name(rdev->bdev));
	rdev->mddev = NULL;
}

/*
 * prevent the device from being mounted, repartitioned or
 * otherwise reused by a RAID array (or any other kernel
 * subsystem), by opening the device. [simply getting an
 * inode is not enough, the SCSI module usage code needs
 * an explicit open() on the device]
 */
static int lock_rdev(mdk_rdev_t *rdev, dev_t dev)
{
	int err = 0;
	struct block_device *bdev;

	bdev = bdget(dev);
	if (!bdev)
		return -ENOMEM;
	err = blkdev_get(bdev, FMODE_READ|FMODE_WRITE, 0, BDEV_RAW);
	if (err)
		return err;
	err = bd_claim(bdev, rdev);
	if (err) {
		blkdev_put(bdev, BDEV_RAW);
		return err;
	}
	rdev->bdev = bdev;
	return err;
}

static void unlock_rdev(mdk_rdev_t *rdev)
{
	struct block_device *bdev = rdev->bdev;
	rdev->bdev = NULL;
	if (!bdev)
		MD_BUG();
	bd_release(bdev);
	blkdev_put(bdev, BDEV_RAW);
}

void md_autodetect_dev(dev_t dev);

static void export_rdev(mdk_rdev_t * rdev)
{
	printk(KERN_INFO "md: export_rdev(%s)\n",bdev_partition_name(rdev->bdev));
	if (rdev->mddev)
		MD_BUG();
	free_disk_sb(rdev);
	list_del_init(&rdev->same_set);
#ifndef MODULE
	md_autodetect_dev(rdev->bdev->bd_dev);
#endif
	unlock_rdev(rdev);
	kfree(rdev);
}

static void kick_rdev_from_array(mdk_rdev_t * rdev)
{
	unbind_rdev_from_array(rdev);
	export_rdev(rdev);
}

static void export_array(mddev_t *mddev)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (!rdev->mddev) {
			MD_BUG();
			continue;
		}
		kick_rdev_from_array(rdev);
	}
	if (!list_empty(&mddev->disks))
		MD_BUG();
	mddev->raid_disks = 0;
}

#undef BAD_CSUM
#undef BAD_MAGIC
#undef OUT_OF_MEM
#undef NO_SB

static void print_desc(mdp_disk_t *desc)
{
	printk(" DISK<N:%d,%s(%d,%d),R:%d,S:%d>\n", desc->number,
		partition_name(MKDEV(desc->major,desc->minor)),
		desc->major,desc->minor,desc->raid_disk,desc->state);
}

static void print_sb(mdp_super_t *sb)
{
	int i;

	printk(KERN_INFO "md:  SB: (V:%d.%d.%d) ID:<%08x.%08x.%08x.%08x> CT:%08x\n",
		sb->major_version, sb->minor_version, sb->patch_version,
		sb->set_uuid0, sb->set_uuid1, sb->set_uuid2, sb->set_uuid3,
		sb->ctime);
	printk(KERN_INFO "md:     L%d S%08d ND:%d RD:%d md%d LO:%d CS:%d\n", sb->level,
		sb->size, sb->nr_disks, sb->raid_disks, sb->md_minor,
		sb->layout, sb->chunk_size);
	printk(KERN_INFO "md:     UT:%08x ST:%d AD:%d WD:%d FD:%d SD:%d CSUM:%08x E:%08lx\n",
		sb->utime, sb->state, sb->active_disks, sb->working_disks,
		sb->failed_disks, sb->spare_disks,
		sb->sb_csum, (unsigned long)sb->events_lo);

	printk(KERN_INFO);
	for (i = 0; i < MD_SB_DISKS; i++) {
		mdp_disk_t *desc;

		desc = sb->disks + i;
		if (desc->number || desc->major || desc->minor ||
		    desc->raid_disk || (desc->state && (desc->state != 4))) {
			printk("     D %2d: ", i);
			print_desc(desc);
		}
	}
	printk(KERN_INFO "md:     THIS: ");
	print_desc(&sb->this_disk);

}

static void print_rdev(mdk_rdev_t *rdev)
{
	printk(KERN_INFO "md: rdev %s, SZ:%08llu F:%d S:%d DN:%d ",
		bdev_partition_name(rdev->bdev),
		(unsigned long long)rdev->size, rdev->faulty, rdev->in_sync, rdev->desc_nr);
	if (rdev->sb_loaded) {
		printk(KERN_INFO "md: rdev superblock:\n");
		print_sb((mdp_super_t*)page_address(rdev->sb_page));
	} else
		printk(KERN_INFO "md: no rdev superblock!\n");
}

void md_print_devices(void)
{
	struct list_head *tmp, *tmp2;
	mdk_rdev_t *rdev;
	mddev_t *mddev;

	printk("\n");
	printk("md:	**********************************\n");
	printk("md:	* <COMPLETE RAID STATE PRINTOUT> *\n");
	printk("md:	**********************************\n");
	ITERATE_MDDEV(mddev,tmp) {
		printk("md%d: ", mdidx(mddev));

		ITERATE_RDEV(mddev,rdev,tmp2)
			printk("<%s>", bdev_partition_name(rdev->bdev));

		ITERATE_RDEV(mddev,rdev,tmp2)
			print_rdev(rdev);
	}
	printk("md:	**********************************\n");
	printk("\n");
}


static int write_disk_sb(mdk_rdev_t * rdev)
{
	sector_t sb_offset;
	sector_t size;

	if (!rdev->sb_loaded) {
		MD_BUG();
		return 1;
	}
	if (rdev->faulty) {
		MD_BUG();
		return 1;
	}

	sb_offset = calc_dev_sboffset(rdev->bdev);
	if (rdev->sb_offset != sb_offset) {
		printk(KERN_INFO "%s's sb offset has changed from %llu to %llu, skipping\n",
		       bdev_partition_name(rdev->bdev), 
		    (unsigned long long)rdev->sb_offset, 
		    (unsigned long long)sb_offset);
		goto skip;
	}
	/*
	 * If the disk went offline meanwhile and it's just a spare, then
	 * its size has changed to zero silently, and the MD code does
	 * not yet know that it's faulty.
	 */
	size = calc_dev_size(rdev->bdev, rdev->mddev);
	if (size != rdev->size) {
		printk(KERN_INFO "%s's size has changed from %llu to %llu since import, skipping\n",
		       bdev_partition_name(rdev->bdev),
		       (unsigned long long)rdev->size, 
		       (unsigned long long)size);
		goto skip;
	}

	printk(KERN_INFO "(write) %s's sb offset: %llu\n", bdev_partition_name(rdev->bdev), (unsigned long long)sb_offset);

	if (!sync_page_io(rdev->bdev, sb_offset<<1, MD_SB_BYTES, rdev->sb_page, WRITE))
		goto fail;
skip:
	return 0;
fail:
	printk("md: write_disk_sb failed for device %s\n", bdev_partition_name(rdev->bdev));
	return 1;
}

static void sync_sbs(mddev_t * mddev)
{
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	ITERATE_RDEV(mddev,rdev,tmp) {
		super_90_sync(mddev, rdev);
		rdev->sb_loaded = 1;
	}
}

static void md_update_sb(mddev_t * mddev)
{
	int err, count = 100;
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	mddev->sb_dirty = 0;
repeat:
	mddev->utime = get_seconds();
	mddev->events ++;

	if (!mddev->events) {
		/*
		 * oops, this 64-bit counter should never wrap.
		 * Either we are in around ~1 trillion A.C., assuming
		 * 1 reboot per second, or we have a bug:
		 */
		MD_BUG();
		mddev->events --;
	}
	sync_sbs(mddev);

	/*
	 * do not write anything to disk if using
	 * nonpersistent superblocks
	 */
	if (!mddev->persistent)
		return;

	printk(KERN_INFO "md: updating md%d RAID superblock on device (in sync %d)\n",
					mdidx(mddev),mddev->in_sync);

	err = 0;
	ITERATE_RDEV(mddev,rdev,tmp) {
		printk(KERN_INFO "md: ");
		if (rdev->faulty)
			printk("(skipping faulty ");

		printk("%s ", bdev_partition_name(rdev->bdev));
		if (!rdev->faulty) {
			err += write_disk_sb(rdev);
		} else
			printk(")\n");
		if (!err && mddev->level == LEVEL_MULTIPATH)
			/* only need to write one superblock... */
			break;
	}
	if (err) {
		if (--count) {
			printk(KERN_ERR "md: errors occurred during superblock update, repeating\n");
			goto repeat;
		}
		printk(KERN_ERR "md: excessive errors occurred during superblock update, exiting\n");
	}
}

/*
 * Import a device. If 'on_disk', then sanity check the superblock
 *
 * mark the device faulty if:
 *
 *   - the device is nonexistent (zero size)
 *   - the device has no valid superblock
 *
 * a faulty rdev _never_ has rdev->sb set.
 */
static mdk_rdev_t *md_import_device(dev_t newdev, int on_disk)
{
	int err;
	mdk_rdev_t *rdev;
	sector_t size;

	rdev = (mdk_rdev_t *) kmalloc(sizeof(*rdev), GFP_KERNEL);
	if (!rdev) {
		printk(KERN_ERR "md: could not alloc mem for %s!\n", partition_name(newdev));
		return ERR_PTR(-ENOMEM);
	}
	memset(rdev, 0, sizeof(*rdev));

	if ((err = alloc_disk_sb(rdev)))
		goto abort_free;

	err = lock_rdev(rdev, newdev);
	if (err) {
		printk(KERN_ERR "md: could not lock %s.\n",
			partition_name(newdev));
		goto abort_free;
	}
	rdev->desc_nr = -1;
	rdev->faulty = 0;
	rdev->in_sync = 0;
	atomic_set(&rdev->nr_pending, 0);

	size = rdev->bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;
	if (!size) {
		printk(KERN_WARNING
		       "md: %s has zero or unknown size, marking faulty!\n",
		       bdev_partition_name(rdev->bdev));
		err = -EINVAL;
		goto abort_free;
	}

	if (on_disk) {
		err = super_90_load(rdev, NULL);
		if (err == -EINVAL) {
			printk(KERN_WARNING "md: %s has invalid sb, not importing!\n",
			       bdev_partition_name(rdev->bdev));
			goto abort_free;
		}
		if (err < 0) {
			printk(KERN_WARNING "md: could not read %s's sb, not importing!\n",
			       bdev_partition_name(rdev->bdev));
			goto abort_free;
		}
	}
	INIT_LIST_HEAD(&rdev->same_set);

	return rdev;

abort_free:
	if (rdev->sb_page) {
		if (rdev->bdev)
			unlock_rdev(rdev);
		free_disk_sb(rdev);
	}
	kfree(rdev);
	return ERR_PTR(err);
}

/*
 * Check a full RAID array for plausibility
 */

#define INCONSISTENT KERN_ERR \
"md: fatal superblock inconsistency in %s -- removing from array\n"

#define OUT_OF_DATE KERN_ERR \
"md: superblock update time inconsistency -- using the most recent one\n"

#define OLD_VERSION KERN_ALERT \
"md: md%d: unsupported raid array version %d.%d.%d\n"

#define NOT_CLEAN_IGNORE KERN_ERR \
"md: md%d: raid array is not clean -- starting background reconstruction\n"

#define UNKNOWN_LEVEL KERN_ERR \
"md: md%d: unsupported raid level %d\n"

static int analyze_sbs(mddev_t * mddev)
{
	int i;
	struct list_head *tmp;
	mdk_rdev_t *rdev, *freshest;

	freshest = NULL;
	ITERATE_RDEV(mddev,rdev,tmp)
		switch (super_90_load(rdev, freshest)) {
		case 1:
			freshest = rdev;
			break;
		case 0:
			break;
		default:
			printk(INCONSISTENT, bdev_partition_name(rdev->bdev));
			kick_rdev_from_array(rdev);
		}


	super_90_validate(mddev, freshest);

	i = 0;
	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev != freshest)
			if (super_90_validate(mddev, rdev)) {
				printk(KERN_WARNING "md: kicking non-fresh %s from array!\n",
				       bdev_partition_name(rdev->bdev));
				kick_rdev_from_array(rdev);
				continue;
			}
		if (mddev->level == LEVEL_MULTIPATH) {
			rdev->desc_nr = i++;
			rdev->raid_disk = rdev->desc_nr;
			rdev->in_sync = 1;
		}
	}


	/*
	 * Check if we can support this RAID array
	 */
	if (mddev->major_version != MD_MAJOR_VERSION ||
			mddev->minor_version > MD_MINOR_VERSION) {

		printk(OLD_VERSION, mdidx(mddev), mddev->major_version,
				mddev->minor_version, mddev->patch_version);
		goto abort;
	}

	if ((mddev->recovery_cp != MaxSector) && ((mddev->level == 1) ||
			(mddev->level == 4) || (mddev->level == 5)))
		printk(NOT_CLEAN_IGNORE, mdidx(mddev));

	return 0;
abort:
	return 1;
}

#undef INCONSISTENT
#undef OUT_OF_DATE
#undef OLD_VERSION
#undef OLD_LEVEL

static int device_size_calculation(mddev_t * mddev)
{
	int data_disks = 0;
	unsigned int readahead;
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	/*
	 * Do device size calculation. Bail out if too small.
	 * (we have to do this after having validated chunk_size,
	 * because device size has to be modulo chunk_size)
	 */

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev->faulty)
			continue;
		if (rdev->size) {
			MD_BUG();
			continue;
		}
		rdev->size = calc_dev_size(rdev->bdev, mddev);
		if (rdev->size < mddev->chunk_size / 1024) {
			printk(KERN_WARNING
				"md: Dev %s smaller than chunk_size: %lluk < %dk\n",
				bdev_partition_name(rdev->bdev),
				(unsigned long long)rdev->size, mddev->chunk_size / 1024);
			return -EINVAL;
		}
	}

	switch (mddev->level) {
		case LEVEL_MULTIPATH:
			data_disks = 1;
			break;
		case -3:
			data_disks = 1;
			break;
		case -2:
			data_disks = 1;
			break;
		case LEVEL_LINEAR:
			zoned_raid_size(mddev);
			data_disks = 1;
			break;
		case 0:
			zoned_raid_size(mddev);
			data_disks = mddev->raid_disks;
			break;
		case 1:
			data_disks = 1;
			break;
		case 4:
		case 5:
			data_disks = mddev->raid_disks-1;
			break;
		default:
			printk(UNKNOWN_LEVEL, mdidx(mddev), mddev->level);
			goto abort;
	}
	if (!md_size[mdidx(mddev)])
		md_size[mdidx(mddev)] = mddev->size * data_disks;

	readahead = (VM_MAX_READAHEAD * 1024) / PAGE_SIZE;
	if (!mddev->level || (mddev->level == 4) || (mddev->level == 5)) {
		readahead = (mddev->chunk_size>>PAGE_SHIFT) * 4 * data_disks;
		if (readahead < data_disks * (MAX_SECTORS>>(PAGE_SHIFT-9))*2)
			readahead = data_disks * (MAX_SECTORS>>(PAGE_SHIFT-9))*2;
	} else {
		// (no multipath branch - it uses the default setting)
		if (mddev->level == -3)
			readahead = 0;
	}

	printk(KERN_INFO "md%d: max total readahead window set to %ldk\n",
		mdidx(mddev), readahead*(PAGE_SIZE/1024));

	printk(KERN_INFO
		"md%d: %d data-disks, max readahead per data-disk: %ldk\n",
			mdidx(mddev), data_disks, readahead/data_disks*(PAGE_SIZE/1024));
	return 0;
abort:
	return 1;
}

static struct gendisk *md_probe(dev_t dev, int *part, void *data)
{
	static DECLARE_MUTEX(disks_sem);
	int unit = MINOR(dev);
	mddev_t *mddev = mddev_find(unit);
	struct gendisk *disk;

	if (!mddev)
		return NULL;

	down(&disks_sem);
	if (disks[unit]) {
		up(&disks_sem);
		mddev_put(mddev);
		return NULL;
	}
	disk = alloc_disk(1);
	if (!disk) {
		up(&disks_sem);
		mddev_put(mddev);
		return NULL;
	}
	disk->major = MD_MAJOR;
	disk->first_minor = mdidx(mddev);
	sprintf(disk->disk_name, "md%d", mdidx(mddev));
	disk->fops = &md_fops;
	disk->private_data = mddev;
	disk->queue = &mddev->queue;
	add_disk(disk);
	disks[mdidx(mddev)] = disk;
	up(&disks_sem);
	return NULL;
}

#define TOO_BIG_CHUNKSIZE KERN_ERR \
"too big chunk_size: %d > %d\n"

#define TOO_SMALL_CHUNKSIZE KERN_ERR \
"too small chunk_size: %d < %ld\n"

#define BAD_CHUNKSIZE KERN_ERR \
"no chunksize specified, see 'man raidtab'\n"

static int do_md_run(mddev_t * mddev)
{
	int pnum, err;
	int chunk_size;
	struct list_head *tmp;
	mdk_rdev_t *rdev;
	struct gendisk *disk;

	if (list_empty(&mddev->disks)) {
		MD_BUG();
		return -EINVAL;
	}

	if (mddev->pers)
		return -EBUSY;

	/*
	 * Resize disks to align partitions size on a given
	 * chunk size.
	 */
	md_size[mdidx(mddev)] = 0;

	/*
	 * Analyze all RAID superblock(s)
	 */
	if (!mddev->raid_disks && analyze_sbs(mddev)) {
		MD_BUG();
		return -EINVAL;
	}

	chunk_size = mddev->chunk_size;
	pnum = level_to_pers(mddev->level);

	if ((pnum != MULTIPATH) && (pnum != RAID1)) {
		if (!chunk_size) {
			/*
			 * 'default chunksize' in the old md code used to
			 * be PAGE_SIZE, baaad.
			 * we abort here to be on the safe side. We dont
			 * want to continue the bad practice.
			 */
			printk(BAD_CHUNKSIZE);
			return -EINVAL;
		}
		if (chunk_size > MAX_CHUNK_SIZE) {
			printk(TOO_BIG_CHUNKSIZE, chunk_size, MAX_CHUNK_SIZE);
			return -EINVAL;
		}
		/*
		 * chunk-size has to be a power of 2 and multiples of PAGE_SIZE
		 */
		if ( (1 << ffz(~chunk_size)) != chunk_size) {
			MD_BUG();
			return -EINVAL;
		}
		if (chunk_size < PAGE_SIZE) {
			printk(TOO_SMALL_CHUNKSIZE, chunk_size, PAGE_SIZE);
			return -EINVAL;
		}
	}

	if (pnum >= MAX_PERSONALITY) {
		MD_BUG();
		return -EINVAL;
	}

	if (!pers[pnum])
	{
#ifdef CONFIG_KMOD
		char module_name[80];
		sprintf (module_name, "md-personality-%d", pnum);
		request_module (module_name);
		if (!pers[pnum])
#endif
		{
			printk(KERN_ERR "md: personality %d is not loaded!\n",
				pnum);
			return -EINVAL;
		}
	}

	if (device_size_calculation(mddev))
		return -EINVAL;

	/*
	 * Drop all container device buffers, from now on
	 * the only valid external interface is through the md
	 * device.
	 * Also find largest hardsector size
	 */
	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev->faulty)
			continue;
		sync_blockdev(rdev->bdev);
		invalidate_bdev(rdev->bdev, 0);
#if 0
	/*
	 * Aside of obvious breakage (code below results in block size set
	 * according to the sector size of last component instead of the
	 * maximal sector size), we have more interesting problem here.
	 * Namely, we actually ought to set _sector_ size for the array
	 * and that requires per-array request queues.  Disabled for now.
	 */
		md_blocksizes[mdidx(mddev)] = 1024;
		if (bdev_hardsect_size(rdev->bdev) > md_blocksizes[mdidx(mddev)])
			md_blocksizes[mdidx(mddev)] = bdev_hardsect_size(rdev->bdev);
#endif
	}

	md_probe(mdidx(mddev), NULL, NULL);
	disk = disks[mdidx(mddev)];
	if (!disk)
		return -ENOMEM;
	mddev->pers = pers[pnum];

	blk_queue_make_request(&mddev->queue, mddev->pers->make_request);
	printk("%s: setting max_sectors to %d, segment boundary to %d\n",
	       disk->disk_name,
	       chunk_size >> 9,
	       (chunk_size>>1)-1);
	blk_queue_max_sectors(&mddev->queue, chunk_size >> 9);
	blk_queue_segment_boundary(&mddev->queue, (chunk_size>>1) - 1);
	mddev->queue.queuedata = mddev;

	err = mddev->pers->run(mddev);
	if (err) {
		printk(KERN_ERR "md: pers->run() failed ...\n");
		mddev->pers = NULL;
		return -EINVAL;
	}
 	atomic_set(&mddev->writes_pending,0);
	mddev->safemode = 0;
	if (mddev->pers->sync_request)
		mddev->in_sync = 0;
	else
		mddev->in_sync = 1;
	
	md_update_sb(mddev);
	md_recover_arrays();
	set_capacity(disk, md_size[mdidx(mddev)]<<1);
	return (0);
}

#undef TOO_BIG_CHUNKSIZE
#undef BAD_CHUNKSIZE

static int restart_array(mddev_t *mddev)
{
	struct gendisk *disk = disks[mdidx(mddev)];
	int err;

	/*
	 * Complain if it has no devices
	 */
	err = -ENXIO;
	if (list_empty(&mddev->disks))
		goto out;

	if (mddev->pers) {
		err = -EBUSY;
		if (!mddev->ro)
			goto out;

		mddev->safemode = 0;
		mddev->in_sync = 0;
		md_update_sb(mddev);
		mddev->ro = 0;
		set_disk_ro(disk, 0);

		printk(KERN_INFO
			"md: md%d switched to read-write mode.\n", mdidx(mddev));
		/*
		 * Kick recovery or resync if necessary
		 */
		md_recover_arrays();
		err = 0;
	} else {
		printk(KERN_ERR "md: md%d has no personality assigned.\n",
			mdidx(mddev));
		err = -EINVAL;
	}

out:
	return err;
}

#define STILL_MOUNTED KERN_WARNING \
"md: md%d still mounted.\n"
#define	STILL_IN_USE \
"md: md%d still in use.\n"

static int do_md_stop(mddev_t * mddev, int ro)
{
	int err = 0;
	struct gendisk *disk = disks[mdidx(mddev)];

	if (atomic_read(&mddev->active)>2) {
		printk(STILL_IN_USE, mdidx(mddev));
		err = -EBUSY;
		goto out;
	}

	if (mddev->pers) {
		if (mddev->sync_thread) {
			if (mddev->recovery_running > 0)
				mddev->recovery_running = -1;
			md_unregister_thread(mddev->sync_thread);
			mddev->sync_thread = NULL;
		}

		invalidate_device(mk_kdev(disk->major, disk->first_minor), 1);

		if (ro) {
			err  = -ENXIO;
			if (mddev->ro)
				goto out;
			mddev->ro = 1;
		} else {
			if (mddev->ro)
				set_disk_ro(disk, 0);
			if (mddev->pers->stop(mddev)) {
				err = -EBUSY;
				if (mddev->ro)
					set_disk_ro(disk, 1);
				goto out;
			}
			mddev->pers = NULL;
			if (mddev->ro)
				mddev->ro = 0;
		}
		if (mddev->raid_disks) {
			/* mark array as shutdown cleanly */
			mddev->in_sync = 1;
			md_update_sb(mddev);
		}
		if (ro)
			set_disk_ro(disk, 1);
	}
	/*
	 * Free resources if final stop
	 */
	if (!ro) {
		struct gendisk *disk;
		printk(KERN_INFO "md: md%d stopped.\n", mdidx(mddev));

		export_array(mddev);

		md_size[mdidx(mddev)] = 0;
		disk = disks[mdidx(mddev)];
		if (disk)
			set_capacity(disk, 0);
	} else
		printk(KERN_INFO "md: md%d switched to read-only mode.\n", mdidx(mddev));
	err = 0;
out:
	return err;
}

static void autorun_array(mddev_t *mddev)
{
	mdk_rdev_t *rdev;
	struct list_head *tmp;
	int err;

	if (list_empty(&mddev->disks)) {
		MD_BUG();
		return;
	}

	printk(KERN_INFO "md: running: ");

	ITERATE_RDEV(mddev,rdev,tmp) {
		printk("<%s>", bdev_partition_name(rdev->bdev));
	}
	printk("\n");

	err = do_md_run (mddev);
	if (err) {
		printk(KERN_WARNING "md :do_md_run() returned %d\n", err);
		do_md_stop (mddev, 0);
	}
}

/*
 * lets try to run arrays based on all disks that have arrived
 * until now. (those are in pending_raid_disks)
 *
 * the method: pick the first pending disk, collect all disks with
 * the same UUID, remove all from the pending list and put them into
 * the 'same_array' list. Then order this list based on superblock
 * update time (freshest comes first), kick out 'old' disks and
 * compare superblocks. If everything's fine then run it.
 *
 * If "unit" is allocated, then bump its reference count
 */
static void autorun_devices(void)
{
	struct list_head candidates;
	struct list_head *tmp;
	mdk_rdev_t *rdev0, *rdev;
	mddev_t *mddev;

	printk(KERN_INFO "md: autorun ...\n");
	while (!list_empty(&pending_raid_disks)) {
		rdev0 = list_entry(pending_raid_disks.next,
					 mdk_rdev_t, same_set);

		printk(KERN_INFO "md: considering %s ...\n", bdev_partition_name(rdev0->bdev));
		INIT_LIST_HEAD(&candidates);
		ITERATE_RDEV_PENDING(rdev,tmp)
			if (super_90_load(rdev, rdev0) >= 0) {
				printk(KERN_INFO "md:  adding %s ...\n", bdev_partition_name(rdev->bdev));
				list_move(&rdev->same_set, &candidates);
			}
		/*
		 * now we have a set of devices, with all of them having
		 * mostly sane superblocks. It's time to allocate the
		 * mddev.
		 */

		mddev = mddev_find(rdev0->preferred_minor);
		if (!mddev) {
			printk(KERN_ERR "md: cannot allocate memory for md drive.\n");
			break;
		}
		if (mddev_lock(mddev)) 
			printk(KERN_WARNING "md: md%d locked, cannot run\n",
			       mdidx(mddev));
		else if (mddev->raid_disks || !list_empty(&mddev->disks)) {
			printk(KERN_WARNING "md: md%d already running, cannot run %s\n",
			       mdidx(mddev), bdev_partition_name(rdev0->bdev));
			mddev_unlock(mddev);
		} else {
			printk(KERN_INFO "md: created md%d\n", mdidx(mddev));
			ITERATE_RDEV_GENERIC(candidates,rdev,tmp) {
				list_del_init(&rdev->same_set);
				bind_rdev_to_array(rdev, mddev);
			}
			autorun_array(mddev);
			mddev_unlock(mddev);
		}
		/* on success, candidates will be empty, on error
		 * it wont...
		 */
		ITERATE_RDEV_GENERIC(candidates,rdev,tmp)
			export_rdev(rdev);
		mddev_put(mddev);
	}
	printk(KERN_INFO "md: ... autorun DONE.\n");
}

/*
 * import RAID devices based on one partition
 * if possible, the array gets run as well.
 */

#define BAD_VERSION KERN_ERR \
"md: %s has RAID superblock version 0.%d, autodetect needs v0.90 or higher\n"

#define OUT_OF_MEM KERN_ALERT \
"md: out of memory.\n"

#define NO_DEVICE KERN_ERR \
"md: disabled device %s\n"

#define AUTOADD_FAILED KERN_ERR \
"md: auto-adding devices to md%d FAILED (error %d).\n"

#define AUTOADD_FAILED_USED KERN_ERR \
"md: cannot auto-add device %s to md%d, already used.\n"

#define AUTORUN_FAILED KERN_ERR \
"md: auto-running md%d FAILED (error %d).\n"

#define MDDEV_BUSY KERN_ERR \
"md: cannot auto-add to md%d, already running.\n"

#define AUTOADDING KERN_INFO \
"md: auto-adding devices to md%d, based on %s's superblock.\n"

#define AUTORUNNING KERN_INFO \
"md: auto-running md%d.\n"

static int autostart_array(dev_t startdev)
{
	int err = -EINVAL, i;
	mdp_super_t *sb = NULL;
	mdk_rdev_t *start_rdev = NULL, *rdev;

	start_rdev = md_import_device(startdev, 1);
	if (IS_ERR(start_rdev)) {
		printk(KERN_WARNING "md: could not import %s!\n", partition_name(startdev));
		return err;
	}

	/* NOTE: this can only work for 0.90.0 superblocks */
	sb = (mdp_super_t*)page_address(start_rdev->sb_page);
	if (sb->major_version != 0 ||
	    sb->minor_version != 90 ) {
		printk(KERN_WARNING "md: can only autostart 0.90.0 arrays\n");
		export_rdev(start_rdev);
		return err;
	}

	if (start_rdev->faulty) {
		printk(KERN_WARNING "md: can not autostart based on faulty %s!\n",
						bdev_partition_name(start_rdev->bdev));
		export_rdev(start_rdev);
		return err;
	}
	list_add(&start_rdev->same_set, &pending_raid_disks);

	for (i = 0; i < MD_SB_DISKS; i++) {
		mdp_disk_t *desc;
		dev_t dev;

		desc = sb->disks + i;
		dev = MKDEV(desc->major, desc->minor);

		if (!dev)
			continue;
		if (dev == startdev)
			continue;
		rdev = md_import_device(dev, 1);
		if (IS_ERR(rdev)) {
			printk(KERN_WARNING "md: could not import %s, trying to run array nevertheless.\n",
			       partition_name(dev));
			continue;
		}
		list_add(&rdev->same_set, &pending_raid_disks);
	}

	/*
	 * possibly return codes
	 */
	autorun_devices();
	return 0;

}

#undef BAD_VERSION
#undef OUT_OF_MEM
#undef NO_DEVICE
#undef AUTOADD_FAILED_USED
#undef AUTOADD_FAILED
#undef AUTORUN_FAILED
#undef AUTOADDING
#undef AUTORUNNING


static int get_version(void * arg)
{
	mdu_version_t ver;

	ver.major = MD_MAJOR_VERSION;
	ver.minor = MD_MINOR_VERSION;
	ver.patchlevel = MD_PATCHLEVEL_VERSION;

	if (copy_to_user(arg, &ver, sizeof(ver)))
		return -EFAULT;

	return 0;
}

static int get_array_info(mddev_t * mddev, void * arg)
{
	mdu_array_info_t info;
	int nr,working,active,failed,spare;
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	nr=working=active=failed=spare=0;
	ITERATE_RDEV(mddev,rdev,tmp) {
		nr++;
		if (rdev->faulty)
			failed++;
		else {
			working++;
			if (rdev->in_sync)
				active++;	
			else
				spare++;
		}
	}

	info.major_version = mddev->major_version;
	info.major_version = mddev->major_version;
	info.minor_version = mddev->minor_version;
	info.patch_version = mddev->patch_version;
	info.ctime         = mddev->ctime;
	info.level         = mddev->level;
	info.size          = mddev->size;
	info.nr_disks      = nr;
	info.raid_disks    = mddev->raid_disks;
	info.md_minor      = mddev->__minor;
	info.not_persistent= !mddev->persistent;

	info.utime         = mddev->utime;
	info.state         = 0;
	if (mddev->recovery_cp == MaxSector)
		info.state = (1<<MD_SB_CLEAN);
	info.active_disks  = active;
	info.working_disks = working;
	info.failed_disks  = failed;
	info.spare_disks   = spare;

	info.layout        = mddev->layout;
	info.chunk_size    = mddev->chunk_size;

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}
#undef SET_FROM_SB


static int get_disk_info(mddev_t * mddev, void * arg)
{
	mdu_disk_info_t info;
	unsigned int nr;
	mdk_rdev_t *rdev;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	nr = info.number;

	rdev = find_rdev_nr(mddev, nr);
	if (rdev) {
		info.major = MAJOR(rdev->bdev->bd_dev);
		info.minor = MINOR(rdev->bdev->bd_dev);
		info.raid_disk = rdev->raid_disk;
		info.state = 0;
		if (rdev->faulty)
			info.state |= (1<<MD_DISK_FAULTY);
		else if (rdev->in_sync) {
			info.state |= (1<<MD_DISK_ACTIVE);
			info.state |= (1<<MD_DISK_SYNC);
		}
	} else {
		info.major = info.minor = 0;
		info.raid_disk = -1;
		info.state = (1<<MD_DISK_REMOVED);
	}

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int add_new_disk(mddev_t * mddev, mdu_disk_info_t *info)
{
	sector_t size;
	mdk_rdev_t *rdev;
	dev_t dev;
	dev = MKDEV(info->major,info->minor);
	if (!mddev->raid_disks) {
		/* expecting a device which has a superblock */
		rdev = md_import_device(dev, 1);
		if (IS_ERR(rdev)) {
			printk(KERN_WARNING "md: md_import_device returned %ld\n", PTR_ERR(rdev));
			return PTR_ERR(rdev);
		}
		if (!list_empty(&mddev->disks)) {
			mdk_rdev_t *rdev0 = list_entry(mddev->disks.next,
							mdk_rdev_t, same_set);
			int err = super_90_load(rdev, rdev0);
			if (err < 0) {
				printk(KERN_WARNING "md: %s has different UUID to %s\n",
				       bdev_partition_name(rdev->bdev), bdev_partition_name(rdev0->bdev));
				export_rdev(rdev);
				return -EINVAL;
			}
		}
		bind_rdev_to_array(rdev, mddev);
		return 0;
	}

	if (!(info->state & (1<<MD_DISK_FAULTY))) {
		rdev = md_import_device (dev, 0);
		if (IS_ERR(rdev)) {
			printk(KERN_WARNING "md: error, md_import_device() returned %ld\n", PTR_ERR(rdev));
			return PTR_ERR(rdev);
		}
		rdev->desc_nr = info->number;
		if (info->raid_disk < mddev->raid_disks)
			rdev->raid_disk = info->raid_disk;
		else
			rdev->raid_disk = -1;

		rdev->faulty = 0;
		if (rdev->raid_disk < mddev->raid_disks)
			rdev->in_sync = (info->state & (1<<MD_DISK_SYNC));
		else
			rdev->in_sync = 0;

		bind_rdev_to_array(rdev, mddev);

		if (!mddev->persistent)
			printk(KERN_INFO "md: nonpersistent superblock ...\n");

		size = calc_dev_size(rdev->bdev, mddev);
		rdev->sb_offset = calc_dev_sboffset(rdev->bdev);

		if (!mddev->size || (mddev->size > size))
			mddev->size = size;
	}

	return 0;
}

static int hot_generate_error(mddev_t * mddev, dev_t dev)
{
	struct request_queue *q;
	mdk_rdev_t *rdev;

	if (!mddev->pers)
		return -ENODEV;

	printk(KERN_INFO "md: trying to generate %s error in md%d ... \n",
		partition_name(dev), mdidx(mddev));

	rdev = find_rdev(mddev, dev);
	if (!rdev) {
		MD_BUG();
		return -ENXIO;
	}

	if (rdev->desc_nr == -1) {
		MD_BUG();
		return -EINVAL;
	}
	if (!rdev->in_sync)
		return -ENODEV;

	q = bdev_get_queue(rdev->bdev);
	if (!q) {
		MD_BUG();
		return -ENODEV;
	}
	printk(KERN_INFO "md: okay, generating error!\n");
//	q->oneshot_error = 1; // disabled for now

	return 0;
}

static int hot_remove_disk(mddev_t * mddev, dev_t dev)
{
	mdk_rdev_t *rdev;

	if (!mddev->pers)
		return -ENODEV;

	printk(KERN_INFO "md: trying to remove %s from md%d ... \n",
		partition_name(dev), mdidx(mddev));

	rdev = find_rdev(mddev, dev);
	if (!rdev)
		return -ENXIO;

	if (rdev->raid_disk >= 0)
		goto busy;

	kick_rdev_from_array(rdev);
	md_update_sb(mddev);

	return 0;
busy:
	printk(KERN_WARNING "md: cannot remove active disk %s from md%d ... \n",
		bdev_partition_name(rdev->bdev), mdidx(mddev));
	return -EBUSY;
}

static int hot_add_disk(mddev_t * mddev, dev_t dev)
{
	int i, err;
	unsigned int size;
	mdk_rdev_t *rdev;

	if (!mddev->pers)
		return -ENODEV;

	printk(KERN_INFO "md: trying to hot-add %s to md%d ... \n",
		partition_name(dev), mdidx(mddev));

	if (!mddev->pers->hot_add_disk) {
		printk(KERN_WARNING "md%d: personality does not support diskops!\n",
		       mdidx(mddev));
		return -EINVAL;
	}

	rdev = md_import_device (dev, 0);
	if (IS_ERR(rdev)) {
		printk(KERN_WARNING "md: error, md_import_device() returned %ld\n", PTR_ERR(rdev));
		return -EINVAL;
	}

	size = calc_dev_size(rdev->bdev, mddev);

	if (size < mddev->size) {
		printk(KERN_WARNING "md%d: disk size %llu blocks < array size %llu\n",
				mdidx(mddev), (unsigned long long)size, 
				(unsigned long long)mddev->size);
		err = -ENOSPC;
		goto abort_export;
	}

	if (rdev->faulty) {
		printk(KERN_WARNING "md: can not hot-add faulty %s disk to md%d!\n",
				bdev_partition_name(rdev->bdev), mdidx(mddev));
		err = -EINVAL;
		goto abort_export;
	}
	rdev->in_sync = 0;
	bind_rdev_to_array(rdev, mddev);

	/*
	 * The rest should better be atomic, we can have disk failures
	 * noticed in interrupt contexts ...
	 */
	rdev->size = size;
	rdev->sb_offset = calc_dev_sboffset(rdev->bdev);

	for (i = mddev->raid_disks; i < mddev->max_disks; i++)
		if (find_rdev_nr(mddev,i)==NULL)
			break;

	if (i == mddev->max_disks) {
		printk(KERN_WARNING "md%d: can not hot-add to full array!\n",
		       mdidx(mddev));
		err = -EBUSY;
		goto abort_unbind_export;
	}

	rdev->desc_nr = i;
	rdev->raid_disk = -1;

	md_update_sb(mddev);

	/*
	 * Kick recovery, maybe this spare has to be added to the
	 * array immediately.
	 */
	md_recover_arrays();

	return 0;

abort_unbind_export:
	unbind_rdev_from_array(rdev);

abort_export:
	export_rdev(rdev);
	return err;
}

static int set_array_info(mddev_t * mddev, mdu_array_info_t *info)
{

	mddev->major_version = MD_MAJOR_VERSION;
	mddev->minor_version = MD_MINOR_VERSION;
	mddev->patch_version = MD_PATCHLEVEL_VERSION;
	mddev->ctime         = get_seconds();

	mddev->level         = info->level;
	mddev->size          = info->size;
	mddev->raid_disks    = info->raid_disks;
	/* don't set __minor, it is determined by which /dev/md* was
	 * openned
	 */
	if (info->state & (1<<MD_SB_CLEAN))
		mddev->recovery_cp = MaxSector;
	else
		mddev->recovery_cp = 0;
	mddev->persistent    = ! info->not_persistent;

	mddev->layout        = info->layout;
	mddev->chunk_size    = info->chunk_size;



	/*
	 * Generate a 128 bit UUID
	 */
	get_random_bytes(mddev->uuid, 16);

	return 0;
}

static int set_disk_faulty(mddev_t *mddev, dev_t dev)
{
	mdk_rdev_t *rdev;

	rdev = find_rdev(mddev, dev);
	if (!rdev)
		return 0;

	md_error(mddev, rdev);
	return 1;
}

static int md_ioctl(struct inode *inode, struct file *file,
			unsigned int cmd, unsigned long arg)
{
	unsigned int minor;
	int err = 0;
	struct hd_geometry *loc = (struct hd_geometry *) arg;
	mddev_t *mddev = NULL;
	kdev_t dev;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	dev = inode->i_rdev;
	minor = minor(dev);
	if (minor >= MAX_MD_DEVS) {
		MD_BUG();
		return -EINVAL;
	}

	/*
	 * Commands dealing with the RAID driver but not any
	 * particular array:
	 */
	switch (cmd)
	{
		case RAID_VERSION:
			err = get_version((void *)arg);
			goto done;

		case PRINT_RAID_DEBUG:
			err = 0;
			md_print_devices();
			goto done;

#ifndef MODULE
		case RAID_AUTORUN:
			err = 0;
			autostart_arrays();
			goto done;
#endif
		default:;
	}

	/*
	 * Commands creating/starting a new array:
	 */

	mddev = inode->i_bdev->bd_inode->u.generic_ip;

	if (!mddev) {
		BUG();
		goto abort;
	}


	if (cmd == START_ARRAY) {
		/* START_ARRAY doesn't need to lock the array as autostart_array
		 * does the locking, and it could even be a different array
		 */
		err = autostart_array(arg);
		if (err) {
			printk(KERN_WARNING "md: autostart %s failed!\n",
			       partition_name(arg));
			goto abort;
		}
		goto done;
	}

	err = mddev_lock(mddev);
	if (err) {
		printk(KERN_INFO "md: ioctl lock interrupted, reason %d, cmd %d\n",
		       err, cmd);
		goto abort;
	}

	switch (cmd)
	{
		case SET_ARRAY_INFO:

			if (!list_empty(&mddev->disks)) {
				printk(KERN_WARNING "md: array md%d already has disks!\n",
					mdidx(mddev));
				err = -EBUSY;
				goto abort_unlock;
			}
			if (mddev->raid_disks) {
				printk(KERN_WARNING "md: array md%d already initialised!\n",
					mdidx(mddev));
				err = -EBUSY;
				goto abort_unlock;
			}
			if (arg) {
				mdu_array_info_t info;
				if (copy_from_user(&info, (void*)arg, sizeof(info))) {
					err = -EFAULT;
					goto abort_unlock;
				}
				err = set_array_info(mddev, &info);
				if (err) {
					printk(KERN_WARNING "md: couldnt set array info. %d\n", err);
					goto abort_unlock;
				}
			}
			goto done_unlock;

		default:;
	}

	/*
	 * Commands querying/configuring an existing array:
	 */
	/* if we are initialised yet, only ADD_NEW_DISK or STOP_ARRAY is allowed */
	if (!mddev->raid_disks && cmd != ADD_NEW_DISK && cmd != STOP_ARRAY && cmd != RUN_ARRAY) {
		err = -ENODEV;
		goto abort_unlock;
	}

	/*
	 * Commands even a read-only array can execute:
	 */
	switch (cmd)
	{
		case GET_ARRAY_INFO:
			err = get_array_info(mddev, (void *)arg);
			goto done_unlock;

		case GET_DISK_INFO:
			err = get_disk_info(mddev, (void *)arg);
			goto done_unlock;

		case RESTART_ARRAY_RW:
			err = restart_array(mddev);
			goto done_unlock;

		case STOP_ARRAY:
			err = do_md_stop (mddev, 0);
			goto done_unlock;

		case STOP_ARRAY_RO:
			err = do_md_stop (mddev, 1);
			goto done_unlock;

	/*
	 * We have a problem here : there is no easy way to give a CHS
	 * virtual geometry. We currently pretend that we have a 2 heads
	 * 4 sectors (with a BIG number of cylinders...). This drives
	 * dosfs just mad... ;-)
	 */
		case HDIO_GETGEO:
			if (!loc) {
				err = -EINVAL;
				goto abort_unlock;
			}
			err = put_user (2, (char *) &loc->heads);
			if (err)
				goto abort_unlock;
			err = put_user (4, (char *) &loc->sectors);
			if (err)
				goto abort_unlock;
			err = put_user(get_capacity(disks[mdidx(mddev)])/8,
						(short *) &loc->cylinders);
			if (err)
				goto abort_unlock;
			err = put_user (get_start_sect(inode->i_bdev),
						(long *) &loc->start);
			goto done_unlock;
	}

	/*
	 * The remaining ioctls are changing the state of the
	 * superblock, so we do not allow read-only arrays
	 * here:
	 */
	if (mddev->ro) {
		err = -EROFS;
		goto abort_unlock;
	}

	switch (cmd)
	{
		case ADD_NEW_DISK:
		{
			mdu_disk_info_t info;
			if (copy_from_user(&info, (void*)arg, sizeof(info)))
				err = -EFAULT;
			else
				err = add_new_disk(mddev, &info);
			goto done_unlock;
		}
		case HOT_GENERATE_ERROR:
			err = hot_generate_error(mddev, arg);
			goto done_unlock;
		case HOT_REMOVE_DISK:
			err = hot_remove_disk(mddev, arg);
			goto done_unlock;

		case HOT_ADD_DISK:
			err = hot_add_disk(mddev, arg);
			goto done_unlock;

		case SET_DISK_FAULTY:
			err = set_disk_faulty(mddev, arg);
			goto done_unlock;

		case RUN_ARRAY:
		{
			err = do_md_run (mddev);
			/*
			 * we have to clean up the mess if
			 * the array cannot be run for some
			 * reason ...
			 * ->pers will not be set, to superblock will
			 * not be updated.
			 */
			if (err)
				do_md_stop (mddev, 0);
			goto done_unlock;
		}

		default:
			if (_IOC_TYPE(cmd) == MD_MAJOR)
				printk(KERN_WARNING "md: %s(pid %d) used obsolete MD ioctl, "
				       "upgrade your software to use new ictls.\n",
				       current->comm, current->pid);
			err = -EINVAL;
			goto abort_unlock;
	}

done_unlock:
abort_unlock:
	mddev_unlock(mddev);

	return err;
done:
	if (err)
		MD_BUG();
abort:
	return err;
}

static int md_open(struct inode *inode, struct file *file)
{
	/*
	 * Succeed if we can find or allocate a mddev structure.
	 */
	mddev_t *mddev = mddev_find(minor(inode->i_rdev));
	int err = -ENOMEM;

	if (!mddev)
		goto out;

	if ((err = mddev_lock(mddev)))
		goto put;

	err = 0;
	mddev_unlock(mddev);
	inode->i_bdev->bd_inode->u.generic_ip = mddev_get(mddev);
 put:
	mddev_put(mddev);
 out:
	return err;
}

static int md_release(struct inode *inode, struct file * file)
{
 	mddev_t *mddev = inode->i_bdev->bd_inode->u.generic_ip;

	if (!mddev)
		BUG();
	mddev_put(mddev);

	return 0;
}

static struct block_device_operations md_fops =
{
	.owner		= THIS_MODULE,
	.open		= md_open,
	.release	= md_release,
	.ioctl		= md_ioctl,
};


static inline void flush_curr_signals(void)
{
	flush_signals(current);
}

int md_thread(void * arg)
{
	mdk_thread_t *thread = arg;

	lock_kernel();

	/*
	 * Detach thread
	 */

	daemonize(thread->name);

	current->exit_signal = SIGCHLD;
	allow_signal(SIGKILL);
	thread->tsk = current;

	/*
	 * md_thread is a 'system-thread', it's priority should be very
	 * high. We avoid resource deadlocks individually in each
	 * raid personality. (RAID5 does preallocation) We also use RR and
	 * the very same RT priority as kswapd, thus we will never get
	 * into a priority inversion deadlock.
	 *
	 * we definitely have to have equal or higher priority than
	 * bdflush, otherwise bdflush will deadlock if there are too
	 * many dirty RAID5 blocks.
	 */
	unlock_kernel();

	complete(thread->event);
	while (thread->run) {
		void (*run)(void *data);

		wait_event_interruptible(thread->wqueue,
					 test_bit(THREAD_WAKEUP, &thread->flags));
		if (current->flags & PF_FREEZE)
			refrigerator(PF_IOTHREAD);

		clear_bit(THREAD_WAKEUP, &thread->flags);

		run = thread->run;
		if (run) {
			run(thread->data);
			blk_run_queues();
		}
		if (signal_pending(current))
			flush_curr_signals();
	}
	complete(thread->event);
	return 0;
}

void md_wakeup_thread(mdk_thread_t *thread)
{
	dprintk("md: waking up MD thread %p.\n", thread);
	set_bit(THREAD_WAKEUP, &thread->flags);
	wake_up(&thread->wqueue);
}

mdk_thread_t *md_register_thread(void (*run) (void *),
						void *data, const char *name)
{
	mdk_thread_t *thread;
	int ret;
	struct completion event;

	thread = (mdk_thread_t *) kmalloc
				(sizeof(mdk_thread_t), GFP_KERNEL);
	if (!thread)
		return NULL;

	memset(thread, 0, sizeof(mdk_thread_t));
	init_waitqueue_head(&thread->wqueue);

	init_completion(&event);
	thread->event = &event;
	thread->run = run;
	thread->data = data;
	thread->name = name;
	ret = kernel_thread(md_thread, thread, 0);
	if (ret < 0) {
		kfree(thread);
		return NULL;
	}
	wait_for_completion(&event);
	return thread;
}

void md_interrupt_thread(mdk_thread_t *thread)
{
	if (!thread->tsk) {
		MD_BUG();
		return;
	}
	dprintk("interrupting MD-thread pid %d\n", thread->tsk->pid);
	send_sig(SIGKILL, thread->tsk, 1);
}

void md_unregister_thread(mdk_thread_t *thread)
{
	struct completion event;

	init_completion(&event);

	thread->event = &event;
	thread->run = NULL;
	thread->name = NULL;
	md_interrupt_thread(thread);
	wait_for_completion(&event);
	kfree(thread);
}

static void md_recover_arrays(void)
{
	if (!md_recovery_thread) {
		MD_BUG();
		return;
	}
	md_wakeup_thread(md_recovery_thread);
}


void md_error(mddev_t *mddev, mdk_rdev_t *rdev)
{
	dprintk("md_error dev:(%d:%d), rdev:(%d:%d), (caller: %p,%p,%p,%p).\n",
		MD_MAJOR,mdidx(mddev),MAJOR(rdev->bdev->bd_dev),MINOR(rdev->bdev->bd_dev),
		__builtin_return_address(0),__builtin_return_address(1),
		__builtin_return_address(2),__builtin_return_address(3));

	if (!mddev) {
		MD_BUG();
		return;
	}

	if (!rdev || rdev->faulty)
		return;
	if (!mddev->pers->error_handler)
		return;
	mddev->pers->error_handler(mddev,rdev);
	md_recover_arrays();
}

static int status_unused(char * page)
{
	int sz = 0, i = 0;
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	sz += sprintf(page + sz, "unused devices: ");

	ITERATE_RDEV_PENDING(rdev,tmp) {
		i++;
		sz += sprintf(page + sz, "%s ",
			      bdev_partition_name(rdev->bdev));
	}
	if (!i)
		sz += sprintf(page + sz, "<none>");

	sz += sprintf(page + sz, "\n");
	return sz;
}


static int status_resync(char * page, mddev_t * mddev)
{
	int sz = 0;
	unsigned long max_blocks, resync, res, dt, db, rt;

	resync = (mddev->curr_resync - atomic_read(&mddev->recovery_active))/2;
	max_blocks = mddev->size;

	/*
	 * Should not happen.
	 */
	if (!max_blocks) {
		MD_BUG();
		return 0;
	}
	res = (resync/1024)*1000/(max_blocks/1024 + 1);
	{
		int i, x = res/50, y = 20-x;
		sz += sprintf(page + sz, "[");
		for (i = 0; i < x; i++)
			sz += sprintf(page + sz, "=");
		sz += sprintf(page + sz, ">");
		for (i = 0; i < y; i++)
			sz += sprintf(page + sz, ".");
		sz += sprintf(page + sz, "] ");
	}
	sz += sprintf(page + sz, " %s =%3lu.%lu%% (%lu/%lu)",
		      (mddev->spares ? "recovery" : "resync"),
		      res/10, res % 10, resync, max_blocks);

	/*
	 * We do not want to overflow, so the order of operands and
	 * the * 100 / 100 trick are important. We do a +1 to be
	 * safe against division by zero. We only estimate anyway.
	 *
	 * dt: time from mark until now
	 * db: blocks written from mark until now
	 * rt: remaining time
	 */
	dt = ((jiffies - mddev->resync_mark) / HZ);
	if (!dt) dt++;
	db = resync - (mddev->resync_mark_cnt/2);
	rt = (dt * ((max_blocks-resync) / (db/100+1)))/100;

	sz += sprintf(page + sz, " finish=%lu.%lumin", rt / 60, (rt % 60)/6);

	sz += sprintf(page + sz, " speed=%ldK/sec", db/dt);

	return sz;
}

static int md_status_read_proc(char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	int sz = 0, j;
	sector_t size;
	struct list_head *tmp, *tmp2;
	mdk_rdev_t *rdev;
	mddev_t *mddev;

	sz += sprintf(page + sz, "Personalities : ");
	for (j = 0; j < MAX_PERSONALITY; j++)
	if (pers[j])
		sz += sprintf(page+sz, "[%s] ", pers[j]->name);

	sz += sprintf(page+sz, "\n");

	ITERATE_MDDEV(mddev,tmp) if (mddev_lock(mddev)==0) {
		sz += sprintf(page + sz, "md%d : %sactive", mdidx(mddev),
						mddev->pers ? "" : "in");
		if (mddev->pers) {
			if (mddev->ro)
				sz += sprintf(page + sz, " (read-only)");
			sz += sprintf(page + sz, " %s", mddev->pers->name);
		}

		size = 0;
		ITERATE_RDEV(mddev,rdev,tmp2) {
			sz += sprintf(page + sz, " %s[%d]",
				bdev_partition_name(rdev->bdev), rdev->desc_nr);
			if (rdev->faulty) {
				sz += sprintf(page + sz, "(F)");
				continue;
			}
			size += rdev->size;
		}

		if (!list_empty(&mddev->disks)) {
			if (mddev->pers)
				sz += sprintf(page + sz, "\n      %llu blocks",
						 (unsigned long long)md_size[mdidx(mddev)]);
			else
				sz += sprintf(page + sz, "\n      %llu blocks", (unsigned long long)size);
		}

		if (!mddev->pers) {
			sz += sprintf(page+sz, "\n");
			mddev_unlock(mddev);
			continue;
		}

		sz += mddev->pers->status (page+sz, mddev);

		sz += sprintf(page+sz, "\n      ");
		if (mddev->curr_resync > 2)
			sz += status_resync (page+sz, mddev);
		else if (mddev->curr_resync == 1 || mddev->curr_resync == 2)
				sz += sprintf(page + sz, "	resync=DELAYED");

		sz += sprintf(page + sz, "\n");
		mddev_unlock(mddev);
	}
	sz += status_unused(page + sz);

	return sz;
}

int register_md_personality(int pnum, mdk_personality_t *p)
{
	if (pnum >= MAX_PERSONALITY) {
		MD_BUG();
		return -EINVAL;
	}

	if (pers[pnum]) {
		MD_BUG();
		return -EBUSY;
	}

	pers[pnum] = p;
	printk(KERN_INFO "md: %s personality registered as nr %d\n", p->name, pnum);
	return 0;
}

int unregister_md_personality(int pnum)
{
	if (pnum >= MAX_PERSONALITY) {
		MD_BUG();
		return -EINVAL;
	}

	printk(KERN_INFO "md: %s personality unregistered\n", pers[pnum]->name);
	pers[pnum] = NULL;
	return 0;
}

void md_sync_acct(mdk_rdev_t *rdev, unsigned long nr_sectors)
{
	rdev->bdev->bd_contains->bd_disk->sync_io += nr_sectors;
}

static int is_mddev_idle(mddev_t *mddev)
{
	mdk_rdev_t * rdev;
	struct list_head *tmp;
	int idle;
	unsigned long curr_events;

	idle = 1;
	ITERATE_RDEV(mddev,rdev,tmp) {
		struct gendisk *disk = rdev->bdev->bd_contains->bd_disk;
		curr_events = disk->read_sectors + disk->write_sectors - disk->sync_io;
		if ((curr_events - rdev->last_events) > 32) {
			rdev->last_events = curr_events;
			idle = 0;
		}
	}
	return idle;
}

void md_done_sync(mddev_t *mddev, int blocks, int ok)
{
	/* another "blocks" (512byte) blocks have been synced */
	atomic_sub(blocks, &mddev->recovery_active);
	wake_up(&mddev->recovery_wait);
	if (!ok) {
		mddev->recovery_error = -EIO;
		mddev->recovery_running = -1;
		md_recover_arrays();
		// stop recovery, signal do_sync ....
	}
}


void md_write_start(mddev_t *mddev)
{
	if (mddev->safemode && !atomic_read(&mddev->writes_pending)) {
		mddev_lock_uninterruptible(mddev);
		atomic_inc(&mddev->writes_pending);
		if (mddev->in_sync) {
			mddev->in_sync = 0;
			md_update_sb(mddev);
		}
		mddev_unlock(mddev);
	} else
		atomic_inc(&mddev->writes_pending);
}

void md_write_end(mddev_t *mddev, mdk_thread_t *thread)
{
	if (atomic_dec_and_test(&mddev->writes_pending) && mddev->safemode)
		md_wakeup_thread(thread);
}
static inline void md_enter_safemode(mddev_t *mddev)
{

	mddev_lock_uninterruptible(mddev);
	if (mddev->safemode && !atomic_read(&mddev->writes_pending) && !mddev->in_sync && !mddev->recovery_running) {
		mddev->in_sync = 1;
		md_update_sb(mddev);
	}
	mddev_unlock(mddev);
}

void md_handle_safemode(mddev_t *mddev)
{
	if (signal_pending(current)) {
		printk(KERN_INFO "md: md%d in safe mode\n",mdidx(mddev));
		mddev->safemode= 1;
		flush_curr_signals();
	}
	if (mddev->safemode)
		md_enter_safemode(mddev);
}


DECLARE_WAIT_QUEUE_HEAD(resync_wait);

#define SYNC_MARKS	10
#define	SYNC_MARK_STEP	(3*HZ)
static void md_do_sync(void *data)
{
	mddev_t *mddev = data;
	mddev_t *mddev2;
	unsigned int max_sectors, currspeed = 0,
		j, window, err;
	unsigned long mark[SYNC_MARKS];
	unsigned long mark_cnt[SYNC_MARKS];
	int last_mark,m;
	struct list_head *tmp;
	unsigned long last_check;

	/* just incase thread restarts... */
	if (mddev->recovery_running <= 0)
		return;

	/* we overload curr_resync somewhat here.
	 * 0 == not engaged in resync at all
	 * 2 == checking that there is no conflict with another sync
	 * 1 == like 2, but have yielded to allow conflicting resync to
	 *		commense
	 * other == active in resync - this many blocks
	 */
	do {
		mddev->curr_resync = 2;

		ITERATE_MDDEV(mddev2,tmp) {
			if (mddev2 == mddev)
				continue;
			if (mddev2->curr_resync && 
			    match_mddev_units(mddev,mddev2)) {
				printk(KERN_INFO "md: delaying resync of md%d until md%d "
				       "has finished resync (they share one or more physical units)\n",
				       mdidx(mddev), mdidx(mddev2));
				if (mddev < mddev2) {/* arbitrarily yield */
					mddev->curr_resync = 1;
					wake_up(&resync_wait);
				}
				if (wait_event_interruptible(resync_wait,
							     mddev2->curr_resync < mddev->curr_resync)) {
					flush_curr_signals();
					err = -EINTR;
					mddev_put(mddev2);
					goto skip;
				}
			}
			if (mddev->curr_resync == 1)
				break;
		}
	} while (mddev->curr_resync < 2);

	max_sectors = mddev->size << 1;

	printk(KERN_INFO "md: syncing RAID array md%d\n", mdidx(mddev));
	printk(KERN_INFO "md: minimum _guaranteed_ reconstruction speed: %d KB/sec/disc.\n", sysctl_speed_limit_min);
	printk(KERN_INFO "md: using maximum available idle IO bandwith "
	       "(but not more than %d KB/sec) for reconstruction.\n",
	       sysctl_speed_limit_max);

	is_mddev_idle(mddev); /* this also initializes IO event counters */
	for (m = 0; m < SYNC_MARKS; m++) {
		mark[m] = jiffies;
		mark_cnt[m] = mddev->recovery_cp;
	}
	last_mark = 0;
	mddev->resync_mark = mark[last_mark];
	mddev->resync_mark_cnt = mark_cnt[last_mark];

	/*
	 * Tune reconstruction:
	 */
	window = 32*(PAGE_SIZE/512);
	printk(KERN_INFO "md: using %dk window, over a total of %d blocks.\n",
	       window/2,max_sectors/2);

	atomic_set(&mddev->recovery_active, 0);
	init_waitqueue_head(&mddev->recovery_wait);
	last_check = 0;

	mddev->recovery_error = 0;

	if (mddev->recovery_cp)
		printk(KERN_INFO "md: resuming recovery of md%d from checkpoint.\n", mdidx(mddev));

	for (j = mddev->recovery_cp; j < max_sectors;) {
		int sectors;

		sectors = mddev->pers->sync_request(mddev, j, currspeed < sysctl_speed_limit_min);
		if (sectors < 0) {
			err = sectors;
			goto out;
		}
		atomic_add(sectors, &mddev->recovery_active);
		j += sectors;
		if (j>1) mddev->curr_resync = j;

		if (last_check + window > j)
			continue;

		last_check = j;

		blk_run_queues();

	repeat:
		if (jiffies >= mark[last_mark] + SYNC_MARK_STEP ) {
			/* step marks */
			int next = (last_mark+1) % SYNC_MARKS;

			mddev->resync_mark = mark[next];
			mddev->resync_mark_cnt = mark_cnt[next];
			mark[next] = jiffies;
			mark_cnt[next] = j - atomic_read(&mddev->recovery_active);
			last_mark = next;
		}


		if (signal_pending(current)) {
			/*
			 * got a signal, exit.
			 */
			printk(KERN_INFO "md: md_do_sync() got signal ... exiting\n");
			flush_curr_signals();
			err = -EINTR;
			goto out;
		}

		/*
		 * this loop exits only if either when we are slower than
		 * the 'hard' speed limit, or the system was IO-idle for
		 * a jiffy.
		 * the system might be non-idle CPU-wise, but we only care
		 * about not overloading the IO subsystem. (things like an
		 * e2fsck being done on the RAID array should execute fast)
		 */
		cond_resched();

		currspeed = (j-mddev->resync_mark_cnt)/2/((jiffies-mddev->resync_mark)/HZ +1) +1;

		if (currspeed > sysctl_speed_limit_min) {
			if ((currspeed > sysctl_speed_limit_max) ||
					!is_mddev_idle(mddev)) {
				current->state = TASK_INTERRUPTIBLE;
				schedule_timeout(HZ/4);
				goto repeat;
			}
		}
	}
	printk(KERN_INFO "md: md%d: sync done.\n",mdidx(mddev));
	err = 0;
	/*
	 * this also signals 'finished resyncing' to md_stop
	 */
 out:
	wait_event(mddev->recovery_wait, !atomic_read(&mddev->recovery_active));

	if (mddev->recovery_running < 0 && 
		!mddev->recovery_error && mddev->curr_resync > 2)
	{
		/* interrupted but no write errors */
		printk(KERN_INFO "md: checkpointing recovery of md%d.\n", mdidx(mddev));
		mddev->recovery_cp = mddev->curr_resync;
	}

	/* tell personality that we are finished */
	mddev->pers->sync_request(mddev, max_sectors, 1);
 skip:
	mddev->curr_resync = 0;
	if (err)
		mddev->recovery_running = -1;
	if (mddev->recovery_running > 0)
		mddev->recovery_running = 0;
	if (mddev->recovery_running == 0)
		mddev->recovery_cp = MaxSector;
	if (mddev->safemode)
		md_enter_safemode(mddev);
	md_recover_arrays();
}


/*
 * This is the kernel thread that watches all md arrays for re-sync and other
 * action that might be needed.
 * It does not do any resync itself, but rather "forks" off other threads
 * to do that as needed.
 * When it is determined that resync is needed, we set "->recovery_running" and
 * create a thread at ->sync_thread.
 * When the thread finishes it clears recovery_running (or sets an error)
 * and wakeup up this thread which will reap the thread and finish up.
 * This thread also removes any faulty devices (with nr_pending == 0).
 *
 * The overall approach is:
 *  1/ if the superblock needs updating, update it.
 *  2/ If a recovery thread is running, don't do anything else.
 *  3/ If recovery has finished, clean up, possibly marking spares active.
 *  4/ If there are any faulty devices, remove them.
 *  5/ If array is degraded, try to add spares devices
 *  6/ If array has spares or is not in-sync, start a resync thread.
 */
void md_do_recovery(void *data)
{
	mddev_t *mddev;
	mdk_rdev_t *rdev;
	struct list_head *tmp, *rtmp;


	dprintk(KERN_INFO "md: recovery thread got woken up ...\n");

	ITERATE_MDDEV(mddev,tmp) if (mddev_lock(mddev)==0) {
		if (!mddev->raid_disks || !mddev->pers || mddev->ro)
			goto unlock;
		if (mddev->sb_dirty)
			md_update_sb(mddev);
		if (mddev->recovery_running > 0)
			/* resync/recovery still happening */
			goto unlock;
		if (mddev->sync_thread) {
			/* resync has finished, collect result */
			md_unregister_thread(mddev->sync_thread);
			mddev->sync_thread = NULL;
			if (mddev->recovery_running == 0) {
				/* success...*/
				/* activate any spares */
				mddev->pers->spare_active(mddev);
				mddev->spares = 0;
			}
			md_update_sb(mddev);
			mddev->recovery_running = 0;
			wake_up(&resync_wait);
			goto unlock;
		}
		if (mddev->recovery_running) {
			/* that's odd.. */
			mddev->recovery_running = 0;
			wake_up(&resync_wait);
		}

		/* no recovery is running.
		 * remove any failed drives, then
		 * add spares if possible
		 */
		mddev->spares = 0;
		ITERATE_RDEV(mddev,rdev,rtmp) {
			if (rdev->raid_disk >= 0 &&
			    rdev->faulty &&
			    atomic_read(&rdev->nr_pending)==0) {
				mddev->pers->hot_remove_disk(mddev, rdev->raid_disk);
				rdev->raid_disk = -1;
			}
			if (!rdev->faulty && rdev->raid_disk >= 0 && !rdev->in_sync)
				mddev->spares++;
		}
		if (mddev->degraded) {
			ITERATE_RDEV(mddev,rdev,rtmp)
				if (rdev->raid_disk < 0
				    && !rdev->faulty) {
					if (mddev->pers->hot_add_disk(mddev,rdev)) {
						mddev->spares++;
						mddev->recovery_cp = 0;
					}
					else
						break;
				}
		}

		if (!mddev->spares && (mddev->recovery_cp == MaxSector )) {
			/* nothing we can do ... */
			goto unlock;
		}
		if (mddev->pers->sync_request) {
			mddev->sync_thread = md_register_thread(md_do_sync,
								mddev,
								"md_resync");
			if (!mddev->sync_thread) {
				printk(KERN_ERR "md%d: could not start resync thread...\n", mdidx(mddev));
				/* leave the spares where they are, it shouldn't hurt */
				mddev->recovery_running = 0;
			} else {
				mddev->recovery_running = 1;
				md_wakeup_thread(mddev->sync_thread);
			}
		}
	unlock:
		mddev_unlock(mddev);
	}
	dprintk(KERN_INFO "md: recovery thread finished ...\n");

}

int md_notify_reboot(struct notifier_block *this,
					unsigned long code, void *x)
{
	struct list_head *tmp;
	mddev_t *mddev;

	if ((code == SYS_DOWN) || (code == SYS_HALT) || (code == SYS_POWER_OFF)) {

		printk(KERN_INFO "md: stopping all md devices.\n");

		ITERATE_MDDEV(mddev,tmp)
			if (mddev_trylock(mddev)==0)
				do_md_stop (mddev, 1);
		/*
		 * certain more exotic SCSI devices are known to be
		 * volatile wrt too early system reboots. While the
		 * right place to handle this issue is the given
		 * driver, we do want to have a safe RAID driver ...
		 */
		mdelay(1000*1);
	}
	return NOTIFY_DONE;
}

struct notifier_block md_notifier = {
	.notifier_call	= md_notify_reboot,
	.next		= NULL,
	.priority	= INT_MAX, /* before any real devices */
};

static void md_geninit(void)
{
	int i;

	for(i = 0; i < MAX_MD_DEVS; i++) {
		md_size[i] = 0;
	}

	dprintk("md: sizeof(mdp_super_t) = %d\n", (int)sizeof(mdp_super_t));

#ifdef CONFIG_PROC_FS
	create_proc_read_entry("mdstat", 0, NULL, md_status_read_proc, NULL);
#endif
}

int __init md_init(void)
{
	static char * name = "mdrecoveryd";
	int minor;

	printk(KERN_INFO "md: md driver %d.%d.%d MAX_MD_DEVS=%d, MD_SB_DISKS=%d\n",
			MD_MAJOR_VERSION, MD_MINOR_VERSION,
			MD_PATCHLEVEL_VERSION, MAX_MD_DEVS, MD_SB_DISKS);

	if (register_blkdev (MAJOR_NR, "md", &md_fops)) {
		printk(KERN_ALERT "md: Unable to get major %d for md\n", MAJOR_NR);
		return (-1);
	}
	devfs_mk_dir (NULL, "md", NULL);
	blk_register_region(MKDEV(MAJOR_NR, 0), MAX_MD_DEVS, THIS_MODULE,
				md_probe, NULL, NULL);
	for (minor=0; minor < MAX_MD_DEVS; ++minor) {
		char name[16];
		sprintf(name, "md/%d", minor);
		devfs_register(NULL, name, DEVFS_FL_DEFAULT, MAJOR_NR, minor,
			       S_IFBLK | S_IRUSR | S_IWUSR, &md_fops, NULL);
	}

	md_recovery_thread = md_register_thread(md_do_recovery, NULL, name);
	if (!md_recovery_thread)
		printk(KERN_ALERT
		       "md: bug: couldn't allocate md_recovery_thread\n");

	register_reboot_notifier(&md_notifier);
	raid_table_header = register_sysctl_table(raid_root_table, 1);

	md_geninit();
	return (0);
}


#ifndef MODULE

/*
 * Searches all registered partitions for autorun RAID arrays
 * at boot time.
 */
static dev_t detected_devices[128];
static int dev_cnt;

void md_autodetect_dev(dev_t dev)
{
	if (dev_cnt >= 0 && dev_cnt < 127)
		detected_devices[dev_cnt++] = dev;
}


static void autostart_arrays(void)
{
	mdk_rdev_t *rdev;
	int i;

	printk(KERN_INFO "md: Autodetecting RAID arrays.\n");

	for (i = 0; i < dev_cnt; i++) {
		dev_t dev = detected_devices[i];

		rdev = md_import_device(dev,1);
		if (IS_ERR(rdev)) {
			printk(KERN_ALERT "md: could not import %s!\n",
				partition_name(dev));
			continue;
		}
		if (rdev->faulty) {
			MD_BUG();
			continue;
		}
		list_add(&rdev->same_set, &pending_raid_disks);
	}
	dev_cnt = 0;

	autorun_devices();
}

#endif

static __exit void md_exit(void)
{
	int i;
	blk_unregister_region(MKDEV(MAJOR_NR,0), MAX_MD_DEVS);
	md_unregister_thread(md_recovery_thread);
	for (i=0; i < MAX_MD_DEVS; i++)
		devfs_remove("md/%d", i);
	devfs_remove("md");

	unregister_blkdev(MAJOR_NR,"md");
	unregister_reboot_notifier(&md_notifier);
	unregister_sysctl_table(raid_table_header);
#ifdef CONFIG_PROC_FS
	remove_proc_entry("mdstat", NULL);
#endif
	for (i = 0; i < MAX_MD_DEVS; i++) {
		struct gendisk *disk = disks[i];
		mddev_t *mddev;
		if (!disks[i])
			continue;
		mddev = disk->private_data;
		del_gendisk(disk);
		put_disk(disk);
		mddev_put(mddev);
	}
}

module_init(md_init)
module_exit(md_exit)

EXPORT_SYMBOL(md_size);
EXPORT_SYMBOL(register_md_personality);
EXPORT_SYMBOL(unregister_md_personality);
EXPORT_SYMBOL(md_error);
EXPORT_SYMBOL(md_sync_acct);
EXPORT_SYMBOL(md_done_sync);
EXPORT_SYMBOL(md_write_start);
EXPORT_SYMBOL(md_write_end);
EXPORT_SYMBOL(md_handle_safemode);
EXPORT_SYMBOL(md_register_thread);
EXPORT_SYMBOL(md_unregister_thread);
EXPORT_SYMBOL(md_wakeup_thread);
EXPORT_SYMBOL(md_print_devices);
EXPORT_SYMBOL(md_interrupt_thread);
MODULE_LICENSE("GPL");
