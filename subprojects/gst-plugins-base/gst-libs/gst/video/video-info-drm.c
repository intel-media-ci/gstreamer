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

#include "video-info-drm.h"

#ifndef GST_CAPS_FEATURE_MEMORY_DMABUF
#define GST_CAPS_FEATURE_MEMORY_DMABUF "memory:DMABuf"
#endif

#ifndef GST_DISABLE_GST_DEBUG
#define GST_CAT_DEFAULT ensure_debug_category()
static GstDebugCategory *
ensure_debug_category (void)
{
  static gsize cat_gonce = 0;

  if (g_once_init_enter (&cat_gonce)) {
    gsize cat_done;

    cat_done = (gsize) _gst_debug_category_new ("video-info-drm", 0,
        "video-info-drm structure");

    g_once_init_leave (&cat_gonce, cat_done);
  }

  return (GstDebugCategory *) cat_gonce;
}
#else
#define ensure_debug_category() /* NOOP */
#endif /* GST_DISABLE_GST_DEBUG */

GstVideoInfoDrm *
gst_video_info_drm_copy (const GstVideoInfoDrm * info)
{
  return g_slice_dup (GstVideoInfoDrm, info);
}

void
gst_video_info_drm_free (GstVideoInfoDrm * info)
{
  g_slice_free (GstVideoInfoDrm, info);
}

G_DEFINE_BOXED_TYPE (GstVideoInfoDrm, gst_video_info_drm,
    (GBoxedCopyFunc) gst_video_info_drm_copy,
    (GBoxedFreeFunc) gst_video_info_drm_free);

void
gst_video_info_drm_init (GstVideoInfoDrm * drm_info)
{
  g_return_if_fail (drm_info != NULL);

  gst_video_info_init (&drm_info->vinfo);

  drm_info->drm_fourcc = DRM_FORMAT_INVALID;
  drm_info->drm_modifier = DRM_FORMAT_MOD_INVALID;
}

GstVideoInfoDrm *
gst_video_info_drm_new (void)
{
  GstVideoInfoDrm *info;

  info = g_slice_new (GstVideoInfoDrm);
  gst_video_info_drm_init (info);

  return info;
}

guint32
gst_video_drm_format_from_string (const gchar * format_str, guint64 * modifier)
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

gchar *
gst_video_drm_format_to_string (guint32 fourcc, guint64 modifier)
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

gboolean
gst_video_is_drm_caps (const GstCaps * caps)
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

GstCaps *
gst_video_get_drm_caps (const GstCaps * caps)
{
  guint size = 0, i;
  GstCaps *new_caps = NULL;
  gboolean have_dma_feature;
  GstCapsFeatures *f;
  GstStructure *s;

  g_return_val_if_fail (caps != NULL, NULL);

  if (gst_video_is_drm_caps (caps))
    return gst_caps_copy (caps);

  new_caps = gst_caps_new_empty ();

  have_dma_feature = FALSE;
  size = gst_caps_get_size (caps);
  for (i = 0; i < size; i++) {
    f = gst_caps_get_features (caps, i);
    s = gst_caps_get_structure (caps, i);

    if (!gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_DMABUF))
      continue;

    have_dma_feature = TRUE;
    gst_caps_append_structure (new_caps, gst_structure_copy (s));
  }

  if (!have_dma_feature)
    gst_clear_caps (&new_caps);

  return new_caps;
}

GstCaps *
gst_video_info_drm_to_caps (const GstVideoInfoDrm * drm_info)
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

  str = gst_video_drm_format_to_string (drm_info->drm_fourcc,
      drm_info->drm_modifier);

  structure = gst_caps_get_structure (caps, 0);
  gst_structure_remove_field (structure, "format");
  gst_structure_set (structure, "drm-format", G_TYPE_STRING, str, NULL);

  g_free (str);

  return caps;
}

gboolean
gst_video_info_drm_from_caps (GstVideoInfoDrm * drm_info, const GstCaps * caps)
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

  if (!gst_video_is_drm_caps (caps)) {
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

  fourcc = gst_video_drm_format_from_string (str, &modifier);
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

GstVideoInfoDrm *
gst_video_info_drm_new_from_caps (const GstCaps * caps)
{
  GstVideoInfoDrm *ret = gst_video_info_drm_new ();

  if (gst_video_info_drm_from_caps (ret, caps)) {
    return ret;
  } else {
    gst_video_info_drm_free (ret);
    return NULL;
  }
}

gboolean
gst_video_info_drm_is_equal (const GstVideoInfoDrm * info,
    const GstVideoInfoDrm * other)
{
  if (info->drm_modifier != other->drm_modifier)
    return FALSE;
  if (info->drm_fourcc != other->drm_fourcc)
    return FALSE;

  return gst_video_info_is_equal (&info->vinfo, &other->vinfo);
}
