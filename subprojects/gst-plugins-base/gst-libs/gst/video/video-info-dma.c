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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "video-info-dma.h"
#include <gst/allocators/gstdmabuf.h>

#ifndef GST_DISABLE_GST_DEBUG
#define GST_CAT_DEFAULT ensure_debug_category()
static GstDebugCategory *
ensure_debug_category (void)
{
  static gsize cat_gonce = 0;

  if (g_once_init_enter (&cat_gonce)) {
    gsize cat_done;

    cat_done = (gsize) _gst_debug_category_new ("video-info-dma-drm", 0,
        "video-info-dma-drm structure");

    g_once_init_leave (&cat_gonce, cat_done);
  }

  return (GstDebugCategory *) cat_gonce;
}
#else
#define ensure_debug_category() /* NOOP */
#endif /* GST_DISABLE_GST_DEBUG */

static GstVideoInfoDmaDrm *
gst_video_info_dma_drm_copy (const GstVideoInfoDmaDrm * drm_info)
{
  return g_slice_dup (GstVideoInfoDmaDrm, drm_info);
}

/**
 * gst_video_info_dma_drm_free:
 * @drm_info: a #GstVideoInfoDmaDrm
 *
 * Free a GstVideoInfoDmaDrm structure previously allocated with
 * gst_video_info_dma_drm_new()
 *
 * Since: 1.24
 */
void
gst_video_info_dma_drm_free (GstVideoInfoDmaDrm * drm_info)
{
  g_slice_free (GstVideoInfoDmaDrm, drm_info);
}

G_DEFINE_BOXED_TYPE (GstVideoInfoDmaDrm, gst_video_info_dma_drm,
    (GBoxedCopyFunc) gst_video_info_dma_drm_copy,
    (GBoxedFreeFunc) gst_video_info_dma_drm_free);

/**
 * gst_video_info_dma_drm_init:
 * @drm_info: (out caller-allocates): a #GstVideoInfoDmaDrm
 *
 * Initialize @drm_info with default values.
 *
 * Since: 1.24
 */
void
gst_video_info_dma_drm_init (GstVideoInfoDmaDrm * drm_info)
{
  g_return_if_fail (drm_info != NULL);

  gst_video_info_init (&drm_info->vinfo);

  drm_info->drm_fourcc = DRM_FORMAT_INVALID;
  drm_info->drm_modifier = DRM_FORMAT_MOD_INVALID;
}

/**
 * gst_video_info_dma_drm_new:
 *
 * Allocate a new #GstVideoInfoDmaDrm that is also initialized with
 * gst_video_info_dma_drm_init().
 *
 * Returns: (transfer full): a new #GstVideoInfoDmaDrm.
 *   Free it with gst_video_info_dma_drm_free().
 *
 * Since: 1.24
 */
GstVideoInfoDmaDrm *
gst_video_info_dma_drm_new (void)
{
  GstVideoInfoDmaDrm *info;

  info = g_slice_new (GstVideoInfoDmaDrm);
  gst_video_info_dma_drm_init (info);

  return info;
}

/**
 * gst_video_is_dma_drm_caps:
 * @caps: a #GstCaps
 *
 * Check whether the @caps is a dma drm kind caps.
 *
 * Returns: %TRUE if the caps is a dma drm caps.
 *
 * Since: 1.24
 */
gboolean
gst_video_is_dma_drm_caps (const GstCaps * caps)
{
  guint size = 0, i;

  g_return_val_if_fail (caps != NULL, FALSE);

  size = gst_caps_get_size (caps);
  if (size == 0)
    return FALSE;

  for (i = 0; i < size; i++) {
    GstCapsFeatures *f = gst_caps_get_features (caps, i);

    if (!gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_DMABUF))
      return FALSE;
  }

  return TRUE;
}

/**
 * gst_video_info_dma_drm_to_caps:
 * @drm_info: a #GstVideoInfoDmaDrm
 *
 * Convert the values of @drm_info into a #GstCaps. Please note that the
 * @caps does not contain format field, but contains a drm-format field
 * instead. The value of drm-format contains a drm fourcc plus a modifier,
 * such as NV12:0x0100000000000002.
 *
 * Returns: (transfer full) (nullable): a new #GstCaps containing the
 *   info of @drm_info.
 *
 * Since: 1.24
 */
GstCaps *
gst_video_info_dma_drm_to_caps (const GstVideoInfoDmaDrm * drm_info)
{
  GstCaps *caps;
  GstStructure *structure;
  gchar *str;

  g_return_val_if_fail (drm_info != NULL, NULL);
  g_return_val_if_fail (drm_info->drm_fourcc != DRM_FORMAT_INVALID, NULL);
  g_return_val_if_fail (drm_info->drm_modifier != DRM_FORMAT_MOD_INVALID, NULL);

  caps = gst_caps_make_writable (gst_video_info_to_caps (&drm_info->vinfo));
  if (!caps) {
    GST_DEBUG ("Failed to create caps from video info");
    return NULL;
  }

  str = gst_video_dma_drm_fourcc_to_string (drm_info->drm_fourcc,
      drm_info->drm_modifier);

  structure = gst_caps_get_structure (caps, 0);
  gst_structure_remove_field (structure, "format");
  gst_structure_set (structure, "drm-format", G_TYPE_STRING, str, NULL);

  g_free (str);

  return caps;
}

/**
 * gst_video_info_dma_drm_from_caps:
 * @drm_info: (out caller-allocates): #GstVideoInfoDmaDrm
 * @caps: a #GstCaps
 *
 * Parse @caps and update @info. Please note that the @caps should be
 * a dma drm caps. The gst_video_is_dma_drm_caps() can be used to verify
 * it before calling this function.
 *
 * Returns: TRUE if @caps could be parsed
 *
 * Since: 1.24
 */
gboolean
gst_video_info_dma_drm_from_caps (GstVideoInfoDmaDrm * drm_info,
    const GstCaps * caps)
{
  GstStructure *structure;
  const gchar *str;
  guint32 fourcc;
  guint64 modifier;
  GstVideoFormat format;
  GstCaps *tmp_caps = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (drm_info != NULL, FALSE);
  g_return_val_if_fail (caps != NULL, FALSE);
  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);

  GST_DEBUG ("parsing caps %" GST_PTR_FORMAT, caps);

  if (!gst_video_is_dma_drm_caps (caps)) {
    GST_DEBUG ("caps %" GST_PTR_FORMAT " is not a drm caps", caps);
    ret = FALSE;
    goto out;
  }

  tmp_caps = gst_caps_make_writable (gst_caps_copy (caps));
  structure = gst_caps_get_structure (tmp_caps, 0);

  if (gst_structure_get_string (structure, "format")) {
    GST_DEBUG ("drm caps %" GST_PTR_FORMAT
        "has format field, which is not allowed", caps);
    ret = FALSE;
    goto out;
  }

  str = gst_structure_get_string (structure, "drm-format");
  if (!str) {
    GST_DEBUG ("drm caps %" GST_PTR_FORMAT "has no drm-format field", caps);
    ret = FALSE;
    goto out;
  }

  fourcc = gst_video_dma_drm_fourcc_from_string (str, &modifier);
  if (fourcc == DRM_FORMAT_INVALID) {
    GST_DEBUG ("Can not parse fourcc in caps %" GST_PTR_FORMAT, caps);
    ret = FALSE;
    goto out;
  }
  if (modifier == DRM_FORMAT_MOD_INVALID) {
    GST_DEBUG ("Can not parse modifier in caps %" GST_PTR_FORMAT, caps);
    ret = FALSE;
    goto out;
  }

  /* If the modifier is linear, set the according format in video info,
     otherwise, just set the format to GST_VIDEO_FORMAT_ENCODED. */
  /* TODO: Some well known tiled format such as NV12_4L4, NV12_16L16,
     NV12_64Z32, NV12_16L32S */
  format = gst_video_format_from_fourcc (fourcc);
  if (modifier == DRM_FORMAT_MOD_LINEAR && format != GST_VIDEO_FORMAT_UNKNOWN) {
    gst_structure_set (structure, "format", G_TYPE_STRING,
        gst_video_format_to_string (format), NULL);
  } else {
    gst_structure_set (structure, "format", G_TYPE_STRING,
        gst_video_format_to_string (GST_VIDEO_FORMAT_ENCODED), NULL);
  }
  gst_structure_remove_field (structure, "drm-format");

  if (!gst_video_info_from_caps (&drm_info->vinfo, tmp_caps)) {
    GST_DEBUG ("Can not parse video info for caps %" GST_PTR_FORMAT, tmp_caps);
    ret = FALSE;
    goto out;
  }

  drm_info->drm_fourcc = fourcc;
  drm_info->drm_modifier = modifier;
  ret = TRUE;

out:
  gst_clear_caps (&tmp_caps);
  return ret;
}

/**
 * gst_video_info_dma_drm_new_from_caps:
 * @caps: a #GstCaps
 *
 * Parse @caps to generate a #GstVideoInfoDmaDrm.
 *
 * Returns: (transfer full) (nullable): A #GstVideoInfoDmaDrm,
 *   or %NULL if @caps couldn't be parsed.
 *
 * Since: 1.24
 */
GstVideoInfoDmaDrm *
gst_video_info_dma_drm_new_from_caps (const GstCaps * caps)
{
  GstVideoInfoDmaDrm *ret = gst_video_info_dma_drm_new ();

  if (gst_video_info_dma_drm_from_caps (ret, caps)) {
    return ret;
  } else {
    gst_video_info_dma_drm_free (ret);
    return NULL;
  }
}

/**
 * gst_video_dma_drm_fourcc_from_string:
 * @format: a drm-format string
 * @modifier: (out) (optional): Return the modifier in @format or %NULL to ignore.
 *
 * Convert the @format string into the drm fourcc value. The @modifier is
 * also parsed if we want. Please note that the @format should follow the
 * fourcc:modifier kind style, such as NV12:0x0100000000000002
 *
 * Returns: The drm fourcc value or DRM_FORMAT_INVALID if @format is invalid.
 *
 * Since: 1.24
 */
guint32
gst_video_dma_drm_fourcc_from_string (const gchar * format_str,
    guint64 * modifier)
{
  const gchar *mod_str = NULL;
  guint32 fourcc = DRM_FORMAT_INVALID;
  guint64 m = DRM_FORMAT_MOD_INVALID;

  g_return_val_if_fail (format_str != NULL, 0);

  mod_str = strchr (format_str, ':');
  if (mod_str) {
    if (mod_str - format_str != 4) {
      /* fourcc always has 4 characters. */
      GST_DEBUG ("%s is not a drm string", format_str);
      return DRM_FORMAT_INVALID;
    }

    m = g_ascii_strtoull (mod_str + 1, NULL, 0);
    if (m == DRM_FORMAT_MOD_LINEAR) {
      GST_DEBUG ("Unrecognized modifier string %s", mod_str + 1);
      return DRM_FORMAT_INVALID;
    }
  } else {
    if (strlen (format_str) != 4) {
      /* fourcc always has 4 characters. */
      GST_DEBUG ("%s is not a drm string", format_str);
      return DRM_FORMAT_INVALID;
    }

    m = DRM_FORMAT_MOD_LINEAR;
  }

  fourcc = GST_MAKE_FOURCC (format_str[0], format_str[1],
      format_str[2], format_str[3]);

  if (modifier)
    *modifier = m;

  return fourcc;
}

/**
 * gst_video_dma_drm_fourcc_to_string:
 * @fourcc: a drm fourcc value.
 * @modifier: the associated modifier value.
 *
 * Returns a string containing drm kind format, such as
 * NV12:0x0100000000000002, or NULL otherwise.
 *
 * Returns: (transfer full) (nullable): the drm kind string composed
 *   of to @fourcc and @modifier.
 *
 * Since: 1.24
 */
gchar *
gst_video_dma_drm_fourcc_to_string (guint32 fourcc, guint64 modifier)
{
  gchar *s;

  g_return_val_if_fail (fourcc != DRM_FORMAT_INVALID, NULL);
  g_return_val_if_fail (modifier != DRM_FORMAT_MOD_INVALID, NULL);

  if (modifier == DRM_FORMAT_MOD_LINEAR) {
    s = g_strdup_printf ("%" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));
  } else {
    s = g_strdup_printf ("%" GST_FOURCC_FORMAT ":0x%016lx",
        GST_FOURCC_ARGS (fourcc), modifier);
  }

  return s;
}

/**
 * gst_video_dma_drm_fourcc_from_format:
 * @format: a #GstVideoFormat
 *
 * Converting the video format into dma drm fourcc. If no
 * matching fourcc found, then DRM_FORMAT_INVALID is returned.
 *
 * Returns: the DRM_FORMAT_* corresponding to the @format.
 *
 * Since: 1.24
 */
guint32
gst_video_dma_drm_fourcc_from_format (GstVideoFormat format)
{
  switch (format) {
    case GST_VIDEO_FORMAT_YUY2:
      return DRM_FORMAT_YUYV;

    case GST_VIDEO_FORMAT_YVYU:
      return DRM_FORMAT_YVYU;

    case GST_VIDEO_FORMAT_UYVY:
      return DRM_FORMAT_UYVY;

    case GST_VIDEO_FORMAT_VYUY:
      return DRM_FORMAT_VYUY;

    case GST_VIDEO_FORMAT_AYUV:
      /* No VUYA fourcc define, just mapping it as AYUV. */
    case GST_VIDEO_FORMAT_VUYA:
      return DRM_FORMAT_AYUV;

    case GST_VIDEO_FORMAT_NV12:
      return DRM_FORMAT_NV12;

    case GST_VIDEO_FORMAT_NV21:
      return DRM_FORMAT_NV21;

    case GST_VIDEO_FORMAT_NV16:
      return DRM_FORMAT_NV16;

    case GST_VIDEO_FORMAT_NV61:
      return DRM_FORMAT_NV61;

    case GST_VIDEO_FORMAT_NV24:
      return DRM_FORMAT_NV24;

    case GST_VIDEO_FORMAT_YUV9:
      return DRM_FORMAT_YUV410;

    case GST_VIDEO_FORMAT_YVU9:
      return DRM_FORMAT_YVU410;

    case GST_VIDEO_FORMAT_Y41B:
      return DRM_FORMAT_YUV411;

    case GST_VIDEO_FORMAT_I420:
      return DRM_FORMAT_YUV420;

    case GST_VIDEO_FORMAT_YV12:
      return DRM_FORMAT_YVU420;

    case GST_VIDEO_FORMAT_Y42B:
      return DRM_FORMAT_YUV422;

    case GST_VIDEO_FORMAT_Y444:
      return DRM_FORMAT_YUV444;

    case GST_VIDEO_FORMAT_RGB16:
      return DRM_FORMAT_RGB565;

    case GST_VIDEO_FORMAT_BGR16:
      return DRM_FORMAT_BGR565;

    case GST_VIDEO_FORMAT_RGBA:
      return DRM_FORMAT_ABGR8888;

    case GST_VIDEO_FORMAT_RGBx:
      return DRM_FORMAT_XBGR8888;

    case GST_VIDEO_FORMAT_BGRA:
      return DRM_FORMAT_ARGB8888;

    case GST_VIDEO_FORMAT_BGRx:
      return DRM_FORMAT_XRGB8888;

    case GST_VIDEO_FORMAT_ARGB:
      return DRM_FORMAT_BGRA8888;

    case GST_VIDEO_FORMAT_xRGB:
      return DRM_FORMAT_BGRX8888;

    case GST_VIDEO_FORMAT_ABGR:
      return DRM_FORMAT_RGBA8888;

    case GST_VIDEO_FORMAT_xBGR:
      return DRM_FORMAT_RGBX8888;

      /* Add more here if needed. */

    default:
      GST_INFO ("No supported fourcc for video format %s",
          gst_video_format_to_string (format));
      return DRM_FORMAT_INVALID;
  }
}

/**
 * gst_video_dma_drm_fourcc_to_format:
 * @fourcc: the dma drm value.
 *
 * Converting a dma drm fourcc into the video format. If no matching
 * video format found, then GST_VIDEO_FORMAT_UNKNOWN is returned.
 *
 * Returns: the GST_VIDEO_FORMAT_* corresponding to the @fourcc.
 *
 * Since: 1.24
 */
GstVideoFormat
gst_video_dma_drm_fourcc_to_format (guint32 fourcc)
{
  switch (fourcc) {
    case DRM_FORMAT_YUYV:
      return GST_VIDEO_FORMAT_YUY2;

    case DRM_FORMAT_YVYU:
      return GST_VIDEO_FORMAT_YVYU;

    case DRM_FORMAT_UYVY:
      return GST_VIDEO_FORMAT_UYVY;

    case DRM_FORMAT_VYUY:
      return GST_VIDEO_FORMAT_VYUY;

    case DRM_FORMAT_AYUV:
      return GST_VIDEO_FORMAT_VUYA;

    case DRM_FORMAT_NV12:
      return GST_VIDEO_FORMAT_NV12;

    case DRM_FORMAT_NV21:
      return GST_VIDEO_FORMAT_NV21;

    case DRM_FORMAT_NV16:
      return GST_VIDEO_FORMAT_NV16;

    case DRM_FORMAT_NV61:
      return GST_VIDEO_FORMAT_NV61;

    case DRM_FORMAT_NV24:
      return GST_VIDEO_FORMAT_NV24;

    case DRM_FORMAT_YUV410:
      return GST_VIDEO_FORMAT_YUV9;

    case DRM_FORMAT_YVU410:
      return GST_VIDEO_FORMAT_YVU9;

    case DRM_FORMAT_YUV411:
      return GST_VIDEO_FORMAT_Y41B;

    case DRM_FORMAT_YUV420:
      return GST_VIDEO_FORMAT_I420;

    case DRM_FORMAT_YVU420:
      return GST_VIDEO_FORMAT_YV12;

    case DRM_FORMAT_YUV422:
      return GST_VIDEO_FORMAT_Y42B;

    case DRM_FORMAT_YUV444:
      return GST_VIDEO_FORMAT_Y444;

    case DRM_FORMAT_RGB565:
      return GST_VIDEO_FORMAT_RGB16;

    case DRM_FORMAT_BGR565:
      return GST_VIDEO_FORMAT_BGR16;

    case DRM_FORMAT_ABGR8888:
      return GST_VIDEO_FORMAT_RGBA;

    case DRM_FORMAT_XBGR8888:
      return GST_VIDEO_FORMAT_RGBx;

    case DRM_FORMAT_ARGB8888:
      return GST_VIDEO_FORMAT_BGRA;

    case DRM_FORMAT_XRGB8888:
      return GST_VIDEO_FORMAT_BGRx;

    case DRM_FORMAT_BGRA8888:
      return GST_VIDEO_FORMAT_ARGB;

    case DRM_FORMAT_BGRX8888:
      return GST_VIDEO_FORMAT_xRGB;

    case DRM_FORMAT_RGBA8888:
      return GST_VIDEO_FORMAT_ABGR;

    case DRM_FORMAT_RGBX8888:
      return GST_VIDEO_FORMAT_xBGR;

      /* Add more here if needed. */

    default:
      GST_INFO ("No supported video format for fourcc %" GST_FOURCC_FORMAT,
          GST_FOURCC_ARGS (fourcc));
      return GST_VIDEO_FORMAT_UNKNOWN;
  }
}
