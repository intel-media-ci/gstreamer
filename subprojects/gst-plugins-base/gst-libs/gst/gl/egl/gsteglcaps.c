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

/**
 * SECTION:gsteglcaps
 * @short_description: EGL modifier
 * @title: GstEGLCaps
 * @see_also:
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsteglcaps.h"
#include "gstegl.h"

#include <gst/gl/gstglmemory.h>
#include <gst/allocators/gstdmabuf.h>
#include <libdrm/drm_fourcc.h>

static void
fmts_append (GValue * fmts, const gchar * fmt_str)
{
  GValue v = G_VALUE_INIT;
  g_value_init (&v, G_TYPE_STRING);
  g_value_set_string (&v, fmt_str);

  gst_value_list_append_value (fmts, &v);
  g_value_unset (&v);
}

static void
mod_fmts_append (GValue * mod_fmts, const gchar * fmt_str, guint64 mod)
{
  gchar *mod_fmt_str = g_strdup_printf ("%s:0x%016lx", fmt_str, mod);

  fmts_append (mod_fmts, mod_fmt_str);
  g_free (mod_fmt_str);
}

static gboolean
gst_gl_get_modifiers (GstGLContext * context, guint32 fourcc, GValue * modifiers)
{
  EGLDisplay egl_dpy = EGL_DEFAULT_DISPLAY;
  GstGLDisplayEGL *gl_dpy_egl;
  EGLuint64KHR *mods;
  int num_mods = 0;
  gboolean ret;
  GValue gmod = G_VALUE_INIT;

  EGLBoolean (*gst_eglQueryDmaBufModifiersEXT) (
                          EGLDisplay dpy,
                          EGLint format,
                          EGLint max_modifiers,
                          EGLuint64KHR *modifiers,
                          EGLBoolean *external_only,
                          EGLint *num_modifiers);
  gst_eglQueryDmaBufModifiersEXT =
      gst_gl_context_get_proc_address (context, "eglQueryDmaBufModifiersEXT");
  if (!gst_eglQueryDmaBufModifiersEXT)
    return FALSE;

  gl_dpy_egl = gst_gl_display_egl_from_gl_display (context->display);
  if (!gl_dpy_egl) {
    GST_WARNING_OBJECT (context,
        "Failed to retrieve GstGLDisplayEGL from %" GST_PTR_FORMAT,
        context->display);
    return FALSE;
  }
  egl_dpy =
      (EGLDisplay) gst_gl_display_get_handle (GST_GL_DISPLAY (gl_dpy_egl));
  gst_object_unref (gl_dpy_egl);

  ret = gst_eglQueryDmaBufModifiersEXT (egl_dpy,
      fourcc, 0, NULL, NULL, &num_mods);
  if (!ret || num_mods == 0) {
      return FALSE;
  }

  mods = g_new (EGLuint64KHR, num_mods);
  ret = gst_eglQueryDmaBufModifiersEXT (egl_dpy,
      fourcc, num_mods, mods, NULL, &num_mods);
  if (!ret || num_mods == 0) {
    g_free (mods);
    return FALSE;
  }

  g_value_init (&gmod, G_TYPE_UINT64);
  for (int i = 0; i < num_mods; ++i) {
    g_value_set_uint64 (&gmod, mods[i]);
    gst_value_list_append_value (modifiers, &gmod);
  }
  g_free (mods);

  return TRUE;
}

static gboolean
dma_formats_to_modifier_formats (const GValue *dma_fmts,
    GValue *dma_mod_fmts)
{
  gint num_fmts = 0;
  GstGLContext *context;
  GstGLDisplay *display;

  display = GST_GL_DISPLAY (gst_gl_display_egl_new ());
  context = gst_gl_context_new (display);
  gst_gl_context_create (context, 0, NULL);

  num_fmts = gst_value_list_get_size (dma_fmts);

  for (gint i = 0; i < num_fmts; i++) {
    const GValue *gfmt;
    const gchar *fmt_str;
    GstVideoFormat fmt;
    guint32 fcc;
    GValue mods = G_VALUE_INIT;

    gfmt = gst_value_list_get_value (dma_fmts, i);
    fmt_str = g_value_get_string (gfmt);
    fmt = gst_video_format_from_string (fmt_str);
    fcc = gst_video_format_to_fourcc (fmt);

    g_value_init (&mods, GST_TYPE_LIST);
    if (!gst_gl_get_modifiers (context, fcc, &mods)){
      fmts_append (dma_mod_fmts, fmt_str);
    }

    for (gint j = 0; j < gst_value_list_get_size (&mods); j++) {
      const GValue *gmod = gst_value_list_get_value (&mods, j);
      guint64 mod = g_value_get_uint64 (gmod);
      GST_DEBUG ("Got mofidier: %s:0x%016lx", fmt_str, mod);

      if (mod == DRM_FORMAT_MOD_INVALID)
        continue;

      if (mod == DRM_FORMAT_MOD_LINEAR)
        fmts_append (dma_mod_fmts, fmt_str);
      else
        mod_fmts_append (dma_mod_fmts, fmt_str, mod);
    }
    g_value_unset (&mods);
  }

  gst_object_unref (context);
  gst_object_unref (display);

  return TRUE;
}

GstCaps *
gst_egl_caps_embed_modifiers (GstCaps * caps, GstGLContext * context)
{
  GstCaps *new_caps = gst_caps_new_empty ();

  for (gint i = 0; i < gst_caps_get_size (caps); i++) {
    GstCaps *tmp_caps = gst_caps_copy_nth (caps, i);
    if (gst_egl_caps_has_feature (tmp_caps, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
      GstStructure *dma_struct;
      const GValue *dma_fmts;
      GValue dma_mod_fmts = G_VALUE_INIT;

      dma_struct = gst_caps_get_structure (tmp_caps, 0);
      dma_fmts = gst_structure_get_value (dma_struct, "format");

      g_value_init (&dma_mod_fmts, GST_TYPE_LIST);
      dma_formats_to_modifier_formats (dma_fmts, &dma_mod_fmts);

      gst_caps_set_value (tmp_caps, "format", &dma_mod_fmts);
      g_value_unset (&dma_mod_fmts);
    }

    gst_caps_append (new_caps, tmp_caps);
  }
  gst_caps_ref (new_caps);

  return new_caps;
}

gboolean
gst_egl_caps_has_feature (const GstCaps * caps, const gchar * feature)
{
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstCapsFeatures *const features = gst_caps_get_features (caps, i);
    if (gst_caps_features_is_any (features))
      continue;
    if (gst_caps_features_contains (features, feature))
      return TRUE;
  }

  return FALSE;
}

guint64
gst_egl_caps_get_modifier (GstCaps * caps)
{
  GstStructure *structure;
  const gchar *mod_fmt_str;
  gchar *mod_str;

  if (!gst_egl_caps_has_feature (caps, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return DRM_FORMAT_MOD_INVALID;

  structure = gst_caps_get_structure (caps, 0);
  mod_fmt_str = gst_structure_get_string (structure, "format");
  mod_str = strchr (mod_fmt_str, ':');

  return mod_str == NULL ? DRM_FORMAT_MOD_LINEAR :
      g_ascii_strtoull (mod_str + 1, NULL, 0);
}

void
gst_egl_caps_set_modifier (GstCaps * caps, guint64 modifier)
{
  GstStructure *structure;
  const gchar *fmt_str;
  gchar *fmt_mod_str;
  GValue fmt_mod = G_VALUE_INIT;

  if (modifier == DRM_FORMAT_MOD_LINEAR ||
        !gst_egl_caps_has_feature (caps, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return;

  structure = gst_caps_get_structure (caps, 0);
  fmt_str = gst_structure_get_string (structure, "format");
  fmt_mod_str = g_strdup_printf ("%s:0x%016lx", fmt_str, modifier);

  g_value_init (&fmt_mod, G_TYPE_STRING);
  g_value_set_string (&fmt_mod, fmt_mod_str);
  gst_caps_set_value (caps, "format", &fmt_mod);

  g_value_unset (&fmt_mod);
  g_free (fmt_mod_str);
}

GstCaps *
gst_egl_caps_remove_modifier (GstCaps * caps)
{
  GstStructure *structure;
  const gchar *mod_fmt_str;
  gchar *mod_fmt_str_cpy, *mod_str;
  GstVideoFormat format;
  GstCaps *new_caps;

  if (!gst_egl_caps_has_feature (caps, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return caps;

  structure = gst_caps_get_structure (caps, 0);
  mod_fmt_str = gst_structure_get_string (structure, "format");

  mod_fmt_str_cpy = g_strdup (mod_fmt_str);
  mod_str = strchr (mod_fmt_str_cpy, ':');
  if (mod_str)
    *mod_str = '\0';
  else {
    g_free (mod_fmt_str_cpy);
    return caps;
  }

  format = gst_video_format_from_string (mod_fmt_str_cpy);
  g_free (mod_fmt_str_cpy);

  new_caps = gst_caps_make_writable (gst_caps_copy (caps));
  structure = gst_caps_get_structure (new_caps, 0);
  gst_structure_set (structure, "format", G_TYPE_STRING,
      gst_video_format_to_string (format), NULL);

  return new_caps;
}
