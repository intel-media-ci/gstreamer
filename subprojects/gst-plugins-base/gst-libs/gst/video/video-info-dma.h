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

#ifndef __GST_VIDEO_INFO_DMA_H__
#define __GST_VIDEO_INFO_DMA_H__

#include "video-info.h"
#include <libdrm/drm_fourcc.h>

G_BEGIN_DECLS

typedef struct _GstVideoInfoDmaDrm GstVideoInfoDmaDrm;

struct _GstVideoInfoDmaDrm
{
  GstVideoInfo vinfo;
  guint32 drm_fourcc;
  guint64 drm_modifier;
};

#define GST_TYPE_VIDEO_INFO_DMA_DRM  (gst_video_info_dma_drm_get_type ())
GST_VIDEO_API
GType                gst_video_info_dma_drm_get_type (void);
GST_VIDEO_API
void                 gst_video_info_dma_drm_free (GstVideoInfoDmaDrm * info);
GST_VIDEO_API
void                 gst_video_info_dma_drm_init (GstVideoInfoDmaDrm * drm_info);
GST_VIDEO_API
GstVideoInfoDmaDrm * gst_video_info_dma_drm_new (void);
GST_VIDEO_API
GstCaps *            gst_video_info_dma_drm_to_caps (const GstVideoInfoDmaDrm * drm_info);
GST_VIDEO_API
gboolean             gst_video_info_dma_drm_from_caps (GstVideoInfoDmaDrm * drm_info, const GstCaps * caps);
GST_VIDEO_API
GstVideoInfoDmaDrm * gst_video_info_dma_drm_new_from_caps (const GstCaps * caps);
GST_VIDEO_API
gboolean             gst_video_is_dma_drm_caps (const GstCaps * caps);

GST_VIDEO_API
guint32              gst_video_dma_drm_fourcc_from_string (const gchar * format_str, guint64 * modifier);
GST_VIDEO_API
gchar *              gst_video_dma_drm_fourcc_to_string (guint32 fourcc, guint64 modifier);
GST_VIDEO_API
guint32              gst_video_dma_drm_fourcc_from_format (GstVideoFormat format);
GST_VIDEO_API
GstVideoFormat       gst_video_dma_drm_fourcc_to_format (guint32 fourcc);

G_END_DECLS

#endif /* __GST_VIDEO_INFO_DMA_H__ */
