/* GStreamer Intel MSDK plugin
 * Copyright (c) 2022, Intel Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <drm_fourcc.h>

#include "gstmsdkcaps.h"
#include "msdk_libva.h"
#include <gst/va/gstvavideoformat.h>

gboolean
gst_msdkcaps_has_feature (const GstCaps * caps, const gchar * feature)
{
  guint i;

  for (i = 0; i < gst_caps_get_size (caps); i++) {
    GstCapsFeatures *const features = gst_caps_get_features (caps, i);
    /* Skip ANY features, we need an exact match for correct evaluation */
    if (gst_caps_features_is_any (features))
      continue;
    if (gst_caps_features_contains (features, feature))
      return TRUE;
  }

  return FALSE;
}

static guint64
_get_modifier (GstMsdkContext * context,
    guint usage_hint, guint va_format, guint va_fourcc)
{
  VAStatus status;
  VASurfaceID surface;
  VASurfaceAttrib attribs[2];
  guint num_attribs = 0;
  uint32_t flags = VA_EXPORT_SURFACE_SEPARATE_LAYERS |
      VA_EXPORT_SURFACE_READ_WRITE;

  VADRMPRIMESurfaceDescriptor desc = { 0 };
  gpointer dpy = gst_msdk_context_get_handle (context);

  attribs[num_attribs].type = VASurfaceAttribPixelFormat;
  attribs[num_attribs].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attribs[num_attribs].value.type = VAGenericValueTypeInteger;
  attribs[num_attribs].value.value.i = va_fourcc;
  num_attribs += 1;
  attribs[num_attribs].type = VASurfaceAttribUsageHint;
  attribs[num_attribs].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attribs[num_attribs].value.type = VAGenericValueTypeInteger;
  attribs[num_attribs].value.value.i = usage_hint;
  num_attribs += 1;

  status = vaCreateSurfaces (dpy, va_format, 64, 64, &surface, 1,
      attribs, num_attribs);
  if (VA_STATUS_SUCCESS != status) {
    GST_WARNING ("Failed to create VA surefaces");
    return DRM_FORMAT_MOD_INVALID;
  }

  status = vaExportSurfaceHandle (dpy, surface,
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, flags, &desc);
  vaDestroySurfaces (dpy, &surface, 1);
  if (VA_STATUS_SUCCESS != status) {
    GST_WARNING ("Failed to export surface handle");
    return DRM_FORMAT_MOD_INVALID;
  }

  return desc.objects[0].drm_format_modifier;
}

static void
_get_modifiers (GstMsdkContext * context,
    guint usage_hint, GstVideoFormat fmt, GValue * modifiers)
{
  guint64 mod = DRM_FORMAT_MOD_INVALID;
  guint64 mod_gen = DRM_FORMAT_MOD_INVALID;
  GValue gmod = G_VALUE_INIT;

  gint mfx_chroma = gst_msdk_get_mfx_chroma_from_format (fmt);
  gint mfx_fourcc = gst_msdk_get_mfx_fourcc_from_format (fmt);
  guint va_format = gst_msdk_get_va_rt_format_from_mfx_rt_format (mfx_chroma);
  guint va_fourcc = gst_msdk_get_va_fourcc_from_mfx_fourcc (mfx_fourcc);

  g_value_init (&gmod, G_TYPE_UINT64);

  mod = _get_modifier (context, usage_hint, va_format, va_fourcc);
  if (mod != DRM_FORMAT_MOD_INVALID && mod != DRM_FORMAT_MOD_LINEAR) {
    g_value_set_uint64 (&gmod, mod);
    gst_value_list_append_value (modifiers, &gmod);
  }

  if (usage_hint != VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC) {
    mod_gen = _get_modifier (context,
        VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC, va_format, va_fourcc);
    if (mod_gen != mod && mod_gen != DRM_FORMAT_MOD_INVALID &&
          mod_gen != DRM_FORMAT_MOD_LINEAR) {
      g_value_set_uint64 (&gmod, mod_gen);
      gst_value_list_append_value (modifiers, &gmod);
    }
  }

  if (mod == DRM_FORMAT_MOD_LINEAR || mod_gen == DRM_FORMAT_MOD_LINEAR) {
    g_value_set_uint64 (&gmod, DRM_FORMAT_MOD_LINEAR);
    gst_value_list_append_value (modifiers, &gmod);
  }

  if (mod == DRM_FORMAT_MOD_INVALID && mod_gen == DRM_FORMAT_MOD_INVALID) {
    GST_WARNING ("Failed to get modifier %s:0x%016llx",
        gst_video_format_to_string (fmt), DRM_FORMAT_MOD_INVALID);

    g_value_set_uint64 (&gmod, DRM_FORMAT_MOD_INVALID);
    gst_value_list_append_value (modifiers, &gmod);
  }

  g_value_unset (&gmod);
}

static void
_list_append_string (GValue * list, const gchar * str)
{
  GValue gval = G_VALUE_INIT;

  g_return_if_fail (list != NULL);
  g_return_if_fail (str != NULL);

  g_value_init (&gval, G_TYPE_STRING);
  g_value_set_string (&gval, str);

  gst_value_list_append_value (list, &gval);
  g_value_unset (&gval);
}

static const gchar *
_drm_format_to_string (const gchar * fmt_str, guint64 modifier)
{
  if (modifier == DRM_FORMAT_MOD_INVALID)
    return NULL;

  if (modifier == DRM_FORMAT_MOD_LINEAR)
    return fmt_str;

  return g_strdup_printf ("%s:0x%016lx", fmt_str, modifier);
}

static gboolean
_dma_fmt_to_drm_fmts (GstMsdkContext * context,
    guint usage_hint, const GValue * dma_fmts, GValue * drm_fmts)
{
  const gchar *fmt_str;
  const gchar *drm_fmt_str;
  GstVideoFormat fmt;
  GValue mods = G_VALUE_INIT;

  if (!dma_fmts)
    return FALSE;

  fmt_str = g_value_get_string (dma_fmts);
  fmt = gst_video_format_from_string (fmt_str);
  if (fmt == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  g_value_init (&mods, GST_TYPE_LIST);
  _get_modifiers (context, usage_hint, fmt, &mods);

  for (gint m = 0; m < gst_value_list_get_size (&mods); m++) {
    const GValue *gmod = gst_value_list_get_value (&mods, m);
    guint64 mod = g_value_get_uint64 (gmod);

    drm_fmt_str = _drm_format_to_string (fmt_str, mod);
    if (!drm_fmt_str)
      continue;

    _list_append_string (drm_fmts, drm_fmt_str);

    GST_DEBUG ("Got mofidier: %s", drm_fmt_str);
  }
  g_value_unset (&mods);

  return TRUE;
}

static gboolean
_dma_fmts_to_drm_fmts (GstMsdkContext * context,
    guint usage_hint, const GValue * dma_fmts, GValue * drm_fmts)
{
  gint size = gst_value_list_get_size (dma_fmts);

  for (gint f = 0; f < size; f++) {
    const GValue *dma_fmt =  gst_value_list_get_value (dma_fmts, f);
    if (!dma_fmt)
      continue;

    _dma_fmt_to_drm_fmts (context, usage_hint, dma_fmt, drm_fmts);
  }

  return TRUE;
}

static guint
_get_usage_hint (GstMsdkContextJobType job_type)
{
  guint hint;

  switch (job_type) {
    case GST_MSDK_JOB_DECODER:
      hint = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER;
      break;
    case GST_MSDK_JOB_ENCODER:
      hint = VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER;
      break;
    case GST_MSDK_JOB_VPP:
      hint = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ |
          VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE;
      break;
    default:
      GST_WARNING ("Unsupported job type %d", job_type);
      hint = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC;
      break;
  }

  return hint;
}

static GstCaps *
_create_drm_caps (GstMsdkContext * context,
    GstMsdkContextJobType job_type, const GValue * dma_fmts ,
    gint min_width, gint max_width, gint min_height, gint max_height)
{
  GstCaps *drm_caps;
  GValue drm_fmts = G_VALUE_INIT;
  guint usage_hint;

  GstMsdkContext *ctx = (context != NULL) ?
      context : gst_msdk_context_new (TRUE, job_type);
  if (!ctx)
    return NULL;

  usage_hint = _get_usage_hint (job_type);

  g_value_init (&drm_fmts, GST_TYPE_LIST);
  if (G_VALUE_HOLDS_STRING (dma_fmts)) {
    _dma_fmt_to_drm_fmts (ctx, usage_hint, dma_fmts, &drm_fmts);
  } else if (GST_VALUE_HOLDS_LIST (dma_fmts)) {
    _dma_fmts_to_drm_fmts (ctx, usage_hint, dma_fmts, &drm_fmts);
  }
  gst_object_unref (ctx);

  if (!gst_value_list_get_size (&drm_fmts)) {
    g_value_unset (&drm_fmts);
    return NULL;
  }

  drm_caps = gst_caps_from_string ("video/x-raw(memory:DMABuf)");
  gst_caps_set_value (drm_caps, "drm-format", &drm_fmts);
  g_value_unset (&drm_fmts);

  gst_caps_set_simple (drm_caps,
      "width", GST_TYPE_INT_RANGE, min_width, max_width,
      "height", GST_TYPE_INT_RANGE, min_height, max_height, NULL);

  return drm_caps;
}

GstCaps *
gst_msdkcaps_create_drm_caps (GstMsdkContext * context,
    GstMsdkContextJobType job_type, const gchar * dma_fmts_str,
    gint min_width, gint max_width, gint min_height, gint max_height)
{
  GstCaps *drm_caps;
  gchar **fmts = NULL;
  GValue dma_fmts = G_VALUE_INIT;

  g_value_init (&dma_fmts, GST_TYPE_LIST);
  fmts = g_strsplit (dma_fmts_str, ", ", 0);
  for (guint f = 0; fmts[f]; f++)
    _list_append_string (&dma_fmts, fmts[f]);
  g_strfreev (fmts);

  drm_caps = _create_drm_caps (context, job_type,
      &dma_fmts, min_width, max_width, min_height, max_height);
  g_value_unset (&dma_fmts);

  GST_DEBUG ("Created dma drm caps %"GST_PTR_FORMAT, drm_caps);

  return drm_caps;
}

GstCaps *
gst_msdkcaps_video_info_to_drm_caps (GstVideoInfo * info, guint64 modifier)
{
  GstVideoInfoDmaDrm drm_info;

  gst_video_info_dma_drm_init (&drm_info);
  drm_info.vinfo = *info;
  drm_info.drm_fourcc =
      gst_va_fourcc_from_video_format (GST_VIDEO_INFO_FORMAT (info));
  drm_info.drm_modifier = modifier;

  return gst_video_info_dma_drm_to_caps (&drm_info);
}

static gboolean
_get_used_formats (GValue * used_fmts,
    const GValue * fmt, const GValue * refer_fmts)
{
  const char *fmt_str;;
  guint len;
  guint size;
  gboolean ret = FALSE;

  if (!fmt || !refer_fmts)
    return FALSE;

  fmt_str = g_value_get_string (fmt);
  len = strlen (fmt_str);
  size = gst_value_list_get_size (refer_fmts);

  for (guint i = 0; i < size; i++) {
    const GValue *val = gst_value_list_get_value (refer_fmts, i);
    const char *val_str = g_value_get_string (val);

    if (strncmp (fmt_str, val_str, len) == 0) {
      gst_value_list_append_value (used_fmts, val);
      ret = TRUE;
    }
  }

  return ret;
}

GstCaps *
gst_msdkcaps_intersect (GstCaps * caps, GstCaps * refer_caps)
{
  GstStructure *dma_s = NULL;
  const GValue *fmts = NULL;
  GValue used_fmts = G_VALUE_INIT;
  gboolean success = FALSE;
  GstCaps *ret = gst_caps_copy (caps);
  guint size = gst_caps_get_size (ret);

  //ret = gst_caps_make_writable (ret);
  for (guint i = 0; i < size; i++) {
    const GValue *refer_fmts = NULL;
    GstCapsFeatures *f = gst_caps_get_features (ret, i);
    if (!gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_DMABUF))
      continue;

    dma_s = gst_caps_get_structure (ret, i);
    fmts =  gst_structure_get_value (dma_s, "format");
    if (!fmts)
      continue;

    for (guint j = 0; j < gst_caps_get_size (refer_caps); j++) {
      GstCapsFeatures *ft = gst_caps_get_features (refer_caps, j);
      if (!gst_caps_features_contains (ft, GST_CAPS_FEATURE_MEMORY_DMABUF))
        continue;
      refer_fmts = gst_structure_get_value (
          gst_caps_get_structure (refer_caps, j), "drm-format");
    }
    if (!refer_fmts)
      continue;

    g_value_init (&used_fmts, GST_TYPE_LIST);
    if (G_VALUE_HOLDS_STRING (fmts)) {
      success = _get_used_formats (&used_fmts, fmts, refer_fmts);
    } else if (GST_VALUE_HOLDS_LIST (fmts)) {
      guint n = gst_value_list_get_size (fmts);
      for (guint k = 0; k < n; k++) {
        const GValue *val = gst_value_list_get_value (fmts, k);
        if (_get_used_formats (&used_fmts, val, refer_fmts))
          success = TRUE;
      }
    }

    if (success) {
      gst_structure_set_value (dma_s, "drm-format", &used_fmts); 
      gst_structure_remove_field (dma_s, "format");
    }

    g_value_unset (&used_fmts);

  }

  ret = gst_caps_intersect (ret, refer_caps);

  GST_DEBUG ("intersected caps: %"GST_PTR_FORMAT, ret);

  return ret;
}


gboolean
get_msdkcaps_fixate_format (GstCaps * caps, GstVideoFormat fmt)
{
  GValue gfmt = G_VALUE_INIT;
  const gchar *fmt_str = gst_video_format_to_string (fmt);
  guint size = gst_caps_get_size (caps);

  g_value_init (&gfmt, G_TYPE_STRING);
  g_value_set_string (&gfmt, fmt_str);

  for (guint i = 0; i < size; i++) {
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    GstStructure *s = gst_caps_get_structure (caps, i);

    if (gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
      GValue used_fmts = G_VALUE_INIT;
      const GValue *drm_fmts = gst_structure_get_value (s, "drm-format");

      g_value_init (&used_fmts, GST_TYPE_LIST);
      if (!_get_used_formats (&used_fmts, &gfmt, drm_fmts)) {
        g_value_unset (&used_fmts);
        goto failed;
      }

      gst_structure_set_value (s, "drm-format", &used_fmts);
      g_value_unset (&used_fmts);

      if (gst_structure_has_field (s, "format"))
        gst_structure_remove_field (s, "format");
    } else {
      const GValue *fmts = gst_structure_get_value (s, "format");
      if (!gst_value_can_intersect (&gfmt, fmts))
        goto failed;

      gst_structure_set_value (s, "format", &gfmt);
    }
  }

  g_value_unset (&gfmt);

  return TRUE;

failed:
  g_value_unset (&gfmt);
  return FALSE;
}

guint64
get_msdkcaps_get_modifier (const GstCaps * caps)
{
  guint64 modifier = DRM_FORMAT_MOD_INVALID;
  guint size = gst_caps_get_size (caps);

  for (guint i = 0; i < size; i++) {
    GstCapsFeatures *f = gst_caps_get_features (caps, i);

    if (gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
      GstStructure *s = gst_caps_get_structure (caps, i);
      const GValue *drm_fmts = gst_structure_get_value (s, "drm-format");
      const gchar *drm_str = NULL;

      if (!drm_fmts)
        continue;

      if (G_VALUE_HOLDS_STRING (drm_fmts))
        drm_str = g_value_get_string (drm_fmts);
      else if (GST_VALUE_HOLDS_LIST (drm_fmts)) {
        const GValue *val = gst_value_list_get_value (drm_fmts, 0);
        drm_str = g_value_get_string (val);
      }

      gst_video_dma_drm_fourcc_from_string (drm_str, &modifier);
    }
  }

  GST_DEBUG ("got modifier: 0x%016lx", modifier);

  return modifier;
}

static void
_drm_format_get_format (GValue * fmts, const gchar * drm_fmt_str)
{
  gchar **tokens;

  if (!drm_fmt_str || !GST_VALUE_HOLDS_LIST (fmts))
    return;

  tokens = g_strsplit (drm_fmt_str, ":", 0);
  _list_append_string (fmts, tokens[0]);

  g_strfreev (tokens);
}

gboolean
get_msdkcaps_remove_drm_format (GstCaps * caps)
{
  guint size = gst_caps_get_size (caps);
  GValue fmts = G_VALUE_INIT;

  for (guint i = 0; i < size; i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);
    const GValue *drm_fmts = gst_structure_get_value (s, "drm-format");
    const gchar *drm_str = NULL;

    if (!drm_fmts)
      continue;

    if (gst_structure_has_field (s, "format")) {
      gst_structure_remove_field (s, "drm-format");
      continue;
    }

    g_value_init (&fmts, GST_TYPE_LIST);

    if (G_VALUE_HOLDS_STRING (drm_fmts)) {
      const GValue *val;
      drm_str = g_value_get_string (drm_fmts);
      _drm_format_get_format (&fmts, drm_str);

      val = gst_value_list_get_value (&fmts, 0);
      gst_structure_set_value (s, "format", val);
    } else if (GST_VALUE_HOLDS_LIST (drm_fmts)) {
      guint n = gst_value_list_get_size (drm_fmts);
      for (guint j = 0; j < n; j++) {
        const GValue *val = gst_value_list_get_value (&fmts, j);
        drm_str = g_value_get_string (val);
        _drm_format_get_format (&fmts, drm_str);
      }

      gst_structure_set_value (s, "format", &fmts);
    }
    g_value_unset (&fmts);

    gst_structure_remove_field (s, "drm-format");
  }

  return TRUE;
}

