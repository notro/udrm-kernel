/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>

#include <uapi/drm/udrm.h>

#include "udrm.h"

static unsigned int fbdefio_delay;
module_param(fbdefio_delay, uint, 0);
MODULE_PARM_DESC(fbdefio_delay, "fbdev deferred io delay in milliseconds");




static int udrm_fb_create_event(struct drm_framebuffer *fb)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);
	struct udrm_event_fb_create ev = {
		.base = {
			.type = UDRM_EVENT_FB_CREATE,
			.length = sizeof(ev),
		},
	};
	struct drm_mode_fb_cmd2 *ufb = &ev.fb;
	struct drm_gem_cma_object *cma_obj;
	int ret, i;

	dev_dbg(fb->dev->dev, "%s: [FB:%d]\n", __func__, fb->base.id);

	ufb->fb_id = fb->base.id;
	ufb->width = fb->width;
	ufb->height = fb->height;
	ufb->pixel_format = fb->pixel_format;
	ufb->flags = fb->flags;

	for (i = 0; i < 4; i++) {
		cma_obj = drm_fb_cma_get_gem_obj(fb, i);
		if (!cma_obj)
			break;

		ufb->pitches[i] = fb->pitches[i];
		ufb->offsets[i] = fb->offsets[i];
		ufb->modifier[i] = fb->modifier[i];
	}

	ret = idr_alloc(&udev->idr, fb, fb->base.id, fb->base.id + 1, GFP_KERNEL);
	if (ret < 1) {
		dev_err(fb->dev->dev, "%s: [FB:%d]: failed to allocate idr %d\n", __func__, fb->base.id, ret);
		return ret;
	}

	ret = udrm_send_event(udev, &ev);

	return ret;
}


static bool tinydrm_check_dirty(struct drm_framebuffer *fb,
			 struct drm_clip_rect **clips, unsigned int *num_clips)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);

	if (!udev->prepared)
		return false;

	/* fbdev can flush even when we're not interested */
	if (udev->pipe.plane.fb != fb)
		return false;

	/* Make sure to flush everything the first time */
	if (!udev->enabled) {
		*clips = NULL;
		*num_clips = 0;
	}

	return true;
}

static void tinydrm_merge_clips(struct drm_clip_rect *dst,
			 struct drm_clip_rect *src, unsigned int num_clips,
			 unsigned int flags, u32 max_width, u32 max_height)
{
	unsigned int i;

	if (!src || !num_clips) {
		dst->x1 = 0;
		dst->x2 = max_width;
		dst->y1 = 0;
		dst->y2 = max_height;
		return;
	}

	dst->x1 = ~0;
	dst->y1 = ~0;
	dst->x2 = 0;
	dst->y2 = 0;

	for (i = 0; i < num_clips; i++) {
		if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY)
			i++;
		dst->x1 = min(dst->x1, src[i].x1);
		dst->x2 = max(dst->x2, src[i].x2);
		dst->y1 = min(dst->y1, src[i].y1);
		dst->y2 = max(dst->y2, src[i].y2);
	}

	if (dst->x2 > max_width || dst->y2 > max_height ||
	    dst->x1 >= dst->x2 || dst->y1 >= dst->y2) {
		DRM_DEBUG_KMS("Illegal clip: x1=%u, x2=%u, y1=%u, y2=%u\n",
			      dst->x1, dst->x2, dst->y1, dst->y2);
		dst->x1 = 0;
		dst->y1 = 0;
		dst->x2 = max_width;
		dst->y2 = max_height;
	}
}

static int udrm_fb_dirty(struct drm_framebuffer *fb,
			     struct drm_file *file_priv,
			     unsigned int flags, unsigned int color,
			     struct drm_clip_rect *clips,
			     unsigned int num_clips)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);
	struct drm_mode_fb_dirty_cmd *dirty;
	struct udrm_event_fb_dirty *ev;
	struct drm_clip_rect clip;
	size_t size_clips, size;
	int ret;

	pr_debug("\n\n\n");
	if (!tinydrm_check_dirty(fb, &clips, &num_clips))
		return -EINVAL;

	/* FIXME: if (fb == tdev->fbdev_helper->fb) */
	if (udev->fbdev_helper && !udev->fbdev_fb_sent) {
		udrm_fb_create_event(udev->fbdev_helper->fb);
		udev->fbdev_fb_sent = true;
	}

	udev->enabled = true;

	tinydrm_merge_clips(&clip, clips, num_clips, flags,
			    fb->width, fb->height);
	clip.x1 = 0;
	clip.x2 = fb->width;
	clips = &clip;
	num_clips = 1;

	size_clips = num_clips * sizeof(struct drm_clip_rect);
	size = sizeof(struct udrm_event_fb_dirty) + size_clips;
	ev = kzalloc(size, GFP_KERNEL);
	if (!ev)
		return -ENOMEM;

	dev_dbg(fb->dev->dev, "%s: [FB:%d]: num_clips=%u, size_clips=%zu, size=%zu\n", __func__, fb->base.id, num_clips, size_clips, size);

	ev->base.type = UDRM_EVENT_FB_DIRTY;
	ev->base.length = size;
	dirty = &ev->fb_dirty_cmd;

	dirty->fb_id = fb->base.id;
	dirty->flags = flags;
	dirty->color = color;
	dirty->num_clips = num_clips;
	//dirty->clips_ptr

	if (num_clips)
		memcpy(ev->clips, clips, size_clips);

//	tinydrm_merge_clips(&clip, clips, num_clips, flags,
//			    fb->width, fb->height);
//	clip.x1 = 0;
//	clip.x2 = fb->width;

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clip.x1, clip.x2, clip.y1, clip.y2);

	ret = udrm_send_event(udev, ev);

	if (ret) {
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);
	}

	return ret;
}

static void udrm_fb_destroy(struct drm_framebuffer *fb)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);
	struct udrm_event_fb_destroy ev = {
		.base = {
			.type = UDRM_EVENT_FB_DESTROY,
			.length = sizeof(ev),
		},
	};
	struct drm_framebuffer *iter;
	int id;

	dev_dbg(fb->dev->dev, "%s: [FB:%d]\n", __func__, fb->base.id);

	idr_for_each_entry(&udev->idr, iter, id) {
		if (fb == iter)
			break;
	}

	if (!iter) {
		dev_err(fb->dev->dev, "%s: failed to find idr\n", __func__);
		return;
	}

	ev.fb_id = id;
	idr_remove(&udev->idr, id);

	udrm_send_event(udev, &ev);

	drm_fb_cma_destroy(fb);
}

static const struct drm_framebuffer_funcs udrm_fb_funcs = {
	.destroy	= udrm_fb_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= udrm_fb_dirty,
};

struct drm_framebuffer *
udrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		   const struct drm_mode_fb_cmd2 *mode_cmd)
{
//	struct udrm_device *udev = drm_to_udrm(drm);
	struct drm_framebuffer *fb;
	int ret;

//	fb = drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
//					  udev->fb_funcs);
	fb = drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
					  &udrm_fb_funcs);
	if (IS_ERR(fb))
		return fb;

	DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", fb->base.id,
		      drm_get_format_name(fb->pixel_format));

	dev_dbg(drm->dev, "%s\n", __func__);
	ret = udrm_fb_create_event(fb);

	return fb;
}


//struct drm_framebuffer *
//udrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
//		  const struct drm_mode_fb_cmd2 *mode_cmd)
//{
//	struct udrm_device *tdev = drm_to_udrm(drm);
//	struct drm_framebuffer *fb;
//
//	fb = drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
//					  tdev->fb_funcs);
//	if (!IS_ERR(fb))
//		DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", fb->base.id,
//			      drm_get_format_name(fb->pixel_format));
//
//	return fb;
//}
//
static int udrm_fbdev_create(struct drm_fb_helper *helper,
				struct drm_fb_helper_surface_size *sizes)
{
	struct udrm_device *tdev = drm_to_udrm(helper->dev);
	int ret;

//	ret = drm_fbdev_cma_create_with_funcs(helper, sizes, tdev->fb_funcs);
	ret = drm_fbdev_cma_create_with_funcs(helper, sizes, &udrm_fb_funcs);
	if (ret)
		return ret;

	strncpy(helper->fbdev->fix.id, helper->dev->driver->name, 16);
	tdev->fbdev_helper = helper;

	if (fbdefio_delay) {
		unsigned long delay;

		delay = msecs_to_jiffies(fbdefio_delay);
		helper->fbdev->fbdefio->delay = delay ? delay : 1;
	}

	DRM_DEBUG_KMS("fbdev: [FB:%d] pixel_format=%s, fbdefio->delay=%ums\n",
		      helper->fb->base.id,
		      drm_get_format_name(helper->fb->pixel_format),
		      jiffies_to_msecs(helper->fbdev->fbdefio->delay));

	return 0;
}

static const struct drm_fb_helper_funcs udrm_fb_helper_funcs = {
	.fb_probe = udrm_fbdev_create,
};

int udrm_fbdev_init(struct udrm_device *tdev)
{
	struct drm_device *drm = &tdev->drm;
	struct drm_fbdev_cma *fbdev;
	int bpp;

	DRM_DEBUG_KMS("\n");

	bpp = drm->mode_config.preferred_depth;
	fbdev = drm_fbdev_cma_init_with_funcs(drm, bpp ? bpp : 32,
					      drm->mode_config.num_crtc,
					      drm->mode_config.num_connector,
					      &udrm_fb_helper_funcs);
	if (IS_ERR(fbdev))
		return PTR_ERR(fbdev);

	tdev->fbdev_cma = fbdev;

	return 0;
}

void udrm_fbdev_fini(struct udrm_device *tdev)
{
	drm_fbdev_cma_fini(tdev->fbdev_cma);
	tdev->fbdev_cma = NULL;
	tdev->fbdev_helper = NULL;
}
