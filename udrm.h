/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_H
#define __LINUX_TINYDRM_H

#include <drm/drm_crtc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_simple_kms_helper.h>

struct udrm_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct work_struct dirty_work;
	struct mutex dev_lock;
	bool prepared;
	bool enabled;
	struct drm_fbdev_cma *fbdev_cma;
	struct drm_fb_helper *fbdev_helper;
	bool fbdev_used;


	struct device dev;
	struct idr 		idr;

	struct mutex		mutex;
	wait_queue_head_t	waitq;
	struct completion	completion;

	struct udrm_event	*ev;
	int			event_ret;

	bool			initialized;
	bool			fbdev_fb_sent;
	struct work_struct	release_work;



	struct drm_display_mode	display_mode;
	struct drm_connector	connector;

};




static inline struct udrm_device *
drm_to_udrm(struct drm_device *drm)
{
	return container_of(drm, struct udrm_device, drm);
}

static inline struct udrm_device *
pipe_to_udrm(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct udrm_device, pipe);
}


/**
 * TINYDRM_MODE - udrm display mode
 * @hd: horizontal resolution, width
 * @vd: vertical resolution, height
 * @hd_mm: display width in millimeters
 * @vd_mm: display height in millimeters
 *
 * This macro creates a &drm_display_mode for use with udrm.
 */
#define TINYDRM_MODE(hd, vd, hd_mm, vd_mm) \
	.hdisplay = (hd), \
	.hsync_start = (hd), \
	.hsync_end = (hd), \
	.htotal = (hd), \
	.vdisplay = (vd), \
	.vsync_start = (vd), \
	.vsync_end = (vd), \
	.vtotal = (vd), \
	.width_mm = (hd_mm), \
	.height_mm = (vd_mm), \
	.type = DRM_MODE_TYPE_DRIVER, \
	.clock = 1 /* pass validation */

int udrm_send_event(struct udrm_device *udev, void *ev_in);

void udrm_lastclose(struct drm_device *drm);
void udrm_gem_cma_free_object(struct drm_gem_object *gem_obj);
struct drm_gem_object *
udrm_gem_cma_prime_import_sg_table(struct drm_device *drm,
				      struct dma_buf_attachment *attach,
				      struct sg_table *sgt);
struct drm_framebuffer *
udrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd);
int
udrm_display_pipe_init(struct udrm_device *tdev,
			  int connector_type,
			  const uint32_t *formats,
			  unsigned int format_count);
int devm_udrm_init(struct device *parent, struct udrm_device *tdev,
		      struct drm_driver *driver);

int udrm_fbdev_init(struct udrm_device *tdev);
void udrm_fbdev_fini(struct udrm_device *tdev);

#endif /* __LINUX_TINYDRM_H */
