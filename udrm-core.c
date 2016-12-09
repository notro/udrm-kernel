/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/dma-buf.h>

#include "udrm.h"

void udrm_lastclose(struct drm_device *drm)
{
	struct udrm_device *tdev = drm_to_udrm(drm);

	DRM_DEBUG_KMS("\n");
	if (tdev->fbdev_used)
		drm_fbdev_cma_restore_mode(tdev->fbdev_cma);
	else
		drm_crtc_force_disable_all(drm);
}

void udrm_gem_cma_free_object(struct drm_gem_object *gem_obj)
{
	if (gem_obj->import_attach) {
		struct drm_gem_cma_object *cma_obj;

		cma_obj = to_drm_gem_cma_obj(gem_obj);
		dma_buf_vunmap(gem_obj->import_attach->dmabuf, cma_obj->vaddr);
		cma_obj->vaddr = NULL;
	}

	drm_gem_cma_free_object(gem_obj);
}

struct drm_gem_object *
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


static const struct drm_mode_config_funcs udrm_mode_config_funcs = {
	.fb_create = udrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
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

static int udrm_init(struct device *parent, struct udrm_device *tdev,
			struct drm_driver *driver)
{
	struct drm_device *drm = &tdev->drm;
	int ret;

	INIT_WORK(&tdev->dirty_work, udrm_dirty_work);
	mutex_init(&tdev->dev_lock);

	ret = drm_dev_init(drm, driver, parent);
	if (ret)
		return ret;

	drm_mode_config_init(drm);
	drm->mode_config.funcs = &udrm_mode_config_funcs;

	return 0;
}

static void udrm_fini(struct udrm_device *tdev)
{
	struct drm_device *drm = &tdev->drm;

	DRM_DEBUG_KMS("\n");

	drm_mode_config_cleanup(drm);
	drm_dev_unref(drm);
	mutex_destroy(&tdev->dev_lock);
}

static void devm_udrm_release(struct device *dev, void *res)
{
	udrm_fini(*(struct udrm_device **)res);
}

int devm_udrm_init(struct device *parent, struct udrm_device *tdev,
		      struct drm_driver *driver)
{
	struct udrm_device **ptr;
	int ret;

	ptr = devres_alloc(devm_udrm_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = udrm_init(parent, tdev, driver);
	if (ret) {
		devres_free(ptr);
		return ret;
	}

	*ptr = tdev;
	devres_add(parent, ptr);

	return 0;
}
