/* GStreamer
 * Copyright (C) 2022 Intel Corporation
 *     Author: Liu Yinhang <yinhang.liu@intel.com>
 *     Author: He Junyan <junyan.he@intel.com>
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

#ifndef __GST_VIDEO_INFO_DRM_H__
#define __GST_VIDEO_INFO_DRM_H__

#include "video-info.h"
#include <libdrm/drm_fourcc.h>

G_BEGIN_DECLS

typedef struct _GstVideoInfoDrm GstVideoInfoDrm;

struct _GstVideoInfoDrm
{
  GstVideoInfo vinfo;
  guint32 drm_fourcc;
  guint64 drm_modifier;
};

#define GST_TYPE_VIDEO_INFO  (gst_video_info_get_type ())
GST_VIDEO_API
GType             gst_video_info_drm_get_type (void);
GST_VIDEO_API
GstVideoInfoDrm * gst_video_info_drm_copy (const GstVideoInfoDrm * info);
GST_VIDEO_API
void              gst_video_info_drm_free (GstVideoInfoDrm * info);
GST_VIDEO_API
void              gst_video_info_drm_init (GstVideoInfoDrm * drm_info);
GST_VIDEO_API
GstVideoInfoDrm * gst_video_info_drm_new (void);
GST_VIDEO_API
guint32           gst_video_drm_format_from_string (const gchar * format_str, guint64 * modifier);
GST_VIDEO_API
gchar *           gst_video_drm_format_to_string (guint32 fourcc, guint64 modifier);
GST_VIDEO_API
gboolean          gst_video_is_drm_caps (const GstCaps * caps);
GST_VIDEO_API
GstCaps *         gst_video_info_drm_to_caps (const GstVideoInfoDrm * drm_info);
GST_VIDEO_API
gboolean          gst_video_info_drm_from_caps (GstVideoInfoDrm * drm_info, const GstCaps * caps);
GST_VIDEO_API
GstVideoInfoDrm * gst_video_info_drm_new_from_caps (const GstCaps * caps);
GST_VIDEO_API
GstCaps *         gst_video_get_drm_caps (const GstCaps * caps);
GST_VIDEO_API
gboolean          gst_video_info_drm_is_equal (const GstVideoInfoDrm * info, const GstVideoInfoDrm * other);

G_END_DECLS

#endif /* __GST_VIDEO_INFO_DRM_H__ */
