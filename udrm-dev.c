#define DEBUG
/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <uapi/drm/udrm.h>

#include "udrm.h"

static struct miscdevice udrm_misc;

int udrm_send_event(struct udrm_device *udev, void *ev_in)
{
	struct udrm_event *ev = ev_in;
	unsigned long time_left;
	int ret = 0;

	ev = kmemdup(ev, ev->length, GFP_KERNEL);
	if (!ev)
		return -ENOMEM;

	DRM_DEBUG("(ev=%p) IN\n", ev);
	mutex_lock(&udev->dev_lock);
	reinit_completion(&udev->completion);

	ret = mutex_lock_interruptible(&udev->mutex);
	if (ret) {
		kfree(ev);
		goto out_unlock;
	}
	udev->ev = ev;
	mutex_unlock(&udev->mutex);

	DRM_DEBUG("ev->type=%u, ev->length=%u\n", udev->ev->type, udev->ev->length);

	wake_up_interruptible(&udev->waitq);

	time_left = wait_for_completion_timeout(&udev->completion, 5 * HZ);
	//ret = udev->event_ret;
	//time_left = 1;
	if (!time_left) {
		DRM_ERROR("timeout waiting for reply\n");
		ret =-ETIMEDOUT;
	}

out_unlock:
	mutex_unlock(&udev->dev_lock);

	DRM_DEBUG("OUT ret=%d, event_ret=%d\n", ret, udev->event_ret);

	return ret;
}

static void udrm_release_work(struct work_struct *work)
{
	struct udrm_device *udev = container_of(work, struct udrm_device,
						    release_work);
	struct drm_device *drm = &udev->drm;

	//drm_device_set_unplugged(drm);

	while (drm->open_count) {
		DRM_DEBUG_KMS("open_count=%d\n", drm->open_count);
		msleep(1000);
	}

	udrm_drm_unregister(udev);
}

static int udrm_open(struct inode *inode, struct file *file)
{
	struct udrm_device *udev;

	udev = kzalloc(sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	mutex_init(&udev->mutex);
	init_waitqueue_head(&udev->waitq);
	init_completion(&udev->completion);
	idr_init(&udev->idr);
	INIT_WORK(&udev->release_work, udrm_release_work);

	file->private_data = udev;
	nonseekable_open(inode, file);

	return 0;
}

static ssize_t udrm_write(struct file *file, const char __user *buffer,
			   size_t count, loff_t *ppos)
{
	struct udrm_device *udev = file->private_data;
	int ret, event_ret;

	DRM_DEBUG_KMS("\n");

	if (!udev->initialized)
		return -EINVAL;

	if (!count)
		return 0;

	if (count != sizeof(int))
		return -EINVAL;

	if (copy_from_user(&event_ret, buffer, sizeof(int)))
		return -EFAULT;

	ret = mutex_lock_interruptible(&udev->mutex);
	if (ret)
		return ret;

	udev->event_ret = event_ret;
	complete(&udev->completion);

	mutex_unlock(&udev->mutex);

	return count;
}

static ssize_t udrm_read(struct file *file, char __user *buffer, size_t count,
			  loff_t *ppos)
{
	struct udrm_device *udev = file->private_data;
	ssize_t ret;

	DRM_DEBUG_KMS("(count=%zu)\n", count);
	if (!count)
		return 0;

	do {
		ret = mutex_lock_interruptible(&udev->mutex);
		if (ret)
			return ret;

		if (!udev->ev && (file->f_flags & O_NONBLOCK)) {
			ret = -EAGAIN;
		} else if (udev->ev) {
			DRM_DEBUG_KMS("udev->ev->length=%u\n", udev->ev->length);
			if (count < udev->ev->length)
				ret = -EINVAL;
			else if (copy_to_user(buffer, udev->ev, udev->ev->length))
				ret = -EFAULT;
			else
				ret = udev->ev->length;
			kfree(udev->ev);
			udev->ev = NULL;
		}

		mutex_unlock(&udev->mutex);

		if (ret)
			break;

		if (!(file->f_flags & O_NONBLOCK))
			ret = wait_event_interruptible(udev->waitq, udev->ev);
	} while (ret == 0);

	return ret;
}

static unsigned int udrm_poll(struct file *file, poll_table *wait)
{
	struct udrm_device *udev = file->private_data;

	DRM_DEBUG_KMS("\n");
	poll_wait(file, &udev->waitq, wait);

	if (udev->ev)
		return POLLIN | POLLRDNORM;

	return 0;
}

static int udrm_release(struct inode *inode, struct file *file)
{
	struct udrm_device *udev = file->private_data;

	if (udev->initialized)
		schedule_work(&udev->release_work);

	return 0;
}

static long udrm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct udrm_device *udev = file->private_data;
	struct udrm_dev_create dev_create;
	int ret = -EINVAL;

	switch (cmd) {
	case UDRM_DEV_CREATE:

		if (copy_from_user(&dev_create, (void __user *)arg, sizeof(dev_create)))
			return -EFAULT;

		ret = udrm_drm_register(udev, &dev_create);
		if (!ret) {
			udev->initialized = true;
			if (copy_to_user((void __user *)arg, &dev_create, sizeof(dev_create)))
				ret = -EFAULT;
		}

		break;
	}

	return ret;
}

static const struct file_operations udrm_fops = {
	.owner		= THIS_MODULE,
	.open		= udrm_open,
	.release	= udrm_release,
	.read		= udrm_read,
	.write		= udrm_write,
	.poll		= udrm_poll,

	.unlocked_ioctl	= udrm_ioctl,
//#ifdef CONFIG_COMPAT
//	.compat_ioctl	= udrm_compat_ioctl,
//#endif

	.llseek		= no_llseek,
};

static struct miscdevice udrm_misc = {
	.fops		= &udrm_fops,
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "udrm",
};

static int __init udrm_init(void)
{
	return misc_register(&udrm_misc);
}
module_init(udrm_init);

static void __exit udrm_exit(void)
{
	misc_deregister(&udrm_misc);
}
module_exit(udrm_exit);

MODULE_AUTHOR("Noralf Trønnes");
MODULE_DESCRIPTION("Userspace driver support for DRM");
MODULE_LICENSE("GPL");
