/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_simple_kms_helper.h>

#include <uapi/drm/udrm.h>

#include "udrm.h"

static int udrm_connector_get_modes(struct drm_connector *connector)
{
	struct udrm_device *udev = drm_to_udrm(connector->dev);
	struct drm_display_mode *mode = &udev->display_mode;

	mode = drm_mode_duplicate(connector->dev, mode);
	if (!mode) {
		DRM_ERROR("Failed to duplicate mode\n");
		return 0;
	}

	if (mode->name[0] == '\0')
		drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if (mode->width_mm) {
		connector->display_info.width_mm = mode->width_mm;
		connector->display_info.height_mm = mode->height_mm;
	}

	return 1;
}

static const struct drm_connector_helper_funcs udrm_connector_hfuncs = {
	.get_modes = udrm_connector_get_modes,
	.best_encoder = drm_atomic_helper_best_encoder,
};

static enum drm_connector_status
udrm_connector_detect(struct drm_connector *connector, bool force)
{
	if (drm_device_is_unplugged(connector->dev))
		return connector_status_disconnected;

	return connector->status;
}

static const struct drm_connector_funcs udrm_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.detect = udrm_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void udrm_display_pipe_enable(struct drm_simple_display_pipe *pipe,
				     struct drm_crtc_state *crtc_state)
{
	struct udrm_device *udev = pipe_to_udrm(pipe);
	struct udrm_event ev = {
		.type = UDRM_EVENT_PIPE_ENABLE,
		.length = sizeof(ev),
	};

	dev_dbg(udev->drm.dev, "%s\n", __func__);
	udev->prepared = true;
	udrm_send_event(udev, &ev);
}

static void udrm_display_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct udrm_device *udev = pipe_to_udrm(pipe);
	struct udrm_event ev = {
		.type = UDRM_EVENT_PIPE_DISABLE,
		.length = sizeof(ev),
	};

	dev_dbg(udev->drm.dev, "%s\n", __func__);
	udev->prepared = false;
	udev->enabled = false;
	udrm_send_event(udev, &ev);
}

static void udrm_display_pipe_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_state)
{
	struct udrm_device *tdev = pipe_to_udrm(pipe);
	struct drm_framebuffer *fb = pipe->plane.state->fb;
	struct drm_crtc *crtc = &tdev->pipe.crtc;

	if (!fb)
		DRM_DEBUG_KMS("fb unset\n");
	else if (fb != old_state->fb)
		DRM_DEBUG_KMS("fb changed\n");
	else
		DRM_DEBUG_KMS("No fb change\n");

	if (fb && (fb != old_state->fb)) {
		struct udrm_device *tdev = pipe_to_udrm(pipe);

		pipe->plane.fb = fb;
		schedule_work(&tdev->dirty_work);
	}

	if (crtc->state->event) {
		DRM_DEBUG_KMS("crtc event\n");
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}

	if (tdev->fbdev_helper && fb == tdev->fbdev_helper->fb)
		tdev->fbdev_used = true;
}

static const struct drm_simple_display_pipe_funcs udrm_pipe_funcs = {
	.enable = udrm_display_pipe_enable,
	.disable = udrm_display_pipe_disable,
	.update = udrm_display_pipe_update,
};

int udrm_display_pipe_init(struct udrm_device *udev,
			  int connector_type,
			  const uint32_t *formats,
			  unsigned int format_count)
{
	const struct drm_display_mode *mode = &udev->display_mode;
	struct drm_connector *connector = &udev->connector;
	struct drm_device *drm = &udev->drm;
	int ret;

	drm->mode_config.min_width = mode->hdisplay;
	drm->mode_config.max_width = mode->hdisplay;
	drm->mode_config.min_height = mode->vdisplay;
	drm->mode_config.max_height = mode->vdisplay;

	drm_connector_helper_add(connector, &udrm_connector_hfuncs);
	ret = drm_connector_init(drm, connector, &udrm_connector_funcs,
				 connector_type);
	if (ret)
		return ret;

	connector->status = connector_status_connected;

	ret = drm_simple_display_pipe_init(drm, &udev->pipe, &udrm_pipe_funcs, formats,
					   format_count, connector);
	if (ret)
		drm_connector_cleanup(connector);

	return ret;
}
