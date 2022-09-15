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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "video-format.h"
#include "video-drm-format.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video-overlay-composition.h>

static gint
_get_dma_index (const GstCaps * caps)
{
  guint size = 0;
  g_return_val_if_fail (GST_IS_CAPS (caps), -1);

  size = gst_caps_get_size (caps);
  for (guint i = 0; i < size; i++) {
    GstCapsFeatures *const f = gst_caps_get_features (caps, i);
    if (gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_DMABUF) &&
        !gst_caps_features_contains (f,
            GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION)) {
      return i;
    }
  }

  return -1;
}

static inline gboolean
_has_dma_feature (const GstCaps * caps)
{
  return _get_dma_index (caps) >= 0;
}

guint64
gst_video_drm_format_get_modifier (const GstCaps * caps)
{
  GstStructure *s;
  const gchar *drm_fmt_str, *mod_str = NULL;

  if (!_has_dma_feature (caps))
    return DRM_FORMAT_MOD_INVALID;

  s = gst_caps_get_structure (caps, _get_dma_index (caps));
  drm_fmt_str = gst_structure_get_string (s, "drm-format");
  mod_str = drm_fmt_str == NULL ? NULL : strchr (drm_fmt_str, ':');

  return mod_str == NULL ? DRM_FORMAT_MOD_LINEAR :
      g_ascii_strtoull (mod_str + 1, NULL, 0);
}


GstVideoFormat
gst_video_drm_format_get_format (const GstCaps * caps)
{
  GstStructure *s;
  const gchar *drm_fmt_str, *mod_str = NULL;
  gchar *fmt_str = NULL;
  GstVideoFormat fmt;

  if (!_has_dma_feature (caps))
    return GST_VIDEO_FORMAT_UNKNOWN;

  s = gst_caps_get_structure (caps, _get_dma_index (caps));
  drm_fmt_str = gst_structure_get_string (s, "drm-format");
  if (!drm_fmt_str)
    return GST_VIDEO_FORMAT_UNKNOWN;

  mod_str = strchr (drm_fmt_str, ':');
  fmt_str = mod_str == NULL ? g_strdup (drm_fmt_str) :
      g_strndup (drm_fmt_str, mod_str - drm_fmt_str);

  fmt = gst_video_format_from_string (fmt_str);
  if (fmt == GST_VIDEO_FORMAT_UNKNOWN)
    GST_ERROR ("unknown drm format %s", fmt_str);

  if (mod_str)
    g_free (fmt_str);

  return fmt;
}

GstCaps *
gst_video_drm_format_set_modifier (GstCaps * caps, guint64 modifier)
{
  GstStructure *s;
  GstVideoFormat fmt;
  gchar *drm_fmt_str;
  GstCaps *mod_caps;

  if (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR)
    return caps;

  fmt = gst_video_drm_format_get_format (caps);
  if (fmt == GST_VIDEO_FORMAT_UNKNOWN)
    return caps;

  mod_caps = gst_caps_make_writable (gst_caps_copy (caps));
  s = gst_caps_get_structure (mod_caps, _get_dma_index (mod_caps));

  drm_fmt_str = g_strdup_printf ("%s:0x%016lx",
      gst_video_format_to_string (fmt), modifier);
  gst_structure_set (s, "drm-format", G_TYPE_STRING, drm_fmt_str, NULL);

  return mod_caps;
}

GstCaps *
gst_video_drm_format_remove_modifier (GstCaps * caps)
{
  GstStructure *s;
  GstVideoFormat fmt;
  GstCaps *nomod_caps;

  fmt = gst_video_drm_format_get_format (caps);
  if (fmt == GST_VIDEO_FORMAT_UNKNOWN)
    return caps;

  nomod_caps = gst_caps_make_writable (gst_caps_copy (caps));
  s = gst_caps_get_structure (nomod_caps, _get_dma_index (caps));
  gst_structure_set (s, "drm-format", G_TYPE_STRING,
      gst_video_format_to_string (fmt), NULL);

  return nomod_caps;
}

