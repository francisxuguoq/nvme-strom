/*
 * NVMe-Strom
 *
 * A Linux kernel driver to support SSD-to-GPU P2P DMA.
 *
 * Copyright (C) 2016 KaiGai Kohei <kaigai@kaigai.gr.jp>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/major.h>
#include <linux/moduleparam.h>
#include <linux/nvme.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <generated/utsrelease.h>
#include "nv-p2p.h"
#include "nvme_strom.h"

/* determine the target kernel to build */
#if defined(RHEL_MAJOR) && (RHEL_MAJOR == 7)
#define STROM_TARGET_KERNEL_RHEL7		1
#include "md.rhel7.h"
#else
#error Not a supported Linux kernel
#endif

/* utility macros */
#define Assert(cond)												\
	do {															\
		if (!(cond)) {												\
			panic("assertion failure (" #cond ") at %s:%d, %s\n",	\
				  __FILE__, __LINE__, __FUNCTION__);				\
		}															\
	} while(0)
#define lengthof(array)	(sizeof (array) / sizeof ((array)[0]))
#define Max(a,b)		((a) > (b) ? (a) : (b))
#define Min(a,b)		((a) < (b) ? (a) : (b))

/* message verbosity control */
static int	verbose = 0;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "turn on/off debug message");

#define prDebug(fmt, ...)												\
	do {																\
		if (verbose > 1)												\
			printk(KERN_ALERT "nvme-strom(%s:%d): " fmt "\n",			\
				   __FUNCTION__, __LINE__, ##__VA_ARGS__);				\
		else if (verbose)												\
			printk(KERN_ALERT "nvme-strom: " fmt "\n", ##__VA_ARGS__);	\
	} while(0)
#define prInfo(fmt, ...)						\
	printk(KERN_INFO "nvme-strom: " fmt "\n", ##__VA_ARGS__)
#define prNotice(fmt, ...)						\
	printk(KERN_NOTICE "nvme-strom: " fmt "\n", ##__VA_ARGS__)
#define prWarn(fmt, ...)						\
	printk(KERN_WARNING "nvme-strom: " fmt "\n", ##__VA_ARGS__)
#define prError(fmt, ...)						\
	printk(KERN_ERR "nvme-strom: " fmt "\n", ##__VA_ARGS__)

/* routines for extra symbols */
#define EXTRA_KSYMS_NEEDS_NVIDIA	1
#include "extra_ksyms.c"

/*
 * extra filesystem signature
 */
#define XFS_SB_MAGIC	0x58465342

/*
 * for boundary alignment requirement
 */
#define GPU_BOUND_SHIFT		16
#define GPU_BOUND_SIZE		((u64)1 << GPU_BOUND_SHIFT)
#define GPU_BOUND_OFFSET	(GPU_BOUND_SIZE-1)
#define GPU_BOUND_MASK		(~GPU_BOUND_OFFSET)

/* procfs entry of "/proc/nvme-strom" */
static struct proc_dir_entry  *nvme_strom_proc = NULL;

/*
 * ================================================================
 *
 * Routines to map/unmap GPU device memory segment
 *
 * ================================================================
 */
struct mapped_gpu_memory
{
	struct list_head	chain;		/* chain to the strom_mgmem_slots[] */
	int					hindex;		/* index of the hash slot */
	int					refcnt;		/* number of the concurrent tasks */
	kuid_t				owner;		/* effective user-id who mapped this
									 * device memory */
	unsigned long		handle;		/* identifier of this entry */
	unsigned long		map_address;/* virtual address of the device memory
									 * (note: just for message output) */
	unsigned long		map_offset;	/* offset from the H/W page boundary */
	unsigned long		map_length;	/* length of the mapped area */
	struct task_struct *wait_task;	/* task waiting for DMA completion */
	size_t				gpu_page_sz;/* page size in bytes; note that
									 * 'page_size' of nvidia_p2p_page_table_t
									 * is one of NVIDIA_P2P_PAGE_SIZE_* */
	size_t				gpu_page_shift;	/* log2 of gpu_page_sz */
	nvidia_p2p_page_table_t *page_table;

	/*
	 * NOTE: User supplied virtual address of device memory may not be
	 * aligned to the hardware page boundary of GPUs. So, we may need to
	 * map the least device memory that wraps the region (vaddress ...
	 * vaddress + length) entirely.
	 * The 'map_offset' is offset of the 'vaddress' from the head of H/W
	 * page boundary. So, if application wants to kick DMA to the location
	 * where handle=1234 and offset=2000 and map_offset=500, the driver
	 * will set up DMA towards the offset=2500 from the head of mapped
	 * physical pages.
	 */

	/*
	 * NOTE: Once a mapped_gpu_memory is registered, it can be released
	 * on random timing, by cuFreeMem(), process termination and etc...
	 * If refcnt > 0, it means someone's P2P DMA is in-progress, so
	 * cleanup routine (that shall be called by nvidia driver) has to
	 * wait for completion of these operations. However, mapped_gpu_memory
	 * shall be released immediately not to use this region any more.
	 */
};
typedef struct mapped_gpu_memory	mapped_gpu_memory;

#define MAPPED_GPU_MEMORY_NSLOTS	48
static spinlock_t		strom_mgmem_locks[MAPPED_GPU_MEMORY_NSLOTS];
static struct list_head	strom_mgmem_slots[MAPPED_GPU_MEMORY_NSLOTS];

/*
 * strom_mapped_gpu_memory_index - index of strom_mgmem_mutex/slots
 */
static inline int
strom_mapped_gpu_memory_index(unsigned long handle)
{
	u32		hash = arch_fast_hash(&handle, sizeof(unsigned long),
								  0x20140702);
	return hash % MAPPED_GPU_MEMORY_NSLOTS;
}

/*
 * strom_get_mapped_gpu_memory
 */
static mapped_gpu_memory *
strom_get_mapped_gpu_memory(unsigned long handle)
{
	int					index = strom_mapped_gpu_memory_index(handle);
	spinlock_t		   *lock = &strom_mgmem_locks[index];
	struct list_head   *slot = &strom_mgmem_slots[index];
	unsigned long		flags;
	mapped_gpu_memory  *mgmem;

	spin_lock_irqsave(lock, flags);
	list_for_each_entry(mgmem, slot, chain)
	{
		if (mgmem->handle == handle &&
			uid_eq(mgmem->owner, current_euid()))
		{
			/* sanity checks */
			Assert((unsigned long)mgmem == handle);
			Assert(mgmem->hindex == index);

			mgmem->refcnt++;
			spin_unlock_irqrestore(lock, flags);

			return mgmem;
		}
	}
	spin_unlock_irqrestore(lock, flags);

	prError("P2P GPU Memory (handle=%lx) not found", handle);

	return NULL;	/* not found */
}

/*
 * strom_put_mapped_gpu_memory
 */
static void
strom_put_mapped_gpu_memory(mapped_gpu_memory *mgmem)
{
	int				index = mgmem->hindex;
	spinlock_t	   *lock = &strom_mgmem_locks[index];
	unsigned long	flags;

	spin_lock_irqsave(lock, flags);
	Assert(mgmem->refcnt > 0);
	if (--mgmem->refcnt == 0)
	{
		if (mgmem->wait_task)
			wake_up_process(mgmem->wait_task);
		mgmem->wait_task = NULL;
	}
	spin_unlock_irqrestore(lock, flags);
}

/*
 * callback_release_mapped_gpu_memory
 */
static void
callback_release_mapped_gpu_memory(void *private)
{
	mapped_gpu_memory  *mgmem = private;
	spinlock_t		   *lock = &strom_mgmem_locks[mgmem->hindex];
	unsigned long		handle = mgmem->handle;
	unsigned long		flags;
	int					rc;

	/* sanity check */
	Assert((unsigned long)mgmem == handle);

	spin_lock_irqsave(lock, flags);
	/*
	 * Detach this mapped GPU memory from the global list first, if
	 * application didn't unmap explicitly.
	 */
	if (mgmem->chain.next || mgmem->chain.prev)
	{
		list_del(&mgmem->chain);
		memset(&mgmem->chain, 0, sizeof(struct list_head));
	}

	/*
	 * wait for completion of the concurrent DMA tasks, if any tasks
	 * are running.
	 */
	if (mgmem->refcnt > 0)
	{
		struct task_struct *wait_task_saved = mgmem->wait_task;

		mgmem->wait_task = current;
		/* sleep until refcnt == 0 */
		set_current_state(TASK_UNINTERRUPTIBLE);
		spin_unlock_irqrestore(lock, flags);

		schedule();

		if (wait_task_saved)
			wake_up_process(wait_task_saved);

		spin_lock_irqsave(lock, flags);
		Assert(mgmem->refcnt == 0);
	}
	spin_unlock_irqrestore(lock, flags);

	/*
	 * OK, no concurrent task does not use this mapped GPU memory region
	 * at this point. So, we can release the page table and relevant safely.
	 */
	rc = __nvidia_p2p_free_page_table(mgmem->page_table);
	if (rc)
		prError("nvidia_p2p_free_page_table (handle=0x%lx, rc=%d)",
				handle, rc);
	kfree(mgmem);

	prNotice("P2P GPU Memory (handle=%p) was released", (void *)handle);

	module_put(THIS_MODULE);
}

/*
 * ioctl_map_gpu_memory
 *
 * ioctl(2) handler for STROM_IOCTL__MAP_GPU_MEMORY
 */
static int
ioctl_map_gpu_memory(StromCmd__MapGpuMemory __user *uarg)
{
	StromCmd__MapGpuMemory karg;
	mapped_gpu_memory  *mgmem;
	unsigned long		map_address;
	unsigned long		map_offset;
	unsigned long		handle;
	unsigned long		flags;
	uint32_t			entries;
	int					rc;

	if (copy_from_user(&karg, uarg, sizeof(karg)))
		return -EFAULT;

	mgmem = kmalloc(sizeof(mapped_gpu_memory), GFP_KERNEL);
	if (!mgmem)
		return -ENOMEM;

	map_address = karg.vaddress & GPU_BOUND_MASK;
	map_offset  = karg.vaddress & GPU_BOUND_OFFSET;
	handle = (unsigned long) mgmem;

	INIT_LIST_HEAD(&mgmem->chain);
	mgmem->hindex		= strom_mapped_gpu_memory_index(handle);
	mgmem->refcnt		= 0;
	mgmem->owner		= current_euid();
	mgmem->handle		= handle;
	mgmem->map_address  = map_address;
	mgmem->map_offset	= map_offset;
	mgmem->map_length	= map_offset + karg.length;
	mgmem->wait_task	= NULL;

	rc = __nvidia_p2p_get_pages(0,	/* p2p_token; deprecated */
								0,	/* va_space_token; deprecated */
								mgmem->map_address,
								mgmem->map_length,
								&mgmem->page_table,
								callback_release_mapped_gpu_memory,
								mgmem);
	if (rc)
	{
		prError("failed on nvidia_p2p_get_pages(addr=%p, len=%zu), rc=%d",
				(void *)map_address, (size_t)map_offset + karg.length, rc);
		goto error_1;
	}

	/* page size in bytes */
	switch (mgmem->page_table->page_size)
	{
		case NVIDIA_P2P_PAGE_SIZE_4KB:
			mgmem->gpu_page_sz = 4 * 1024;
			mgmem->gpu_page_shift = 12;
			break;
		case NVIDIA_P2P_PAGE_SIZE_64KB:
			mgmem->gpu_page_sz = 64 * 1024;
			mgmem->gpu_page_shift = 16;
			break;
		case NVIDIA_P2P_PAGE_SIZE_128KB:
			mgmem->gpu_page_sz = 128 * 1024;
			mgmem->gpu_page_shift = 17;
			break;
		default:
			rc = -EINVAL;
			goto error_2;
	}

	/* return the handle of mapped_gpu_memory */
	entries = mgmem->page_table->entries;
	if (put_user(mgmem->handle, &uarg->handle) ||
		put_user(mgmem->gpu_page_sz, &uarg->gpu_page_sz) ||
		put_user(entries, &uarg->gpu_npages))
	{
		rc = -EFAULT;
		goto error_2;
	}

	prNotice("P2P GPU Memory (handle=%p) mapped "
			 "(version=%u, page_size=%zu, entries=%u)",
			 (void *)mgmem->handle,
			 mgmem->page_table->version,
			 mgmem->gpu_page_sz,
			 mgmem->page_table->entries);

	/*
	 * Warning message if mapped device memory is not aligned well
	 */
	if ((mgmem->map_offset & (PAGE_SIZE - 1)) != 0 ||
		(mgmem->map_length & (PAGE_SIZE - 1)) != 0)
	{
		prWarn("Gpu memory mapping (handle=%lx) is not aligned well "
			   "(map_offset=%lx map_length=%lx). "
			   "It may be inconvenient to submit DMA requests",
			   mgmem->handle,
			   mgmem->map_offset,
			   mgmem->map_length);
	}
	__module_get(THIS_MODULE);

	/* attach this mapped_gpu_memory */
	spin_lock_irqsave(&strom_mgmem_locks[mgmem->hindex], flags);
	list_add(&mgmem->chain, &strom_mgmem_slots[mgmem->hindex]);
	spin_unlock_irqrestore(&strom_mgmem_locks[mgmem->hindex], flags);

	return 0;

error_2:
	__nvidia_p2p_put_pages(0, 0, mgmem->map_address, mgmem->page_table);
error_1:
	kfree(mgmem);

	return rc;
}

/*
 * ioctl_unmap_gpu_memory
 *
 * ioctl(2) handler for STROM_IOCTL__UNMAP_GPU_MEMORY
 */
static int
ioctl_unmap_gpu_memory(StromCmd__UnmapGpuMemory __user *uarg)
{
	StromCmd__UnmapGpuMemory karg;
	mapped_gpu_memory  *mgmem;
	spinlock_t		   *lock;
	struct list_head   *slot;
	unsigned long		flags;
	int					i, rc;

	if (copy_from_user(&karg, uarg, sizeof(karg)))
		return -EFAULT;

	i = strom_mapped_gpu_memory_index(karg.handle);
	lock = &strom_mgmem_locks[i];
	slot = &strom_mgmem_slots[i];

	spin_lock_irqsave(lock, flags);
	list_for_each_entry(mgmem, slot, chain)
	{
		/*
		 * NOTE: I'm not 100% certain whether UID is the right check to
		 * determine availability of the virtual address of GPU device.
		 * So, this behavior may be changed in the later version.
		 */
		if (mgmem->handle == karg.handle &&
			uid_eq(mgmem->owner, current_euid()))
		{
			list_del(&mgmem->chain);
			memset(&mgmem->chain, 0, sizeof(struct list_head));
			spin_unlock_irqrestore(lock, flags);

			rc = __nvidia_p2p_put_pages(0, 0,
										mgmem->map_address,
										mgmem->page_table);
			if (rc)
				prError("failed on nvidia_p2p_put_pages: %d", rc);
			return rc;
		}
	}
	spin_unlock_irqrestore(lock, flags);

	prError("no mapped GPU memory found (handle: %lx)", karg.handle);
	return -ENOENT;
}

/*
 * ioctl_list_gpu_memory
 *
 * ioctl(2) handler for STROM_IOCTL__LIST_GPU_MEMORY
 */
static int
ioctl_list_gpu_memory(StromCmd__ListGpuMemory __user *uarg)
{
	StromCmd__ListGpuMemory karg;
	spinlock_t		   *lock;
	struct list_head   *slot;
	unsigned long		flags;
	mapped_gpu_memory  *mgmem;
	int					i, j;
	int					retval = 0;

	if (copy_from_user(&karg, uarg,
					   offsetof(StromCmd__ListGpuMemory, handles)))
		return -EFAULT;

	karg.nitems = 0;
	for (i=0; i < MAPPED_GPU_MEMORY_NSLOTS; i++)
	{
		lock = &strom_mgmem_locks[i];
		slot = &strom_mgmem_slots[i];

		spin_lock_irqsave(lock, flags);
		list_for_each_entry(mgmem, slot, chain)
		{
			j = karg.nitems++;
			if (j < karg.nrooms)
			{
				if (put_user(mgmem->handle, &uarg->handles[j]))
					retval = -EFAULT;
			}
			else
				retval = -ENOBUFS;
		}
		spin_unlock_irqrestore(lock, flags);
	}
	/* write back */
	if (copy_to_user(uarg, &karg,
					 offsetof(StromCmd__ListGpuMemory, handles)))
		retval = -EFAULT;

	return retval;
}

/*
 * ioctl_info_gpu_memory
 *
 * ioctl(2) handler for STROM_IOCTL__INFO_GPU_MEMORY
 */
static int
ioctl_info_gpu_memory(StromCmd__InfoGpuMemory __user *uarg)
{
	StromCmd__InfoGpuMemory karg;
	mapped_gpu_memory *mgmem;
	nvidia_p2p_page_table_t *page_table;
	size_t		length;
	int			i, rc = 0;

	length = offsetof(StromCmd__InfoGpuMemory, paddrs);
	if (copy_from_user(&karg, uarg, length))
		return -EFAULT;

	mgmem = strom_get_mapped_gpu_memory(karg.handle);
	if (!mgmem)
		return -ENOENT;

	page_table       = mgmem->page_table;
	karg.nitems      = page_table->entries;
	karg.version     = page_table->version;
	karg.gpu_page_sz = mgmem->gpu_page_sz;
	karg.owner       = __kuid_val(mgmem->owner);
	karg.map_offset  = mgmem->map_offset;
	karg.map_length  = mgmem->map_length;
	if (copy_to_user((void __user *)uarg, &karg, length))
		rc = -EFAULT;
	else
	{
		for (i=0; i < page_table->entries; i++)
		{
			if (i >= karg.nrooms)
			{
				rc = -ENOBUFS;
				break;
			}
			if (put_user(page_table->pages[i]->physical_address,
						 &uarg->paddrs[i]))
			{
				rc = -EFAULT;
				break;
			}
		}
	}
	strom_put_mapped_gpu_memory(mgmem);

	return rc;
}

/*
 * strom_get_block - a generic version of get_block_t for the supported
 * filesystems. It assumes the target filesystem is already checked by
 * file_is_supported_nvme, so we have minimum checks here.
 */
static inline int
strom_get_block(struct inode *inode, sector_t iblock,
				struct buffer_head *bh, int create)
{
	struct super_block	   *i_sb = inode->i_sb;

	if (i_sb->s_magic == EXT4_SUPER_MAGIC)
		return __ext4_get_block(inode, iblock, bh, create);
	else if (i_sb->s_magic == XFS_SB_MAGIC)
		return __xfs_get_blocks(inode, iblock, bh, create);
	else
		return -ENOTSUPP;
}

/*
 * ioctl_check_file - checks whether the supplied file descriptor is
 * capable to perform P2P DMA from NVMe SSD.
 * Here are various requirement on filesystem / devices.
 *
 * - application has permission to read the file.
 * - filesystem has to be Ext4 or XFS, because Linux has no portable way
 *   to identify device blocks underlying a particular range of the file.
 * - block device of the file has to be NVMe-SSD, managed by the inbox
 *   driver of Linux. RAID configuration is not available to use.
 * - file has to be larger than or equal to PAGE_SIZE, because Ext4/XFS
 *   are capable to have file contents inline, for very small files.
 */
static int
file_is_supported_nvme(struct file *filp, bool is_writable,
					   struct nvme_ns **p_nvme_ns)
{
	struct inode	   *f_inode = filp->f_inode;
	struct super_block *i_sb = f_inode->i_sb;
	struct file_system_type *s_type = i_sb->s_type;
	struct block_device *s_bdev = i_sb->s_bdev;
	struct gendisk	   *bd_disk = s_bdev->bd_disk;
	struct nvme_ns	   *nvme_ns = (struct nvme_ns *)bd_disk->private_data;
	const char		   *dname;
	int					rc;

	/*
	 * must have proper permission to the target file
	 */
	if ((filp->f_mode & (is_writable ? FMODE_WRITE : FMODE_READ)) == 0)
	{
		prError("process (pid=%u) has no permission to read file",
				current->pid);
		return -EACCES;
	}

	/*
	 * check whether it is on supported filesystem
	 *
	 * MEMO: Linux VFS has no reliable way to lookup underlying block
	 *   number of individual files (and, may be impossible in some
	 *   filesystems), so our module solves file offset <--> block number
	 *   on a part of supported filesystems.
	 *
	 * supported: ext4, xfs
	 */
	if (!((i_sb->s_magic == EXT4_SUPER_MAGIC &&
		   strcmp(s_type->name, "ext4") == 0 &&
		   s_type->owner == mod_ext4_get_block) ||
		  (i_sb->s_magic == XFS_SB_MAGIC &&
		   strcmp(s_type->name, "xfs") == 0 &&
		   s_type->owner == mod_xfs_get_blocks)))
	{
		prError("file_system_type name=%s, not supported", s_type->name);
		return -ENOTSUPP;
	}

	/*
	 * check whether the file size is, at least, more than PAGE_SIZE
	 *
	 * MEMO: It is a rough alternative to prevent inline files on Ext4/XFS.
	 * Contents of these files are stored with inode, instead of separate
	 * data blocks. It usually makes no sense on SSD-to-GPU Direct fature.
	 */
	if (!is_writable)
	{
		spin_lock(&f_inode->i_lock);
		if (f_inode->i_size < PAGE_SIZE)
		{
			size_t		i_size = f_inode->i_size;
			spin_unlock(&f_inode->i_lock);
			prError("file size too small (%zu bytes), not suitable", i_size);
			return -ENOTSUPP;
		}
		spin_unlock(&f_inode->i_lock);
	}

	/*
	 * check whether underlying block device is NVMe-SSD
	 *
	 * MEMO: Our assumption is, the supplied file is located on NVMe-SSD,
	 * with other software layer (like dm-based RAID1).
	 */
#if 0
	if (bd_disk->major == MD_MAJOR)
	{
		struct mddev   *mddev;
		struct md_rdev *rdev;
		int				index = 0;

		/* unpartitioned md device has 'md%d' */
		dname = bd_disk->disk_name;
		if (dname[0] == 'm' &&
			dname[1] == 'd')
		{
			const char *pos = dname + 2;

			while (*pos >= '0' && *pos <= '9')
				pos++;
			if (*pos == '\0')
				dname = NULL;
		}

		if (dname)
		{
			prError("block device '%s' is not supported", dname);
			return -ENOTSUPP;
		}
		mddev = bd_disk->private_data;

		prNotice("mddev {flags=%08lx suspended=%d ro=%d ready=%d major=%d minor=%d patch=%d persistent=%d}", mddev->flags, mddev->suspended, mddev->ro, mddev->ready, mddev->major_version, mddev->minor_version, mddev->patch_version, mddev->persistent);
		prNotice("mddev {chunk_sectors=%d level=%d layout=%d raid_disks=%d max_disks=%d}", mddev->chunk_sectors, mddev->level, mddev->layout, mddev->raid_disks, mddev->max_disks);
		prNotice("mddev {dev_sectors=%lu array_sectors=%lu external_size=%d}", (long)mddev->dev_sectors, (long)mddev->array_sectors, mddev->external_size);

		rdev_for_each(rdev, mddev)
		{
			prNotice("rdev[%d] {sectors=%lu data_offset=%lu, new_data_offset=%lu sb_start=%lu}", index, (long)rdev->sectors, (long)rdev->data_offset, (long)rdev->new_data_offset, (long)rdev->sb_start);
			index++;
		}
		return -ENOTSUPP;
	}
#endif

	/* 'devext' shall wrap NVMe-SSD device */
	if (bd_disk->major != BLOCK_EXT_MAJOR)
	{
		prError("block device major number = %d, not 'blkext'",
				bd_disk->major);
		return -ENOTSUPP;
	}

	/* disk_name should be 'nvme%dn%d' */
	dname = bd_disk->disk_name;
	if (dname[0] == 'n' &&
		dname[1] == 'v' &&
		dname[2] == 'm' &&
		dname[3] == 'e')
	{
		const char *pos = dname + 4;
		const char *pos_saved = pos;

		while (*pos >= '0' && *pos <= '9')
			pos++;
		if (pos != pos_saved && *pos == 'n')
		{
			pos_saved = ++pos;

			while (*pos >= '0' && *pos <= '9')
				pos++;
			if (pos != pos_saved && *pos == '\0')
				dname = NULL;/* OK, it is NVMe-SSD */
		}
	}

	if (dname)
	{
		prError("block device '%s' is not supported", dname);
		return -ENOTSUPP;
	}

	/* try to call ioctl */
	if (!bd_disk->fops->ioctl)
	{
		prError("block device '%s' does not provide ioctl",
				bd_disk->disk_name);
		return -ENOTSUPP;
	}

	rc = bd_disk->fops->ioctl(s_bdev, 0, NVME_IOCTL_ID, 0UL);
	if (rc < 0)
	{
		prError("ioctl(NVME_IOCTL_ID) on '%s' returned an error: %d",
				bd_disk->disk_name, rc);
		return -ENOTSUPP;
	}

	/*
	 * check block size of the device.
	 */
	if (i_sb->s_blocksize > PAGE_CACHE_SIZE)
	{
		prError("block size of '%s' is %zu; larger than PAGE_CACHE_SIZE",
				bd_disk->disk_name, (size_t)i_sb->s_blocksize);
		return -ENOTSUPP;
	}

	if (p_nvme_ns)
		*p_nvme_ns = nvme_ns;

	/* OK, we assume the underlying device is supported NVMe-SSD */
	return 0;
}

/*
 * ioctl_check_file
 *
 * ioctl(2) handler for STROM_IOCTL__CHECK_FILE
 */
static int
ioctl_check_file(StromCmd__CheckFile __user *uarg)
{
	StromCmd__CheckFile karg;
	struct file	   *filp;
	int				rc;

	if (copy_from_user(&karg, uarg, sizeof(karg)))
		return -EFAULT;

	filp = fget(karg.fdesc);
	if (!filp)
		return -EBADF;

	rc = file_is_supported_nvme(filp, false, NULL);

	fput(filp);

	return (rc < 0 ? rc : 0);
}

/* ================================================================
 *
 * Main part for SSD-to-GPU P2P DMA
 *
 * ================================================================
 */

/*
 * NOTE: It looks to us Intel 750 SSD does not accept DMA request larger
 * than 128KB. However, we are not certain whether it is restriction for
 * all the NVMe-SSD devices. Right now, 128KB is a default of the max unit
 * length of DMA request.
 */
#define STROM_DMA_SSD2GPU_MAXLEN		(128 * 1024)

struct strom_dma_task
{
	struct list_head	chain;
	unsigned long		dma_task_id;/* ID of this DMA task */
	int					hindex;		/* index of hash slot */
	atomic_t			refcnt;		/* reference counter */
	bool				frozen;		/* (DEBUG) no longer newly referenced */
	mapped_gpu_memory  *mgmem;		/* destination GPU memory segment */
	/* reference to the backing file */
	struct file		   *filp;		/* source file */
	struct nvme_ns	   *nvme_ns;	/* NVMe namespace (=SCSI LUN) */
	size_t				blocksz;	/* blocksize of this partition */
	int					blocksz_shift;	/* log2 of 'blocksz' */
	sector_t			start_sect;	/* first sector of the source partition */
	sector_t			nr_sects;	/* number of sectors of the partition */
	unsigned int		max_nblocks;/* upper limit of @nr_blocks */

	/*
	 * status of asynchronous tasks
	 *
	 * MEMO: Pay attention to error status of the asynchronous tasks.
	 * Asynchronous task may cause errors on random timing, and kernel
	 * space wants to inform this status on the next call. On the other
	 * hands, application may invoke ioctl(2) to reference DMA results,
	 * but may not. So, we have to keep an error status somewhere, but
	 * also needs to be released on appropriate timing; to avoid kernel
	 * memory leak by rude applications.
	 * If any errors, we attach strom_dma_task structure on file handler
	 * used for ioctl(2). The error status shall be reclaimed on the
	 * next time when application wait for a particular DMA task, or
	 * this file handler is closed.
	 */
	long				dma_status;
	struct file		   *ioctl_filp;

	/* state of the current pending SSD2GPU DMA request */
	loff_t				dest_offset;/* current destination offset */
	sector_t			src_block;	/* head of the source blocks */
	unsigned int		nr_blocks;	/* # of the contigunous source blocks */
	/* temporary buffer for locked page cache in a chunk */
	struct page		   *file_pages[STROM_DMA_SSD2GPU_MAXLEN / PAGE_CACHE_SIZE];
};
typedef struct strom_dma_task	strom_dma_task;

#define STROM_DMA_TASK_NSLOTS		240
static spinlock_t		strom_dma_task_locks[STROM_DMA_TASK_NSLOTS];
static struct list_head	strom_dma_task_slots[STROM_DMA_TASK_NSLOTS];
static struct list_head	failed_dma_task_slots[STROM_DMA_TASK_NSLOTS];
static wait_queue_head_t strom_dma_task_waitq[STROM_DMA_TASK_NSLOTS];

/*
 * strom_dma_task_index
 */
static inline int
strom_dma_task_index(unsigned long dma_task_id)
{
	u32		hash = arch_fast_hash(&dma_task_id, sizeof(unsigned long),
								  0x20120106);
	return hash % STROM_DMA_TASK_NSLOTS;
}

/*
 * strom_create_dma_task
 */
static strom_dma_task *
strom_create_dma_task(unsigned long handle,
					  int fdesc,
					  struct file *ioctl_filp)
{
	mapped_gpu_memory	   *mgmem;
	strom_dma_task		   *dtask;
	struct file			   *filp;
	struct super_block	   *i_sb;
	struct block_device	   *s_bdev;
	struct nvme_ns		   *nvme_ns;
	long					retval;
	unsigned long			flags;

	/* ensure the source file is supported */
	filp = fget(fdesc);
	if (!filp)
	{
		prError("file descriptor %d of process %u is not available",
				fdesc, current->tgid);
		retval = -EBADF;
		goto error_0;
	}
	retval = file_is_supported_nvme(filp, false, &nvme_ns);
	if (retval < 0)
		goto error_1;
	i_sb = filp->f_inode->i_sb;
	s_bdev = i_sb->s_bdev;

	/* get destination GPU memory */
	mgmem = strom_get_mapped_gpu_memory(handle);
	if (!mgmem)
	{
		retval = -ENOENT;
		goto error_1;
	}

	/* allocate strom_dma_task object */
	dtask = kzalloc(sizeof(strom_dma_task), GFP_KERNEL);
	if (!dtask)
	{
		retval = -ENOMEM;
		goto error_2;
	}
	dtask->dma_task_id	= (unsigned long) dtask;
	dtask->hindex		= strom_dma_task_index(dtask->dma_task_id);
    atomic_set(&dtask->refcnt, 1);
	dtask->frozen		= false;
    dtask->mgmem		= mgmem;
    dtask->filp			= filp;
	dtask->nvme_ns		= nvme_ns;
	dtask->blocksz		= i_sb->s_blocksize;
	dtask->blocksz_shift = i_sb->s_blocksize_bits;
	Assert(dtask->blocksz == (1UL << dtask->blocksz_shift));
	dtask->start_sect	= s_bdev->bd_part->start_sect;
	dtask->nr_sects		= s_bdev->bd_part->nr_sects;
	dtask->max_nblocks = STROM_DMA_SSD2GPU_MAXLEN >> dtask->blocksz_shift;
    dtask->dma_status	= 0;
    dtask->ioctl_filp	= get_file(ioctl_filp);
	dtask->dest_offset	= 0;
	dtask->src_block	= 0;
	dtask->nr_blocks	= 0;

	/* OK, this strom_dma_task is now tracked */
	spin_lock_irqsave(&strom_dma_task_locks[dtask->hindex], flags);
	list_add_rcu(&dtask->chain, &strom_dma_task_slots[dtask->hindex]);
	spin_unlock_irqrestore(&strom_dma_task_locks[dtask->hindex], flags);

	return dtask;

error_2:
	strom_put_mapped_gpu_memory(mgmem);
error_1:
	fput(filp);
error_0:
	return ERR_PTR(retval);
}

/*
 * strom_get_dma_task
 */
static strom_dma_task *
strom_get_dma_task(strom_dma_task *dtask)
{
	int		refcnt_new;

	Assert(!dtask->frozen);
	refcnt_new = atomic_inc_return(&dtask->refcnt);
	Assert(refcnt_new > 1);

	return dtask;
}

/*
 * strom_put_dma_task
 */
static void
strom_put_dma_task(strom_dma_task *dtask, long dma_status)
{
	int					hindex = dtask->hindex;
	unsigned long		flags = 0;
	bool				has_spinlock = false;

	if (unlikely(dma_status))
	{
		spin_lock_irqsave(&strom_dma_task_locks[hindex], flags);
		if (!dtask->dma_status)
			dtask->dma_status = dma_status;
		has_spinlock = true;
	}

	if (atomic_dec_and_test(&dtask->refcnt))
	{
		mapped_gpu_memory *mgmem = dtask->mgmem;
		struct file	   *ioctl_filp = dtask->ioctl_filp;
		struct file	   *data_filp = dtask->filp;
		long			dma_status;

		if (!has_spinlock)
			spin_lock_irqsave(&strom_dma_task_locks[hindex], flags);
		/* should be released after the final async job is submitted */
		Assert(dtask->frozen);
		/* fetch status under the lock */
		dma_status = dtask->dma_status;
		/* detach from the global hash table */
		list_del_rcu(&dtask->chain);
		/* move to the error task list, if any error */
		if (unlikely(dma_status))
		{
			dtask->ioctl_filp = NULL;
			dtask->filp = NULL;
			dtask->mgmem = NULL;

			list_add_tail_rcu(&dtask->chain, &failed_dma_task_slots[hindex]);
		}
		spin_unlock_irqrestore(&strom_dma_task_locks[hindex], flags);
		/* wake up all the waiting tasks, if any */
		wake_up_all(&strom_dma_task_waitq[hindex]);

		/* release the dtask object, if no error */
		if (likely(!dma_status))
			kfree(dtask);
		strom_put_mapped_gpu_memory(mgmem);
		fput(data_filp);
		fput(ioctl_filp);

		prDebug("DMA task (id=%p) was completed", dtask);
	}
	else if (has_spinlock)
		spin_unlock_irqrestore(&strom_dma_task_locks[hindex], flags);
}

/*
 * DMA transaction for SSD->GPU asynchronous copy
 */
#ifdef STROM_TARGET_KERNEL_RHEL7
#include "nvme_strom.rhel7.c"
#else
#error "no platform specific NVMe-SSD routines"
#endif

/* alternative of the core nvme_alloc_iod */
static struct nvme_iod *
nvme_alloc_iod(size_t nbytes,
			   mapped_gpu_memory *mgmem,
			   struct nvme_dev *dev, gfp_t gfp)
{
	struct nvme_iod *iod;
	unsigned int	nsegs;
	unsigned int	nprps;
	unsigned int	npages;

	/*
	 * Will slightly overestimate the number of pages needed.  This is OK
	 * as it only leads to a small amount of wasted memory for the lifetime of
	 * the I/O.
	 */
	nsegs = DIV_ROUND_UP(nbytes + mgmem->gpu_page_sz, mgmem->gpu_page_sz);
	nprps = DIV_ROUND_UP(nbytes + dev->page_size, dev->page_size);
	npages = DIV_ROUND_UP(8 * nprps, dev->page_size - 8);

	iod = kmalloc(offsetof(struct nvme_iod, sg[nsegs]) +
				  sizeof(__le64) * npages, gfp);
	if (iod)
	{
		iod->private = 0;
		iod->npages = -1;
		iod->offset = offsetof(struct nvme_iod, sg[nsegs]);
		iod->length = nbytes;
		iod->nents = 0;
		iod->first_dma = 0ULL;
	}
	sg_init_table(iod->sg, nsegs);

	return iod;
}

static int
submit_ssd2gpu_memcpy(strom_dma_task *dtask)
{
	mapped_gpu_memory  *mgmem = dtask->mgmem;
	nvidia_p2p_page_table_t *page_table = mgmem->page_table;
	struct nvme_ns	   *nvme_ns = dtask->nvme_ns;
	struct nvme_dev	   *nvme_dev = nvme_ns->dev;
	struct nvme_iod	   *iod;
	size_t				offset;
	size_t				total_nbytes;
	dma_addr_t			base_addr;
	int					length;
	int					i, base;
	int					retval;

	total_nbytes = (dtask->nr_blocks << dtask->blocksz_shift);
	if (!total_nbytes || total_nbytes > STROM_DMA_SSD2GPU_MAXLEN)
		return -EINVAL;
	if (dtask->dest_offset < mgmem->map_offset ||
		dtask->dest_offset + total_nbytes > (mgmem->map_offset +
											 mgmem->map_length))
		return -ERANGE;

	iod = nvme_alloc_iod(total_nbytes,
						 mgmem,
						 nvme_dev,
						 GFP_KERNEL);
	if (!iod)
		return -ENOMEM;

	base = (dtask->dest_offset >> mgmem->gpu_page_shift);
	offset = (dtask->dest_offset & (mgmem->gpu_page_sz - 1));
	prDebug("base=%d offset=%zu dest_offset=%zu total_nbytes=%zu",
			base, offset, (size_t)dtask->dest_offset, total_nbytes);

	for (i=0; i < page_table->entries; i++)
	{
		if (!total_nbytes)
			break;

		base_addr = page_table->pages[base + i]->physical_address;
		length = Min(total_nbytes, mgmem->gpu_page_sz - offset);
		iod->sg[i].page_link = 0;
		iod->sg[i].dma_address = base_addr + offset;
		iod->sg[i].length = length;
		iod->sg[i].dma_length = length;
		iod->sg[i].offset = 0;

		offset = 0;
		total_nbytes -= length;
	}

	if (total_nbytes)
	{
		__nvme_free_iod(nvme_dev, iod);
		return -EINVAL;
	}
	sg_mark_end(&iod->sg[i]);
	iod->nents = i;

	retval = nvme_submit_async_read_cmd(dtask, iod);
	if (retval)
		__nvme_free_iod(nvme_dev, iod);

	return retval;
}

/*
 * strom_memcpy_ssd2gpu_wait - synchronization of a dma_task
 */
static int
strom_memcpy_ssd2gpu_wait(unsigned long dma_task_id,
						  long *p_dma_task_status,
						  int task_state)
{
	int					hindex = strom_dma_task_index(dma_task_id);
	spinlock_t		   *lock = &strom_dma_task_locks[hindex];
	wait_queue_head_t  *waitq = &strom_dma_task_waitq[hindex];
	unsigned long		flags = 0;
	strom_dma_task	   *dtask;
	struct list_head   *slot;
	int					retval = 0;

	DEFINE_WAIT(__wait);
	for (;;)
	{
		bool	has_spinlock = false;
		bool	task_is_running = false;

		rcu_read_lock();
	retry:
		/* check error status first */
		slot = &failed_dma_task_slots[hindex];
		list_for_each_entry_rcu(dtask, slot, chain)
		{
			if (dtask->dma_task_id == dma_task_id)
			{
				if (!has_spinlock)
				{
					rcu_read_unlock();
					has_spinlock = true;
					spin_lock_irqsave(lock, flags);
					goto retry;
				}
				if (p_dma_task_status)
					*p_dma_task_status = dtask->dma_status;
				list_del(&dtask->chain);
				spin_unlock_irqrestore(lock, flags);
				kfree(dtask);
				retval = -EIO;

				goto out;
			}
		}

		/* check whether it is a running task or not */
		slot = &strom_dma_task_slots[hindex];
		list_for_each_entry_rcu(dtask, slot, chain)
		{
			if (dtask->dma_task_id == dma_task_id)
			{
				task_is_running = true;
				break;
			}
		}
		if (has_spinlock)
			spin_unlock_irqrestore(lock, flags);
		else
			rcu_read_unlock();

		if (!task_is_running)
			break;
		if (signal_pending(current))
		{
			retval = -EINTR;
			break;
		}
		/* wait for completion of DMA task */
		prepare_to_wait(waitq, &__wait, task_state);
		schedule();
	}
out:
	finish_wait(waitq, &__wait);

	return retval;
}

/*
 * ioctl(2) handler for STROM_IOCTL__MEMCPY_SSD2GPU_WAIT
 */
static int
ioctl_memcpy_ssd2gpu_wait(StromCmd__MemCpySsdToGpuWait __user *uarg,
						  struct file *ioctl_filp)
{
	StromCmd__MemCpySsdToGpuWait karg;
	long		retval;

	if (copy_from_user(&karg, uarg, sizeof(StromCmd__MemCpySsdToGpuWait)))
		return -EFAULT;

	karg.status = 0;
	retval = strom_memcpy_ssd2gpu_wait(karg.dma_task_id,
									   &karg.status,
									   TASK_INTERRUPTIBLE);
	if (copy_to_user(uarg, &karg, sizeof(StromCmd__MemCpySsdToGpuWait)))
		return -EFAULT;

	return retval;
}

/*
 * write back a chunk to user buffer
 */
static int
__memcpy_ssd2gpu_writeback(strom_dma_task *dtask,
						   int nr_pages,
						   loff_t fpos,
						   char __user *dest_uaddr)
{
	struct file	   *filp = dtask->filp;
	struct page	   *fpage;
	char		   *kaddr;
	pgoff_t			fp_index = fpos >> PAGE_CACHE_SHIFT;
	loff_t			left;
	int				i, retval = 0;

	for (i=0; i < nr_pages; i++)
	{
		fpage = dtask->file_pages[i];
		/* Synchronous read, if not cached */
		if (!fpage)
		{
			fpage = read_mapping_page(filp->f_mapping, fp_index + i, NULL);
			if (IS_ERR(fpage))
			{
				retval = PTR_ERR(fpage);
				break;
			}
			lock_page(fpage);
			dtask->file_pages[i] = fpage;
		}
		Assert(fpage != NULL);

		/* write-back the pages to userspace, like file_read_actor() */
		if (unlikely(fault_in_pages_writeable(dest_uaddr, PAGE_CACHE_SIZE)))
			left = 1;	/* go to slow way */
		else
		{
			kaddr = kmap_atomic(fpage);
			left = __copy_to_user_inatomic(dest_uaddr, kaddr,
										   PAGE_CACHE_SIZE);
			kunmap_atomic(kaddr);
		}

		/* Do it by the slow way, if needed */
		if (unlikely(left))
		{
			kaddr = kmap(fpage);
			left = __copy_to_user(dest_uaddr, kaddr, PAGE_CACHE_SIZE);
			kunmap(fpage);

			if (unlikely(left))
			{
				retval = -EFAULT;
				break;
			}
		}
		dest_uaddr += PAGE_CACHE_SIZE;
	}
	return retval;
}

/*
 * Submit a P2P DMA request
 */
static int
__memcpy_ssd2gpu_submit_dma(strom_dma_task *dtask,
							int nr_pages,
							loff_t fpos,
							loff_t dest_offset,
							unsigned int *p_nr_dma_submit,
							unsigned int *p_nr_dma_blocks)
{
	struct file		   *filp = dtask->filp;
	struct buffer_head	bh;
	unsigned int		nr_blocks;
	loff_t				curr_offset = dest_offset;
	int					i, retval = 0;

	for (i=0; i < nr_pages; i++, fpos += PAGE_CACHE_SIZE)
	{
		/* lookup the source block number */
		memset(&bh, 0, sizeof(bh));
		bh.b_size = dtask->blocksz;

		retval = strom_get_block(filp->f_inode,
								 fpos >> dtask->blocksz_shift,
								 &bh, 0);
		if (retval)
		{
			prError("strom_get_block: %d", retval);
			break;
		}
		nr_blocks = PAGE_CACHE_SIZE >> dtask->blocksz_shift;

		/* merge with pending request if possible */
		if (dtask->nr_blocks > 0 &&
			dtask->nr_blocks + nr_blocks <= dtask->max_nblocks &&
			dtask->src_block + dtask->nr_blocks == bh.b_blocknr &&
			dtask->dest_offset +
			dtask->nr_blocks * dtask->blocksz == curr_offset)
		{
			dtask->nr_blocks += nr_blocks;
		}
		else
		{
			/* submit pending SSD2GPU DMA */
			if (dtask->nr_blocks > 0)
			{
				(*p_nr_dma_submit)++;
				(*p_nr_dma_blocks) += dtask->nr_blocks;
				retval = submit_ssd2gpu_memcpy(dtask);
				if (retval)
				{
					prError("submit_ssd2gpu_memcpy: %d", retval);
					break;
				}
			}
			dtask->dest_offset = curr_offset;
			dtask->src_block = bh.b_blocknr;
			dtask->nr_blocks = nr_blocks;
		}
		curr_offset += PAGE_CACHE_SIZE;
	}
	return retval;
}

/*
 * main logic of STROM_IOCTL__MEMCPY_SSD2GPU_WRITEBACK
 */
static int
memcpy_ssd2gpu_writeback(StromCmd__MemCpySsdToGpuWriteBack *karg,
						 strom_dma_task *dtask,
						 uint32_t *chunk_ids_in,
						 uint32_t *chunk_ids_out)
{
	mapped_gpu_memory *mgmem = dtask->mgmem;
	struct file	   *filp = dtask->filp;
	char __user	   *dest_uaddr;
	size_t			dest_offset;
	unsigned int	nr_pages = (karg->chunk_sz >> PAGE_CACHE_SHIFT);
	int				threshold = nr_pages / 2;
	size_t			i_size;
	int				retval = 0;
	int				i, j, k;

	/* sanity checks */
	if ((karg->chunk_sz & (PAGE_CACHE_SIZE - 1)) != 0 ||	/* alignment */
		karg->chunk_sz < PAGE_CACHE_SIZE ||					/* >= 4KB */
		karg->chunk_sz > STROM_DMA_SSD2GPU_MAXLEN)			/* <= 128KB */
		return -EINVAL;

	dest_offset = mgmem->map_offset + karg->offset;
	if (dest_offset + karg->nr_chunks * karg->chunk_sz > mgmem->map_length)
		return -ERANGE;

	i_size = i_size_read(filp->f_inode);
	for (i=0; i < karg->nr_chunks; i++)
	{
		loff_t			chunk_id = chunk_ids_in[i];
		loff_t			fpos;
		struct page	   *fpage;
		int				score = 0;

		if (karg->relseg_sz == 0)
			fpos = chunk_id * karg->chunk_sz;
		else
			fpos = (chunk_id % karg->relseg_sz) * karg->chunk_sz;
		Assert((fpos & (PAGE_CACHE_SIZE - 1)) == 0);
		if (fpos > i_size)
			return -ERANGE;

		for (j=0, k=fpos >> PAGE_CACHE_SHIFT; j < nr_pages; j++, k++)
		{
			fpage = find_lock_page(filp->f_mapping, k);
			dtask->file_pages[j] = fpage;
			if (fpage)
				score += (PageDirty(fpage) ? threshold + 1 : 1);
		}

		if (score > threshold)
		{
			/*
			 * Write-back of file pages if majority of the chunk is cached,
			 * then application shall call cuMemcpyHtoD for RAM2GPU DMA.
			 */
			karg->nr_ram2gpu++;
			dest_uaddr = karg->wb_buffer +
				karg->chunk_sz * (karg->nr_chunks - karg->nr_ram2gpu);
			retval = __memcpy_ssd2gpu_writeback(dtask,
												nr_pages,
												fpos,
												dest_uaddr);
			chunk_ids_out[karg->nr_chunks -
						  karg->nr_ram2gpu] = (uint32_t)chunk_id;
		}
		else
		{
			retval = __memcpy_ssd2gpu_submit_dma(dtask,
												 nr_pages,
												 fpos,
												 dest_offset,
												 &karg->nr_dma_submit,
												 &karg->nr_dma_blocks);
			chunk_ids_out[karg->nr_ssd2gpu] = (uint32_t)chunk_id;
			dest_offset += karg->chunk_sz;
			karg->nr_ssd2gpu++;
		}

		/*
		 * MEMO: score==0 means no pages were cached, so we can skip loop
		 * to unlock/release pages. It's a small optimization.
		 */
		if (score > 0)
		{
			for (j=0; j < nr_pages; j++)
			{
				fpage = dtask->file_pages[j];
				if (fpage)
				{
					unlock_page(fpage);
					page_cache_release(fpage);
				}
			}
		}

		if (retval)
			return retval;
	}
	/* submit pending SSD2GPU DMA request, if any */
	if (dtask->nr_blocks > 0)
	{
		karg->nr_dma_submit++;
		karg->nr_dma_blocks += dtask->nr_blocks;
		submit_ssd2gpu_memcpy(dtask);
	}
	Assert(karg->nr_ram2gpu + karg->nr_ssd2gpu == karg->nr_chunks);

	return 0;
}

/*
 * ioctl(2) handler for STROM_IOCTL__MEMCPY_SSD2GPU_WRITEBACK
 */
static int
ioctl_memcpy_ssd2gpu_writeback(StromCmd__MemCpySsdToGpuWriteBack __user *uarg,
							   struct file *ioctl_filp)
{
	StromCmd__MemCpySsdToGpuWriteBack karg;
	strom_dma_task *dtask;
	uint32_t	   *chunk_ids_in = NULL;
	uint32_t	   *chunk_ids_out = NULL;
	int				retval;

	if (copy_from_user(&karg, uarg, sizeof(StromCmd__MemCpySsdToGpuWriteBack)))
		return -EFAULT;
	chunk_ids_in = kmalloc(2 * sizeof(uint32_t) * karg.nr_chunks, GFP_KERNEL);
	if (!chunk_ids_in)
		return -ENOMEM;
	if (copy_from_user(chunk_ids_in, karg.chunk_ids,
					   sizeof(uint32_t) * karg.nr_chunks))
	{
		retval = -EFAULT;
		goto out;
	}
	chunk_ids_out = chunk_ids_in + karg.nr_chunks;

	/* setup DMA task */
	dtask = strom_create_dma_task(karg.handle,
								  karg.file_desc,
								  ioctl_filp);
	if (IS_ERR(dtask))
	{
		retval = PTR_ERR(dtask);
		goto out;
	}
	karg.dma_task_id = dtask->dma_task_id;
	karg.nr_ram2gpu = 0;
	karg.nr_ssd2gpu = 0;
	karg.nr_dma_submit = 0;
	karg.nr_dma_blocks = 0;
	
	retval = memcpy_ssd2gpu_writeback(&karg, dtask,
									  chunk_ids_in,
									  chunk_ids_out);
	/* no more async jobs shall not acquire the @dtask any more */
	dtask->frozen = true;
	barrier();

	strom_put_dma_task(dtask, 0);

	/* write back the results */
	if (!retval)
	{
		if (copy_to_user(uarg, &karg,
						 offsetof(StromCmd__MemCpySsdToGpuWriteBack, handle)))
			retval = -EFAULT;
		else if (copy_to_user(karg.chunk_ids, chunk_ids_out,
							  sizeof(uint32_t) * karg.nr_chunks))
			retval = -EFAULT;
	}
	/* synchronization of completion if any error */
	if (retval)
		strom_memcpy_ssd2gpu_wait(karg.dma_task_id, NULL,
								  TASK_UNINTERRUPTIBLE);
out:
	kfree(chunk_ids_in);
	return retval;
}

/* ================================================================
 *
 * file_operations of '/proc/nvme-strom' entry
 *
 * ================================================================
 */
static const char  *strom_proc_signature =		\
	"version: " NVME_STROM_VERSION "\n"			\
	"target: " UTS_RELEASE "\n"					\
	"build: " NVME_STROM_BUILD_TIMESTAMP "\n";

static int
strom_proc_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t
strom_proc_read(struct file *filp, char __user *buf, size_t len, loff_t *pos)
{
	size_t		sig_len = strlen(strom_proc_signature);

	if (*pos >= sig_len)
		return 0;
	if (*pos + len >= sig_len)
		len = sig_len - *pos;
	if (copy_to_user(buf, strom_proc_signature + *pos, len))
		return -EFAULT;
	*pos += len;

	return len;
}

static int
strom_proc_release(struct inode *inode, struct file *filp)
{
	int			i;

	for (i=0; i < STROM_DMA_TASK_NSLOTS; i++)
	{
		spinlock_t		   *lock = &strom_dma_task_locks[i];
		struct list_head   *slot = &failed_dma_task_slots[i];
		unsigned long		flags;
		strom_dma_task	   *dtask;
		strom_dma_task	   *dnext;

		spin_lock_irqsave(lock, flags);
		list_for_each_entry_safe(dtask, dnext, slot, chain)
		{
			if (dtask->ioctl_filp == filp)
			{
				prNotice("Unreferenced asynchronous SSD2GPU DMA error "
						 "(dma_task_id: %lu, status=%ld)",
						 dtask->dma_task_id, dtask->dma_status);
				list_del_rcu(&dtask->chain);
				kfree(dtask);
			}
		}
		spin_unlock_irqrestore(lock, flags);
	}
	return 0;
}

static long
strom_proc_ioctl(struct file *ioctl_filp,
				 unsigned int cmd,
				 unsigned long arg)
{
	long		retval;

	switch (cmd)
	{
		case STROM_IOCTL__CHECK_FILE:
			retval = ioctl_check_file((void __user *) arg);
			break;

		case STROM_IOCTL__MAP_GPU_MEMORY:
			retval = ioctl_map_gpu_memory((void __user *) arg);
			break;

		case STROM_IOCTL__UNMAP_GPU_MEMORY:
			retval = ioctl_unmap_gpu_memory((void __user *) arg);
			break;

		case STROM_IOCTL__LIST_GPU_MEMORY:
			retval = ioctl_list_gpu_memory((void __user *) arg);
			break;

		case STROM_IOCTL__INFO_GPU_MEMORY:
			retval = ioctl_info_gpu_memory((void __user *) arg);
			break;

		case STROM_IOCTL__MEMCPY_SSD2GPU_WRITEBACK:
			retval = ioctl_memcpy_ssd2gpu_writeback((void __user *) arg,
													ioctl_filp);
			break;

		case STROM_IOCTL__MEMCPY_SSD2GPU_WAIT:
			retval = ioctl_memcpy_ssd2gpu_wait((void __user *) arg,
											   ioctl_filp);
			break;

		default:
			retval = -EINVAL;
			break;
	}
	return retval;
}

/* device file operations */
static const struct file_operations nvme_strom_fops = {
	.owner			= THIS_MODULE,
	.open			= strom_proc_open,
	.read			= strom_proc_read,
	.release		= strom_proc_release,
	.unlocked_ioctl	= strom_proc_ioctl,
	.compat_ioctl	= strom_proc_ioctl,
};

int	__init nvme_strom_init(void)
{
	int			i, rc;

	/* init strom_mgmem_mutex/slots */
	for (i=0; i < MAPPED_GPU_MEMORY_NSLOTS; i++)
	{
		spin_lock_init(&strom_mgmem_locks[i]);
		INIT_LIST_HEAD(&strom_mgmem_slots[i]);
	}

	/* init strom_dma_task_locks/slots */
	for (i=0; i < STROM_DMA_TASK_NSLOTS; i++)
	{
		spin_lock_init(&strom_dma_task_locks[i]);
		INIT_LIST_HEAD(&strom_dma_task_slots[i]);
		INIT_LIST_HEAD(&failed_dma_task_slots[i]);
		init_waitqueue_head(&strom_dma_task_waitq[i]);
	}

	/* make "/proc/nvme-strom" entry */
	nvme_strom_proc = proc_create("nvme-strom",
								  0444,
								  NULL,
								  &nvme_strom_fops);
	if (!nvme_strom_proc)
		return -ENOMEM;

	/* solve mandatory symbols */
	rc = strom_init_extra_symbols();
	if (rc)
	{
		proc_remove(nvme_strom_proc);
		return rc;
	}
	prNotice("/proc/nvme-strom entry was registered");

	return 0;
}
module_init(nvme_strom_init);

void __exit nvme_strom_exit(void)
{
	strom_exit_extra_symbols();
	proc_remove(nvme_strom_proc);
	prNotice("/proc/nvme-strom entry was unregistered");
}
module_exit(nvme_strom_exit);

MODULE_AUTHOR("KaiGai Kohei <kaigai@kaigai.gr.jp>");
MODULE_DESCRIPTION("SSD-to-GPU Direct Stream Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.5");
