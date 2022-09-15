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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_VIDEO_DRM_FORMAT_H__
#define __GST_VIDEO_DRM_FORMAT_H__

#include <libdrm/drm_fourcc.h>

G_BEGIN_DECLS

GST_VIDEO_API
guint64           gst_video_drm_format_get_modifier (const GstCaps * caps);

GST_VIDEO_API
GstVideoFormat    gst_video_drm_format_get_format (const GstCaps * caps);

GST_VIDEO_API
GstCaps *         gst_video_drm_format_set_modifier (GstCaps * caps, guint64 modifier);

GST_VIDEO_API
GstCaps *         gst_video_drm_format_remove_modifier (GstCaps * caps);

G_END_DECLS

#endif /* __GST_VIDEO_DRM_FORMAT_H__ */
