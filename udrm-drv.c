#define DEBUG
/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/leds.h>
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

	dev_dbg(udev->drm.dev, "%s(ev=%p) IN\n", __func__, ev);
	mutex_lock(&udev->dev_lock);
	reinit_completion(&udev->completion);

	ret = mutex_lock_interruptible(&udev->mutex);
	if (ret) {
		kfree(ev);
		goto out_unlock;
	}
	udev->ev = ev;
	mutex_unlock(&udev->mutex);

	dev_dbg(udev->drm.dev, "%s: ev->type=%u, ev->length=%u\n", __func__, udev->ev->type, udev->ev->length);

	wake_up_interruptible(&udev->waitq);

	time_left = wait_for_completion_timeout(&udev->completion, 5 * HZ);
	//ret = udev->event_ret;
	//time_left = 1;
	if (!time_left) {
		dev_err(&udev->dev, "%s: timeout waiting for reply\n", __func__);
		ret =-ETIMEDOUT;
	}

out_unlock:
	mutex_unlock(&udev->dev_lock);

	dev_dbg(udev->drm.dev, "%s OUT ret=%d, event_ret=%d\n", __func__, ret, udev->event_ret);
	return ret;
}

static int udrm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data,
					     struct drm_file *file_priv)
{
	struct drm_prime_handle *args = data;

	/* FIXME: only the userspace driver should use this */

	/* check flags are valid */
	if (args->flags & ~(DRM_CLOEXEC | DRM_RDWR))
		return -EINVAL;

	return dev->driver->prime_handle_to_fd(dev, file_priv, args->handle,
					       args->flags, &args->fd);
}

static const struct drm_ioctl_desc udrm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(UDRM_PRIME_HANDLE_TO_FD, udrm_prime_handle_to_fd_ioctl, DRM_CONTROL_ALLOW|DRM_UNLOCKED),
};

static const struct file_operations udrm_drm_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= no_llseek,
	.mmap		= drm_gem_cma_mmap,
};

static struct drm_driver udrm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	.gem_free_object	= udrm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = udrm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.dumb_create		= drm_gem_cma_dumb_create,
	.dumb_map_offset	= drm_gem_cma_dumb_map_offset,
	.dumb_destroy		= drm_gem_dumb_destroy,
	.fops			= &udrm_drm_fops,
	.lastclose		= udrm_lastclose,

	.ioctls			= udrm_ioctls,
	.num_ioctls		= ARRAY_SIZE(udrm_ioctls),

	.name			= "udrm",
	.desc			= "DRM userspace driver support",
	.date			= "20161119",
	.major			= 1,
	.minor			= 0,
};

static const uint32_t udrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_mode_config_funcs udrm_mode_config_funcs = {
	.fb_create = udrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int udrm_register(struct udrm_device *udev, struct udrm_dev_create *dev_create)
{
	struct device *dev = &udev->dev;
	struct drm_device *drm;
	struct drm_driver *drv;
	int ret;

	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
	}

	drv = devm_kmalloc(&udev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	*drv = udrm_driver;
	drv->name = kstrdup(dev_create->name, GFP_KERNEL);
	if (!drv->name)
		return -ENOMEM;

	ret = devm_udrm_init(&udev->dev, udev, drv);
	if (ret)
		return ret;

	drm = &udev->drm;
	drm->mode_config.funcs = &udrm_mode_config_funcs;

	ret = drm_mode_convert_umode(&udev->display_mode, &dev_create->mode);
	if (ret)
		return ret;

	drm_mode_debug_printmodeline(&udev->display_mode);

	ret = udrm_display_pipe_init(udev,
					DRM_MODE_CONNECTOR_VIRTUAL,
					udrm_formats,
					ARRAY_SIZE(udrm_formats));
	if (ret)
		return ret;

	drm->mode_config.preferred_depth = 16;

	drm_mode_config_reset(drm);

	DRM_DEBUG_KMS("preferred_depth=%u\n", drm->mode_config.preferred_depth);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	ret = udrm_fbdev_init(udev);
	if (ret)
		DRM_ERROR("Failed to initialize fbdev: %d\n", ret);

	dev_create->index = drm->primary->index;

	return 0;
}

static void udrm_unregister(struct udrm_device *udev)
{
	struct drm_device *drm = &udev->drm;

	DRM_DEBUG_KMS("\n");

	drm_crtc_force_disable_all(drm);
	cancel_work_sync(&udev->dirty_work);
	udrm_fbdev_fini(udev);
	drm_dev_unregister(drm);
}

/*********************************************************************************************************************************/

static void udrm_release_work(struct work_struct *work)
{
	struct udrm_device *udev = container_of(work, struct udrm_device,
						    release_work);
	struct drm_device *drm = &udev->drm;

	//drm_device_set_unplugged(drm);

	while (drm->open_count) {
		dev_dbg(drm->dev, "%s: open_count=%d\n", __func__, drm->open_count);
		msleep(1000);
	}

	udrm_unregister(udev);


	dev_dbg(&udev->dev, "%s: dev.refcount=%d\n", __func__, atomic_read(&udev->dev.kobj.kref.refcount));

	device_unregister(&udev->dev);
}

static void udrm_device_release(struct device *dev)
{
	//struct udrm_device *udev = container_of(dev, struct udrm_device, dev);

	dev_dbg(dev, "%s\n", __func__);
	/* FIXME: there's use after free */
	//kfree(udev);
}

static int udrm_open(struct inode *inode, struct file *file)
{
	struct udrm_device *udev;
	int ret;

	udev = kzalloc(sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	/* FIXME: tinydrm currently needs a parent device */
	dev_set_name(&udev->dev, "udrm%u", 0);
	udev->dev.release = udrm_device_release;
	ret = device_register(&udev->dev);
	if (ret) {
		put_device(&udev->dev);
		kfree(udev);
		return ret;
	}

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

	dev_dbg(udev->drm.dev, "%s\n", __func__);

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

	dev_dbg(&udev->dev, "%s(count=%zu)\n", __func__, count);
	if (!count)
		return 0;

	do {
		ret = mutex_lock_interruptible(&udev->mutex);
		if (ret)
			return ret;

		if (!udev->ev && (file->f_flags & O_NONBLOCK)) {
			ret = -EAGAIN;
		} else if (udev->ev) {
			dev_dbg(&udev->dev, "%s udev->ev->length=%u\n", __func__, udev->ev->length);
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

		dev_dbg(&udev->dev, "%s: ret=%d\n", __func__, ret);
		if (ret)
			break;

		if (!(file->f_flags & O_NONBLOCK))
			ret = wait_event_interruptible(udev->waitq, udev->ev);
		dev_dbg(&udev->dev, "%s: while: ret=%d\n", __func__, ret);
	} while (ret == 0);

	return ret;
}

static unsigned int udrm_poll(struct file *file, poll_table *wait)
{
	struct udrm_device *udev = file->private_data;

	dev_dbg(udev->drm.dev, "%s\n", __func__);
	poll_wait(file, &udev->waitq, wait);

	if (udev->ev)
		return POLLIN | POLLRDNORM;

	return 0;
}

static int udrm_release(struct inode *inode, struct file *file)
{
	struct udrm_device *udev = file->private_data;
	int i;

	dev_dbg(&udev->dev, "%s: refcount=%d\n", __func__, atomic_read(&udev->dev.kobj.kref.refcount));

	if (udev->initialized) {
		schedule_work(&udev->release_work);
	} else {
		dev_dbg(&udev->dev, "%s: refcount=%d\n", __func__, atomic_read(&udev->dev.kobj.kref.refcount));

		device_unregister(&udev->dev);
		for (i = atomic_read(&udev->dev.kobj.kref.refcount); i > 0; i--)
			put_device(&udev->dev);
	}

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

		ret = udrm_register(udev, &dev_create);
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
//	.name		= "udrm",
	.name		= "utinydrm",
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
