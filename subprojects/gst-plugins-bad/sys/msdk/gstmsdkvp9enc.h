/*
 * GStreamer Intel MSDK plugin
 * Copyright (c) 2019 Intel Corporation. All rights reserved.
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

#ifndef __GST_MSDKVP9ENC_H__
#define __GST_MSDKVP9ENC_H__

#include "gstmsdkenc.h"

G_BEGIN_DECLS

typedef struct _GstMsdkVP9Enc GstMsdkVP9Enc;
typedef struct _GstMsdkVP9EncClass GstMsdkVP9EncClass;

struct _GstMsdkVP9Enc
{
  GstMsdkEnc base;

  gint profile;

  mfxExtVP9Param ext_vp9;
};

struct _GstMsdkVP9EncClass
{
  GstMsdkEncClass parent_class;
};

gboolean
gst_msdkvp9enc_register (GstPlugin * plugin,
    GstMsdkContext * context, GstCaps * sink_caps,
    GstCaps * src_caps, guint rank);

G_END_DECLS

#endif /* __GST_MSDKVP9ENC_H__ */
