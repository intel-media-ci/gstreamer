/*
 * Copyright (C) 2017 Ericsson AB. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstnvdec.h"
#include "gstnvenc.h"
#include "gstnvav1dec.h"
#include "gstnvh264dec.h"
#include "gstnvh265dec.h"
#include "gstnvvp8dec.h"
#include "gstnvvp9dec.h"
#include "gstnvdecoder.h"
#include "gstcudamemorycopy.h"
#include "gstcudafilter.h"
#include <gst/cuda/gstcudamemory.h>
#ifdef HAVE_NVCODEC_NVMM
#include "gstcudanvmm.h"
#endif

#ifdef G_OS_WIN32
#include <gst/d3d11/gstd3d11.h>
#endif
#include "gstnvh264encoder.h"
#include "gstnvh265encoder.h"

GST_DEBUG_CATEGORY (gst_nvcodec_debug);
GST_DEBUG_CATEGORY (gst_nvdec_debug);
GST_DEBUG_CATEGORY (gst_nvenc_debug);
GST_DEBUG_CATEGORY (gst_nv_decoder_debug);

#ifdef HAVE_NVCODEC_NVMM
GST_DEBUG_CATEGORY (gst_cuda_nvmm_debug);
#endif

#define GST_CAT_DEFAULT gst_nvcodec_debug

static gboolean
plugin_init (GstPlugin * plugin)
{
  CUresult cuda_ret;
  const char *err_name = NULL, *err_desc = NULL;
  gint dev_count = 0;
  guint i;
  gboolean nvdec_available = TRUE;
  gboolean nvenc_available = TRUE;
  /* hardcoded minimum supported version */
  guint api_major_ver = 8;
  guint api_minor_ver = 1;
  GList *h264_enc_cdata = NULL;
  GList *h265_enc_cdata = NULL;

  GST_DEBUG_CATEGORY_INIT (gst_nvcodec_debug, "nvcodec", 0, "nvcodec");
  GST_DEBUG_CATEGORY_INIT (gst_nvdec_debug, "nvdec", 0, "nvdec");
  GST_DEBUG_CATEGORY_INIT (gst_nvenc_debug, "nvenc", 0, "nvenc");
  GST_DEBUG_CATEGORY_INIT (gst_nv_decoder_debug, "nvdecoder", 0, "nvdecoder");

#ifdef HAVE_NVCODEC_NVMM
  GST_DEBUG_CATEGORY_INIT (gst_cuda_nvmm_debug, "cudanvmm", 0, "cudanvmm");
#endif

  if (!gst_cuda_load_library ()) {
    GST_WARNING ("Failed to load cuda library");
    return TRUE;
  }

  /* get available API version from nvenc and it will be passed to
   * nvdec */
  if (!gst_nvenc_load_library (&api_major_ver, &api_minor_ver)) {
    GST_WARNING ("Failed to load nvenc library");
    nvenc_available = FALSE;
  }

  if (!gst_cuvid_load_library (api_major_ver, api_minor_ver)) {
    GST_WARNING ("Failed to load nvdec library version %u.%u", api_major_ver,
        api_minor_ver);
    nvdec_available = FALSE;
  }

  if (!nvdec_available && !nvenc_available)
    return TRUE;

  cuda_ret = CuInit (0);
  if (cuda_ret != CUDA_SUCCESS) {
    CuGetErrorName (cuda_ret, &err_name);
    CuGetErrorString (cuda_ret, &err_desc);
    GST_ERROR ("Failed to init cuda, cuInit ret: 0x%x: %s: %s",
        (int) cuda_ret, err_name, err_desc);
    return TRUE;
  }

  cuda_ret = CuDeviceGetCount (&dev_count);
  if (cuda_ret != CUDA_SUCCESS || !dev_count) {
    CuGetErrorName (cuda_ret, &err_name);
    CuGetErrorString (cuda_ret, &err_desc);
    GST_ERROR ("No available device, cuDeviceGetCount ret: 0x%x: %s %s",
        (int) cuda_ret, err_name, err_desc);
    return TRUE;
  }

  for (i = 0; i < dev_count; i++) {
    GstCudaContext *context = gst_cuda_context_new (i);
    CUcontext cuda_ctx;

    if (!context) {
      GST_WARNING ("Failed to create context for device %d", i);
      continue;
    }

    cuda_ctx = gst_cuda_context_get_handle (context);
    if (nvdec_available) {
      gint j;

      for (j = 0; j < cudaVideoCodec_NumCodecs; j++) {
        GstCaps *sink_template = NULL;
        GstCaps *src_template = NULL;
        cudaVideoCodec codec = (cudaVideoCodec) j;
        gboolean register_cuviddec = FALSE;

        if (gst_nv_decoder_check_device_caps (cuda_ctx,
                codec, &sink_template, &src_template)) {
          const gchar *codec_name = gst_cuda_video_codec_to_string (codec);

          GST_INFO ("CUDA video codec %s, sink template %" GST_PTR_FORMAT
              "src template %" GST_PTR_FORMAT, codec_name,
              sink_template, src_template);

          switch (codec) {
            case cudaVideoCodec_H264:
              /* higher than avdec_h264 */
              gst_nv_h264_dec_register (plugin,
                  i, GST_RANK_PRIMARY + 1, sink_template, src_template);
              break;
            case cudaVideoCodec_HEVC:
              /* higher than avdec_h265 */
              gst_nv_h265_dec_register (plugin,
                  i, GST_RANK_PRIMARY + 1, sink_template, src_template);
              break;
            case cudaVideoCodec_VP8:
              gst_nv_vp8_dec_register (plugin,
                  i, GST_RANK_PRIMARY, sink_template, src_template);
              break;
            case cudaVideoCodec_VP9:
              gst_nv_vp9_dec_register (plugin,
                  i, GST_RANK_PRIMARY, sink_template, src_template);
              break;
            case cudaVideoCodec_AV1:
              /* rust dav1ddec has "primary" rank */
              gst_nv_av1_dec_register (plugin, i, GST_RANK_PRIMARY + 1,
                  sink_template, src_template);
              break;
            default:
              register_cuviddec = TRUE;
              break;
          }

          if (register_cuviddec) {
            gst_nvdec_plugin_init (plugin,
                i, codec, codec_name, sink_template, src_template);
          }

          gst_caps_unref (sink_template);
          gst_caps_unref (src_template);
        }
      }
    }

    if (nvenc_available) {
      GstNvEncoderClassData *cdata;

#ifdef G_OS_WIN32
      if (g_win32_check_windows_version (6, 0, 0, G_WIN32_OS_ANY)) {
        gint64 adapter_luid;
        GstD3D11Device *d3d11_device;

        g_object_get (context, "dxgi-adapter-luid", &adapter_luid, NULL);
        d3d11_device = gst_d3d11_device_new_for_adapter_luid (adapter_luid,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT);
        if (!d3d11_device) {
          GST_WARNING ("Failed to d3d11 create device");
        } else {
          cdata = gst_nv_h264_encoder_register_d3d11 (plugin,
              d3d11_device, GST_RANK_NONE);
          if (cdata)
            h264_enc_cdata = g_list_append (h264_enc_cdata, cdata);

          cdata = gst_nv_h265_encoder_register_d3d11 (plugin,
              d3d11_device, GST_RANK_NONE);
          if (cdata)
            h265_enc_cdata = g_list_append (h265_enc_cdata, cdata);

          gst_object_unref (d3d11_device);
        }
      }
#endif
      cdata =
          gst_nv_h264_encoder_register_cuda (plugin, context, GST_RANK_NONE);
      if (cdata)
        h264_enc_cdata = g_list_append (h264_enc_cdata, cdata);

      cdata =
          gst_nv_h265_encoder_register_cuda (plugin, context, GST_RANK_NONE);
      if (cdata)
        h265_enc_cdata = g_list_append (h265_enc_cdata, cdata);

      gst_nvenc_plugin_init (plugin, i, cuda_ctx);
    }

    gst_object_unref (context);
  }

  if (h264_enc_cdata) {
    gst_nv_h264_encoder_register_auto_select (plugin, h264_enc_cdata,
        GST_RANK_NONE);
  }
  if (h265_enc_cdata) {
    gst_nv_h265_encoder_register_auto_select (plugin, h265_enc_cdata,
        GST_RANK_NONE);
  }

  gst_cuda_memory_copy_register (plugin, GST_RANK_NONE);

  gst_cuda_filter_plugin_init (plugin);
  gst_cuda_memory_init_once ();

#ifdef HAVE_NVCODEC_NVMM
  if (gst_cuda_nvmm_init_once ()) {
    GST_INFO ("Enable NVMM support");
  }
#endif

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, nvcodec,
    "GStreamer NVCODEC plugin", plugin_init, VERSION, "LGPL",
    GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
