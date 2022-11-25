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

#include "gstmsdkcaps.h"

#include <gst/va/gstva.h>

static guint default_width = GST_ROUND_UP_16 (320);
static guint default_height = GST_ROUND_UP_16 (240);

#define DEFAULT_VIDEO_FORMAT GST_VIDEO_FORMAT_NV12

#define PROFILE_DELIMITER ", "

typedef struct
{
  mfxU32 id;
  const gchar *names;
} Profile;

static const Profile profs_avc[] = {
  {MFX_PROFILE_AVC_MAIN, "main"},
  {MFX_PROFILE_AVC_BASELINE, "baseline"},
  {MFX_PROFILE_AVC_EXTENDED, "extended"},
  {MFX_PROFILE_AVC_HIGH, "high"},
  {MFX_PROFILE_AVC_CONSTRAINED_BASELINE, "constrained-baseline"},
  {MFX_PROFILE_AVC_CONSTRAINED_HIGH, "constrained-high"},
  {MFX_PROFILE_AVC_PROGRESSIVE_HIGH, "progressive-high"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_hevc[] = {
  {MFX_PROFILE_HEVC_MAIN, "main"},
  {MFX_PROFILE_HEVC_MAIN10, "main-10, main-10-still-picture"},
  {MFX_PROFILE_HEVC_MAINSP, "main-still-picture"},
  {MFX_PROFILE_HEVC_REXT, "main-444, main-444-10, main-422-10, main-12"},
  {MFX_PROFILE_HEVC_SCC, "screen-extended-main, screen-extended-main-10, "
      "screen-extended-main-444, screen-extended-main-444-10"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_mpeg2[] = {
  {MFX_PROFILE_MPEG2_MAIN, "main"},
  {MFX_PROFILE_MPEG2_SIMPLE, "simple"},
  {MFX_PROFILE_MPEG2_HIGH,"high"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_vc1[] = {
  {MFX_PROFILE_VC1_MAIN, "main"},
  {MFX_PROFILE_VC1_SIMPLE, "simple"},
  {MFX_PROFILE_VC1_ADVANCED, "advanced"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_vp8[] = {
  {MFX_PROFILE_VP8_0, "0"},
  {MFX_PROFILE_VP8_1, "1"},
  {MFX_PROFILE_VP8_2, "2"},
  {MFX_PROFILE_VP8_3, "3"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_vp9[] = {
  {MFX_PROFILE_VP9_0, "0"},
  {MFX_PROFILE_VP9_1, "1"},
  {MFX_PROFILE_VP9_2, "2"},
  {MFX_PROFILE_VP9_3, "3"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_av1[] = {
  {MFX_PROFILE_AV1_MAIN, "main"},
  {MFX_PROFILE_AV1_HIGH, "high"},
  {MFX_PROFILE_AV1_PRO, "pro"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const Profile profs_jpeg[] = {
  {MFX_PROFILE_JPEG_BASELINE, "baseline"},
  {MFX_PROFILE_UNKNOWN, NULL}
};

static const struct CodecProfiles
{
  guint codec;
  const gchar *media_type;
  const Profile *profiles;
} codec_profs[] = {
  {MFX_CODEC_AVC, "video/x-h264", profs_avc},
  {MFX_CODEC_HEVC, "video/x-h265", profs_hevc},
  {MFX_CODEC_MPEG2, "video/mpeg", profs_mpeg2},
  {MFX_CODEC_VC1, "video/x-wmv", profs_vc1},
  {MFX_CODEC_VP8, "video/x-vp8", profs_vp8},
  {MFX_CODEC_VP9, "video/x-vp9", profs_vp9},
  {MFX_CODEC_AV1, "video/x-av1", profs_av1},
  {MFX_CODEC_JPEG, "image/jpeg", profs_jpeg}
};

typedef struct
{
  guint min_width;
  guint max_width;
  guint min_height;
  guint max_height;
} ResolutionRange;

typedef gboolean (*IsParamSupportedFunc) (MsdkSession * session,
    mfxVideoParam * in, mfxVideoParam * out);

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

static gboolean
_fill_frameinfo (GstVideoFormat format, mfxFrameInfo * frameinfo)
{
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  frameinfo->ChromaFormat = gst_msdk_get_mfx_chroma_from_format (format);
  frameinfo->FourCC = gst_msdk_get_mfx_fourcc_from_format (format);

  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_YV12:
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_VUYA:
      frameinfo->BitDepthLuma = 8;
      frameinfo->BitDepthChroma = 8;
      frameinfo->Shift = 0;
      break;
    case GST_VIDEO_FORMAT_BGR10A2_LE:
      frameinfo->BitDepthLuma = 10;
      frameinfo->BitDepthChroma = 10;
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
      frameinfo->BitDepthLuma = 10;
      frameinfo->BitDepthChroma = 10;
      frameinfo->Shift = 1;
      break;
#if (MFX_VERSION >= 1027)
    case GST_VIDEO_FORMAT_Y210:
      frameinfo->BitDepthLuma = 10;
      frameinfo->BitDepthChroma = 10;
      frameinfo->Shift = 1;
      break;
    case GST_VIDEO_FORMAT_Y410:
      frameinfo->BitDepthLuma = 10;
      frameinfo->BitDepthChroma = 10;
      frameinfo->Shift = 0;
      break;
#endif
#if (MFX_VERSION >= 1031)
    case GST_VIDEO_FORMAT_P012_LE:
    case GST_VIDEO_FORMAT_Y212_LE:
    case GST_VIDEO_FORMAT_Y412_LE:
      frameinfo->BitDepthLuma = 12;
      frameinfo->BitDepthChroma = 12;
      frameinfo->Shift = 1;
      break;
#endif
#if (MFX_VERSION >=2004)
    case GST_VIDEO_FORMAT_RGBP:
    case GST_VIDEO_FORMAT_BGRP:
      frameinfo->BitDepthLuma = 8;
      frameinfo->BitDepthChroma = 8;
      frameinfo->Shift = 0;
      break;
#endif
    default:
      GST_WARNING ("Unsupported format: %s",
          gst_video_format_to_string (format));
      return FALSE;
  }

  return TRUE;
}

static const gchar *
_profile_to_string (guint codec, mfxU32 profile)
{
  if (profile == MFX_PROFILE_UNKNOWN)
    return NULL;

  for (int c = 0; c < G_N_ELEMENTS (codec_profs); c++) {
    if (codec_profs[c].codec == codec) {
      const Profile *p = codec_profs[c].profiles;
      for (; p->id != MFX_PROFILE_UNKNOWN; p++) {
        if (p->id == profile)
          return p->names;
      }
    }
  }

  return NULL;
}

static const gchar *
_get_media_type (guint codec)
{
  for (int c = 0; c < G_N_ELEMENTS (codec_profs); c++) {
    if (codec_profs[c].codec == codec)
      return codec_profs[c].media_type;
  }

  return NULL;
}

static mfxU16
_get_main_codec_profile (guint codec)
{
  for (int c = 0; c < G_N_ELEMENTS (codec_profs); c++) {
    if (codec_profs[c].codec == codec) {
      return codec_profs[c].profiles->id;
    }
  }

  return MFX_PROFILE_UNKNOWN;
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

#if (MFX_VERSION >= 2000)

static void
_init_param (mfxVideoParam * param, guint codec_id, mfxU16 pattern)
{
  g_return_if_fail (param != NULL);

  memset (param, 0, sizeof (mfxVideoParam));
  param->mfx.CodecId = codec_id;
  param->IOPattern = pattern;
  param->mfx.FrameInfo.Width = default_width;
  param->mfx.FrameInfo.Height = default_height;
  param->mfx.FrameInfo.CropW = param->mfx.FrameInfo.Width;
  param->mfx.FrameInfo.CropH = param->mfx.FrameInfo.Height;
  param->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
  param->mfx.FrameInfo.FrameRateExtN = 30;
  param->mfx.FrameInfo.FrameRateExtD = 1;
  param->mfx.FrameInfo.AspectRatioW = 1;
  param->mfx.FrameInfo.AspectRatioH = 1;
  param->mfx.CodecProfile = _get_main_codec_profile (codec_id);
  _fill_frameinfo (DEFAULT_VIDEO_FORMAT, &param->mfx.FrameInfo);
}

static gboolean
_get_min_width (MsdkSession * session, mfxVideoParam * in,
    mfxVideoParam * out, IsParamSupportedFunc func,
    const mfxRange32U * width, guint * min_w)
{
  g_return_val_if_fail (func != NULL, FALSE);

  in->mfx.FrameInfo.Height = default_height;
  in->mfx.FrameInfo.CropH = default_height;
  out->mfx.FrameInfo.Height = in->mfx.FrameInfo.Height;
  out->mfx.FrameInfo.CropH = in->mfx.FrameInfo.CropH;

  for (guint w = width->Min; w < *min_w;) {
    in->mfx.FrameInfo.Width = w;
    in->mfx.FrameInfo.CropW = w;
    out->mfx.FrameInfo.Width = in->mfx.FrameInfo.Width;
    out->mfx.FrameInfo.CropW = in->mfx.FrameInfo.CropW;

    if (func (session, in, out)) {
      *min_w = in->mfx.FrameInfo.Width;
       return TRUE;
    } else {
      if (out->mfx.FrameInfo.Width != 0) {
        if (func (session, out, out)) {
          *min_w = out->mfx.FrameInfo.Width;
          return TRUE;
        }
      }
    }

    w += width->Step;
  }

  return FALSE;
}

static gboolean
_get_min_height (MsdkSession * session, mfxVideoParam * in,
    mfxVideoParam * out, IsParamSupportedFunc func,
    const mfxRange32U * height, guint * min_h)
{
  g_return_val_if_fail (func != NULL, FALSE);

  in->mfx.FrameInfo.Width = default_width;
  in->mfx.FrameInfo.CropW = default_width;
  out->mfx.FrameInfo.Width = in->mfx.FrameInfo.Width;
  out->mfx.FrameInfo.CropW = in->mfx.FrameInfo.CropW;

  for (guint h = height->Min; h < *min_h;) {
    in->mfx.FrameInfo.Height = h;
    in->mfx.FrameInfo.CropH = h;
    out->mfx.FrameInfo.Height = h;
    out->mfx.FrameInfo.CropH = h;

    if (func (session, in, out)) {
      *min_h = in->mfx.FrameInfo.Height;
      return TRUE;
    } else {
      if (out->mfx.FrameInfo.Height != 0) {
        if (func (session, out, out)) {
          *min_h = out->mfx.FrameInfo.Height;
          return TRUE;
        }
      }
    }

    h += height->Step;
  }

  return FALSE;
}

static gboolean
_get_max_width (MsdkSession * session, mfxVideoParam * in,
    mfxVideoParam * out, IsParamSupportedFunc func,
    const mfxRange32U * width, guint * max_w)
{
  g_return_val_if_fail (func != NULL, FALSE);

  in->mfx.FrameInfo.Height = default_height;
  in->mfx.FrameInfo.CropH = default_height;
  out->mfx.FrameInfo.Height = in->mfx.FrameInfo.Height;
  out->mfx.FrameInfo.CropH = in->mfx.FrameInfo.CropH;

  for (guint w = width->Max; w > *max_w;) {
    in->mfx.FrameInfo.Width = w;
    in->mfx.FrameInfo.CropW = w;
    out->mfx.FrameInfo.Width = in->mfx.FrameInfo.Width;
    out->mfx.FrameInfo.CropW = in->mfx.FrameInfo.CropW;

    if (func (session, in, out)) {
      *max_w = in->mfx.FrameInfo.Width;
       return TRUE;
    } else {
      if (out->mfx.FrameInfo.Width != 0) {
        if (func (session, out, out)) {
          *max_w = out->mfx.FrameInfo.Width;
          return TRUE;
        }
      }
    }

    w -= width->Step;
  }

  return FALSE;
}

static gboolean
_get_max_height (MsdkSession * session, mfxVideoParam * in,
    mfxVideoParam * out, IsParamSupportedFunc func,
    const mfxRange32U * height, guint * max_h)
{
  g_return_val_if_fail (func != NULL, FALSE);

  in->mfx.FrameInfo.Width = default_width;
  in->mfx.FrameInfo.CropW = default_width;
  out->mfx.FrameInfo.Width = in->mfx.FrameInfo.Width;
  out->mfx.FrameInfo.CropW = in->mfx.FrameInfo.CropW;

  for (guint h = height->Max; h > *max_h;) {
    in->mfx.FrameInfo.Height = h;
    in->mfx.FrameInfo.CropH = h;
    out->mfx.FrameInfo.Height = in->mfx.FrameInfo.Height;
    out->mfx.FrameInfo.CropH = in->mfx.FrameInfo.CropH;

    if (func (session, in, out)) {
      *max_h = in->mfx.FrameInfo.Height;
      return TRUE;
    } else {
      if (out->mfx.FrameInfo.Height != 0) {
        if (func (session, out, out)) {
          *max_h = out->mfx.FrameInfo.Height;
          return TRUE;
        }
      }
    }

    h -= height->Step;
  }

  return FALSE;
}

static gboolean
_get_supported_formats (MsdkSession * session,
    IsParamSupportedFunc func, guint codec_id,
    mfxU16 pattern, GValue * supported_fmts)
{
  GstVideoFormat fmt;
  const GValue *gfmt;
  const gchar *fmt_str = NULL;
  GValue fmts = G_VALUE_INIT;
  guint size;
  mfxVideoParam in, out;

  g_return_val_if_fail (func != NULL, FALSE);
  g_return_val_if_fail (supported_fmts != NULL, FALSE);

  _init_param (&in, codec_id, pattern);
  in.mfx.CodecProfile = MFX_PROFILE_UNKNOWN;
  out = in;

  g_value_init (&fmts, GST_TYPE_LIST);
  gst_msdk_get_video_format_list (&fmts);

  size = gst_value_list_get_size (&fmts);
  for (guint i = 0; i < size; i++) {
    gfmt = gst_value_list_get_value (&fmts, i);
    fmt_str = g_value_get_string (gfmt);
    fmt = gst_video_format_from_string (fmt_str);

    if (!_fill_frameinfo (fmt, &in.mfx.FrameInfo))
      continue;

    out.mfx.FrameInfo.BitDepthLuma = in.mfx.FrameInfo.BitDepthLuma;
    out.mfx.FrameInfo.BitDepthChroma = in.mfx.FrameInfo.BitDepthChroma;
    out.mfx.FrameInfo.Shift = in.mfx.FrameInfo.Shift;

    in.mfx.LowPower = MFX_CODINGOPTION_UNKNOWN;
    if (!func (session, &in, &out)) {
      in.mfx.LowPower = (out.mfx.LowPower == MFX_CODINGOPTION_ON) ?
          MFX_CODINGOPTION_OFF : MFX_CODINGOPTION_ON;

      if (!func (session, &in, &out))
        continue;
    }

    gst_value_list_append_value (supported_fmts, gfmt);
  }
  g_value_unset (&fmts);

  return gst_value_list_get_size (supported_fmts) == 0 ? FALSE : TRUE;
}

static gboolean
_enc_is_param_supported (MsdkSession * session,
    mfxVideoParam * in, mfxVideoParam * out)
{
  mfxStatus status = MFXVideoENCODE_Query (session->session, in, out);
  if (status == MFX_ERR_NONE)
    return TRUE;

  return FALSE;
}

static inline gint
_enc_get_codec_index (mfxEncoderDescription * enc_desc, guint codec_id)
{
  guint c;

  for (c = 0; c < enc_desc->NumCodecs; c++) {
    if (enc_desc->Codecs[c].CodecID == codec_id) {
      break;
    }
  }

  if (c >= enc_desc->NumCodecs) {
    GST_WARNING ("Unsupported codec: %"GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS(codec_id));
    return -1;
  }

  return c;
}

static gboolean
_enc_get_resolution_range (MsdkSession * session,
    mfxEncoderDescription * enc_desc, guint codec_id,
    ResolutionRange * res_range)
{
  mfxVideoParam in, out;
  guint c;
  mfxRange32U *width, *height;
  ResolutionRange res = { default_width, default_width,
      default_height , default_height };

  g_return_val_if_fail (res_range != NULL, FALSE);

  _init_param (&in, codec_id, MFX_IOPATTERN_IN_VIDEO_MEMORY);
  out = in;

  c = _enc_get_codec_index (enc_desc, codec_id);
  width = &enc_desc->Codecs[c].Profiles->MemDesc->Width;
  height = &enc_desc->Codecs[c].Profiles->MemDesc->Height;

  IsParamSupportedFunc func = _enc_is_param_supported;
  if (!_get_min_width (session, &in, &out, func, width, &res.min_width) ||
        !_get_max_width (session, &in, &out, func, width, &res.max_width) ||
        !_get_min_height (session, &in, &out, func, height, &res.min_height) ||
        !_get_max_height (session, &in, &out, func, height, &res.max_height))
    return FALSE;

  GST_DEBUG ("Got %"GST_FOURCC_FORMAT
     " supported resolution range width: [%d, %d], height: [%d, %d]",
     GST_FOURCC_ARGS (enc_desc->Codecs[c].CodecID),
     res.min_width, res.max_width, res.min_height, res.max_height);

  res_range->min_width = res.min_width;
  res_range->max_width = res.max_width;
  res_range->min_height = res.min_height;
  res_range->max_height = res.max_height;

  return TRUE;
}

static gboolean
_enc_get_profiles (MsdkSession * session,
    mfxEncoderDescription * enc_desc, guint codec_id, GValue * supported_profs)
{
  guint c;
  mfxVideoParam in, out;
  GstVideoFormat fmt = GST_VIDEO_FORMAT_UNKNOWN;
  const gchar *prof_str = NULL;
  gchar **profs = NULL;

  g_return_val_if_fail (supported_profs != NULL, FALSE);

  _init_param (&in, codec_id, MFX_IOPATTERN_IN_VIDEO_MEMORY);
  out = in;

  c = _enc_get_codec_index (enc_desc, codec_id);
  for (guint p = 0; p < enc_desc->Codecs[c].NumProfiles; p++) {
    in.mfx.CodecProfile = enc_desc->Codecs[c].Profiles[p].Profile;

    for (guint f = 0;
          f < enc_desc->Codecs[c].Profiles[p].MemDesc->NumColorFormats; f++) {
      fmt = gst_msdk_get_video_format_from_mfx_fourcc (
          enc_desc->Codecs[c].Profiles[p].MemDesc->ColorFormats[f]);

      if (!_fill_frameinfo (fmt, &in.mfx.FrameInfo))
        continue;

      out.mfx.FrameInfo.BitDepthLuma = in.mfx.FrameInfo.BitDepthLuma;
      out.mfx.FrameInfo.BitDepthChroma = in.mfx.FrameInfo.BitDepthChroma;
      out.mfx.FrameInfo.Shift = in.mfx.FrameInfo.Shift;

      in.mfx.LowPower = MFX_CODINGOPTION_UNKNOWN;
      if (!_enc_is_param_supported (session, &in, &out)) {
        in.mfx.LowPower = (out.mfx.LowPower == MFX_CODINGOPTION_ON) ?
            MFX_CODINGOPTION_OFF : MFX_CODINGOPTION_ON;

        if (!_enc_is_param_supported (session, &in, &out))
          continue;
      }

      prof_str = _profile_to_string (codec_id, in.mfx.CodecProfile);
      if (!prof_str)
        continue;

      profs = g_strsplit (prof_str, PROFILE_DELIMITER, 0);
      for (guint p = 0; profs[p]; p++)
        _list_append_string (supported_profs, profs[p]);
      break;
    }
  }

  return gst_value_list_get_size (supported_profs) == 0 ? FALSE : TRUE;
}

static GstCaps *
_enc_create_sink_caps (MsdkSession * session, guint codec_id,
    ResolutionRange * res)
{
  GstCaps *caps, *dma_caps, *va_caps;
  GValue supported_fmts = G_VALUE_INIT;

  g_value_init (&supported_fmts, GST_TYPE_LIST);
  if (!_get_supported_formats (session, _enc_is_param_supported,
        codec_id, MFX_IOPATTERN_IN_VIDEO_MEMORY, &supported_fmts)) {
    g_value_unset (&supported_fmts);
    GST_WARNING ("Failed to get supported formats for %"GST_FOURCC_FORMAT"ENC",
        GST_FOURCC_ARGS (codec_id));
    return NULL;
  }

  caps = gst_caps_from_string ("video/x-raw");
  gst_caps_set_value (caps, "format", &supported_fmts);

  dma_caps = gst_caps_from_string ("video/x-raw(memory:DMABuf)");
  gst_caps_set_value (dma_caps, "format", &supported_fmts);
  g_value_unset (&supported_fmts);
  gst_caps_append (caps, dma_caps);

  va_caps = gst_caps_from_string ("video/x-raw(memory:VAMemory),format=NV12");
  gst_caps_append (caps, va_caps);

  gst_caps_set_simple (caps,
      "width", GST_TYPE_INT_RANGE, res->min_width, res->max_width,
      "height", GST_TYPE_INT_RANGE, res->min_height, res->max_height,
      "interlace-mode", G_TYPE_STRING, "progressive", NULL);

  GST_DEBUG ("Create %"GST_FOURCC_FORMAT" sink_caps %" GST_PTR_FORMAT,
      GST_FOURCC_ARGS (codec_id), caps);

  return caps;
}

static GstCaps *
_enc_create_src_caps (MsdkSession * session,
    mfxEncoderDescription * enc_desc, guint codec_id,
    ResolutionRange * res)
{
  GstCaps *caps;
  const gchar *media_type = NULL;
  GValue supported_profs = G_VALUE_INIT;

  media_type = _get_media_type (codec_id);
  if (!media_type)
    goto failed;

  caps = gst_caps_new_empty_simple (media_type);

  g_value_init (&supported_profs, GST_TYPE_LIST);
  if (!_enc_get_profiles (session, enc_desc, codec_id, &supported_profs)) {
    g_value_unset (&supported_profs);
    goto failed;
  }

  gst_caps_set_value (caps, "profile", &supported_profs);
  g_value_unset (&supported_profs);

  gst_caps_set_simple (caps,
      "width", GST_TYPE_INT_RANGE, res->min_width, res->max_width,
      "height", GST_TYPE_INT_RANGE, res->min_height, res->max_height, NULL);

  GST_DEBUG ("Create %"GST_FOURCC_FORMAT" src_caps %" GST_PTR_FORMAT,
      GST_FOURCC_ARGS (codec_id), caps);

  return caps;

failed:
  GST_WARNING ("Failed to create src caps for %" GST_FOURCC_FORMAT"ENC",
      GST_FOURCC_ARGS (codec_id));
  return NULL;
}

gboolean
gst_msdkcaps_enc_create_caps (MsdkSession * session,
    gpointer enc_description, guint codec_id,
    GstCaps ** sink_caps, GstCaps ** src_caps)
{
  mfxEncoderDescription *enc_desc;
  GstCaps *in_caps, *out_caps;
  ResolutionRange res_range = { 1, G_MAXUINT, 1, G_MAXUINT };

  g_return_val_if_fail (session, FALSE);
  g_return_val_if_fail (enc_description, FALSE);

  enc_desc = (mfxEncoderDescription *)enc_description;

  if (_enc_get_codec_index (enc_desc, codec_id) < 0)
    goto failed;

  if (!_enc_get_resolution_range (session, enc_desc, codec_id, &res_range))
    goto failed;

  in_caps = _enc_create_sink_caps (session, codec_id, &res_range);
  if (!in_caps)
    goto failed;

  out_caps = _enc_create_src_caps (session, enc_desc, codec_id, &res_range);
  if (!out_caps)
    goto failed;

  *sink_caps = in_caps;
  *src_caps = out_caps;

  return TRUE;

failed:
  GST_WARNING ("Failed to create caps for %"GST_FOURCC_FORMAT"ENC",
      GST_FOURCC_ARGS (codec_id));
  return FALSE;
}

#else

static gboolean
_get_profiles (guint codec_id, GValue * supported_profs)
{
  const Profile *profiles = NULL;
  gchar **profs = NULL;

  g_return_val_if_fail (supported_profs != NULL, FALSE);

  for (guint c = 0; c < G_N_ELEMENTS (codec_profs); c++) {
    if (codec_profs[c].codec == codec_id) {
      profiles = codec_profs[c].profiles;
      break;
    }
  }
  if (!profiles)
    return FALSE;

  for (; profiles->id != MFX_PROFILE_UNKNOWN; profiles++) {
    profs = g_strsplit (profiles->names, PROFILE_DELIMITER, 0);
    for (guint p = 0; profs[p]; p++)
      _list_append_string (supported_profs, profs[p]);
  }

  return TRUE;
}

static const char *
_get_enc_raw_formats (guint codec_id)
{
  switch (codec_id) {
    case MFX_CODEC_AVC:
      return "NV12, I420, YV12, YUY2, UYVY, BGRA";
    case MFX_CODEC_HEVC:
      return "NV12, I420, YV12, YUY2, UYVY, BGRA, BGR10A2_LE, "
          "P010_10LE, VUYA, Y410, Y210, P012_LE";
    case MFX_CODEC_MPEG2:
     return  "NV12, I420, YV12, YUY2, UYVY, BGRA";
    case MFX_CODEC_VP9:
      return "NV12, I420, YV12, YUY2, UYVY, BGRA, P010_10LE, VUYA,Y410";
    case MFX_CODEC_AV1:
      return "NV12, P010_10LE";
    case MFX_CODEC_JPEG:
      return "NV12, I420, YV12, YUY2, UYVY, BGRA";
    default:
      break;
  }

  return "unsupported codec";
}

static const char *
_get_enc_dma_formats (guint codec_id)
{
  switch (codec_id) {
    case MFX_CODEC_AVC:
      return "NV12";
    case MFX_CODEC_HEVC:
      return "(string){ NV12, P010_10LE }";
    case MFX_CODEC_MPEG2:
     return  "NV12";
    case MFX_CODEC_VP9:
      return "(string){ NV12, P010_10LE }";
    case MFX_CODEC_AV1:
      return "(string){ NV12, P010_10LE }";
    case MFX_CODEC_JPEG:
      return "NV12";
    default:
      break;
  }

  return "unsupported codec";
}

gboolean
gst_msdkcaps_enc_create_caps (MsdkSession * session,
    gpointer enc_description, guint codec_id,
    GstCaps ** sink_caps, GstCaps ** src_caps)
{
  GstCaps *in_caps, *dma_caps, *va_caps;
  GstCaps *out_caps;
  gchar *raw_caps_str, *dma_caps_str;
  const gchar *media_type = NULL;
  GValue supported_profs = G_VALUE_INIT;

  raw_caps_str = g_strdup_printf ("video/x-raw, format=(string){ %s }",
      _get_enc_raw_formats (codec_id));
  in_caps = gst_caps_from_string (raw_caps_str);
  g_free (raw_caps_str);

  dma_caps_str = g_strdup_printf ("video/x-raw(memory:DMABuf), format=%s",
      _get_enc_dma_formats (codec_id));
  dma_caps = gst_caps_from_string (dma_caps_str);
  g_free (dma_caps_str);
  gst_caps_append (in_caps, dma_caps);

  va_caps = gst_caps_from_string ("video/x-raw(memory:VAMemory), "
      "format = (string)NV12");
  gst_caps_append (in_caps, va_caps);

  gst_caps_set_simple (in_caps,
      "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1,
      "interlace-mode", G_TYPE_STRING, "progressive", NULL);

  media_type = _get_media_type (codec_id);
  if (!media_type)
    goto failed;

  out_caps = gst_caps_new_empty_simple (media_type);

  g_value_init (&supported_profs, GST_TYPE_LIST);
  if (!_get_profiles (codec_id, &supported_profs)) {
    g_value_unset (&supported_profs);
    goto failed;
  }

  gst_caps_set_value (out_caps, "profile", &supported_profs);
  g_value_unset (&supported_profs);

  gst_caps_set_simple (out_caps,
      "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

  *sink_caps = in_caps;
  *src_caps = out_caps;

  return TRUE;

failed:
  GST_WARNING ("Failed to create caps for %"GST_FOURCC_FORMAT"ENC",
      GST_FOURCC_ARGS (codec_id));
  return FALSE;
}
#endif

