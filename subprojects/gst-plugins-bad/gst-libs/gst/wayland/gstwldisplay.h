/* GStreamer Wayland Library
 *
 * Copyright (C) 2014 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#pragma once

#include <gst/wayland/wayland.h>

#include <gst/video/video.h>
#include <gst/video/video-info.h>

G_BEGIN_DECLS

#define GST_TYPE_WL_DISPLAY (gst_wl_display_get_type ())

GST_WL_API
G_DECLARE_FINAL_TYPE (GstWlDisplay, gst_wl_display, GST, WL_DISPLAY, GObject);

struct _GstWlDisplay
{
  GObject parent_instance;
};

GST_WL_API
GstWlDisplay *gst_wl_display_new (const gchar * name, GError ** error);

GST_WL_API
GstWlDisplay *gst_wl_display_new_existing (struct wl_display * display,
    gboolean take_ownership, GError ** error);

/* see wlbuffer.c for explanation */
GST_WL_API
void gst_wl_display_register_buffer (GstWlDisplay * self, gpointer gstmem,
    gpointer wlbuffer);

GST_WL_API
void gst_wl_display_unregister_buffer (GstWlDisplay * self, gpointer gstmem);

GST_WL_API
gpointer gst_wl_display_lookup_buffer (GstWlDisplay * self, gpointer gstmem);

GST_WL_API
gboolean gst_wl_display_check_format_for_shm (GstWlDisplay * self,
    GstVideoFormat format);

GST_WL_API
gboolean gst_wl_display_check_format_for_dmabuf (GstWlDisplay * self,
    GstVideoFormat format);

GST_WL_API
struct wl_display *gst_wl_display_get_display (GstWlDisplay * self);

GST_WL_API
struct wl_event_queue *gst_wl_display_get_event_queue (GstWlDisplay * self);

GST_WL_API
struct wl_compositor *gst_wl_display_get_compositor (GstWlDisplay * self);

GST_WL_API
struct wl_subcompositor *gst_wl_display_get_subcompositor (GstWlDisplay * self);

GST_WL_API
struct xdg_wm_base *gst_wl_display_get_xdg_wm_base (GstWlDisplay * self);

GST_WL_API
struct zwp_fullscreen_shell_v1 *gst_wl_display_get_fullscreen_shell_v1 (GstWlDisplay * self);

GST_WL_API
struct wp_viewporter *gst_wl_display_get_viewporter (GstWlDisplay * self);

GST_WL_API
struct wl_shm *gst_wl_display_get_shm (GstWlDisplay * self);

GST_WL_API
GArray *gst_wl_display_get_shm_formats (GstWlDisplay * self);

GST_WL_API
GArray *gst_wl_display_get_dmabuf_formats (GstWlDisplay * self);

GST_WL_API
struct zwp_linux_dmabuf_v1 *gst_wl_display_get_dmabuf_v1 (GstWlDisplay * self);

GST_WL_API
gboolean gst_wl_display_has_own_display (GstWlDisplay * self);

G_END_DECLS
