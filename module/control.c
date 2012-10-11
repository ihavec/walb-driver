/**
 * control.c - control interface for walb.
 *
 * Copyright(C) 2010, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/rwsem.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "kern.h"
#include "hashtbl.h"
#include "control.h"
#include "alldevs.h"

#include "walb/ioctl.h"

/*******************************************************************************
 * Prototype of static functions.
 *******************************************************************************/

static int ioctl_dev_start(struct walb_ctl *ctl);
static int ioctl_dev_stop(struct walb_ctl *ctl);
static int ioctl_get_major(struct walb_ctl *ctl);
static int ioctl_get_dev_list(struct walb_ctl *ctl);
static int ioctl_get_num_dev(struct walb_ctl *ctl);
static int dispatch_ioctl(struct walb_ctl *ctl);
static int ctl_ioctl(unsigned int command, struct walb_ctl __user *user);
static long walb_ctl_ioctl(
	struct file *file, unsigned int command, unsigned long u);
#ifdef CONFIG_COMPAT
static long walb_ctl_compat_ioctl(
	struct file *file, unsigned int command, unsigned long u);
#else
#define walb_ctl_compat_ioctl NULL
#endif

/*******************************************************************************
 * Implementation of static functions.
 *******************************************************************************/

/**
 * Start walb device.
 *
 * @ctl walb_ctl data.
 *	command == WALB_IOCTL_DEV_START
 *	Input:
 *	  u2k
 *	    wminor (even value --> v	: wdev minor,
 *				   v + 1: wlog minor,
 *		   WALB_DYNAMIC_MINOR means automatic assignment),
 *	    lmajor, lminor,
 *	    dmajor, dminor,
 *	    buf_size (<= DISK_NAME_LEN),
 *	    (char *)__buf (name of walb device. terminated by '\0').
 *	Output:
 *	  error: 0 in success.
 *	  k2u
 *	    wmajor, wminor
 *	    buf_size (<= DISK_NAME_LEN)
 *	    (char *)__buf (name of walb device. terminated by '\0')
 *
 * @return 0 in success, or -EFAULT.
 */
static int ioctl_dev_start(struct walb_ctl *ctl)
{
	dev_t ldevt, ddevt;
	unsigned int wminor;
	struct walb_dev *wdev;
	char *name;
	
	ASSERT(ctl->command == WALB_IOCTL_DEV_START);

	print_walb_ctl(ctl); /* debug */
	
	ldevt = MKDEV(ctl->u2k.lmajor, ctl->u2k.lminor);
	ddevt = MKDEV(ctl->u2k.dmajor, ctl->u2k.dminor);
	LOGd("ioctl_dev_start: (ldevt %u:%u) (ddevt %u:%u)\n",
		MAJOR(ldevt), MINOR(ldevt),
		MAJOR(ddevt), MINOR(ddevt));
	if (ctl->u2k.buf_size > 0) {
		name = (char *)ctl->u2k.__buf;
		LOGd("name len: %zu\n", strnlen(name, DISK_NAME_LEN));
	} else {
		name = NULL;
	}

	alldevs_write_lock();
	
	if (ctl->u2k.wminor == WALB_DYNAMIC_MINOR) {
		wminor = get_free_minor();
	} else {
		wminor = ctl->u2k.wminor;
		if (wminor % 2 != 0) { wminor --; }
	}
	LOGd("ioctl_dev_start: wminor: %u\n", wminor);
	
	wdev = prepare_wdev(wminor, ldevt, ddevt, name);
	if (wdev == NULL) {
		alldevs_write_unlock();
		ctl->error = -1;
		goto error0;
	}
	
	if (alldevs_add(wdev) != 0) {
		alldevs_write_unlock();
		ctl->error = -2;
		goto error1;
	}
	
	register_wdev(wdev);
	alldevs_write_unlock();

	/* Return values to userland. */
	ctl->k2u.wmajor = walb_major_;
	ctl->k2u.wminor = wminor;
	strncpy(ctl->k2u.__buf, get_super_sector(wdev->lsuper0)->name,
		DISK_NAME_LEN);
	ctl->error = 0;

	print_walb_ctl(ctl); /* debug */
	return 0;
	
	/* not tested */

/* error2: */
/*	   alldevs_dell(wdev); */
error1:
	destroy_wdev(wdev);
error0:
	return -EFAULT;
}

/**
 * Stop walb device.
 *
 * @ctl walb_ctl data.
 *	command == WALB_IOCTL_DEV_STOP
 *	Input:
 *	  u2k
 *	    wmajor, wminor,
 *	Output:
 *	  error: 0 in success.
 *
 * @return 0 in success, or -EFAULT.
 */
static int ioctl_dev_stop(struct walb_ctl *ctl)
{
	dev_t wdevt;
	unsigned int wmajor, wminor;
	struct walb_dev *wdev;
	
	ASSERT(ctl->command == WALB_IOCTL_DEV_STOP);

	/* Input */
	wmajor = ctl->u2k.wmajor;
	wminor = ctl->u2k.wminor;
	if (wmajor != walb_major_) {
		LOGe("Device major id is invalid.\n");
		goto error0;
	}
	wdevt = MKDEV(wmajor, wminor);

	alldevs_read_lock();
	wdev = search_wdev_with_minor(wminor);
	alldevs_read_unlock();

	if (wdev == NULL) {
		LOGe("Walb dev with minor %u not found.\n",
			wminor);
		ctl->error = -1;
		goto error0;
	}

	unregister_wdev(wdev);
	
	alldevs_write_lock();
	alldevs_del(wdev);
	alldevs_write_unlock();
	
	destroy_wdev(wdev);

	/* Set result */
	ctl->error = 0;

	return 0;
	
	/* not tested */
	
error0:	       
	return -EFAULT;
}

/**
 * Get major.
 *
 * @ctl walb ctl.
 *   See walb/ioctl.h for input/output details.
 * RETURN:
 *   0.
 */
static int ioctl_get_major(struct walb_ctl *ctl)
{
	ASSERT(ctl);
	ASSERT(ctl->command == WALB_IOCTL_GET_MAJOR);

	ctl->k2u.wmajor = walb_major_;
	ctl->error = 0;
	return 0;
}

/**
 * Get device list over a range.
 *
 * @ctl walb ctl.
 *   See walb/ioctl.h for input/output details.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_get_dev_list(struct walb_ctl *ctl)
{
	unsigned int *minor;
	struct walb_disk_data *ddata;
	size_t n;
	
	ASSERT(ctl);
	ASSERT(ctl->command == WALB_IOCTL_GET_DEV_LIST);

	if (ctl->u2k.buf_size < sizeof(unsigned int) * 2) {
		LOGe("Buffer size is too small.\n");
		goto error0;
	}
	minor = (unsigned int *)ctl->u2k.__buf;
	ASSERT(minor);
	if (minor[0] >= minor[1]) {
		LOGe("minor[0] must be < minor[1].\n");
		goto error0;
	}
	ddata = (struct walb_disk_data *)ctl->k2u.__buf;
	if (ddata) {
		n = ctl->k2u.buf_size / sizeof(struct walb_disk_data);
	} else {
		n = (size_t)UINT_MAX;
	}
	ctl->val_int = get_wdev_list_range(ddata, NULL, n, minor[0], minor[1]);
	return 0;

error0:
	return -EFAULT;
}

/**
 * Get number of devices.
 *
 * @ctl walb ctl.
 *   See walb/ioctl.h for input/output details.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_get_num_dev(struct walb_ctl *ctl)
{
	ASSERT(ctl->command == WALB_IOCTL_GET_NUM_DEV);

	ctl->val_int = (int)get_n_devices();
	ASSERT(get_wdev_list_range(NULL, NULL, UINT_MAX, 0, UINT_MAX)
		== ctl->val_int);
	return 0;
}

/**
 * Dispatcher WALB_IOCTL_CONTROL
 *
 * @ctl walb_ctl data.
 *
 * @return 0 in success,
 *	   -ENOTTY in invalid command,
 *	   -EFAULT in command failed.
 */
static int dispatch_ioctl(struct walb_ctl *ctl)
{
	int ret = 0;
	ASSERT(ctl != NULL);
	
	switch(ctl->command) {
	case WALB_IOCTL_DEV_START:
		ret = ioctl_dev_start(ctl);
		break;
	case WALB_IOCTL_DEV_STOP:
		ret = ioctl_dev_stop(ctl);
		break;
	case WALB_IOCTL_GET_MAJOR:
		ret = ioctl_get_major(ctl);
		break;
	case WALB_IOCTL_GET_DEV_LIST:
		ret = ioctl_get_dev_list(ctl);
		break;
	case WALB_IOCTL_GET_NUM_DEV:
		ret = ioctl_get_num_dev(ctl);
		break;
	default:
		LOGe("dispatch_ioctl: command %d is not supported.\n",
			ctl->command);
		ret = -ENOTTY;
	}
	return ret;
}

/**
 * Execute ioctl for /dev/walb/control.
 *
 * @command ioctl command.
 * @user walb_ctl data in userland.
 * 
 * @return 0 in success,
 *	   -ENOTTY in invalid command,
 *	   -EFAULT in command failed.
 */
static int ctl_ioctl(unsigned int command, struct walb_ctl __user *user)
{
	int ret = 0;
	struct walb_ctl *ctl;

	if (command != WALB_IOCTL_CONTROL) {
		LOGe("ioctl cmd must be %08lx but %08x\n",
			WALB_IOCTL_CONTROL, command);
		return -ENOTTY;
	}

	ctl = walb_get_ctl(user, GFP_KERNEL);
	if (ctl == NULL) { goto error0; }

	ret = dispatch_ioctl(ctl);
	
	if (walb_put_ctl(user, ctl) != 0) {
		LOGe("walb_put_ctl failed.\n");
		goto error0;
	}
	return ret;

error0:
	return -EFAULT;
}

static long walb_ctl_ioctl(
	struct file *file, unsigned int command, unsigned long u)
{
	int ret;
	u32 version;
	
	if (command == WALB_IOCTL_VERSION) {
		version = WALB_VERSION;
		ret = __put_user(version, (u32 __user *)u);
	} else {
		ret = (long)ctl_ioctl(command, (struct walb_ctl __user *)u);
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long walb_ctl_compat_ioctl(
	struct file *file, unsigned int command, unsigned long u)
{
	return walb_ctl_ioctl(file, command, (unsigned long)compat_ptr(u));
}
#endif

/*******************************************************************************
 * Static data.
 *******************************************************************************/

static const struct file_operations ctl_fops_ = {
	.open = nonseekable_open,
	.unlocked_ioctl = walb_ctl_ioctl,
	.compat_ioctl = walb_ctl_compat_ioctl,
	.owner = THIS_MODULE,
};

static struct miscdevice walb_misc_ = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = WALB_NAME,
	.nodename = WALB_DIR_NAME "/" WALB_CONTROL_NAME,
	.fops = &ctl_fops_,
};

/*******************************************************************************
 * Implementation of global functions.
 *******************************************************************************/

/**
 * Allocate memory and call @copy_from_user().
 */
void* walb_alloc_and_copy_from_user(
	void __user *userbuf,
	size_t buf_size,
	gfp_t gfp_mask)
{
	void *buf;
	
	if (buf_size == 0 || userbuf == NULL) {
		goto error0;
	}

	if (buf_size <= PAGE_SIZE) {
		buf = kmalloc(buf_size, gfp_mask);
	} else {
		BUG_ON(gfp_mask != GFP_KERNEL);
		buf = vmalloc(buf_size);
	}
	if (buf == NULL) {
		LOGe("memory allocation for walb_ctl.u2k.buf failed.\n");
		goto error0;
	}

	if (copy_from_user(buf, userbuf, buf_size)) {
		LOGe("copy_from_user failed\n");
		goto error1;
	}
	return buf;

error1:
	kfree(buf);
error0:
	return NULL;
}

/**
 * Call @copy_to_user() and free memory.
 *
 * @return 0 in success, or -1.
 *   Even if an error occurs, the memory will be deallocated.
 */
int walb_copy_to_user_and_free(
	void __user *userbuf,
	void *buf,
	size_t buf_size)
{
	int ret = 0;
	
	if (buf_size == 0 || userbuf == NULL || buf == NULL) {
		ret = -1;
		goto fin;
	}
	if (copy_to_user(userbuf, buf, buf_size)) {
		ret = -1;
		goto fin;
	}
fin:
	if (buf_size <= PAGE_SIZE) {
		kfree(buf);
	} else {
		vfree(buf);
	}
	return ret;
}

/**
 * Alloc required memory and copy userctl data.
 *
 * @userctl userctl pointer.
 * @return gfp_mask mask for kmalloc.
 */
struct walb_ctl* walb_get_ctl(void __user *userctl, gfp_t gfp_mask)
{
	struct walb_ctl *ctl;
	
	/* Allocate walb_ctl memory. */
	ctl = kzalloc(sizeof(struct walb_ctl), gfp_mask);
	if (ctl == NULL) {
		LOGe("memory allocation for walb_ctl failed.\n");
		goto error0;
	}

	/* Copy ctl. */
	if (copy_from_user(ctl, userctl, sizeof(struct walb_ctl))) {
		LOGe("copy_from_user failed.\n");
		goto error1;
	}

	/* Allocate and copy ctl->u2k.__buf. */
	if (ctl->u2k.buf_size > 0) {
		ctl->u2k.__buf = walb_alloc_and_copy_from_user
			((void __user *)ctl->u2k.buf,
				ctl->u2k.buf_size, gfp_mask);
		if (ctl->u2k.__buf == NULL) {
			goto error1;
		}
	}
	/* Allocate ctl->k2u.__buf. */
	if (ctl->k2u.buf_size > 0) {
		ctl->k2u.__buf = kzalloc(ctl->k2u.buf_size, gfp_mask);
		if (ctl->k2u.__buf == NULL) {
			goto error2;
		}
	}
	return ctl;

#if 0
error3:
	if (ctl->k2u.buf_size > 0) {
		kfree(ctl->k2u.__buf);
	}
#endif
error2:
	if (ctl->u2k.buf_size > 0) {
		kfree(ctl->u2k.__buf);
	}
error1:
	kfree(ctl);
error0:
	return NULL;
}

/**
 * Copy ctl data to userland and deallocate memory.
 *
 * @userctl userctl pointer.
 * @ctl ctl to put.
 *
 * @return 0 in success, or false.
 */
int walb_put_ctl(void __user *userctl, struct walb_ctl *ctl)
{
	/* Free ctl->u2k.__buf. */
	if (ctl->u2k.buf_size > 0) {
		kfree(ctl->u2k.__buf);
	}

	/* Copy and free ctl->k2u.__buf. */
	if (ctl->k2u.buf_size > 0) {
		if (walb_copy_to_user_and_free
			(ctl->k2u.buf, ctl->k2u.__buf, ctl->k2u.buf_size) != 0) {
			goto error0;
		}
	}

	/* Copy ctl. */
	if (copy_to_user(userctl, ctl, sizeof(struct walb_ctl))) {
		LOGe("copy_to_user failed.\n");
		goto error0;
	}
	
	kfree(ctl);
	return 0;
	
error0:
	kfree(ctl);
	return -1;
}

/**
 * Init walb control device.
 *
 * @return 0 in success, or -1.
 */
int __init walb_control_init(void)
{
	int ret;

	ret = misc_register(&walb_misc_);
	if (ret < 0) { goto error0; }

	LOGi("walb control device minor %u\n", walb_misc_.minor);
	return 0;

error0:
	return -1;
}

/**
 * Exit walb control device.
 */
void walb_control_exit(void)
{
	
	misc_deregister(&walb_misc_);
}

MODULE_LICENSE("Dual BSD/GPL");
