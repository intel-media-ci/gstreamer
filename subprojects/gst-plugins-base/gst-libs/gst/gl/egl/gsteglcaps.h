/* GStreamer
 *  Copyright (C) 2022 Intel Corporation
 *     Author: Liu Yinhang <yinhang.liu@intel.com>
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
 * License along with this library; if not, write to the0
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_EGL_CAPS_H_
#define _GST_EGL_CAPS_H_

#include <gst/gl/gstglcontext.h>
#include <gst/gl/egl/gstgldisplay_egl.h>

G_BEGIN_DECLS

GST_GL_API
GstCaps *        gst_egl_caps_embed_modifiers    (GstCaps * caps, GstGLContext * context);

GST_GL_API
gboolean         gst_egl_caps_has_feature        (const GstCaps * caps, const gchar * feature);

GST_GL_API
guint64          gst_egl_caps_get_modifier       (GstCaps * caps);

GST_GL_API
void             gst_egl_caps_set_modifier       (GstCaps * caps, guint64 modifier);

GST_GL_API
GstCaps *        gst_egl_caps_remove_modifier    (GstCaps * caps);

G_END_DECLS

#endif /* _GST_EGL_CAPS_H_ */
