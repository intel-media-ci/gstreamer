/* GStreamer Intel MSDK plugin
 * Copyright (c) 2018, Intel Corporation
 *
 * Author: Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 *
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
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGDECE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * SECTION: element-msdkmpeg2dec
 * @title: msdkmpeg2dec
 * @short_description: Intel MSDK MPEG2 decoder
 *
 * MPEG2 video decoder based on Intel MFX
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 filesrc location=sample.mpeg2 ! mpegvideoparse ! msdkmpeg2dec ! glimagesink
 * ```
 *
 * Since: 1.14
 *
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstmsdkmpeg2dec.h"

GST_DEBUG_CATEGORY_EXTERN (gst_msdkmpeg2dec_debug);
#define GST_CAT_DEFAULT gst_msdkmpeg2dec_debug

#define RAW_FORMAT "NV12"

#ifdef _WIN32
#define SRC_CAPS_STR \
    GST_MSDK_CAPS_MAKE (RAW_FORMAT)
#else
#define SRC_CAPS_STR \
    GST_MSDK_CAPS_MAKE (RAW_FORMAT) "; " \
    GST_MSDK_CAPS_MAKE_WITH_VA_FEATURE ("NV12")
#endif

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpeg, "
        "width = (int) [ 1, MAX ], height = (int) [ 1, MAX ], "
        "mpegversion = (int) 2, " "systemstream = (boolean) false")
    );

#define gst_msdkmpeg2dec_parent_class parent_class
G_DEFINE_TYPE (GstMsdkMPEG2Dec, gst_msdkmpeg2dec, GST_TYPE_MSDKDEC);

static gboolean
gst_msdkmpeg2dec_configure (GstMsdkDec * decoder)
{
  GstMsdkMPEG2Dec *mpeg2dec = GST_MSDKMPEG2DEC (decoder);
  decoder->param.mfx.CodecId = MFX_CODEC_MPEG2;

  /* This is a deprecated attribute in msdk-2017 version, but some
   * customers still using this for low-latency streaming of non-b-frame
   * encoded streams */
  decoder->param.mfx.DecodedOrder = mpeg2dec->output_order;
  return TRUE;
}

static void
gst_msdkdec_mpeg2_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMsdkMPEG2Dec *thiz = GST_MSDKMPEG2DEC (object);
  GstState state;

  GST_OBJECT_LOCK (thiz);
  state = GST_STATE (thiz);

  if (!gst_msdkdec_prop_check_state (state, pspec)) {
    GST_WARNING_OBJECT (thiz, "setting property in wrong state");
    GST_OBJECT_UNLOCK (thiz);
    return;
  }
  switch (prop_id) {
    case GST_MSDKDEC_PROP_OUTPUT_ORDER:
      thiz->output_order = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
  return;
}

static void
gst_msdkdec_mpeg2_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMsdkMPEG2Dec *thiz = GST_MSDKMPEG2DEC (object);

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case GST_MSDKDEC_PROP_OUTPUT_ORDER:
      g_value_set_enum (value, thiz->output_order);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_msdkmpeg2dec_class_init (GstMsdkMPEG2DecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstMsdkDecClass *decoder_class;
  GstCaps *src_caps, *drm_caps;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_MSDKDEC_CLASS (klass);

  gobject_class->set_property = gst_msdkdec_mpeg2_set_property;
  gobject_class->get_property = gst_msdkdec_mpeg2_get_property;

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_msdkmpeg2dec_configure);

  gst_element_class_set_static_metadata (element_class,
      "Intel MSDK MPEG2 decoder",
      "Codec/Decoder/Video/Hardware",
      "MPEG2 video decoder based on " MFX_API_SDK,
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>");

  gst_msdkdec_prop_install_output_oder_property (gobject_class);

  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  src_caps = gst_caps_from_string (SRC_CAPS_STR);
  drm_caps = gst_msdkcaps_create_drm_caps (NULL,
      GST_MSDK_JOB_DECODER, RAW_FORMAT, 1, G_MAXINT, 1, G_MAXINT);
  gst_caps_append (src_caps, drm_caps);
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps));
}

static void
gst_msdkmpeg2dec_init (GstMsdkMPEG2Dec * thiz)
{
  thiz->output_order = PROP_OUTPUT_ORDER_DEFAULT;
}
