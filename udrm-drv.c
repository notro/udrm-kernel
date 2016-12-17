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
#include <linux/dma-buf.h>

#include <uapi/drm/udrm.h>

#include "udrm.h"

static void udrm_lastclose(struct drm_device *drm)
{
	struct udrm_device *tdev = drm_to_udrm(drm);

	DRM_DEBUG_KMS("\n");
	if (tdev->fbdev_used)
		drm_fbdev_cma_restore_mode(tdev->fbdev_cma);
	else
		drm_crtc_force_disable_all(drm);
}

static void udrm_gem_cma_free_object(struct drm_gem_object *gem_obj)
{
	if (gem_obj->import_attach) {
		struct drm_gem_cma_object *cma_obj;

		cma_obj = to_drm_gem_cma_obj(gem_obj);
		dma_buf_vunmap(gem_obj->import_attach->dmabuf, cma_obj->vaddr);
		cma_obj->vaddr = NULL;
	}

	drm_gem_cma_free_object(gem_obj);
}

static struct drm_gem_object *
udrm_gem_cma_prime_import_sg_table(struct drm_device *drm,
				      struct dma_buf_attachment *attach,
				      struct sg_table *sgt)
{
	struct drm_gem_cma_object *cma_obj;
	struct drm_gem_object *obj;
	void *vaddr;

	vaddr = dma_buf_vmap(attach->dmabuf);
	if (!vaddr) {
		DRM_ERROR("Failed to vmap PRIME buffer\n");
		return ERR_PTR(-ENOMEM);
	}

	obj = drm_gem_cma_prime_import_sg_table(drm, attach, sgt);
	if (IS_ERR(obj)) {
		dma_buf_vunmap(attach->dmabuf, vaddr);
		return obj;
	}

	cma_obj = to_drm_gem_cma_obj(obj);
	cma_obj->vaddr = vaddr;

	return obj;
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

static void udrm_dirty_work(struct work_struct *work)
{
	struct udrm_device *udev = container_of(work, struct udrm_device,
						   dirty_work);
	struct drm_framebuffer *fb = udev->pipe.plane.fb;
	struct drm_crtc *crtc = &udev->pipe.crtc;

	if (fb)
		fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	if (udev->event) {
		DRM_DEBUG_KMS("crtc event\n");
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, udev->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		udev->event = NULL;
	}
}

static const struct drm_mode_config_funcs udrm_mode_config_funcs = {
	.fb_create = udrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int udrm_drm_init(struct udrm_device *udev, char *drv_name)
{
	struct drm_driver *drv = &udev->driver;
	struct drm_device *drm = &udev->drm;
	int ret;

	drv->name = kstrdup(drv_name, GFP_KERNEL);
	if (!drv->name)
		return -ENOMEM;

	drv->driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC;
	drv->gem_free_object		= udrm_gem_cma_free_object;
	drv->gem_vm_ops			= &drm_gem_cma_vm_ops;
	drv->prime_handle_to_fd		= drm_gem_prime_handle_to_fd;
	drv->prime_fd_to_handle		= drm_gem_prime_fd_to_handle;
	drv->gem_prime_import		= drm_gem_prime_import;
	drv->gem_prime_export		= drm_gem_prime_export;
	drv->gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table;
	drv->gem_prime_import_sg_table	= udrm_gem_cma_prime_import_sg_table;
	drv->gem_prime_vmap		= drm_gem_cma_prime_vmap;
	drv->gem_prime_vunmap		= drm_gem_cma_prime_vunmap;
	drv->gem_prime_mmap		= drm_gem_cma_prime_mmap;
	drv->dumb_create		= drm_gem_cma_dumb_create;
	drv->dumb_map_offset		= drm_gem_cma_dumb_map_offset;
	drv->dumb_destroy		= drm_gem_dumb_destroy;
	drv->fops			= &udrm_drm_fops;
	drv->lastclose			= udrm_lastclose;

	drv->ioctls		= udrm_ioctls;
	drv->num_ioctls		= ARRAY_SIZE(udrm_ioctls);

	drv->desc		= "DRM userspace driver support";
	drv->date		= "20161119";
	drv->major		= 1;
	drv->minor		= 0;

	INIT_WORK(&udev->dirty_work, udrm_dirty_work);
	mutex_init(&udev->dev_lock);

	ret = drm_dev_init(drm, drv, NULL);
	if (ret)
		return ret;

	drm_mode_config_init(drm);
	drm->mode_config.funcs = &udrm_mode_config_funcs;

	return 0;
}

static void udrm_drm_fini(struct udrm_device *udev)
{
	struct drm_device *drm = &udev->drm;

	DRM_DEBUG_KMS("udrm_drm_fini\n");

	mutex_destroy(&udev->dev_lock);
	drm_mode_config_cleanup(drm);
	drm_dev_unref(drm);
}

static int udrm_create_buf(struct udrm_device *udev, u32 mode,
			   uint32_t *formats, unsigned int num_formats)
{
	int i, fd, ret, max_cpp = 0;
	size_t len;

	if (mode & UDRM_BUF_MODE_EMUL_XRGB8888)
		udev->emulate_xrgb8888 = true;

	for (i = 0; i < num_formats; i++) {
		if (udev->emulate_xrgb8888 && formats[i] == DRM_FORMAT_XRGB8888)
			continue;
		max_cpp = max(max_cpp, drm_format_plane_cpp(formats[i], 0));
	}

	len = udev->display_mode.hdisplay * udev->display_mode.vdisplay * max_cpp;

	udev->dmabuf = udrm_dmabuf_alloc_attrs(NULL, len,
					       DMA_ATTR_WRITE_COMBINE, O_RDWR);
	if (IS_ERR(udev->dmabuf))
		return PTR_ERR(udev->dmabuf);

	fd = dma_buf_fd(udev->dmabuf, O_RDWR);
	if (fd < 0) {
		dma_buf_put(udev->dmabuf);
		return fd;
	}

	udev->buf_fd = fd;

	return 0;
}

int udrm_drm_register(struct udrm_device *udev,
		      struct udrm_dev_create *dev_create,
		      uint32_t *formats, unsigned int num_formats)
{
	struct drm_device *drm;
	int ret;

	ret = drm_mode_convert_umode(&udev->display_mode, &dev_create->mode);
	if (ret)
		return ret;

	drm_mode_debug_printmodeline(&udev->display_mode);

	if (dev_create->buf_mode) {
		ret = udrm_create_buf(udev, dev_create->buf_mode, formats, num_formats);
		if (ret)
			return ret;
	} else {
		udev->buf_fd = -1;
	}

	ret = udrm_drm_init(udev, dev_create->name);
	if (ret)
		return ret;;

	drm = &udev->drm;
	drm->mode_config.funcs = &udrm_mode_config_funcs;

	ret = udrm_display_pipe_init(udev, DRM_MODE_CONNECTOR_VIRTUAL,
				     formats, num_formats);
	if (ret)
		goto err_fini;

	drm->mode_config.preferred_depth = drm_format_plane_cpp(formats[0], 0) * 8;

	drm_mode_config_reset(drm);

	DRM_DEBUG_KMS("preferred_depth=%u\n", drm->mode_config.preferred_depth);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_fini;

	ret = udrm_fbdev_init(udev);
	if (ret)
		DRM_ERROR("Failed to initialize fbdev: %d\n", ret);

	dev_create->index = drm->primary->index;
	dev_create->buf_fd = udev->buf_fd;

	return 0;

err_fini:
	udrm_drm_fini(udev);

	return ret;
}

void udrm_drm_unregister(struct udrm_device *udev)
{
	struct drm_device *drm = &udev->drm;

	DRM_DEBUG_KMS("udrm_drm_unregister\n");

	drm_crtc_force_disable_all(drm);
	cancel_work_sync(&udev->dirty_work);
	udrm_fbdev_fini(udev);
	drm_dev_unregister(drm);

	udrm_drm_fini(udev);
}
