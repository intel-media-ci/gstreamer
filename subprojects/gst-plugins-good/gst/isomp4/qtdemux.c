/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2003> David A. Schleef <ds@schleef.org>
 * Copyright (C) <2006> Wim Taymans <wim@fluendo.com>
 * Copyright (C) <2007> Julien Moutte <julien@fluendo.com>
 * Copyright (C) <2009> Tim-Philipp Müller <tim centricular net>
 * Copyright (C) <2009> STEricsson <benjamin.gaignard@stericsson.com>
 * Copyright (C) <2013> Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 * Copyright (C) <2013> Intel Corporation
 * Copyright (C) <2014> Centricular Ltd
 * Copyright (C) <2015> YouView TV Ltd.
 * Copyright (C) <2016> British Broadcasting Corporation
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

/**
 * SECTION:element-qtdemux
 * @title: qtdemux
 *
 * Demuxes a .mov file into raw or compressed audio and/or video streams.
 *
 * This element supports both push and pull-based scheduling, depending on the
 * capabilities of the upstream elements.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 filesrc location=test.mov ! qtdemux name=demux  demux.audio_0 ! queue ! decodebin ! audioconvert ! audioresample ! autoaudiosink   demux.video_0 ! queue ! decodebin ! videoconvert ! videoscale ! autovideosink
 * ]| Play (parse and decode) a .mov file and try to output it to
 * an automatically detected soundcard and videosink. If the MOV file contains
 * compressed audio or video data, this will only work if you have the
 * right decoder elements/plugins installed.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib/gi18n-lib.h>

#include <glib/gprintf.h>
#include <gst/base/base.h>
#include <gst/tag/tag.h>
#include <gst/audio/audio.h>
#include <gst/riff/riff.h>
#include <gst/pbutils/pbutils.h>

#include "gstisomp4elements.h"
#include "qtatomparser.h"
#include "qtdemux_types.h"
#include "qtdemux_dump.h"
#include "fourcc.h"
#include "descriptors.h"
#include "qtdemux_lang.h"
#include "qtdemux.h"
#include "qtpalette.h"
#include "qtdemux_tags.h"
#include "qtdemux_tree.h"
#include "qtdemux-webvtt.h"

#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <gst/math-compat.h>

#ifdef HAVE_ZLIB
# include <zlib.h>
#endif

/* max. size considered 'sane' for non-mdat atoms */
#define QTDEMUX_MAX_ATOM_SIZE (32*1024*1024)

/* if the sample index is larger than this, something is likely wrong */
#define QTDEMUX_MAX_SAMPLE_INDEX_SIZE (200*1024*1024)

/* For converting qt creation times to unix epoch times */
#define QTDEMUX_SECONDS_PER_DAY (60 * 60 * 24)
#define QTDEMUX_LEAP_YEARS_FROM_1904_TO_1970 17
#define QTDEMUX_SECONDS_FROM_1904_TO_1970 (((1970 - 1904) * (guint64) 365 + \
    QTDEMUX_LEAP_YEARS_FROM_1904_TO_1970) * QTDEMUX_SECONDS_PER_DAY)

#define QTDEMUX_TREE_NODE_FOURCC(n) (QT_FOURCC(((guint8 *) (n)->data) + 4))

#define STREAM_IS_EOS(s) ((s)->time_position == GST_CLOCK_TIME_NONE)

#define ABSDIFF(x, y) ( (x) > (y) ? ((x) - (y)) : ((y) - (x)) )

#define QTDEMUX_STREAM(s) ((QtDemuxStream *)(s))
#define QTDEMUX_N_STREAMS(demux) ((demux)->active_streams->len)
#define QTDEMUX_NTH_STREAM(demux,idx) \
   QTDEMUX_STREAM(g_ptr_array_index((demux)->active_streams,idx))
#define QTDEMUX_NTH_OLD_STREAM(demux,idx) \
   QTDEMUX_STREAM(g_ptr_array_index((demux)->old_streams,idx))

#define CUR_STREAM(s) (&((s)->stsd_entries[(s)->cur_stsd_entry_index]))

GST_DEBUG_CATEGORY (qtdemux_debug);
#define GST_CAT_DEFAULT qtdemux_debug

typedef struct _QtDemuxCencSampleSetInfo QtDemuxCencSampleSetInfo;
typedef struct _QtDemuxAavdEncryptionInfo QtDemuxAavdEncryptionInfo;

/* Macros for converting to/from timescale */
#define QTSTREAMTIME_TO_GSTTIME(stream, value) (gst_util_uint64_scale((value), GST_SECOND, (stream)->timescale))
#define GSTTIME_TO_QTSTREAMTIME(stream, value) (gst_util_uint64_scale((value), (stream)->timescale, GST_SECOND))

#define QTTIME_TO_GSTTIME(qtdemux, value) (gst_util_uint64_scale((value), GST_SECOND, (qtdemux)->timescale))
#define GSTTIME_TO_QTTIME(qtdemux, value) (gst_util_uint64_scale((value), (qtdemux)->timescale, GST_SECOND))

/* timestamp is the DTS */
#define QTSAMPLE_DTS(stream,sample) (QTSTREAMTIME_TO_GSTTIME((stream), (sample)->timestamp))
/* timestamp + offset + cslg_shift is the outgoing PTS */
#define QTSAMPLE_PTS(stream,sample) (QTSTREAMTIME_TO_GSTTIME((stream), (sample)->timestamp + (stream)->cslg_shift + (sample)->pts_offset))
/* timestamp + offset is the PTS used for internal seek calculations */
#define QTSAMPLE_PTS_NO_CSLG(stream,sample) (QTSTREAMTIME_TO_GSTTIME((stream), (sample)->timestamp + (sample)->pts_offset))
/* timestamp + duration - dts is the duration */
#define QTSAMPLE_DUR_DTS(stream, sample, dts) (QTSTREAMTIME_TO_GSTTIME ((stream), (sample)->timestamp + (sample)->duration) - (dts))

#define QTSAMPLE_KEYFRAME(stream,sample) ((stream)->all_keyframe || (sample)->keyframe)

#define QTDEMUX_EXPOSE_GET_LOCK(demux) (&((demux)->expose_lock))
#define QTDEMUX_EXPOSE_LOCK(demux) G_STMT_START { \
    GST_TRACE("Locking from thread %p", g_thread_self()); \
    g_mutex_lock (QTDEMUX_EXPOSE_GET_LOCK (demux)); \
    GST_TRACE("Locked from thread %p", g_thread_self()); \
 } G_STMT_END

#define QTDEMUX_EXPOSE_UNLOCK(demux) G_STMT_START { \
    GST_TRACE("Unlocking from thread %p", g_thread_self()); \
    g_mutex_unlock (QTDEMUX_EXPOSE_GET_LOCK (demux)); \
 } G_STMT_END

/*
 * Quicktime has tracks and segments. A track is a continuous piece of
 * multimedia content. The track is not always played from start to finish but
 * instead, pieces of the track are 'cut out' and played in sequence. This is
 * what the segments do.
 *
 * Inside the track we have keyframes (K) and delta frames. The track has its
 * own timing, which starts from 0 and extends to end. The position in the track
 * is called the media_time.
 *
 * The segments now describe the pieces that should be played from this track
 * and are basically tuples of media_time/duration/rate entries. We can have
 * multiple segments and they are all played after one another. An example:
 *
 * segment 1: media_time: 1 second, duration: 1 second, rate 1
 * segment 2: media_time: 3 second, duration: 2 second, rate 2
 *
 * To correctly play back this track, one must play: 1 second of media starting
 * from media_time 1 followed by 2 seconds of media starting from media_time 3
 * at a rate of 2.
 *
 * Each of the segments will be played at a specific time, the first segment at
 * time 0, the second one after the duration of the first one, etc.. Note that
 * the time in resulting playback is not identical to the media_time of the
 * track anymore.
 *
 * Visually, assuming the track has 4 second of media_time:
 *
 *                (a)                   (b)          (c)              (d)
 *         .-----------------------------------------------------------.
 * track:  | K.....K.........K........K.......K.......K...........K... |
 *         '-----------------------------------------------------------'
 *         0              1              2              3              4
 *           .------------^              ^   .----------^              ^
 *          /              .-------------'  /       .------------------'
 *         /              /          .-----'       /
 *         .--------------.         .--------------.
 *         | segment 1    |         | segment 2    |
 *         '--------------'         '--------------'
 *
 * The challenge here is to cut out the right pieces of the track for each of
 * the playback segments. This fortunately can easily be done with the SEGMENT
 * events of GStreamer.
 *
 * For playback of segment 1, we need to provide the decoder with the keyframe
 * (a), in the above figure, but we must instruct it only to output the decoded
 * data between second 1 and 2. We do this with a SEGMENT event for 1 to 2, time
 * position set to the time of the segment: 0.
 *
 * We then proceed to push data from keyframe (a) to frame (b). The decoder
 * decodes but clips all before media_time 1.
 *
 * After finishing a segment, we push out a new SEGMENT event with the clipping
 * boundaries of the new data.
 *
 * This is a good usecase for the GStreamer accumulated SEGMENT events.
 */

struct _QtDemuxSegment
{
  /* global time and duration, all gst time */
  GstClockTime time;
  GstClockTime stop_time;
  GstClockTime duration;
  /* media time of trak, all gst time */
  GstClockTime media_start;
  GstClockTime media_stop;
  gdouble rate;
  /* Media start time in trak timescale units */
  guint32 trak_media_start;
};

#define QTSEGMENT_IS_EMPTY(s) ((s)->media_start == GST_CLOCK_TIME_NONE)

/* Used with fragmented MP4 files (mfra atom) */
struct _QtDemuxRandomAccessEntry
{
  GstClockTime ts;
  guint64 moof_offset;
};


/* Contains properties and cryptographic info for a set of samples from a
 * track protected using Common Encryption (cenc) */
struct _QtDemuxCencSampleSetInfo
{
  GstStructure *default_properties;

  /* sample groups */
  GPtrArray *track_group_properties;
  GPtrArray *fragment_group_properties;
  GPtrArray *sample_to_group_map;

  /* @crypto_info holds one GstStructure per sample */
  GPtrArray *crypto_info;
};

struct _QtDemuxAavdEncryptionInfo
{
  GstStructure *default_properties;
};

static const gchar *
qt_demux_state_string (enum QtDemuxState state)
{
  switch (state) {
    case QTDEMUX_STATE_INITIAL:
      return "<INITIAL>";
    case QTDEMUX_STATE_HEADER:
      return "<HEADER>";
    case QTDEMUX_STATE_MOVIE:
      return "<MOVIE>";
    case QTDEMUX_STATE_BUFFER_MDAT:
      return "<BUFFER_MDAT>";
    default:
      return "<UNKNOWN>";
  }
}

static GstFlowReturn qtdemux_add_fragmented_samples (GstQTDemux * qtdemux);

static void gst_qtdemux_check_send_pending_segment (GstQTDemux * demux);

static GstStaticPadTemplate gst_qtdemux_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/quicktime; video/mj2; audio/x-m4a; "
        "application/x-3gp")
    );

static GstStaticPadTemplate gst_qtdemux_videosrc_template =
GST_STATIC_PAD_TEMPLATE ("video_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_qtdemux_audiosrc_template =
GST_STATIC_PAD_TEMPLATE ("audio_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_qtdemux_subsrc_template =
GST_STATIC_PAD_TEMPLATE ("subtitle_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_qtdemux_metasrc_template =
GST_STATIC_PAD_TEMPLATE ("meta_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

#define gst_qtdemux_parent_class parent_class
G_DEFINE_TYPE (GstQTDemux, gst_qtdemux, GST_TYPE_ELEMENT);
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (qtdemux, "qtdemux",
    GST_RANK_PRIMARY, GST_TYPE_QTDEMUX, isomp4_element_init (plugin));

static void gst_qtdemux_dispose (GObject * object);
static void gst_qtdemux_finalize (GObject * object);

static guint32
gst_qtdemux_find_index_linear (GstQTDemux * qtdemux, QtDemuxStream * str,
    GstClockTime media_time);
static guint32
gst_qtdemux_find_index_for_given_media_offset_linear (GstQTDemux * qtdemux,
    QtDemuxStream * str, gint64 media_offset);

#if 0
static void gst_qtdemux_set_index (GstElement * element, GstIndex * index);
static GstIndex *gst_qtdemux_get_index (GstElement * element);
#endif
static GstStateChangeReturn gst_qtdemux_change_state (GstElement * element,
    GstStateChange transition);
static void gst_qtdemux_set_context (GstElement * element,
    GstContext * context);
static gboolean qtdemux_sink_activate (GstPad * sinkpad, GstObject * parent);
static gboolean qtdemux_sink_activate_mode (GstPad * sinkpad,
    GstObject * parent, GstPadMode mode, gboolean active);

static void gst_qtdemux_loop (GstPad * pad);
static GstFlowReturn gst_qtdemux_chain (GstPad * sinkpad, GstObject * parent,
    GstBuffer * inbuf);
static gboolean gst_qtdemux_handle_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_qtdemux_handle_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_qtdemux_setcaps (GstQTDemux * qtdemux, GstCaps * caps);
static gboolean gst_qtdemux_configure_stream (GstQTDemux * qtdemux,
    QtDemuxStream * stream);
static void gst_qtdemux_stream_check_and_change_stsd_index (GstQTDemux * demux,
    QtDemuxStream * stream);
static GstFlowReturn gst_qtdemux_process_adapter (GstQTDemux * demux,
    gboolean force);

static void gst_qtdemux_check_seekability (GstQTDemux * demux);

static gboolean qtdemux_parse_moov (GstQTDemux * qtdemux,
    const guint8 * buffer, guint length);
static gboolean qtdemux_parse_node (GstQTDemux * qtdemux, GNode * node,
    const guint8 * buffer, guint length);
static gboolean qtdemux_parse_tree (GstQTDemux * qtdemux);

static void gst_qtdemux_handle_esds (GstQTDemux * qtdemux,
    QtDemuxStream * stream, QtDemuxStreamStsdEntry * entry, GNode * esds,
    GstTagList * list);
static GstCaps *qtdemux_video_caps (GstQTDemux * qtdemux,
    QtDemuxStream * stream, QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    GNode * stsd_entry, gchar ** codec_name);
static GstCaps *qtdemux_audio_caps (GstQTDemux * qtdemux,
    QtDemuxStream * stream, QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    guint8 stsd_version, guint32 version, GNode * stsd_entry,
    gchar ** codec_name);
static GstCaps *qtdemux_sub_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc, GNode * stsd_entry,
    gchar ** codec_name);
static GstCaps *qtdemux_meta_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc, GNode * stsd_entry,
    gchar ** codec_name);
static GstCaps *qtdemux_generic_caps (GstQTDemux * qtdemux,
    QtDemuxStream * stream, QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    GNode * stsd_entry, gchar ** codec_name);

static gboolean qtdemux_parse_samples (GstQTDemux * qtdemux,
    QtDemuxStream * stream, guint32 n);
static GstFlowReturn qtdemux_expose_streams (GstQTDemux * qtdemux);
static QtDemuxStream *gst_qtdemux_stream_ref (QtDemuxStream * stream);
static void gst_qtdemux_stream_unref (QtDemuxStream * stream);
static void gst_qtdemux_stream_clear (QtDemuxStream * stream);
static GstFlowReturn qtdemux_prepare_streams (GstQTDemux * qtdemux);
static void qtdemux_do_allocation (QtDemuxStream * stream,
    GstQTDemux * qtdemux);
static gboolean gst_qtdemux_activate_segment (GstQTDemux * qtdemux,
    QtDemuxStream * stream, guint32 seg_idx, GstClockTime offset);
static gboolean gst_qtdemux_stream_update_segment (GstQTDemux * qtdemux,
    QtDemuxStream * stream, gint seg_idx, GstClockTime offset,
    GstClockTime * _start, GstClockTime * _stop);
static void gst_qtdemux_send_gap_for_segment (GstQTDemux * demux,
    QtDemuxStream * stream, gint segment_index, GstClockTime pos);

static void qtdemux_check_if_is_gapless_audio (GstQTDemux * qtdemux);

static gboolean qtdemux_pull_mfro_mfra (GstQTDemux * qtdemux);
static void check_update_duration (GstQTDemux * qtdemux, GstClockTime duration);

static gchar *qtdemux_uuid_bytes_to_string (gconstpointer uuid_bytes);

static GstStructure *qtdemux_get_cenc_sample_properties (GstQTDemux * qtdemux,
    QtDemuxStream * stream, guint sample_index);
static void gst_qtdemux_append_protection_system_id (GstQTDemux * qtdemux,
    const gchar * id);
static void qtdemux_gst_structure_free (GstStructure * gststructure);
static void gst_qtdemux_reset (GstQTDemux * qtdemux, gboolean hard);
static void qtdemux_clear_protection_events_on_all_streams (GstQTDemux *
    qtdemux);

static void
gst_qtdemux_class_init (GstQTDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->dispose = gst_qtdemux_dispose;
  gobject_class->finalize = gst_qtdemux_finalize;

  gstelement_class->change_state = GST_DEBUG_FUNCPTR (gst_qtdemux_change_state);
#if 0
  gstelement_class->set_index = GST_DEBUG_FUNCPTR (gst_qtdemux_set_index);
  gstelement_class->get_index = GST_DEBUG_FUNCPTR (gst_qtdemux_get_index);
#endif
  gstelement_class->set_context = GST_DEBUG_FUNCPTR (gst_qtdemux_set_context);

  gst_tag_register_musicbrainz_tags ();

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_qtdemux_sink_template);
  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_qtdemux_videosrc_template);
  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_qtdemux_audiosrc_template);
  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_qtdemux_subsrc_template);
  gst_element_class_set_static_metadata (gstelement_class, "QuickTime demuxer",
      "Codec/Demuxer",
      "Demultiplex a QuickTime file into audio and video streams",
      "David Schleef <ds@schleef.org>, Wim Taymans <wim@fluendo.com>");

  GST_DEBUG_CATEGORY_INIT (qtdemux_debug, "qtdemux", 0, "qtdemux plugin");
  gst_riff_init ();
}

static void
gst_qtdemux_init (GstQTDemux * qtdemux)
{
  qtdemux->sinkpad =
      gst_pad_new_from_static_template (&gst_qtdemux_sink_template, "sink");
  gst_pad_set_activate_function (qtdemux->sinkpad, qtdemux_sink_activate);
  gst_pad_set_activatemode_function (qtdemux->sinkpad,
      qtdemux_sink_activate_mode);
  gst_pad_set_chain_function (qtdemux->sinkpad, gst_qtdemux_chain);
  gst_pad_set_event_function (qtdemux->sinkpad, gst_qtdemux_handle_sink_event);
  gst_pad_set_query_function (qtdemux->sinkpad, gst_qtdemux_handle_sink_query);
  gst_element_add_pad (GST_ELEMENT_CAST (qtdemux), qtdemux->sinkpad);

  qtdemux->adapter = gst_adapter_new ();
  g_queue_init (&qtdemux->protection_event_queue);
  qtdemux->flowcombiner = gst_flow_combiner_new ();
  g_mutex_init (&qtdemux->expose_lock);

  qtdemux->active_streams = g_ptr_array_new_with_free_func
      ((GDestroyNotify) gst_qtdemux_stream_unref);
  qtdemux->old_streams = g_ptr_array_new_with_free_func
      ((GDestroyNotify) gst_qtdemux_stream_unref);

  GST_OBJECT_FLAG_SET (qtdemux, GST_ELEMENT_FLAG_INDEXABLE);

  gst_qtdemux_reset (qtdemux, TRUE);
}

static void
gst_qtdemux_finalize (GObject * object)
{
  GstQTDemux *qtdemux = GST_QTDEMUX (object);

  g_free (qtdemux->redirect_location);
  g_free (qtdemux->cenc_aux_info_sizes);
  g_mutex_clear (&qtdemux->expose_lock);

  g_ptr_array_free (qtdemux->active_streams, TRUE);
  g_ptr_array_free (qtdemux->old_streams, TRUE);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_qtdemux_dispose (GObject * object)
{
  GstQTDemux *qtdemux = GST_QTDEMUX (object);

  g_clear_object (&qtdemux->adapter);
  gst_clear_tag_list (&qtdemux->tag_list);
  g_clear_pointer (&qtdemux->flowcombiner, gst_flow_combiner_unref);

  g_queue_clear_full (&qtdemux->protection_event_queue,
      (GDestroyNotify) gst_event_unref);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_qtdemux_post_no_playable_stream_error (GstQTDemux * qtdemux)
{
  if (qtdemux->redirect_location) {
    GST_ELEMENT_ERROR_WITH_DETAILS (qtdemux, STREAM, DEMUX,
        (_("This file contains no playable streams.")),
        ("no known streams found, a redirect message has been posted"),
        ("redirect-location", G_TYPE_STRING, qtdemux->redirect_location, NULL));
  } else {
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file contains no playable streams.")),
        ("no known streams found"));
  }
}

static GstBuffer *
_gst_buffer_new_wrapped (gpointer mem, gsize size, GFreeFunc free_func)
{
  return gst_buffer_new_wrapped_full (free_func ? 0 : GST_MEMORY_FLAG_READONLY,
      mem, size, 0, size, mem, free_func);
}

static GstFlowReturn
gst_qtdemux_pull_atom (GstQTDemux * qtdemux, guint64 offset, guint64 size,
    GstBuffer ** buf)
{
  GstFlowReturn flow;
  GstMapInfo map;
  gsize bsize;

  if (G_UNLIKELY (size == 0)) {
    GstFlowReturn ret;
    GstBuffer *tmp = NULL;

    ret = gst_qtdemux_pull_atom (qtdemux, offset, sizeof (guint32), &tmp);
    if (ret != GST_FLOW_OK)
      return ret;

    gst_buffer_map (tmp, &map, GST_MAP_READ);
    size = QT_UINT32 (map.data);
    GST_DEBUG_OBJECT (qtdemux, "size 0x%08" G_GINT64_MODIFIER "x", size);

    gst_buffer_unmap (tmp, &map);
    gst_buffer_unref (tmp);
  }

  /* Sanity check: catch bogus sizes (fuzzed/broken files) */
  if (G_UNLIKELY (size > QTDEMUX_MAX_ATOM_SIZE)) {
    if (qtdemux->state != QTDEMUX_STATE_MOVIE && qtdemux->got_moov) {
      /* we're pulling header but already got most interesting bits,
       * so never mind the rest (e.g. tags) (that much) */
      GST_WARNING_OBJECT (qtdemux, "atom has bogus size %" G_GUINT64_FORMAT,
          size);
      return GST_FLOW_EOS;
    } else {
      GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
          (_("This file is invalid and cannot be played.")),
          ("atom has bogus size %" G_GUINT64_FORMAT, size));
      return GST_FLOW_ERROR;
    }
  }

  flow = gst_pad_pull_range (qtdemux->sinkpad, offset, size, buf);

  if (G_UNLIKELY (flow != GST_FLOW_OK))
    return flow;

  bsize = gst_buffer_get_size (*buf);
  /* Catch short reads - we don't want any partial atoms */
  if (G_UNLIKELY (bsize < size)) {
    GST_WARNING_OBJECT (qtdemux,
        "short read: %" G_GSIZE_FORMAT " < %" G_GUINT64_FORMAT, bsize, size);
    gst_buffer_unref (*buf);
    *buf = NULL;
    return GST_FLOW_EOS;
  }

  return flow;
}

#if 1
static gboolean
gst_qtdemux_src_convert (GstQTDemux * qtdemux, GstPad * pad,
    GstFormat src_format, gint64 src_value, GstFormat dest_format,
    gint64 * dest_value)
{
  gboolean res = TRUE;
  QtDemuxStream *stream = gst_pad_get_element_private (pad);
  gint32 index;

  if (stream->subtype != FOURCC_vide) {
    res = FALSE;
    goto done;
  }

  switch (src_format) {
    case GST_FORMAT_TIME:
      switch (dest_format) {
        case GST_FORMAT_BYTES:{
          index = gst_qtdemux_find_index_linear (qtdemux, stream, src_value);
          if (-1 == index) {
            res = FALSE;
            goto done;
          }

          *dest_value = stream->samples[index].offset;

          GST_DEBUG_OBJECT (qtdemux, "Format Conversion Time->Offset :%"
              GST_TIME_FORMAT "->%" G_GUINT64_FORMAT,
              GST_TIME_ARGS (src_value), *dest_value);
          break;
        }
        default:
          res = FALSE;
          break;
      }
      break;
    case GST_FORMAT_BYTES:
      switch (dest_format) {
        case GST_FORMAT_TIME:{
          index =
              gst_qtdemux_find_index_for_given_media_offset_linear (qtdemux,
              stream, src_value);

          if (-1 == index) {
            res = FALSE;
            goto done;
          }

          *dest_value =
              QTSTREAMTIME_TO_GSTTIME (stream,
              stream->samples[index].timestamp);
          GST_DEBUG_OBJECT (qtdemux,
              "Format Conversion Offset->Time :%" G_GUINT64_FORMAT "->%"
              GST_TIME_FORMAT, src_value, GST_TIME_ARGS (*dest_value));
          break;
        }
        default:
          res = FALSE;
          break;
      }
      break;
    default:
      res = FALSE;
      break;
  }

done:
  return res;
}
#endif

static gboolean
gst_qtdemux_get_duration (GstQTDemux * qtdemux, GstClockTime * duration)
{
  gboolean res = FALSE;

  *duration = GST_CLOCK_TIME_NONE;

  if (qtdemux->duration != 0 &&
      qtdemux->duration != G_MAXINT64 && qtdemux->timescale != 0) {
    /* If this is single-stream audio media with gapless data,
     * report the duration of the valid subset of the overall data. */
    if (qtdemux->gapless_audio_info.type != GAPLESS_AUDIO_INFO_TYPE_NONE)
      *duration = qtdemux->gapless_audio_info.valid_duration;
    else
      *duration = QTTIME_TO_GSTTIME (qtdemux, qtdemux->duration);
    res = TRUE;
  } else {
    *duration = GST_CLOCK_TIME_NONE;
  }

  return res;
}

static gboolean
gst_qtdemux_handle_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  gboolean res = FALSE;
  GstQTDemux *qtdemux = GST_QTDEMUX (parent);

  GST_LOG_OBJECT (pad, "%s query", GST_QUERY_TYPE_NAME (query));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:{
      GstFormat fmt;

      gst_query_parse_position (query, &fmt, NULL);
      if (fmt == GST_FORMAT_TIME
          && GST_CLOCK_TIME_IS_VALID (qtdemux->segment.position)) {
        gst_query_set_position (query, GST_FORMAT_TIME,
            qtdemux->segment.position);
        res = TRUE;
      }
    }
      break;
    case GST_QUERY_DURATION:{
      GstFormat fmt;

      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt == GST_FORMAT_TIME) {
        /* First try to query upstream */
        res = gst_pad_query_default (pad, parent, query);
        if (!res) {
          GstClockTime duration;
          if (gst_qtdemux_get_duration (qtdemux, &duration) && duration > 0) {
            gst_query_set_duration (query, GST_FORMAT_TIME, duration);
            res = TRUE;
          }
        }
      }
      break;
    }
    case GST_QUERY_CONVERT:{
      GstFormat src_fmt, dest_fmt;
      gint64 src_value, dest_value = 0;

      gst_query_parse_convert (query, &src_fmt, &src_value, &dest_fmt, NULL);

      res = gst_qtdemux_src_convert (qtdemux, pad,
          src_fmt, src_value, dest_fmt, &dest_value);
      if (res)
        gst_query_set_convert (query, src_fmt, src_value, dest_fmt, dest_value);

      break;
    }
    case GST_QUERY_FORMATS:
      gst_query_set_formats (query, 2, GST_FORMAT_TIME, GST_FORMAT_BYTES);
      res = TRUE;
      break;
    case GST_QUERY_SEEKING:{
      GstFormat fmt;
      gboolean seekable;

      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);

      if (fmt == GST_FORMAT_BYTES) {
        /* We always refuse BYTES seeks from downstream */
        break;
      }

      /* try upstream first */
      res = gst_pad_query_default (pad, parent, query);

      if (!res) {
        gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
        if (fmt == GST_FORMAT_TIME) {
          GstClockTime duration;

          gst_qtdemux_get_duration (qtdemux, &duration);
          seekable = TRUE;
          if (!qtdemux->pullbased) {
            GstQuery *q;

            /* we might be able with help from upstream */
            seekable = FALSE;
            q = gst_query_new_seeking (GST_FORMAT_BYTES);
            if (gst_pad_peer_query (qtdemux->sinkpad, q)) {
              gst_query_parse_seeking (q, &fmt, &seekable, NULL, NULL);
              GST_LOG_OBJECT (qtdemux, "upstream BYTE seekable %d", seekable);
            }
            gst_query_unref (q);
          }
          gst_query_set_seeking (query, GST_FORMAT_TIME, seekable, 0, duration);
          res = TRUE;
        }
      }
      break;
    }
    case GST_QUERY_SEGMENT:
    {
      GstFormat format;
      gint64 start, stop;

      format = qtdemux->segment.format;

      start =
          gst_segment_to_stream_time (&qtdemux->segment, format,
          qtdemux->segment.start);
      if ((stop = qtdemux->segment.stop) == -1)
        stop = qtdemux->segment.duration;
      else
        stop = gst_segment_to_stream_time (&qtdemux->segment, format, stop);

      gst_query_set_segment (query, qtdemux->segment.rate, format, start, stop);
      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }

  return res;
}

static void
gst_qtdemux_push_tags (GstQTDemux * qtdemux, QtDemuxStream * stream)
{
  if (G_LIKELY (stream->pad)) {
    GST_DEBUG_OBJECT (qtdemux, "Checking pad %s:%s for tags",
        GST_DEBUG_PAD_NAME (stream->pad));

    if (!gst_tag_list_is_empty (stream->stream_tags)) {
      GST_DEBUG_OBJECT (qtdemux, "Sending tags %" GST_PTR_FORMAT,
          stream->stream_tags);
      gst_pad_push_event (stream->pad,
          gst_event_new_tag (gst_tag_list_ref (stream->stream_tags)));
    }

    if (G_UNLIKELY (stream->send_global_tags)) {
      GST_DEBUG_OBJECT (qtdemux, "Sending global tags %" GST_PTR_FORMAT,
          qtdemux->tag_list);
      gst_pad_push_event (stream->pad,
          gst_event_new_tag (gst_tag_list_ref (qtdemux->tag_list)));
      stream->send_global_tags = FALSE;
    }
  }
}

/* push event on all source pads; takes ownership of the event */
static void
gst_qtdemux_push_event (GstQTDemux * qtdemux, GstEvent * event)
{
  gboolean has_valid_stream = FALSE;
  GstEventType etype = GST_EVENT_TYPE (event);
  guint i;

  GST_DEBUG_OBJECT (qtdemux, "pushing %s event on all source pads",
      GST_EVENT_TYPE_NAME (event));

  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    GstPad *pad;
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    GST_DEBUG_OBJECT (qtdemux, "pushing on track-id %u", stream->track_id);

    if ((pad = stream->pad)) {
      has_valid_stream = TRUE;

      if (etype == GST_EVENT_EOS) {
        /* let's not send twice */
        if (stream->sent_eos)
          continue;
        stream->sent_eos = TRUE;
      }

      gst_pad_push_event (pad, gst_event_ref (event));
    }
  }

  gst_event_unref (event);

  /* if it is EOS and there are no pads, post an error */
  if (!has_valid_stream && etype == GST_EVENT_EOS) {
    gst_qtdemux_post_no_playable_stream_error (qtdemux);
  }
}

typedef struct
{
  guint64 media_time;
} FindData;

static gint
find_func (QtDemuxSample * s1, gint64 * media_time, gpointer user_data)
{
  if ((gint64) s1->timestamp > *media_time)
    return 1;
  if ((gint64) s1->timestamp == *media_time)
    return 0;

  return -1;
}

/* find the index of the sample that includes the data for @media_time using a
 * binary search.  Only to be called in optimized cases of linear search below.
 *
 * Returns the index of the sample with the corresponding *DTS*.
 */
static guint32
gst_qtdemux_find_index (GstQTDemux * qtdemux, QtDemuxStream * str,
    guint64 media_time)
{
  QtDemuxSample *result;
  guint32 index;

  /* convert media_time to mov format */
  media_time =
      gst_util_uint64_scale_ceil (media_time, str->timescale, GST_SECOND);

  result = gst_util_array_binary_search (str->samples, str->stbl_index + 1,
      sizeof (QtDemuxSample), (GCompareDataFunc) find_func,
      GST_SEARCH_MODE_BEFORE, &media_time, NULL);

  if (G_LIKELY (result))
    index = result - str->samples;
  else
    index = 0;

  return index;
}



/* find the index of the sample that includes the data for @media_offset using a
 * linear search
 *
 * Returns the index of the sample.
 */
static guint32
gst_qtdemux_find_index_for_given_media_offset_linear (GstQTDemux * qtdemux,
    QtDemuxStream * str, gint64 media_offset)
{
  QtDemuxSample *result = str->samples;
  guint32 index = 0;

  if (result == NULL || str->n_samples == 0)
    return -1;

  if (media_offset == result->offset)
    return index;

  result++;
  while (index < str->n_samples - 1) {
    if (!qtdemux_parse_samples (qtdemux, str, index + 1))
      goto parse_failed;

    if (media_offset < result->offset)
      break;

    index++;
    result++;
  }
  return index;

  /* ERRORS */
parse_failed:
  {
    GST_LOG_OBJECT (qtdemux, "Parsing of index %u failed!", index + 1);
    return -1;
  }
}

/* find the index of the sample that includes the data for @media_time using a
 * linear search, and keeping in mind that not all samples may have been parsed
 * yet.  If possible, it will delegate to binary search.
 *
 * Returns the index of the sample.
 */
static guint32
gst_qtdemux_find_index_linear (GstQTDemux * qtdemux, QtDemuxStream * str,
    GstClockTime media_time)
{
  guint32 index = 0;
  guint64 mov_time;
  QtDemuxSample *sample;

  /* convert media_time to mov format */
  mov_time =
      gst_util_uint64_scale_ceil (media_time, str->timescale, GST_SECOND);

  sample = str->samples;
  if (mov_time == sample->timestamp + sample->pts_offset)
    return index;

  /* use faster search if requested time in already parsed range */
  sample = str->samples + str->stbl_index;
  if (str->stbl_index >= 0 && mov_time <= sample->timestamp) {
    index = gst_qtdemux_find_index (qtdemux, str, media_time);
    sample = str->samples + index;
  } else {
    while (index < str->n_samples - 1) {
      if (!qtdemux_parse_samples (qtdemux, str, index + 1))
        goto parse_failed;

      sample = str->samples + index + 1;
      if (mov_time < sample->timestamp) {
        sample = str->samples + index;
        break;
      }

      index++;
    }
  }

  /* sample->timestamp is now <= media_time, need to find the corresponding
   * PTS now by looking backwards */
  while (index > 0 && sample->timestamp + sample->pts_offset > mov_time) {
    index--;
    sample = str->samples + index;
  }

  return index;

  /* ERRORS */
parse_failed:
  {
    GST_LOG_OBJECT (qtdemux, "Parsing of index %u failed!", index + 1);
    return -1;
  }
}

/* find the index of the keyframe needed to decode the sample at @index
 * of stream @str, or of a subsequent keyframe (depending on @next)
 *
 * Returns the index of the keyframe.
 */
static guint32
gst_qtdemux_find_keyframe (GstQTDemux * qtdemux, QtDemuxStream * str,
    guint32 index, gboolean next)
{
  guint32 new_index = index;

  if (index >= str->n_samples) {
    new_index = str->n_samples;
    goto beach;
  }

  /* all keyframes, return index */
  if (str->all_keyframe) {
    new_index = index;
    goto beach;
  }

  /* else search until we have a keyframe */
  while (new_index < str->n_samples) {
    if (next && !qtdemux_parse_samples (qtdemux, str, new_index))
      goto parse_failed;

    if (str->samples[new_index].keyframe)
      break;

    if (new_index == 0)
      break;

    if (next)
      new_index++;
    else
      new_index--;
  }

  if (new_index == str->n_samples) {
    GST_DEBUG_OBJECT (qtdemux, "no next keyframe");
    new_index = -1;
  }

beach:
  GST_DEBUG_OBJECT (qtdemux, "searching for keyframe index %s index %u "
      "gave %u", next ? "after" : "before", index, new_index);

  return new_index;

  /* ERRORS */
parse_failed:
  {
    GST_LOG_OBJECT (qtdemux, "Parsing of index %u failed!", new_index);
    return -1;
  }
}

/* find the segment for @time_position for @stream
 *
 * Returns the index of the segment containing @time_position.
 * Returns the last segment and sets the @eos variable to TRUE
 * if the time is beyond the end. @eos may be NULL
 */
static guint32
gst_qtdemux_find_segment (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstClockTime time_position)
{
  gint i;
  guint32 seg_idx;

  GST_LOG_OBJECT (stream->pad, "finding segment for %" GST_TIME_FORMAT,
      GST_TIME_ARGS (time_position));

  seg_idx = -1;
  for (i = 0; i < stream->n_segments; i++) {
    QtDemuxSegment *segment = &stream->segments[i];

    GST_LOG_OBJECT (stream->pad,
        "looking at segment %" GST_TIME_FORMAT "-%" GST_TIME_FORMAT,
        GST_TIME_ARGS (segment->time), GST_TIME_ARGS (segment->stop_time));

    /* For the last segment we include stop_time in the last segment */
    if (i < stream->n_segments - 1) {
      if (segment->time <= time_position && time_position < segment->stop_time) {
        GST_LOG_OBJECT (stream->pad, "segment %d matches", i);
        seg_idx = i;
        break;
      }
    } else {
      /* Last segment always matches */
      seg_idx = i;
      break;
    }
  }
  return seg_idx;
}

/* move the stream @str to the sample position @index.
 *
 * Updates @str->sample_index and marks discontinuity if needed.
 */
static void
gst_qtdemux_move_stream (GstQTDemux * qtdemux, QtDemuxStream * str,
    guint32 index)
{
  /* no change needed */
  if (index == str->sample_index)
    return;

  GST_DEBUG_OBJECT (qtdemux, "moving to sample %u of %u", index,
      str->n_samples);

  /* position changed, we have a discont */
  str->sample_index = index;
  str->offset_in_sample = 0;
  /* Each time we move in the stream we store the position where we are
   * starting from */
  str->from_sample = index;
  str->discont = TRUE;
}

static void
gst_qtdemux_adjust_seek (GstQTDemux * qtdemux, gint64 desired_time,
    gboolean use_sparse, gboolean next, gint64 * key_time, gint64 * key_offset)
{
  guint64 min_offset;
  gint64 min_byte_offset = -1;
  guint i;

  min_offset = next ? G_MAXUINT64 : desired_time;

  /* for each stream, find the index of the sample in the segment
   * and move back to the previous keyframe. */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *str;
    guint32 index, kindex;
    guint32 seg_idx;
    GstClockTime media_start;
    GstClockTime media_time;
    GstClockTime seg_time;
    QtDemuxSegment *seg;
    gboolean empty_segment = FALSE;

    str = QTDEMUX_NTH_STREAM (qtdemux, i);

    if (CUR_STREAM (str)->sparse && !use_sparse)
      continue;

    /* raw audio streams can be ignored as we can seek anywhere within them */
    if (str->subtype == FOURCC_soun && str->need_clip)
      continue;

    seg_idx = gst_qtdemux_find_segment (qtdemux, str, desired_time);
    GST_DEBUG_OBJECT (qtdemux, "align segment %d", seg_idx);

    /* get segment and time in the segment */
    seg = &str->segments[seg_idx];
    seg_time = (desired_time - seg->time) * seg->rate;

    while (QTSEGMENT_IS_EMPTY (seg)) {
      seg_time = 0;
      empty_segment = TRUE;
      GST_DEBUG_OBJECT (str->pad, "Segment %d is empty, moving to next one",
          seg_idx);
      seg_idx++;
      if (seg_idx == str->n_segments)
        break;
      seg = &str->segments[seg_idx];
    }

    if (seg_idx == str->n_segments) {
      /* FIXME track shouldn't have the last segment as empty, but if it
       * happens we better handle it */
      continue;
    }

    /* get the media time in the segment */
    media_start = seg->media_start + seg_time;

    /* get the index of the sample with media time */
    index = gst_qtdemux_find_index_linear (qtdemux, str, media_start);
    GST_DEBUG_OBJECT (qtdemux, "sample for %" GST_TIME_FORMAT " at %u"
        " at offset %" G_GUINT64_FORMAT " (empty segment: %d)",
        GST_TIME_ARGS (media_start), index, str->samples[index].offset,
        empty_segment);

    /* shift to next frame if we are looking for next keyframe */
    if (next && QTSAMPLE_PTS_NO_CSLG (str, &str->samples[index]) < media_start
        && index < str->stbl_index)
      index++;

    if (!empty_segment) {
      /* find previous or next keyframe */
      kindex = gst_qtdemux_find_keyframe (qtdemux, str, index, next);

      /* if looking for next one, we will settle for one before if none found after */
      if (next && kindex == -1)
        kindex = gst_qtdemux_find_keyframe (qtdemux, str, index, FALSE);

      /* Update the requested time whenever a keyframe was found, to make it
       * accurate and avoid having the first buffer fall outside of the segment
       */
      if (kindex != -1) {
        index = kindex;

        /* get timestamp of keyframe */
        media_time = QTSAMPLE_PTS_NO_CSLG (str, &str->samples[kindex]);
        GST_DEBUG_OBJECT (qtdemux,
            "keyframe at %u with time %" GST_TIME_FORMAT " at offset %"
            G_GUINT64_FORMAT, kindex, GST_TIME_ARGS (media_time),
            str->samples[kindex].offset);

        /* keyframes in the segment get a chance to change the
         * desired_offset. keyframes out of the segment are
         * ignored. */
        if (media_time >= seg->media_start) {
          GstClockTime seg_time;

          /* this keyframe is inside the segment, convert back to
           * segment time */
          seg_time = (media_time - seg->media_start) + seg->time;

          /* Adjust the offset based on the earliest suitable keyframe found,
           * based on which GST_SEEK_FLAG_SNAP_* is present (indicated by 'next').
           * For SNAP_BEFORE we look for the earliest keyframe before desired_time,
           * and in case of SNAP_AFTER - for the closest one after it. */
          if (seg_time < min_offset)
            min_offset = seg_time;
        }
      }
    }

    if (min_byte_offset < 0 || str->samples[index].offset < min_byte_offset)
      min_byte_offset = str->samples[index].offset;
  }

  if (key_time)
    *key_time = min_offset;
  if (key_offset)
    *key_offset = min_byte_offset;
}

static gboolean
gst_qtdemux_convert_seek (GstPad * pad, GstFormat * format,
    GstSeekType cur_type, gint64 * cur, GstSeekType stop_type, gint64 * stop)
{
  gboolean res;

  g_return_val_if_fail (format != NULL, FALSE);
  g_return_val_if_fail (cur != NULL, FALSE);
  g_return_val_if_fail (stop != NULL, FALSE);

  if (*format == GST_FORMAT_TIME)
    return TRUE;

  res = TRUE;
  if (cur_type != GST_SEEK_TYPE_NONE)
    res = gst_pad_query_convert (pad, *format, *cur, GST_FORMAT_TIME, cur);
  if (res && stop_type != GST_SEEK_TYPE_NONE)
    res = gst_pad_query_convert (pad, *format, *stop, GST_FORMAT_TIME, stop);

  if (res)
    *format = GST_FORMAT_TIME;

  return res;
}

/* perform seek in push based mode:
   find BYTE position to move to based on time and delegate to upstream
*/
static gboolean
gst_qtdemux_do_push_seek (GstQTDemux * qtdemux, GstPad * pad, GstEvent * event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType cur_type, stop_type;
  gint64 cur, stop, key_cur;
  gboolean res;
  gint64 byte_cur;
  gint64 original_stop;
  guint32 seqnum;

  GST_DEBUG_OBJECT (qtdemux, "doing push-based seek");

  gst_event_parse_seek (event, &rate, &format, &flags,
      &cur_type, &cur, &stop_type, &stop);
  seqnum = gst_event_get_seqnum (event);

  /* Directly send the instant-rate-change event here before taking the
   * stream-lock so that it can be applied as soon as possible */
  if (flags & GST_SEEK_FLAG_INSTANT_RATE_CHANGE) {
    GstEvent *ev;

    /* instant rate change only supported if direction does not change. All
     * other requirements are already checked before creating the seek event
     * but let's double-check here to be sure */
    if ((qtdemux->segment.rate > 0 && rate < 0) ||
        (qtdemux->segment.rate < 0 && rate > 0) ||
        cur_type != GST_SEEK_TYPE_NONE ||
        stop_type != GST_SEEK_TYPE_NONE || (flags & GST_SEEK_FLAG_FLUSH)) {
      GST_ERROR_OBJECT (qtdemux,
          "Instant rate change seeks only supported in the "
          "same direction, without flushing and position change");
      return FALSE;
    }

    ev = gst_event_new_instant_rate_change (rate / qtdemux->segment.rate,
        (GstSegmentFlags) flags);
    gst_event_set_seqnum (ev, seqnum);
    gst_qtdemux_push_event (qtdemux, ev);
    return TRUE;
  }

  /* only forward streaming and seeking is possible */
  if (rate <= 0)
    goto unsupported_seek;

  /* convert to TIME if needed and possible */
  if (!gst_qtdemux_convert_seek (pad, &format, cur_type, &cur,
          stop_type, &stop))
    goto no_format;

  /* Upstream seek in bytes will have undefined stop, but qtdemux stores
   * the original stop position to use when upstream pushes the new segment
   * for this seek */
  original_stop = stop;
  stop = -1;

  /* find reasonable corresponding BYTE position,
   * also try to mind about keyframes, since we can not go back a bit for them
   * later on */
  /* determining @next here based on SNAP_BEFORE/SNAP_AFTER should
   * mostly just work, but let's not yet boldly go there  ... */
  gst_qtdemux_adjust_seek (qtdemux, cur, FALSE, FALSE, &key_cur, &byte_cur);

  if (byte_cur == -1)
    goto abort_seek;

  GST_DEBUG_OBJECT (qtdemux, "Pushing BYTE seek rate %g, "
      "start %" G_GINT64_FORMAT ", stop %" G_GINT64_FORMAT, rate, byte_cur,
      stop);

  GST_OBJECT_LOCK (qtdemux);
  qtdemux->seek_offset = byte_cur;
  if (!(flags & GST_SEEK_FLAG_KEY_UNIT)) {
    qtdemux->push_seek_start = cur;
  } else {
    qtdemux->push_seek_start = key_cur;
  }

  if (stop_type == GST_SEEK_TYPE_NONE) {
    qtdemux->push_seek_stop = qtdemux->segment.stop;
  } else {
    qtdemux->push_seek_stop = original_stop;
  }
  GST_OBJECT_UNLOCK (qtdemux);

  qtdemux->segment_seqnum = seqnum;
  /* BYTE seek event */
  event = gst_event_new_seek (rate, GST_FORMAT_BYTES, flags, cur_type, byte_cur,
      stop_type, stop);
  gst_event_set_seqnum (event, seqnum);
  res = gst_pad_push_event (qtdemux->sinkpad, event);

  return res;

  /* ERRORS */
abort_seek:
  {
    GST_DEBUG_OBJECT (qtdemux, "could not determine byte position to seek to, "
        "seek aborted.");
    return FALSE;
  }
unsupported_seek:
  {
    GST_DEBUG_OBJECT (qtdemux, "unsupported seek, seek aborted.");
    return FALSE;
  }
no_format:
  {
    GST_DEBUG_OBJECT (qtdemux, "unsupported format given, seek aborted.");
    return FALSE;
  }
}

/* perform the seek.
 *
 * We set all segment_indexes in the streams to unknown and
 * adjust the time_position to the desired position. this is enough
 * to trigger a segment switch in the streaming thread to start
 * streaming from the desired position.
 *
 * Keyframe seeking is a little more complicated when dealing with
 * segments. Ideally we want to move to the previous keyframe in
 * the segment but there might not be a keyframe in the segment. In
 * fact, none of the segments could contain a keyframe. We take a
 * practical approach: seek to the previous keyframe in the segment,
 * if there is none, seek to the beginning of the segment.
 *
 * Called with STREAM_LOCK
 */
static gboolean
gst_qtdemux_perform_seek (GstQTDemux * qtdemux, GstSegment * segment,
    guint32 seqnum, GstSeekFlags flags)
{
  gint64 desired_offset;
  guint i;

  desired_offset = segment->position;

  GST_DEBUG_OBJECT (qtdemux, "seeking to %" GST_TIME_FORMAT,
      GST_TIME_ARGS (desired_offset));

  /* may not have enough fragmented info to do this adjustment,
   * and we can't scan (and probably should not) at this time with
   * possibly flushing upstream */
  if ((flags & GST_SEEK_FLAG_KEY_UNIT) && !qtdemux->fragmented) {
    gint64 min_offset;
    gboolean next, before, after;

    before = !!(flags & GST_SEEK_FLAG_SNAP_BEFORE);
    after = !!(flags & GST_SEEK_FLAG_SNAP_AFTER);
    next = after && !before;
    if (segment->rate < 0)
      next = !next;

    gst_qtdemux_adjust_seek (qtdemux, desired_offset, TRUE, next, &min_offset,
        NULL);
    GST_DEBUG_OBJECT (qtdemux, "keyframe seek, align to %"
        GST_TIME_FORMAT, GST_TIME_ARGS (min_offset));
    desired_offset = min_offset;
  }

  /* and set all streams to the final position */
  GST_OBJECT_LOCK (qtdemux);
  gst_flow_combiner_reset (qtdemux->flowcombiner);
  GST_OBJECT_UNLOCK (qtdemux);
  qtdemux->segment_seqnum = seqnum;
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);

    stream->time_position = desired_offset;
    stream->accumulated_base = 0;
    stream->sample_index = -1;
    stream->offset_in_sample = 0;
    stream->segment_index = -1;
    stream->sent_eos = FALSE;
    stream->last_keyframe_dts = GST_CLOCK_TIME_NONE;

    if (segment->flags & GST_SEEK_FLAG_FLUSH)
      gst_segment_init (&stream->segment, GST_FORMAT_TIME);
  }
  segment->position = desired_offset;
  if (segment->rate >= 0) {
    segment->start = desired_offset;
    /* We need to update time as we update start in that direction */
    segment->time = desired_offset;

    /* we stop at the end */
    if (segment->stop == -1)
      segment->stop = segment->duration;
  } else {
    segment->stop = desired_offset;
  }

  if (qtdemux->fragmented)
    qtdemux->fragmented_seek_pending = TRUE;

  return TRUE;
}

/* do a seek in pull based mode */
static gboolean
gst_qtdemux_do_seek (GstQTDemux * qtdemux, GstPad * pad, GstEvent * event)
{
  gdouble rate = 1.0;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType cur_type, stop_type;
  gint64 cur, stop;
  gboolean flush, instant_rate_change;
  gboolean update;
  GstSegment seeksegment;
  guint32 seqnum = GST_SEQNUM_INVALID;
  GstEvent *flush_event;
  gboolean ret;

  GST_DEBUG_OBJECT (qtdemux, "doing seek with event");

  gst_event_parse_seek (event, &rate, &format, &flags,
      &cur_type, &cur, &stop_type, &stop);
  seqnum = gst_event_get_seqnum (event);

  /* we have to have a format as the segment format. Try to convert
   * if not. */
  if (!gst_qtdemux_convert_seek (pad, &format, cur_type, &cur,
          stop_type, &stop))
    goto no_format;

  GST_DEBUG_OBJECT (qtdemux, "seek format %s", gst_format_get_name (format));

  flush = !!(flags & GST_SEEK_FLAG_FLUSH);
  instant_rate_change = !!(flags & GST_SEEK_FLAG_INSTANT_RATE_CHANGE);

  /* Directly send the instant-rate-change event here before taking the
   * stream-lock so that it can be applied as soon as possible */
  if (instant_rate_change) {
    GstEvent *ev;

    /* instant rate change only supported if direction does not change. All
     * other requirements are already checked before creating the seek event
     * but let's double-check here to be sure */
    if ((qtdemux->segment.rate > 0 && rate < 0) ||
        (qtdemux->segment.rate < 0 && rate > 0) ||
        cur_type != GST_SEEK_TYPE_NONE ||
        stop_type != GST_SEEK_TYPE_NONE || flush) {
      GST_ERROR_OBJECT (qtdemux,
          "Instant rate change seeks only supported in the "
          "same direction, without flushing and position change");
      return FALSE;
    }

    ev = gst_event_new_instant_rate_change (rate / qtdemux->segment.rate,
        (GstSegmentFlags) flags);
    gst_event_set_seqnum (ev, seqnum);
    gst_qtdemux_push_event (qtdemux, ev);
    return TRUE;
  }

  /* stop streaming, either by flushing or by pausing the task */
  if (flush) {
    flush_event = gst_event_new_flush_start ();
    if (seqnum != GST_SEQNUM_INVALID)
      gst_event_set_seqnum (flush_event, seqnum);
    /* unlock upstream pull_range */
    gst_pad_push_event (qtdemux->sinkpad, gst_event_ref (flush_event));
    /* make sure out loop function exits */
    gst_qtdemux_push_event (qtdemux, flush_event);
  } else {
    /* non flushing seek, pause the task */
    gst_pad_pause_task (qtdemux->sinkpad);
  }

  /* wait for streaming to finish */
  GST_PAD_STREAM_LOCK (qtdemux->sinkpad);

  /* copy segment, we need this because we still need the old
   * segment when we close the current segment. */
  memcpy (&seeksegment, &qtdemux->segment, sizeof (GstSegment));

  /* configure the segment with the seek variables */
  GST_DEBUG_OBJECT (qtdemux, "configuring seek");
  if (!gst_segment_do_seek (&seeksegment, rate, format, flags,
          cur_type, cur, stop_type, stop, &update)) {
    ret = FALSE;
    GST_ERROR_OBJECT (qtdemux, "inconsistent seek values, doing nothing");
  } else {
    /* now do the seek */
    ret = gst_qtdemux_perform_seek (qtdemux, &seeksegment, seqnum, flags);
  }

  /* prepare for streaming again */
  if (flush) {
    flush_event = gst_event_new_flush_stop (TRUE);
    if (seqnum != GST_SEQNUM_INVALID)
      gst_event_set_seqnum (flush_event, seqnum);

    gst_pad_push_event (qtdemux->sinkpad, gst_event_ref (flush_event));
    gst_qtdemux_push_event (qtdemux, flush_event);
  }

  /* commit the new segment */
  memcpy (&qtdemux->segment, &seeksegment, sizeof (GstSegment));

  if (qtdemux->segment.flags & GST_SEEK_FLAG_SEGMENT) {
    GstMessage *msg = gst_message_new_segment_start (GST_OBJECT_CAST (qtdemux),
        qtdemux->segment.format, qtdemux->segment.position);
    if (seqnum != GST_SEQNUM_INVALID)
      gst_message_set_seqnum (msg, seqnum);
    gst_element_post_message (GST_ELEMENT_CAST (qtdemux), msg);
  }

  /* restart streaming, NEWSEGMENT will be sent from the streaming thread. */
  gst_pad_start_task (qtdemux->sinkpad, (GstTaskFunction) gst_qtdemux_loop,
      qtdemux->sinkpad, NULL);

  GST_PAD_STREAM_UNLOCK (qtdemux->sinkpad);

  return ret;

  /* ERRORS */
no_format:
  {
    GST_DEBUG_OBJECT (qtdemux, "unsupported format given, seek aborted.");
    return FALSE;
  }
}

static gboolean
qtdemux_ensure_index (GstQTDemux * qtdemux)
{
  guint i;

  GST_DEBUG_OBJECT (qtdemux, "collecting all metadata for all streams");

  /* Build complete index */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);

    if (!qtdemux_parse_samples (qtdemux, stream, stream->n_samples - 1)) {
      GST_LOG_OBJECT (qtdemux,
          "Building complete index of track-id %u for seeking failed!",
          stream->track_id);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_qtdemux_handle_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  gboolean res = TRUE;
  GstQTDemux *qtdemux = GST_QTDEMUX (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_RECONFIGURE:
      GST_OBJECT_LOCK (qtdemux);
      gst_flow_combiner_reset (qtdemux->flowcombiner);
      GST_OBJECT_UNLOCK (qtdemux);
      res = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_SEEK:
    {
      GstSeekFlags flags = 0;
      GstFormat seek_format;
      gboolean instant_rate_change;

#ifndef GST_DISABLE_GST_DEBUG
      GstClockTime ts = gst_util_get_timestamp ();
#endif
      guint32 seqnum = gst_event_get_seqnum (event);

      qtdemux->received_seek = TRUE;

      gst_event_parse_seek (event, NULL, &seek_format, &flags, NULL, NULL, NULL,
          NULL);
      instant_rate_change = !!(flags & GST_SEEK_FLAG_INSTANT_RATE_CHANGE);

      if (seqnum == qtdemux->segment_seqnum) {
        GST_LOG_OBJECT (pad,
            "Drop duplicated SEEK event seqnum %" G_GUINT32_FORMAT, seqnum);
        gst_event_unref (event);
        return TRUE;
      }

      if (qtdemux->upstream_format_is_time && qtdemux->fragmented) {
        /* seek should be handled by upstream, we might need to re-download fragments */
        GST_DEBUG_OBJECT (qtdemux,
            "let upstream handle seek for fragmented playback");
        goto upstream;
      }

      if (seek_format == GST_FORMAT_BYTES) {
        GST_DEBUG_OBJECT (pad, "Rejecting seek request in bytes format");
        gst_event_unref (event);
        return FALSE;
      }

      gst_event_parse_seek_trickmode_interval (event,
          &qtdemux->trickmode_interval);

      /* Build complete index for seeking;
       * if not a fragmented file at least and we're really doing a seek,
       * not just an instant-rate-change */
      if (!qtdemux->fragmented && !instant_rate_change) {
        if (!qtdemux_ensure_index (qtdemux))
          goto index_failed;
      }
#ifndef GST_DISABLE_GST_DEBUG
      ts = gst_util_get_timestamp () - ts;
      GST_INFO_OBJECT (qtdemux,
          "Time taken to parse index %" GST_TIME_FORMAT, GST_TIME_ARGS (ts));
#endif
      if (qtdemux->pullbased) {
        res = gst_qtdemux_do_seek (qtdemux, pad, event);
      } else if (gst_pad_push_event (qtdemux->sinkpad, gst_event_ref (event))) {
        GST_DEBUG_OBJECT (qtdemux, "Upstream successfully seeked");
        res = TRUE;
      } else if (qtdemux->state == QTDEMUX_STATE_MOVIE
          && QTDEMUX_N_STREAMS (qtdemux)
          && !qtdemux->fragmented) {
        res = gst_qtdemux_do_push_seek (qtdemux, pad, event);
      } else {
        GST_DEBUG_OBJECT (qtdemux,
            "ignoring seek in push mode in current state");
        res = FALSE;
      }
      gst_event_unref (event);
    }
      break;
    default:
    upstream:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }

done:
  return res;

  /* ERRORS */
index_failed:
  {
    GST_ERROR_OBJECT (qtdemux, "Index failed");
    gst_event_unref (event);
    res = FALSE;
    goto done;
  }
}

/* Find, for each track, the first sample in coding order that has a file offset >= @byte_pos.
 *
 * If @fw is false, the coding order is explored backwards.
 *
 * If @set is true, each stream will be moved to its matched sample, or EOS if no matching
 * sample is found for that track.
 *
 * The stream and sample index of the sample with the minimum offset in the direction explored
 * (see @fw) is returned in the output parameters @_stream and @_index respectively.
 *
 * @_time is set to the QTSAMPLE_PTS of the matched sample with the minimum QTSAMPLE_PTS in the
 * direction explored, which may not always match the QTSAMPLE_PTS of the sample returned in
 * @_stream and @_index. */
static void
gst_qtdemux_find_sample (GstQTDemux * qtdemux, gint64 byte_pos, gboolean fw,
    gboolean set, QtDemuxStream ** _stream, gint * _index, gint64 * _time)
{
  gint i, index;
  gint64 time, min_time;
  QtDemuxStream *stream;
  gint iter;

  min_time = -1;
  stream = NULL;
  index = -1;

  for (iter = 0; iter < QTDEMUX_N_STREAMS (qtdemux); iter++) {
    QtDemuxStream *str;
    gint inc;
    gboolean set_sample;

    str = QTDEMUX_NTH_STREAM (qtdemux, iter);
    set_sample = !set;

    if (fw) {
      i = 0;
      inc = 1;
    } else {
      i = str->n_samples - 1;
      inc = -1;
    }

    for (; (i >= 0) && (i < str->n_samples); i += inc) {
      if (str->samples[i].size == 0)
        continue;

      if (fw && (str->samples[i].offset < byte_pos))
        continue;

      if (!fw && (str->samples[i].offset + str->samples[i].size > byte_pos))
        continue;

      /* move stream to first available sample */
      if (set) {
        gst_qtdemux_move_stream (qtdemux, str, i);
        set_sample = TRUE;
      }

      /* avoid index from sparse streams since they might be far away */
      if (!CUR_STREAM (str)->sparse) {
        /* determine min/max time */
        time = QTSAMPLE_PTS (str, &str->samples[i]);
        if (min_time == -1 || (!fw && time > min_time) ||
            (fw && time < min_time)) {
          min_time = time;
        }

        /* determine stream with leading sample, to get its position */
        if (!stream ||
            (fw && (str->samples[i].offset < stream->samples[index].offset)) ||
            (!fw && (str->samples[i].offset > stream->samples[index].offset))) {
          stream = str;
          index = i;
        }
      }
      break;
    }

    /* no sample for this stream, mark eos */
    if (!set_sample)
      gst_qtdemux_move_stream (qtdemux, str, str->n_samples);
  }

  if (_time)
    *_time = min_time;
  if (_stream)
    *_stream = stream;
  if (_index)
    *_index = index;
}

/* Copied from mpegtsbase code */
/* FIXME: replace this function when we add new util function for stream-id creation */
static gchar *
_get_upstream_id (GstQTDemux * demux)
{
  gchar *upstream_id = gst_pad_get_stream_id (demux->sinkpad);

  if (!upstream_id) {
    /* Try to create one from the upstream URI, else use a randome number */
    GstQuery *query;
    gchar *uri = NULL;

    /* Try to generate one from the URI query and
     * if it fails take a random number instead */
    query = gst_query_new_uri ();
    if (gst_element_query (GST_ELEMENT_CAST (demux), query)) {
      gst_query_parse_uri (query, &uri);
    }

    if (uri) {
      GChecksum *cs;

      /* And then generate an SHA256 sum of the URI */
      cs = g_checksum_new (G_CHECKSUM_SHA256);
      g_checksum_update (cs, (const guchar *) uri, strlen (uri));
      g_free (uri);
      upstream_id = g_strdup (g_checksum_get_string (cs));
      g_checksum_free (cs);
    } else {
      /* Just get some random number if the URI query fails */
      GST_FIXME_OBJECT (demux, "Creating random stream-id, consider "
          "implementing a deterministic way of creating a stream-id");
      upstream_id =
          g_strdup_printf ("%08x%08x%08x%08x", g_random_int (), g_random_int (),
          g_random_int (), g_random_int ());
    }

    gst_query_unref (query);
  }
  return upstream_id;
}

static QtDemuxStream *
_create_stream (GstQTDemux * demux, guint32 track_id)
{
  QtDemuxStream *stream;
  gchar *upstream_id;

  stream = g_new0 (QtDemuxStream, 1);
  stream->demux = demux;
  stream->track_id = track_id;
  upstream_id = _get_upstream_id (demux);
  stream->stream_id = g_strdup_printf ("%s/%03u", upstream_id, track_id);
  g_free (upstream_id);
  /* new streams always need a discont */
  stream->discont = TRUE;
  /* we enable clipping for raw audio/video streams */
  stream->need_clip = FALSE;
  stream->process_func = NULL;
  stream->segment_index = -1;
  stream->time_position = 0;
  stream->sample_index = -1;
  stream->offset_in_sample = 0;
  stream->new_stream = TRUE;
  stream->multiview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
  stream->multiview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;
  stream->protected = FALSE;
  stream->protection_scheme_type = 0;
  stream->protection_scheme_version = 0;
  stream->protection_scheme_info = NULL;
  stream->n_samples_moof = 0;
  stream->duration_moof = 0;
  stream->duration_last_moof = 0;
  stream->alignment = 1;
  stream->needs_row_alignment = FALSE;
  stream->stream_tags = gst_tag_list_new_empty ();
  gst_tag_list_set_scope (stream->stream_tags, GST_TAG_SCOPE_STREAM);
  g_queue_init (&stream->protection_scheme_event_queue);
  gst_video_info_init (&stream->info);
  gst_video_info_init (&stream->pre_info);
  stream->ref_count = 1;
  /* consistent default for push based mode */
  gst_segment_init (&stream->segment, GST_FORMAT_TIME);
  return stream;
}

static gboolean
gst_qtdemux_setcaps (GstQTDemux * demux, GstCaps * caps)
{
  GstStructure *structure;
  const gchar *variant;
  const GstCaps *mediacaps = NULL;

  GST_DEBUG_OBJECT (demux, "Sink set caps: %" GST_PTR_FORMAT, caps);

  structure = gst_caps_get_structure (caps, 0);
  variant = gst_structure_get_string (structure, "variant");

  if (variant && strcmp (variant, "mse-bytestream") == 0) {
    demux->variant = VARIANT_MSE_BYTESTREAM;
  }

  if (variant && strcmp (variant, "mss-fragmented") == 0) {
    QtDemuxStream *stream;
    const GValue *value;

    demux->fragmented = TRUE;
    demux->variant = VARIANT_MSS_FRAGMENTED;

    if (QTDEMUX_N_STREAMS (demux) > 1) {
      /* can't do this, we can only renegotiate for another mss format */
      return FALSE;
    }

    value = gst_structure_get_value (structure, "media-caps");
    /* create stream */
    if (value) {
      const GValue *timescale_v;

      /* TODO update when stream changes during playback */

      if (QTDEMUX_N_STREAMS (demux) == 0) {
        stream = _create_stream (demux, 1);
        g_ptr_array_add (demux->active_streams, stream);
        /* mss has no stsd/stsd entry, use id 0 as default */
        stream->stsd_entries_length = 1;
        stream->stsd_sample_description_id = stream->cur_stsd_entry_index = 0;
        stream->stsd_entries = g_new0 (QtDemuxStreamStsdEntry, 1);
      } else {
        stream = QTDEMUX_NTH_STREAM (demux, 0);
      }

      timescale_v = gst_structure_get_value (structure, "timescale");
      if (timescale_v) {
        stream->timescale = g_value_get_uint64 (timescale_v);
      } else {
        /* default mss timescale */
        stream->timescale = 10000000;
      }
      demux->timescale = stream->timescale;

      mediacaps = gst_value_get_caps (value);
      if (!CUR_STREAM (stream)->caps
          || !gst_caps_is_equal_fixed (mediacaps, CUR_STREAM (stream)->caps)) {
        GST_DEBUG_OBJECT (demux, "We have a new caps %" GST_PTR_FORMAT,
            mediacaps);
        stream->new_caps = TRUE;
      }
      gst_caps_replace (&CUR_STREAM (stream)->caps, (GstCaps *) mediacaps);
      structure = gst_caps_get_structure (mediacaps, 0);
      if (g_str_has_prefix (gst_structure_get_name (structure), "video")) {
        stream->subtype = FOURCC_vide;

        gst_structure_get_int (structure, "width", &CUR_STREAM (stream)->width);
        gst_structure_get_int (structure, "height",
            &CUR_STREAM (stream)->height);
        gst_structure_get_fraction (structure, "framerate",
            &CUR_STREAM (stream)->fps_n, &CUR_STREAM (stream)->fps_d);
      } else if (g_str_has_prefix (gst_structure_get_name (structure), "audio")) {
        gint rate = 0;
        stream->subtype = FOURCC_soun;
        gst_structure_get_int (structure, "channels",
            &CUR_STREAM (stream)->n_channels);
        gst_structure_get_int (structure, "rate", &rate);
        CUR_STREAM (stream)->rate = rate;
      } else if (gst_structure_has_name (structure, "application/x-cenc")) {
        if (gst_structure_has_field (structure, "original-media-type")) {
          const gchar *media_type =
              gst_structure_get_string (structure, "original-media-type");
          if (g_str_has_prefix (media_type, "video")) {
            stream->subtype = FOURCC_vide;
          } else if (g_str_has_prefix (media_type, "audio")) {
            stream->subtype = FOURCC_soun;
          }
        }
      }
    }
    gst_caps_replace (&demux->media_caps, (GstCaps *) mediacaps);
  }

  return TRUE;
}

static void
gst_qtdemux_reset (GstQTDemux * qtdemux, gboolean hard)
{
  gint i;

  GST_DEBUG_OBJECT (qtdemux, "Resetting demux");

  if (hard || qtdemux->upstream_format_is_time
      || qtdemux->variant == VARIANT_MSE_BYTESTREAM) {
    qtdemux->state = QTDEMUX_STATE_INITIAL;
    qtdemux->neededbytes = 16;
    qtdemux->todrop = 0;
    qtdemux->pullbased = FALSE;
    g_clear_pointer (&qtdemux->redirect_location, g_free);
    qtdemux->first_mdat = -1;
    qtdemux->header_size = 0;
    qtdemux->mdatoffset = -1;
    qtdemux->restoredata_offset = -1;
    if (qtdemux->mdatbuffer)
      gst_buffer_unref (qtdemux->mdatbuffer);
    if (qtdemux->restoredata_buffer)
      gst_buffer_unref (qtdemux->restoredata_buffer);
    qtdemux->mdatbuffer = NULL;
    qtdemux->restoredata_buffer = NULL;
    qtdemux->mdatleft = 0;
    qtdemux->mdatsize = 0;
    if (qtdemux->comp_brands)
      gst_buffer_unref (qtdemux->comp_brands);
    qtdemux->comp_brands = NULL;
    qtdemux->last_moov_offset = -1;
    if (qtdemux->moov_node_compressed) {
      g_node_destroy (qtdemux->moov_node_compressed);
      if (qtdemux->moov_node)
        g_free (qtdemux->moov_node->data);
    }
    qtdemux->moov_node_compressed = NULL;
    if (qtdemux->moov_node)
      g_node_destroy (qtdemux->moov_node);
    qtdemux->moov_node = NULL;
    if (qtdemux->tag_list)
      gst_mini_object_unref (GST_MINI_OBJECT_CAST (qtdemux->tag_list));
    qtdemux->tag_list = gst_tag_list_new_empty ();
    gst_tag_list_set_scope (qtdemux->tag_list, GST_TAG_SCOPE_GLOBAL);
#if 0
    if (qtdemux->element_index)
      gst_object_unref (qtdemux->element_index);
    qtdemux->element_index = NULL;
#endif
    qtdemux->major_brand = 0;
    qtdemux->upstream_format_is_time = FALSE;
    qtdemux->upstream_seekable = FALSE;
    qtdemux->upstream_size = 0;

    qtdemux->fragment_start = -1;
    qtdemux->fragment_start_offset = -1;
    qtdemux->duration = 0;
    qtdemux->moof_offset = 0;
    qtdemux->chapters_track_id = 0;
    qtdemux->have_group_id = FALSE;
    qtdemux->group_id = G_MAXUINT;

    qtdemux->gapless_audio_info.type = GAPLESS_AUDIO_INFO_TYPE_NONE;
    qtdemux->gapless_audio_info.num_start_padding_pcm_frames = 0;
    qtdemux->gapless_audio_info.num_end_padding_pcm_frames = 0;
    qtdemux->gapless_audio_info.num_valid_pcm_frames = 0;

    g_queue_clear_full (&qtdemux->protection_event_queue,
        (GDestroyNotify) gst_event_unref);

    qtdemux->received_seek = FALSE;
    qtdemux->first_moof_already_parsed = FALSE;
  }
  qtdemux->offset = 0;
  gst_adapter_clear (qtdemux->adapter);
  gst_segment_init (&qtdemux->segment, GST_FORMAT_TIME);
  qtdemux->need_segment = TRUE;

  if (hard) {
    qtdemux->segment_seqnum = GST_SEQNUM_INVALID;
    qtdemux->trickmode_interval = 0;
    g_ptr_array_set_size (qtdemux->active_streams, 0);
    g_ptr_array_set_size (qtdemux->old_streams, 0);
    qtdemux->n_video_streams = 0;
    qtdemux->n_audio_streams = 0;
    qtdemux->n_sub_streams = 0;
    qtdemux->n_meta_streams = 0;
    qtdemux->exposed = FALSE;
    qtdemux->fragmented = FALSE;
    qtdemux->variant = VARIANT_NONE;
    gst_caps_replace (&qtdemux->media_caps, NULL);
    qtdemux->timescale = 0;
    qtdemux->got_moov = FALSE;
    qtdemux->start_utc_time = GST_CLOCK_TIME_NONE;
    qtdemux->cenc_aux_info_offset = 0;
    g_free (qtdemux->cenc_aux_info_sizes);
    qtdemux->cenc_aux_info_sizes = NULL;
    qtdemux->cenc_aux_sample_count = 0;
    if (qtdemux->protection_system_ids) {
      g_ptr_array_free (qtdemux->protection_system_ids, TRUE);
      qtdemux->protection_system_ids = NULL;
    }
    qtdemux->streams_aware = GST_OBJECT_PARENT (qtdemux)
        && GST_OBJECT_FLAG_IS_SET (GST_OBJECT_PARENT (qtdemux),
        GST_BIN_FLAG_STREAMS_AWARE);

    if (qtdemux->preferred_protection_system_id) {
      g_free (qtdemux->preferred_protection_system_id);
      qtdemux->preferred_protection_system_id = NULL;
    }
  } else if (qtdemux->variant == VARIANT_MSS_FRAGMENTED) {
    gst_flow_combiner_reset (qtdemux->flowcombiner);
    g_ptr_array_foreach (qtdemux->active_streams,
        (GFunc) gst_qtdemux_stream_clear, NULL);
  } else if (qtdemux->variant == VARIANT_MSE_BYTESTREAM) {
    /* Do nothing */
  } else {
    gst_flow_combiner_reset (qtdemux->flowcombiner);
    for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
      QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
      stream->sent_eos = FALSE;
      stream->time_position = 0;
      stream->accumulated_base = 0;
      stream->last_keyframe_dts = GST_CLOCK_TIME_NONE;
    }
  }
}

static void
qtdemux_clear_protection_events_on_all_streams (GstQTDemux * qtdemux)
{
  for (unsigned i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    g_queue_clear_full (&stream->protection_scheme_event_queue,
        (GDestroyNotify) gst_event_unref);
  }
}

/* Maps the @segment to the qt edts internal segments and pushes
 * the corresponding segment event.
 *
 * If it ends up being at a empty segment, a gap will be pushed and the next
 * edts segment will be activated in sequence.
 *
 * To be used in push-mode only */
static void
gst_qtdemux_map_and_push_segments (GstQTDemux * qtdemux, GstSegment * segment)
{
  gint i, iter;

  for (iter = 0; iter < QTDEMUX_N_STREAMS (qtdemux); iter++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, iter);

    stream->time_position = segment->start;

    /* in push mode we should be guaranteed that we will have empty segments
     * at the beginning and then one segment after, other scenarios are not
     * supported and are discarded when parsing the edts */
    for (i = 0; i < stream->n_segments; i++) {
      if (stream->segments[i].stop_time > segment->start) {
        /* push the empty segment and move to the next one */
        gst_qtdemux_activate_segment (qtdemux, stream, i,
            stream->time_position);
        if (QTSEGMENT_IS_EMPTY (&stream->segments[i])) {
          gst_qtdemux_send_gap_for_segment (qtdemux, stream, i,
              stream->time_position);

          /* accumulate previous segments */
          if (GST_CLOCK_TIME_IS_VALID (stream->segment.stop))
            stream->accumulated_base +=
                (stream->segment.stop -
                stream->segment.start) / ABS (stream->segment.rate);
          continue;
        }

        g_assert (i == stream->n_segments - 1);
      }
    }
  }
}

static void
gst_qtdemux_stream_concat (GstQTDemux * qtdemux, GPtrArray * dest,
    GPtrArray * src)
{
  guint i;
  guint len;

  len = src->len;

  if (len == 0)
    return;

  for (i = 0; i < len; i++) {
    QtDemuxStream *stream = g_ptr_array_index (src, i);

#ifndef GST_DISABLE_GST_DEBUG
    GST_DEBUG_OBJECT (qtdemux, "Move stream %p (stream-id %s) to %p",
        stream, GST_STR_NULL (stream->stream_id), dest);
#endif
    g_ptr_array_add (dest, gst_qtdemux_stream_ref (stream));
  }

  g_ptr_array_set_size (src, 0);
}

static gboolean
gst_qtdemux_handle_sink_event (GstPad * sinkpad, GstObject * parent,
    GstEvent * event)
{
  GstQTDemux *demux = GST_QTDEMUX (parent);
  gboolean res = TRUE;

  GST_LOG_OBJECT (demux, "handling %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
    {
      gint64 offset = 0;
      QtDemuxStream *stream;
      gint idx;
      GstSegment segment;

      /* some debug output */
      gst_event_copy_segment (event, &segment);
      GST_DEBUG_OBJECT (demux, "received newsegment %" GST_SEGMENT_FORMAT,
          &segment);

      if (segment.format == GST_FORMAT_TIME) {
        demux->upstream_format_is_time = TRUE;
        demux->segment_seqnum = gst_event_get_seqnum (event);
      } else {
        GST_DEBUG_OBJECT (demux, "Not storing upstream newsegment, "
            "not in time format");

        /* chain will send initial newsegment after pads have been added */
        if (demux->state != QTDEMUX_STATE_MOVIE || !QTDEMUX_N_STREAMS (demux)) {
          GST_DEBUG_OBJECT (demux, "still starting, eating event");
          goto exit;
        }
      }

      /* check if this matches a time seek we received previously
       * FIXME for backwards compatibility reasons we use the
       * seek_offset here to compare. In the future we might want to
       * change this to use the seqnum as it uniquely should identify
       * the segment that corresponds to the seek. */
      GST_DEBUG_OBJECT (demux, "Stored seek offset: %" G_GINT64_FORMAT
          ", received segment offset %" G_GINT64_FORMAT,
          demux->seek_offset, segment.start);
      if (segment.format == GST_FORMAT_BYTES
          && demux->seek_offset == segment.start) {
        GST_OBJECT_LOCK (demux);
        offset = segment.start;

        segment.format = GST_FORMAT_TIME;
        segment.start = demux->push_seek_start;
        segment.stop = demux->push_seek_stop;
        GST_DEBUG_OBJECT (demux, "Replaced segment with stored seek "
            "segment %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
            GST_TIME_ARGS (segment.start), GST_TIME_ARGS (segment.stop));
        GST_OBJECT_UNLOCK (demux);
      }

      /* we only expect a BYTE segment, e.g. following a seek */
      if (segment.format == GST_FORMAT_BYTES) {
        if (GST_CLOCK_TIME_IS_VALID (segment.start)) {
          offset = segment.start;

          gst_qtdemux_find_sample (demux, segment.start, TRUE, FALSE, NULL,
              NULL, (gint64 *) & segment.start);
          if ((gint64) segment.start < 0)
            segment.start = 0;
        }
        if (GST_CLOCK_TIME_IS_VALID (segment.stop)) {
          gst_qtdemux_find_sample (demux, segment.stop, FALSE, FALSE, NULL,
              NULL, (gint64 *) & segment.stop);
          /* keyframe seeking should already arrange for start >= stop,
           * but make sure in other rare cases */
          segment.stop = MAX (segment.stop, segment.start);
        }
      } else if (segment.format == GST_FORMAT_TIME) {
        /* push all data on the adapter before starting this
         * new segment */
        gst_qtdemux_process_adapter (demux, TRUE);
      } else {
        GST_DEBUG_OBJECT (demux, "unsupported segment format, ignoring");
        goto exit;
      }

      /* We shouldn't modify upstream driven TIME FORMAT segment */
      if (!demux->upstream_format_is_time) {
        /* accept upstream's notion of segment and distribute along */
        segment.format = GST_FORMAT_TIME;
        segment.position = segment.time = segment.start;
        segment.duration = demux->segment.duration;
        segment.base = gst_segment_to_running_time (&demux->segment,
            GST_FORMAT_TIME, demux->segment.position);
      }

      gst_segment_copy_into (&segment, &demux->segment);
      GST_DEBUG_OBJECT (demux, "Pushing newseg %" GST_SEGMENT_FORMAT, &segment);

      /* map segment to internal qt segments and push on each stream */
      if (QTDEMUX_N_STREAMS (demux)) {
        demux->need_segment = TRUE;
        gst_qtdemux_check_send_pending_segment (demux);
      }

      /* clear leftover in current segment, if any */
      gst_adapter_clear (demux->adapter);

      /* set up streaming thread */
      demux->offset = offset;
      if (demux->upstream_format_is_time) {
        GST_DEBUG_OBJECT (demux, "Upstream is driving in time format, "
            "set values to restart reading from a new atom");
        demux->neededbytes = 16;
        demux->todrop = 0;
      } else {
        gst_qtdemux_find_sample (demux, offset, TRUE, TRUE, &stream, &idx,
            NULL);
        if (stream) {
          demux->todrop = stream->samples[idx].offset - offset;
          demux->neededbytes = demux->todrop + stream->samples[idx].size;
        } else {
          /* set up for EOS */
          demux->neededbytes = -1;
          demux->todrop = 0;
        }
      }
    exit:
      gst_event_unref (event);
      res = TRUE;
      goto drop;
    }
    case GST_EVENT_FLUSH_START:
    {
      if (gst_event_get_seqnum (event) == demux->offset_seek_seqnum) {
        gst_event_unref (event);
        goto drop;
      }
      QTDEMUX_EXPOSE_LOCK (demux);
      res = gst_pad_event_default (demux->sinkpad, parent, event);
      QTDEMUX_EXPOSE_UNLOCK (demux);
      goto drop;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      guint64 dur;

      dur = demux->segment.duration;
      gst_qtdemux_reset (demux, FALSE);
      demux->segment.duration = dur;

      if (gst_event_get_seqnum (event) == demux->offset_seek_seqnum) {
        gst_event_unref (event);
        goto drop;
      }
      break;
    }
    case GST_EVENT_EOS:
      /* If we are in push mode, and get an EOS before we've seen any streams,
       * then error out - we have nowhere to send the EOS */
      if (!demux->pullbased) {
        gint i;
        gboolean has_valid_stream = FALSE;
        for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
          if (QTDEMUX_NTH_STREAM (demux, i)->pad != NULL) {
            has_valid_stream = TRUE;
            break;
          }
        }
        if (!has_valid_stream)
          gst_qtdemux_post_no_playable_stream_error (demux);
        else {
          GST_DEBUG_OBJECT (demux, "Data still available after EOS: %u",
              (guint) gst_adapter_available (demux->adapter));
          if (gst_qtdemux_process_adapter (demux, TRUE) != GST_FLOW_OK) {
            res = FALSE;
          }
        }
      }
      break;
    case GST_EVENT_CAPS:{
      GstCaps *caps = NULL;

      gst_event_parse_caps (event, &caps);
      gst_qtdemux_setcaps (demux, caps);
      res = TRUE;
      gst_event_unref (event);
      goto drop;
    }
    case GST_EVENT_PROTECTION:
    {
      const gchar *system_id = NULL;

      gst_event_parse_protection (event, &system_id, NULL, NULL);
      GST_DEBUG_OBJECT (demux, "Received protection event for system ID %s",
          system_id);
      gst_qtdemux_append_protection_system_id (demux, system_id);
      /* save the event for later, for source pads that have not been created */
      g_queue_push_tail (&demux->protection_event_queue, gst_event_ref (event));
      /* send it to all pads that already exist */
      gst_qtdemux_push_event (demux, event);
      res = TRUE;
      goto drop;
    }
    case GST_EVENT_STREAM_START:
    {
      res = TRUE;
      gst_event_unref (event);

      /* Drain all the buffers */
      gst_qtdemux_process_adapter (demux, TRUE);
      gst_qtdemux_reset (demux, FALSE);
      /* We expect new moov box after new stream-start event */
      if (demux->exposed) {
        gst_qtdemux_stream_concat (demux,
            demux->old_streams, demux->active_streams);
      }

      goto drop;
    }
    default:
      break;
  }

  res = gst_pad_event_default (demux->sinkpad, parent, event) & res;

drop:
  return res;
}

static gboolean
gst_qtdemux_handle_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstQTDemux *demux = GST_QTDEMUX (parent);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_BITRATE:
    {
      GstClockTime duration;

      /* populate demux->upstream_size if not done yet */
      gst_qtdemux_check_seekability (demux);

      if (demux->upstream_size != -1
          && gst_qtdemux_get_duration (demux, &duration)) {
        guint bitrate =
            gst_util_uint64_scale (8 * demux->upstream_size, GST_SECOND,
            duration);

        GST_LOG_OBJECT (demux, "bitrate query byte length: %" G_GUINT64_FORMAT
            " duration %" GST_TIME_FORMAT " resulting a bitrate of %u",
            demux->upstream_size, GST_TIME_ARGS (duration), bitrate);

        /* TODO: better results based on ranges/index tables */
        gst_query_set_bitrate (query, bitrate);
        res = TRUE;
      }
      break;
    }
    default:
      res = gst_pad_query_default (pad, (GstObject *) demux, query);
      break;
  }

  return res;
}


#if 0
static void
gst_qtdemux_set_index (GstElement * element, GstIndex * index)
{
  GstQTDemux *demux = GST_QTDEMUX (element);

  GST_OBJECT_LOCK (demux);
  if (demux->element_index)
    gst_object_unref (demux->element_index);
  if (index) {
    demux->element_index = gst_object_ref (index);
  } else {
    demux->element_index = NULL;
  }
  GST_OBJECT_UNLOCK (demux);
  /* object lock might be taken again */
  if (index)
    gst_index_get_writer_id (index, GST_OBJECT (element), &demux->index_id);
  GST_DEBUG_OBJECT (demux, "Set index %" GST_PTR_FORMAT "for writer id %d",
      demux->element_index, demux->index_id);
}

static GstIndex *
gst_qtdemux_get_index (GstElement * element)
{
  GstIndex *result = NULL;
  GstQTDemux *demux = GST_QTDEMUX (element);

  GST_OBJECT_LOCK (demux);
  if (demux->element_index)
    result = gst_object_ref (demux->element_index);
  GST_OBJECT_UNLOCK (demux);

  GST_DEBUG_OBJECT (demux, "Returning index %" GST_PTR_FORMAT, result);

  return result;
}
#endif

static void
gst_qtdemux_stbl_free (QtDemuxStream * stream)
{
  g_free ((gpointer) stream->stco.data);
  stream->stco.data = NULL;
  g_free ((gpointer) stream->stsz.data);
  stream->stsz.data = NULL;
  g_free ((gpointer) stream->stsc.data);
  stream->stsc.data = NULL;
  g_free ((gpointer) stream->stts.data);
  stream->stts.data = NULL;
  g_free ((gpointer) stream->stss.data);
  stream->stss.data = NULL;
  g_free ((gpointer) stream->stps.data);
  stream->stps.data = NULL;
  g_free ((gpointer) stream->ctts.data);
  stream->ctts.data = NULL;
}

static void
gst_qtdemux_stream_flush_segments_data (QtDemuxStream * stream)
{
  g_free (stream->segments);
  stream->segments = NULL;
  stream->segment_index = -1;
  stream->accumulated_base = 0;
}

static void
gst_qtdemux_stream_flush_samples_data (QtDemuxStream * stream)
{
  g_free (stream->samples);
  stream->samples = NULL;
  gst_qtdemux_stbl_free (stream);

  /* fragments */
  g_free (stream->ra_entries);
  stream->ra_entries = NULL;
  stream->n_ra_entries = 0;

  stream->sample_index = -1;
  stream->stbl_index = -1;
  stream->n_samples = 0;
  stream->time_position = 0;

  stream->n_samples_moof = 0;
  stream->duration_moof = 0;
  stream->duration_last_moof = 0;
}

static void
gst_qtdemux_stream_clear (QtDemuxStream * stream)
{
  gint i;
  if (stream->allocator)
    gst_object_unref (stream->allocator);
  while (stream->buffers) {
    gst_buffer_unref (GST_BUFFER_CAST (stream->buffers->data));
    stream->buffers = g_slist_delete_link (stream->buffers, stream->buffers);
  }
  for (i = 0; i < stream->stsd_entries_length; i++) {
    QtDemuxStreamStsdEntry *entry = &stream->stsd_entries[i];
    if (entry->rgb8_palette) {
      gst_memory_unref (entry->rgb8_palette);
      entry->rgb8_palette = NULL;
    }
    entry->sparse = FALSE;
  }

  if (stream->stream_tags)
    gst_tag_list_unref (stream->stream_tags);

  stream->stream_tags = gst_tag_list_new_empty ();
  gst_tag_list_set_scope (stream->stream_tags, GST_TAG_SCOPE_STREAM);
  g_free (stream->redirect_uri);
  stream->redirect_uri = NULL;
  stream->sent_eos = FALSE;
  stream->protected = FALSE;
  if (stream->protection_scheme_info) {
    if (stream->protection_scheme_type == FOURCC_cenc
        || stream->protection_scheme_type == FOURCC_cbcs) {
      QtDemuxCencSampleSetInfo *info =
          (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;
      if (info->default_properties)
        gst_structure_free (info->default_properties);
      if (info->crypto_info)
        g_ptr_array_free (info->crypto_info, TRUE);
      if (info->fragment_group_properties)
        g_ptr_array_free (info->fragment_group_properties, TRUE);
      if (info->track_group_properties)
        g_ptr_array_free (info->track_group_properties, TRUE);
      if (info->sample_to_group_map)
        g_ptr_array_free (info->sample_to_group_map, FALSE);
    }
    if (stream->protection_scheme_type == FOURCC_aavd) {
      QtDemuxAavdEncryptionInfo *info =
          (QtDemuxAavdEncryptionInfo *) stream->protection_scheme_info;
      if (info->default_properties)
        gst_structure_free (info->default_properties);
    }
    g_free (stream->protection_scheme_info);
    stream->protection_scheme_info = NULL;
  }
  stream->protection_scheme_type = 0;
  stream->protection_scheme_version = 0;
  g_queue_clear_full (&stream->protection_scheme_event_queue,
      (GDestroyNotify) gst_event_unref);
  gst_qtdemux_stream_flush_segments_data (stream);
  gst_qtdemux_stream_flush_samples_data (stream);
}

static void
gst_qtdemux_stream_reset (QtDemuxStream * stream)
{
  gint i;
  gst_qtdemux_stream_clear (stream);
  for (i = 0; i < stream->stsd_entries_length; i++) {
    QtDemuxStreamStsdEntry *entry = &stream->stsd_entries[i];
    if (entry->caps) {
      gst_caps_unref (entry->caps);
      entry->caps = NULL;
    }
  }
  g_free (stream->stsd_entries);
  stream->stsd_entries = NULL;
  stream->stsd_entries_length = 0;
}

static QtDemuxStream *
gst_qtdemux_stream_ref (QtDemuxStream * stream)
{
  g_atomic_int_add (&stream->ref_count, 1);

  return stream;
}

static void
gst_qtdemux_stream_unref (QtDemuxStream * stream)
{
  if (g_atomic_int_dec_and_test (&stream->ref_count)) {
    gst_qtdemux_stream_reset (stream);
    gst_tag_list_unref (stream->stream_tags);
    if (stream->pad) {
      GstQTDemux *demux = stream->demux;
      gst_element_remove_pad (GST_ELEMENT_CAST (demux), stream->pad);
      GST_OBJECT_LOCK (demux);
      gst_flow_combiner_remove_pad (demux->flowcombiner, stream->pad);
      GST_OBJECT_UNLOCK (demux);
    }
    g_free (stream->stream_id);
    g_free (stream);
  }
}

static GstStateChangeReturn
gst_qtdemux_change_state (GstElement * element, GstStateChange transition)
{
  GstQTDemux *qtdemux = GST_QTDEMUX (element);
  GstStateChangeReturn result = GST_STATE_CHANGE_FAILURE;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_qtdemux_reset (qtdemux, TRUE);
      break;
    default:
      break;
  }

  result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:{
      gst_qtdemux_reset (qtdemux, TRUE);
      break;
    }
    default:
      break;
  }

  return result;
}

static void
gst_qtdemux_set_context (GstElement * element, GstContext * context)
{
  GstQTDemux *qtdemux = GST_QTDEMUX (element);

  g_return_if_fail (GST_IS_CONTEXT (context));

  if (gst_context_has_context_type (context,
          "drm-preferred-decryption-system-id")) {
    const GstStructure *s;

    s = gst_context_get_structure (context);
    g_free (qtdemux->preferred_protection_system_id);
    qtdemux->preferred_protection_system_id =
        g_strdup (gst_structure_get_string (s, "decryption-system-id"));
    GST_DEBUG_OBJECT (element, "set preferred decryption system to %s",
        qtdemux->preferred_protection_system_id);
  }

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static void
qtdemux_parse_ftyp (GstQTDemux * qtdemux, const guint8 * buffer, gint length)
{
  /* counts as header data */
  qtdemux->header_size += length;

  /* only consider at least a sufficiently complete ftyp atom */
  if (length >= 20) {
    GstBuffer *buf;
    guint32 minor_version;
    const guint8 *p;

    qtdemux->major_brand = QT_FOURCC (buffer + 8);
    GST_DEBUG_OBJECT (qtdemux, "ftyp major brand: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (qtdemux->major_brand));
    minor_version = QT_UINT32 (buffer + 12);
    GST_DEBUG_OBJECT (qtdemux, "ftyp minor version: %u", minor_version);
    if (qtdemux->comp_brands)
      gst_buffer_unref (qtdemux->comp_brands);
    buf = qtdemux->comp_brands = gst_buffer_new_and_alloc (length - 16);
    gst_buffer_fill (buf, 0, buffer + 16, length - 16);

    p = buffer + 16;
    length = length - 16;
    while (length > 0) {
      GST_DEBUG_OBJECT (qtdemux, "ftyp compatible brand: %" GST_FOURCC_FORMAT,
          GST_FOURCC_ARGS (QT_FOURCC (p)));
      length -= 4;
      p += 4;
    }
  }
}

static void
qtdemux_parse_styp (GstQTDemux * qtdemux, const guint8 * buffer, gint length)
{
  /* only consider at least a sufficiently complete styp atom */
  if (length >= 20) {
    GstBuffer *buf;
    guint32 major_brand;
    guint32 minor_version;
    const guint8 *p;

    major_brand = QT_FOURCC (buffer + 8);
    GST_DEBUG_OBJECT (qtdemux, "styp major brand: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (major_brand));
    minor_version = QT_UINT32 (buffer + 12);
    GST_DEBUG_OBJECT (qtdemux, "styp minor version: %u", minor_version);
    buf = qtdemux->comp_brands = gst_buffer_new_and_alloc (length - 16);
    gst_buffer_fill (buf, 0, buffer + 16, length - 16);

    p = buffer + 16;
    length = length - 16;
    while (length > 0) {
      GST_DEBUG_OBJECT (qtdemux, "styp compatible brand: %" GST_FOURCC_FORMAT,
          GST_FOURCC_ARGS (QT_FOURCC (p)));
      length -= 4;
      p += 4;
    }
  }
}

static void
qtdemux_update_default_sample_cenc_settings (GstQTDemux * qtdemux,
    QtDemuxCencSampleSetInfo * info, guint32 is_encrypted,
    guint32 protection_scheme_type, guint8 iv_size, const guint8 * kid,
    guint crypt_byte_block, guint skip_byte_block, guint8 constant_iv_size,
    const guint8 * constant_iv)
{
  GstBuffer *kid_buf = gst_buffer_new_allocate (NULL, 16, NULL);
  gst_buffer_fill (kid_buf, 0, kid, 16);
  if (info->default_properties)
    gst_structure_free (info->default_properties);
  info->default_properties =
      gst_structure_new ("application/x-cenc",
      "iv_size", G_TYPE_UINT, iv_size,
      "encrypted", G_TYPE_BOOLEAN, (is_encrypted == 1),
      "kid", GST_TYPE_BUFFER, kid_buf, NULL);
  GST_DEBUG_OBJECT (qtdemux, "default sample properties: "
      "is_encrypted=%u, iv_size=%u", is_encrypted, iv_size);
  gst_buffer_unref (kid_buf);
  if (protection_scheme_type == FOURCC_cbcs) {
    if (crypt_byte_block != 0 || skip_byte_block != 0) {
      gst_structure_set (info->default_properties, "crypt_byte_block",
          G_TYPE_UINT, crypt_byte_block, "skip_byte_block", G_TYPE_UINT,
          skip_byte_block, NULL);
    }
    if (constant_iv != NULL) {
      GstBuffer *constant_iv_buf =
          gst_buffer_new_allocate (NULL, constant_iv_size, NULL);
      gst_buffer_fill (constant_iv_buf, 0, constant_iv, constant_iv_size);
      gst_structure_set (info->default_properties, "constant_iv_size",
          G_TYPE_UINT, constant_iv_size, "iv", GST_TYPE_BUFFER, constant_iv_buf,
          NULL);
      gst_buffer_unref (constant_iv_buf);
    }
    gst_structure_set (info->default_properties, "cipher-mode",
        G_TYPE_STRING, "cbcs", NULL);
  } else {
    gst_structure_set (info->default_properties, "cipher-mode",
        G_TYPE_STRING, "cenc", NULL);
  }
}

static gboolean
qtdemux_update_default_piff_encryption_settings (GstQTDemux * qtdemux,
    QtDemuxCencSampleSetInfo * info, GstByteReader * br)
{
  guint32 algorithm_id = 0;
  const guint8 *kid;
  gboolean is_encrypted = TRUE;
  guint8 iv_size = 8;

  if (!gst_byte_reader_get_uint24_le (br, &algorithm_id)) {
    GST_ERROR_OBJECT (qtdemux, "Error getting box's algorithm ID field");
    return FALSE;
  }

  algorithm_id >>= 8;
  if (algorithm_id == 0) {
    is_encrypted = FALSE;
  } else if (algorithm_id == 1) {
    GST_DEBUG_OBJECT (qtdemux, "AES 128-bits CTR encrypted stream");
  } else if (algorithm_id == 2) {
    GST_DEBUG_OBJECT (qtdemux, "AES 128-bits CBC encrypted stream");
  }

  if (!gst_byte_reader_get_uint8 (br, &iv_size))
    return FALSE;

  if (!gst_byte_reader_get_data (br, 16, &kid))
    return FALSE;

  qtdemux_update_default_sample_cenc_settings (qtdemux, info,
      is_encrypted, FOURCC_cenc, iv_size, kid, 0, 0, 0, NULL);
  gst_structure_set (info->default_properties, "piff_algorithm_id",
      G_TYPE_UINT, algorithm_id, NULL);
  return TRUE;
}


static void
qtdemux_parse_piff (GstQTDemux * qtdemux, const guint8 * buffer, gint length,
    guint offset)
{
  GstByteReader br;
  guint8 version;
  guint32 flags = 0;
  guint i;
  guint iv_size = 8;
  QtDemuxStream *stream;
  GstStructure *structure;
  QtDemuxCencSampleSetInfo *ss_info = NULL;
  const gchar *system_id;
  gboolean uses_sub_sample_encryption = FALSE;
  guint32 sample_count;

  if (QTDEMUX_N_STREAMS (qtdemux) == 0)
    return;

  stream = QTDEMUX_NTH_STREAM (qtdemux, 0);

  structure = gst_caps_get_structure (CUR_STREAM (stream)->caps, 0);
  if (!gst_structure_has_name (structure, "application/x-cenc")) {
    GST_WARNING_OBJECT (qtdemux,
        "Attempting PIFF box parsing on an unencrypted stream.");
    return;
  }

  if (!gst_structure_get (structure, GST_PROTECTION_SYSTEM_ID_CAPS_FIELD,
          G_TYPE_STRING, &system_id, NULL)) {
    GST_WARNING_OBJECT (qtdemux, "%s field not present in caps",
        GST_PROTECTION_SYSTEM_ID_CAPS_FIELD);
    return;
  }

  gst_qtdemux_append_protection_system_id (qtdemux, system_id);

  stream->protected = TRUE;
  stream->protection_scheme_type = FOURCC_cenc;

  if (!stream->protection_scheme_info)
    stream->protection_scheme_info = g_new0 (QtDemuxCencSampleSetInfo, 1);

  ss_info = (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;
  if (!ss_info->default_properties) {
    ss_info->default_properties =
        gst_structure_new ("application/x-cenc",
        "iv_size", G_TYPE_UINT, iv_size, "encrypted", G_TYPE_BOOLEAN, TRUE,
        NULL);

  }

  if (ss_info->crypto_info) {
    GST_LOG_OBJECT (qtdemux, "unreffing existing crypto_info");
    g_ptr_array_free (ss_info->crypto_info, TRUE);
    ss_info->crypto_info = NULL;
  }

  /* skip UUID */
  gst_byte_reader_init (&br, buffer + offset + 16, length - offset - 16);

  if (!gst_byte_reader_get_uint8 (&br, &version)) {
    GST_ERROR_OBJECT (qtdemux, "Error getting box's version field");
    return;
  }

  if (!gst_byte_reader_get_uint24_be (&br, &flags)) {
    GST_ERROR_OBJECT (qtdemux, "Error getting box's flags field");
    return;
  }

  if ((flags & 0x000001)) {
    if (!qtdemux_update_default_piff_encryption_settings (qtdemux, ss_info,
            &br))
      return;
  } else if ((flags & 0x000002)) {
    uses_sub_sample_encryption = TRUE;
  }

  if (!gst_structure_get_uint (ss_info->default_properties, "iv_size",
          &iv_size)) {
    GST_ERROR_OBJECT (qtdemux, "Error getting encryption IV size field");
    return;
  }

  if (!gst_byte_reader_get_uint32_be (&br, &sample_count)) {
    GST_ERROR_OBJECT (qtdemux, "Error getting box's sample count field");
    return;
  }

  ss_info->crypto_info =
      g_ptr_array_new_full (sample_count,
      (GDestroyNotify) qtdemux_gst_structure_free);

  for (i = 0; i < sample_count; ++i) {
    GstStructure *properties;
    guint8 *data;
    GstBuffer *buf;

    properties = qtdemux_get_cenc_sample_properties (qtdemux, stream, i);
    if (properties == NULL) {
      GST_ERROR_OBJECT (qtdemux, "failed to get properties for sample %u", i);
      qtdemux->cenc_aux_sample_count = i;
      return;
    }

    if (!gst_byte_reader_dup_data (&br, iv_size, &data)) {
      GST_ERROR_OBJECT (qtdemux, "IV data not present for sample %u", i);
      gst_structure_free (properties);
      qtdemux->cenc_aux_sample_count = i;
      return;
    }
    buf = gst_buffer_new_wrapped (data, iv_size);
    gst_structure_set (properties, "iv", GST_TYPE_BUFFER, buf, NULL);
    gst_buffer_unref (buf);

    if (uses_sub_sample_encryption) {
      guint16 n_subsamples;
      const GValue *kid_buf_value;

      if (!gst_byte_reader_get_uint16_be (&br, &n_subsamples)
          || n_subsamples == 0) {
        GST_ERROR_OBJECT (qtdemux,
            "failed to get subsample count for sample %u", i);
        gst_structure_free (properties);
        qtdemux->cenc_aux_sample_count = i;
        return;
      }
      GST_LOG_OBJECT (qtdemux, "subsample count: %u", n_subsamples);
      if (!gst_byte_reader_dup_data (&br, n_subsamples * 6, &data)) {
        GST_ERROR_OBJECT (qtdemux, "failed to get subsample data for sample %u",
            i);
        gst_structure_free (properties);
        qtdemux->cenc_aux_sample_count = i;
        return;
      }
      buf = gst_buffer_new_wrapped (data, n_subsamples * 6);

      kid_buf_value =
          gst_structure_get_value (ss_info->default_properties, "kid");

      gst_structure_set (properties,
          "subsample_count", G_TYPE_UINT, n_subsamples,
          "subsamples", GST_TYPE_BUFFER, buf, NULL);
      gst_structure_set_value (properties, "kid", kid_buf_value);
      gst_buffer_unref (buf);
    } else {
      gst_structure_set (properties, "subsample_count", G_TYPE_UINT, 0, NULL);
    }

    g_ptr_array_add (ss_info->crypto_info, properties);
  }

  qtdemux->cenc_aux_sample_count = sample_count;
}

static void
qtdemux_parse_uuid (GstQTDemux * qtdemux, const guint8 * buffer, gint length)
{
  static const guint8 xmp_uuid[] = { 0xBE, 0x7A, 0xCF, 0xCB,
    0x97, 0xA9, 0x42, 0xE8,
    0x9C, 0x71, 0x99, 0x94,
    0x91, 0xE3, 0xAF, 0xAC
  };
  static const guint8 playready_uuid[] = {
    0xd0, 0x8a, 0x4f, 0x18, 0x10, 0xf3, 0x4a, 0x82,
    0xb6, 0xc8, 0x32, 0xd8, 0xab, 0xa1, 0x83, 0xd3
  };

  static const guint8 piff_sample_encryption_uuid[] = {
    0xa2, 0x39, 0x4f, 0x52, 0x5a, 0x9b, 0x4f, 0x14,
    0xa2, 0x44, 0x6c, 0x42, 0x7c, 0x64, 0x8d, 0xf4
  };

  guint offset;

  /* counts as header data */
  qtdemux->header_size += length;

  offset = (QT_UINT32 (buffer) == 0) ? 16 : 8;

  if (length <= offset + 16) {
    GST_DEBUG_OBJECT (qtdemux, "uuid atom is too short, skipping");
    return;
  }

  if (memcmp (buffer + offset, xmp_uuid, 16) == 0) {
    GstBuffer *buf;
    GstTagList *taglist;

    buf = _gst_buffer_new_wrapped ((guint8 *) buffer + offset + 16,
        length - offset - 16, NULL);
    taglist = gst_tag_list_from_xmp_buffer (buf);
    gst_buffer_unref (buf);

    /* make sure we have a usable taglist */
    qtdemux->tag_list = gst_tag_list_make_writable (qtdemux->tag_list);

    qtdemux_handle_xmp_taglist (qtdemux, qtdemux->tag_list, taglist);

  } else if (memcmp (buffer + offset, playready_uuid, 16) == 0) {
    int len;
    const gunichar2 *s_utf16;
    char *contents;

    len = GST_READ_UINT16_LE (buffer + offset + 0x30);
    s_utf16 = (const gunichar2 *) (buffer + offset + 0x32);
    contents = g_utf16_to_utf8 (s_utf16, len / 2, NULL, NULL, NULL);
    GST_ERROR_OBJECT (qtdemux, "contents: %s", contents);

    g_free (contents);

    GST_ELEMENT_ERROR (qtdemux, STREAM, DECRYPT,
        (_("Cannot play stream because it is encrypted with PlayReady DRM.")),
        (NULL));
  } else if (memcmp (buffer + offset, piff_sample_encryption_uuid, 16) == 0) {
    qtdemux_parse_piff (qtdemux, buffer, length, offset);
  } else {
    GST_DEBUG_OBJECT (qtdemux, "Ignoring unknown uuid: %08x-%08x-%08x-%08x",
        GST_READ_UINT32_LE (buffer + offset),
        GST_READ_UINT32_LE (buffer + offset + 4),
        GST_READ_UINT32_LE (buffer + offset + 8),
        GST_READ_UINT32_LE (buffer + offset + 12));
  }
}

static void
qtdemux_parse_sidx (GstQTDemux * qtdemux, const guint8 * buffer, gint length)
{
  GstSidxParser sidx_parser;
  GstIsoffParserResult res;
  guint consumed;

  gst_isoff_qt_sidx_parser_init (&sidx_parser);

  res =
      gst_isoff_qt_sidx_parser_add_data (&sidx_parser, buffer, length,
      &consumed);
  GST_DEBUG_OBJECT (qtdemux, "sidx parse result: %d", res);
  if (res == GST_ISOFF_QT_PARSER_DONE) {
    check_update_duration (qtdemux, sidx_parser.cumulative_pts);
  }
  gst_isoff_qt_sidx_parser_clear (&sidx_parser);
}

static void
qtdemux_parse_cstb (GstQTDemux * qtdemux, GstByteReader * data)
{
  guint64 start_time;
  guint32 entry_count;

  GST_DEBUG_OBJECT (qtdemux, "Parsing CorrectStartTime box");

  qtdemux->start_utc_time = GST_CLOCK_TIME_NONE;

  if (gst_byte_reader_get_remaining (data) < 4) {
    GST_WARNING_OBJECT (qtdemux, "Too small CorrectStartTime box");
    return;
  }

  entry_count = gst_byte_reader_get_uint32_be_unchecked (data);
  if (entry_count == 0)
    return;

  /* XXX: We assume that all start times are the same as different start times
   * would violate the MP4 synchronization model, so we just take the first
   * one here and apply it to all tracks.
   */

  if (gst_byte_reader_get_remaining (data) < entry_count * 12) {
    GST_WARNING_OBJECT (qtdemux, "Too small CorrectStartTime box");
    return;
  }

  /* Skip track id */
  gst_byte_reader_skip_unchecked (data, 4);

  /* In 100ns intervals */
  start_time = gst_byte_reader_get_uint64_be_unchecked (data);

  /* Convert from Jan 1 1601 to Jan 1 1970 */
  if (start_time < 11644473600 * G_GUINT64_CONSTANT (10000000)) {
    GST_WARNING_OBJECT (qtdemux, "Start UTC time before UNIX epoch");
    return;
  }
  start_time -= 11644473600 * G_GUINT64_CONSTANT (10000000);

  /* Convert to GstClockTime */
  start_time *= 100;

  GST_DEBUG_OBJECT (qtdemux, "Start UTC time: %" GST_TIME_FORMAT,
      GST_TIME_ARGS (start_time));

  qtdemux->start_utc_time = start_time;
}

/* caller verifies at least 8 bytes in buf */
static void
extract_initial_length_and_fourcc (const guint8 * data, gsize size,
    guint64 * plength, guint32 * pfourcc)
{
  guint64 length;
  guint32 fourcc;

  length = QT_UINT32 (data);
  GST_DEBUG ("length 0x%08" G_GINT64_MODIFIER "x", length);
  fourcc = QT_FOURCC (data + 4);
  GST_DEBUG ("atom type %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));

  if (length == 0) {
    length = G_MAXUINT64;
  } else if (length == 1 && size >= 16) {
    /* this means we have an extended size, which is the 64 bit value of
     * the next 8 bytes */
    length = QT_UINT64 (data + 8);
    GST_DEBUG ("length 0x%08" G_GINT64_MODIFIER "x", length);
  }

  if (plength)
    *plength = length;
  if (pfourcc)
    *pfourcc = fourcc;
}

static gboolean
qtdemux_parse_mehd (GstQTDemux * qtdemux, GstByteReader * br)
{
  guint32 version = 0;
  GstClockTime duration = 0;

  if (!gst_byte_reader_get_uint32_be (br, &version))
    goto failed;

  version >>= 24;
  if (version == 1) {
    if (!gst_byte_reader_get_uint64_be (br, &duration))
      goto failed;
  } else {
    guint32 dur = 0;

    if (!gst_byte_reader_get_uint32_be (br, &dur))
      goto failed;
    duration = dur;
  }

  GST_INFO_OBJECT (qtdemux, "mehd duration: %" G_GUINT64_FORMAT, duration);
  qtdemux->duration = duration;

  return TRUE;

failed:
  {
    GST_DEBUG_OBJECT (qtdemux, "parsing mehd failed");
    return FALSE;
  }
}

static gboolean
qtdemux_parse_trex (GstQTDemux * qtdemux, QtDemuxStream * stream,
    guint32 * ds_duration, guint32 * ds_size, guint32 * ds_flags)
{
  if (!stream->parsed_trex && qtdemux->moov_node) {
    GNode *mvex, *trex;
    GstByteReader trex_data;

    mvex = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_mvex);
    if (mvex) {
      trex = qtdemux_tree_get_child_by_type_full (mvex, FOURCC_trex,
          &trex_data);
      while (trex) {
        guint32 id = 0, sdi = 0, dur = 0, size = 0, flags = 0;

        /* skip version/flags */
        if (!gst_byte_reader_skip (&trex_data, 4))
          goto next;
        if (!gst_byte_reader_get_uint32_be (&trex_data, &id))
          goto next;
        if (id != stream->track_id)
          goto next;
        if (!gst_byte_reader_get_uint32_be (&trex_data, &sdi))
          goto next;
        if (!gst_byte_reader_get_uint32_be (&trex_data, &dur))
          goto next;
        if (!gst_byte_reader_get_uint32_be (&trex_data, &size))
          goto next;
        if (!gst_byte_reader_get_uint32_be (&trex_data, &flags))
          goto next;

        GST_DEBUG_OBJECT (qtdemux, "fragment defaults for stream %d; "
            "duration %d,  size %d, flags 0x%x", stream->track_id,
            dur, size, flags);

        stream->parsed_trex = TRUE;
        stream->def_sample_description_index = sdi;
        stream->def_sample_duration = dur;
        stream->def_sample_size = size;
        stream->def_sample_flags = flags;

      next:
        /* iterate all siblings */
        trex = qtdemux_tree_get_sibling_by_type_full (trex, FOURCC_trex,
            &trex_data);
      }
    }
  }

  *ds_duration = stream->def_sample_duration;
  *ds_size = stream->def_sample_size;
  *ds_flags = stream->def_sample_flags;

  /* even then, above values are better than random ... */
  if (G_UNLIKELY (!stream->parsed_trex)) {
    GST_INFO_OBJECT (qtdemux,
        "failed to find fragment defaults for stream %d", stream->track_id);
    return FALSE;
  }

  return TRUE;
}

/* This method should be called whenever a more accurate duration might
 * have been found. It will update all relevant variables if/where needed
 */
static void
check_update_duration (GstQTDemux * qtdemux, GstClockTime duration)
{
  guint i;
  guint64 movdur;
  GstClockTime prevdur;

  movdur = GSTTIME_TO_QTTIME (qtdemux, duration);

  if (movdur > qtdemux->duration) {
    prevdur = QTTIME_TO_GSTTIME (qtdemux, qtdemux->duration);
    GST_DEBUG_OBJECT (qtdemux,
        "Updating total duration to %" GST_TIME_FORMAT " was %" GST_TIME_FORMAT,
        GST_TIME_ARGS (duration), GST_TIME_ARGS (prevdur));
    qtdemux->duration = movdur;
    GST_DEBUG_OBJECT (qtdemux,
        "qtdemux->segment.duration: %" GST_TIME_FORMAT " .stop: %"
        GST_TIME_FORMAT, GST_TIME_ARGS (qtdemux->segment.duration),
        GST_TIME_ARGS (qtdemux->segment.stop));
    if (qtdemux->segment.duration == prevdur) {
      /* If the current segment has duration/stop identical to previous duration
       * update them also (because they were set at that point in time with
       * the wrong duration */
      /* We convert the value *from* the timescale version to avoid rounding errors */
      GstClockTime fixeddur = QTTIME_TO_GSTTIME (qtdemux, movdur);
      GST_DEBUG_OBJECT (qtdemux, "Updated segment.duration and segment.stop");
      qtdemux->segment.duration = fixeddur;
      qtdemux->segment.stop = fixeddur;
    }
  }

  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);

    movdur = GSTTIME_TO_QTSTREAMTIME (stream, duration);
    if (movdur > stream->duration) {
      GST_DEBUG_OBJECT (qtdemux,
          "Updating stream #%d duration to %" GST_TIME_FORMAT, i,
          GST_TIME_ARGS (duration));
      stream->duration = movdur;
      /* internal duration tracking state has been updated above, so */
      /* preserve an open-ended dummy segment rather than repeatedly updating
       * it and spamming downstream accordingly with segment events */
      /* also mangle the edit list end time when fragmented with a single edit
       * list that may only cover any non-fragmented data */
      if ((stream->dummy_segment ||
              (qtdemux->fragmented && stream->n_segments == 1)) &&
          GST_CLOCK_TIME_IS_VALID (stream->segments[0].duration)) {
        /* Update all dummy values to new duration */
        stream->segments[0].stop_time = duration;
        stream->segments[0].duration = duration;
        stream->segments[0].media_stop = duration;

        /* let downstream know we possibly have a new stop time */
        if (stream->segment_index != -1) {
          GstClockTime pos;

          if (qtdemux->segment.rate >= 0) {
            pos = stream->segment.start;
          } else {
            pos = stream->segment.stop;
          }

          gst_qtdemux_stream_update_segment (qtdemux, stream,
              stream->segment_index, pos, NULL, NULL);
        }
      }
    }
  }
}

static gboolean
qtdemux_parse_trun (GstQTDemux * qtdemux, GstByteReader * trun,
    QtDemuxStream * stream, guint32 d_sample_duration, guint32 d_sample_size,
    guint32 d_sample_flags, gint64 moof_offset, gint64 moof_length,
    gint64 * base_offset, gint64 * running_offset, gint64 decode_ts,
    gboolean has_tfdt, guint trun_node_total)
{
  GstClockTime gst_ts = GST_CLOCK_TIME_NONE;
  guint64 timestamp;
  gint32 data_offset = 0;
  guint8 version;
  guint32 flags = 0, first_flags = 0, samples_count = 0;
  gint i;
  guint8 *data;
  guint entry_size, dur_offset, size_offset, flags_offset = 0, ct_offset = 0;
  guint new_n_samples;
  QtDemuxSample *sample;
  gboolean ismv = FALSE;
  gint64 initial_offset;
  gint32 min_ct = 0;

  GST_LOG_OBJECT (qtdemux, "parsing trun track-id %u; "
      "default dur %u, size %u, flags 0x%x, base offset %" G_GINT64_FORMAT ", "
      "decode ts %" G_GINT64_FORMAT, stream->track_id, d_sample_duration,
      d_sample_size, d_sample_flags, *base_offset, decode_ts);

  if (stream->pending_seek && moof_offset < stream->pending_seek->moof_offset) {
    GST_INFO_OBJECT (stream->pad, "skipping trun before seek target fragment");
    return TRUE;
  }

  /* presence of stss or not can't really tell us much,
   * and flags and so on tend to be marginally reliable in these files */
  if (stream->subtype == FOURCC_soun) {
    GST_DEBUG_OBJECT (qtdemux,
        "sound track in fragmented file; marking all keyframes");
    stream->all_keyframe = TRUE;
  }

  if (!gst_byte_reader_get_uint8 (trun, &version) ||
      !gst_byte_reader_get_uint24_be (trun, &flags))
    goto fail;

  if (!gst_byte_reader_get_uint32_be (trun, &samples_count))
    goto fail;

  if (flags & TR_DATA_OFFSET) {
    /* note this is really signed */
    if (!gst_byte_reader_get_int32_be (trun, &data_offset))
      goto fail;

    if (qtdemux->variant == VARIANT_MSS_FRAGMENTED
        && data_offset <= moof_length
        && *base_offset == -1 && trun_node_total == 1) {
      /* MSS spec states that, if only one TrunBox is specified, then the
       * DataOffset field MUST be the sum of the lengths of the MoofBox and all
       * the fields in the MdatBox field */
      GST_WARNING_OBJECT (qtdemux,
          "trun offset is less than the moof size, assuming offset is after moof");
      data_offset = moof_length + 8;
    }
    GST_LOG_OBJECT (qtdemux, "trun data offset %u", data_offset);
    /* default base offset = first byte of moof */
    if (*base_offset == -1) {
      GST_LOG_OBJECT (qtdemux, "base_offset at moof");
      *base_offset = moof_offset;
    }
    *running_offset = *base_offset + data_offset;
  } else {
    /* if no offset at all, that would mean data starts at moof start,
     * which is a bit wrong and is ismv crappy way, so compensate
     * assuming data is in mdat following moof */
    if (*base_offset == -1) {
      *base_offset = moof_offset + moof_length + 8;
      GST_LOG_OBJECT (qtdemux, "base_offset assumed in mdat after moof");
      ismv = TRUE;
    }
    if (*running_offset == -1)
      *running_offset = *base_offset;
  }

  GST_LOG_OBJECT (qtdemux, "running offset now %" G_GINT64_FORMAT,
      *running_offset);
  GST_LOG_OBJECT (qtdemux, "trun offset %u, flags 0x%x, entries %u",
      data_offset, flags, samples_count);

  if (flags & TR_FIRST_SAMPLE_FLAGS) {
    if (G_UNLIKELY (flags & TR_SAMPLE_FLAGS)) {
      GST_DEBUG_OBJECT (qtdemux,
          "invalid flags; SAMPLE and FIRST_SAMPLE present, discarding latter");
      flags ^= TR_FIRST_SAMPLE_FLAGS;
    } else {
      if (!gst_byte_reader_get_uint32_be (trun, &first_flags))
        goto fail;
      GST_LOG_OBJECT (qtdemux, "first flags: 0x%x", first_flags);
    }
  }

  /* FIXME ? spec says other bits should also be checked to determine
   * entry size (and prefix size for that matter) */
  entry_size = 0;
  dur_offset = size_offset = 0;
  if (flags & TR_SAMPLE_DURATION) {
    GST_LOG_OBJECT (qtdemux, "entry duration present");
    dur_offset = entry_size;
    entry_size += 4;
  }
  if (flags & TR_SAMPLE_SIZE) {
    GST_LOG_OBJECT (qtdemux, "entry size present");
    size_offset = entry_size;
    entry_size += 4;
  }
  if (flags & TR_SAMPLE_FLAGS) {
    GST_LOG_OBJECT (qtdemux, "entry flags present");
    flags_offset = entry_size;
    entry_size += 4;
  }
  if (flags & TR_COMPOSITION_TIME_OFFSETS) {
    GST_LOG_OBJECT (qtdemux, "entry ct offset present");
    ct_offset = entry_size;
    entry_size += 4;
  }

  if (!qt_atom_parser_has_chunks (trun, samples_count, entry_size))
    goto fail;
  data = (guint8 *) gst_byte_reader_peek_data_unchecked (trun);

  if (!g_uint_checked_add (&new_n_samples, stream->n_samples, samples_count) ||
      new_n_samples >= QTDEMUX_MAX_SAMPLE_INDEX_SIZE / sizeof (QtDemuxSample))
    goto index_too_big;

  GST_DEBUG_OBJECT (qtdemux, "allocating n_samples %u * %u (%.2f MB)",
      new_n_samples, (guint) sizeof (QtDemuxSample),
      (new_n_samples) * sizeof (QtDemuxSample) / (1024.0 * 1024.0));

  /* create a new array of samples if it's the first sample parsed */
  if (stream->n_samples == 0) {
    g_assert (stream->samples == NULL);
    stream->samples = g_try_new0 (QtDemuxSample, samples_count);
    /* or try to reallocate it with space enough to insert the new samples */
  } else
    stream->samples = g_try_renew (QtDemuxSample, stream->samples,
        new_n_samples);
  if (stream->samples == NULL)
    goto out_of_memory;

  if (qtdemux->fragment_start != -1) {
    timestamp = GSTTIME_TO_QTSTREAMTIME (stream, qtdemux->fragment_start);
    qtdemux->fragment_start = -1;
  } else {
    if (stream->n_samples == 0) {
      if (decode_ts > 0) {
        timestamp = decode_ts;
      } else if (stream->pending_seek != NULL) {
        /* if we don't have a timestamp from a tfdt box, we'll use the one
         * from the mfra seek table */
        GST_INFO_OBJECT (stream->pad, "pending seek ts = %" GST_TIME_FORMAT,
            GST_TIME_ARGS (stream->pending_seek->ts));

        /* FIXME: this is not fully correct, the timestamp refers to the random
         * access sample refered to in the tfra entry, which may not necessarily
         * be the first sample in the tfrag/trun (but hopefully/usually is) */
        timestamp = GSTTIME_TO_QTSTREAMTIME (stream, stream->pending_seek->ts);
      } else {
        timestamp = 0;
      }

      gst_ts = QTSTREAMTIME_TO_GSTTIME (stream, timestamp);
      GST_INFO_OBJECT (stream->pad, "first sample ts %" GST_TIME_FORMAT,
          GST_TIME_ARGS (gst_ts));
    } else {
      /* If this is a GST_FORMAT_BYTES stream and we have a tfdt then use it
       * instead of the sum of sample durations */
      if (has_tfdt && !qtdemux->upstream_format_is_time) {
        timestamp = decode_ts;
        gst_ts = QTSTREAMTIME_TO_GSTTIME (stream, timestamp);
        GST_INFO_OBJECT (qtdemux, "first sample ts %" GST_TIME_FORMAT
            " (using tfdt)", GST_TIME_ARGS (gst_ts));
      } else {
        /* subsequent fragments extend stream */
        timestamp =
            stream->samples[stream->n_samples - 1].timestamp +
            stream->samples[stream->n_samples - 1].duration;
        gst_ts = QTSTREAMTIME_TO_GSTTIME (stream, timestamp);
        GST_INFO_OBJECT (qtdemux, "first sample ts %" GST_TIME_FORMAT
            " (extends previous samples)", GST_TIME_ARGS (gst_ts));
      }
    }
  }

  initial_offset = *running_offset;

  sample = stream->samples + stream->n_samples;
  for (i = 0; i < samples_count; i++) {
    guint32 dur, size, sflags;
    gint32 ct;

    /* first read sample data */
    if (flags & TR_SAMPLE_DURATION) {
      dur = QT_UINT32 (data + dur_offset);
    } else {
      dur = d_sample_duration;
    }
    if (flags & TR_SAMPLE_SIZE) {
      size = QT_UINT32 (data + size_offset);
    } else {
      size = d_sample_size;
    }
    if (flags & TR_FIRST_SAMPLE_FLAGS) {
      if (i == 0) {
        sflags = first_flags;
      } else {
        sflags = d_sample_flags;
      }
    } else if (flags & TR_SAMPLE_FLAGS) {
      sflags = QT_UINT32 (data + flags_offset);
    } else {
      sflags = d_sample_flags;
    }

    if (flags & TR_COMPOSITION_TIME_OFFSETS) {
      /* Read offsets as signed numbers regardless of trun version as very
       * high offsets are unlikely and there are files out there that use
       * version=0 truns with negative offsets */
      ct = QT_UINT32 (data + ct_offset);

      /* FIXME: Set offset to 0 for "no decode samples". This needs
       * to be handled in a codec specific manner ideally. */
      if (ct == G_MININT32)
        ct = 0;
    } else {
      ct = 0;
    }
    data += entry_size;

    /* fill the sample information */
    sample->offset = *running_offset;
    sample->pts_offset = ct;
    sample->size = size;
    sample->timestamp = timestamp;
    sample->duration = dur;
    /* sample-is-difference-sample */
    /* ismv seems to use 0x40 for keyframe, 0xc0 for non-keyframe,
     * now idea how it relates to bitfield other than massive LE/BE confusion */
    sample->keyframe = ismv ? ((sflags & 0xff) == 0x40) : !(sflags & 0x10000);
    *running_offset += size;
    timestamp += dur;
    stream->duration_moof += dur;
    sample++;

    if (ct < min_ct)
      min_ct = ct;
  }

  /* Shift PTS/DTS to allow for negative composition offsets while keeping
   * A/V sync in place. This is similar to the code handling ctts/cslg in the
   * non-fragmented case.
   */
  if (min_ct < 0)
    stream->cslg_shift = -min_ct;
  else
    stream->cslg_shift = 0;

  GST_DEBUG_OBJECT (qtdemux, "Using cslg_shift %" G_GUINT64_FORMAT,
      stream->cslg_shift);

  /* Update total duration if needed */
  check_update_duration (qtdemux, QTSTREAMTIME_TO_GSTTIME (stream, timestamp));

  /* Pre-emptively figure out size of mdat based on trun information.
   * If the [mdat] atom is effectivelly read, it will be replaced by the actual
   * size, else we will still be able to use this when dealing with gap'ed
   * input */
  qtdemux->mdatleft = *running_offset - initial_offset;
  qtdemux->mdatoffset = initial_offset;
  qtdemux->mdatsize = qtdemux->mdatleft;

  stream->n_samples += samples_count;
  stream->n_samples_moof += samples_count;

  if (stream->pending_seek != NULL)
    stream->pending_seek = NULL;

  return TRUE;

fail:
  {
    GST_WARNING_OBJECT (qtdemux, "failed to parse trun");
    return FALSE;
  }
out_of_memory:
  {
    GST_WARNING_OBJECT (qtdemux, "failed to allocate %u + %u samples",
        stream->n_samples, samples_count);
    return FALSE;
  }
index_too_big:
  {
    GST_WARNING_OBJECT (qtdemux,
        "not allocating index of %u + %u samples, would "
        "be larger than %uMB (broken file?)", stream->n_samples, samples_count,
        QTDEMUX_MAX_SAMPLE_INDEX_SIZE >> 20);
    return FALSE;
  }
}

/* find stream with @id */
static inline QtDemuxStream *
qtdemux_find_stream (GstQTDemux * qtdemux, guint32 id)
{
  QtDemuxStream *stream;
  gint i;

  /* check */
  if (G_UNLIKELY (!id)) {
    GST_DEBUG_OBJECT (qtdemux, "invalid track id 0");
    return NULL;
  }

  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    if (stream->track_id == id)
      return stream;
  }
  if (qtdemux->variant == VARIANT_MSS_FRAGMENTED) {
    /* mss should have only 1 stream anyway */
    return QTDEMUX_NTH_STREAM (qtdemux, 0);
  }

  return NULL;
}

static gboolean
qtdemux_parse_mfhd (GstQTDemux * qtdemux, GstByteReader * mfhd,
    guint32 * fragment_number)
{
  if (!gst_byte_reader_skip (mfhd, 4))
    goto fail;
  if (!gst_byte_reader_get_uint32_be (mfhd, fragment_number))
    goto fail;
  return TRUE;
fail:
  {
    GST_WARNING_OBJECT (qtdemux, "Failed to parse mfhd atom");
    return FALSE;
  }
}

static gboolean
qtdemux_parse_tfhd (GstQTDemux * qtdemux, GstByteReader * tfhd,
    QtDemuxStream ** stream, guint32 * default_sample_duration,
    guint32 * default_sample_size, guint32 * default_sample_flags,
    gint64 * base_offset)
{
  guint32 flags = 0;
  guint32 track_id = 0;

  if (!gst_byte_reader_skip (tfhd, 1) ||
      !gst_byte_reader_get_uint24_be (tfhd, &flags))
    goto invalid_track;

  if (!gst_byte_reader_get_uint32_be (tfhd, &track_id))
    goto invalid_track;

  *stream = qtdemux_find_stream (qtdemux, track_id);
  if (G_UNLIKELY (!*stream))
    goto unknown_stream;

  if (flags & TF_DEFAULT_BASE_IS_MOOF)
    *base_offset = qtdemux->moof_offset;

  if (flags & TF_BASE_DATA_OFFSET)
    if (!gst_byte_reader_get_uint64_be (tfhd, (guint64 *) base_offset))
      goto invalid_track;

  /* obtain stream defaults */
  if (qtdemux_parse_trex (qtdemux, *stream,
          default_sample_duration, default_sample_size, default_sample_flags)) {

    /* Default sample description index is only valid if trex parsing succeeded */
    (*stream)->stsd_sample_description_id =
        (*stream)->def_sample_description_index - 1;
  }

  if (flags & TF_SAMPLE_DESCRIPTION_INDEX) {
    guint32 sample_description_index;
    if (!gst_byte_reader_get_uint32_be (tfhd, &sample_description_index))
      goto invalid_track;
    (*stream)->stsd_sample_description_id = sample_description_index - 1;
  }

  if (qtdemux->variant == VARIANT_MSS_FRAGMENTED) {
    /* mss has no stsd entry */
    (*stream)->stsd_sample_description_id = 0;
  }

  if (flags & TF_DEFAULT_SAMPLE_DURATION)
    if (!gst_byte_reader_get_uint32_be (tfhd, default_sample_duration))
      goto invalid_track;

  if (flags & TF_DEFAULT_SAMPLE_SIZE)
    if (!gst_byte_reader_get_uint32_be (tfhd, default_sample_size))
      goto invalid_track;

  if (flags & TF_DEFAULT_SAMPLE_FLAGS)
    if (!gst_byte_reader_get_uint32_be (tfhd, default_sample_flags))
      goto invalid_track;

  return TRUE;

invalid_track:
  {
    GST_WARNING_OBJECT (qtdemux, "invalid track fragment header");
    return FALSE;
  }
unknown_stream:
  {
    GST_DEBUG_OBJECT (qtdemux, "unknown stream (%u) in tfhd", track_id);
    return TRUE;
  }
}

static gboolean
qtdemux_parse_tfdt (GstQTDemux * qtdemux, GstByteReader * br,
    guint64 * decode_time)
{
  guint32 version = 0;

  if (!gst_byte_reader_get_uint32_be (br, &version))
    return FALSE;

  version >>= 24;
  if (version == 1) {
    if (!gst_byte_reader_get_uint64_be (br, decode_time))
      goto failed;
  } else {
    guint32 dec_time = 0;
    if (!gst_byte_reader_get_uint32_be (br, &dec_time))
      goto failed;
    *decode_time = dec_time;
  }

  GST_INFO_OBJECT (qtdemux, "Track fragment decode time: %" G_GUINT64_FORMAT,
      *decode_time);

  return TRUE;

failed:
  {
    GST_DEBUG_OBJECT (qtdemux, "parsing tfdt failed");
    return FALSE;
  }
}

/* Returns a pointer to a GstStructure containing the properties of
 * the stream sample identified by @sample_index. The caller must unref
 * the returned object after use. Returns NULL if unsuccessful. */
static GstStructure *
qtdemux_get_cenc_sample_properties (GstQTDemux * qtdemux,
    QtDemuxStream * stream, guint sample_index)
{
  QtDemuxCencSampleSetInfo *info = NULL;
  GstStructure *properties = NULL;

  g_return_val_if_fail (stream != NULL, NULL);
  g_return_val_if_fail (stream->protected, NULL);
  g_return_val_if_fail (stream->protection_scheme_info != NULL, NULL);

  info = (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;

  /* First check if the sample is associated with the 'seig' sample group. */
  if (info->sample_to_group_map
      && sample_index < info->sample_to_group_map->len)
    properties = g_ptr_array_index (info->sample_to_group_map, sample_index);

  /* If not, use the default properties for this sample. */
  if (!properties)
    properties = info->default_properties;

  return gst_structure_copy (properties);
}

static gboolean
qtdemux_parse_sbgp (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstByteReader * br, guint32 group, GPtrArray ** sample_to_group_array,
    GstStructure * default_properties, GPtrArray * track_properties_array,
    GPtrArray * group_properties_array)
{
  guint32 flags = 0;
  guint8 version = 0;
  guint32 count = 0;
  const guint8 *grouping_type_data = NULL;
  guint32 grouping_type = 0;

  g_return_val_if_fail (qtdemux != NULL, FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (br != NULL, FALSE);
  g_return_val_if_fail (*sample_to_group_array == NULL, FALSE);
  g_return_val_if_fail (group_properties_array != NULL, FALSE);

  if (!gst_byte_reader_get_uint32_be (br, &flags))
    return FALSE;

  if (!gst_byte_reader_get_data (br, 4, &grouping_type_data))
    return FALSE;

  grouping_type = QT_FOURCC (grouping_type_data);
  if (grouping_type != group) {
    /* There may be other groups, so just log this... */
    GST_DEBUG_OBJECT (qtdemux, "Unsupported grouping type: '%"
        GST_FOURCC_FORMAT "'", GST_FOURCC_ARGS (grouping_type));
    return FALSE;
  }

  version = (flags >> 24);
  if (version > 0) {
    GST_WARNING_OBJECT (qtdemux, "Unsupported 'sbgp' box version: %hhu",
        version);
    return FALSE;
  }

  if (!gst_byte_reader_get_uint32_be (br, &count))
    return FALSE;

  GST_LOG_OBJECT (qtdemux, "flags: %08x, type: '%" GST_FOURCC_FORMAT
      "', count: %u", flags, GST_FOURCC_ARGS (grouping_type), count);

  if (count > 0)
    *sample_to_group_array = g_ptr_array_sized_new (count);

  while (count--) {
    guint32 samples;
    guint32 index;
    GstStructure *properties = NULL;

    if (!gst_byte_reader_get_uint32_be (br, &samples))
      goto error;

    if (!gst_byte_reader_get_uint32_be (br, &index))
      goto error;

    if (index > 0x10000) {
      /* Index is referring the current fragment. */
      index -= 0x10001;
      if (group_properties_array && index < group_properties_array->len)
        properties = g_ptr_array_index (group_properties_array, index);
      else
        GST_ERROR_OBJECT (qtdemux, "invalid group index %u", index);
    } else if (index > 0) {
      /* Index is referring to the whole track. */
      index--;
      if (track_properties_array && index < track_properties_array->len)
        properties = g_ptr_array_index (track_properties_array, index);
      else
        GST_ERROR_OBJECT (qtdemux, "invalid group index %u", index);
    } else {
      /* If zero, then this range of samples does not belong to this group,
         perhaps to another one or to none at all. */
    }

    GST_DEBUG_OBJECT (qtdemux, "assigning group '%" GST_FOURCC_FORMAT
        "' index %i for the next %i samples: %" GST_PTR_FORMAT,
        GST_FOURCC_ARGS (grouping_type), index, samples, properties);

    while (samples--)
      g_ptr_array_add (*sample_to_group_array, properties);
  }

  return TRUE;

error:
  g_ptr_array_free (*sample_to_group_array, TRUE);
  *sample_to_group_array = NULL;
  return FALSE;
}

static gboolean
qtdemux_parse_sgpd (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstByteReader * br, guint32 group, GPtrArray ** properties)
{
  guint32 flags = 0;
  guint8 version = 0;
  guint32 default_length = 0;
  guint32 count = 0;
  const guint8 *grouping_type_data = NULL;
  guint32 grouping_type = 0;
  const guint32 min_entry_size = 20;

  g_return_val_if_fail (qtdemux != NULL, FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (br != NULL, FALSE);
  g_return_val_if_fail (*properties == NULL, FALSE);

  if (!gst_byte_reader_get_uint32_be (br, &flags))
    return FALSE;

  if (!gst_byte_reader_get_data (br, 4, &grouping_type_data))
    return FALSE;

  grouping_type = QT_FOURCC (grouping_type_data);
  if (grouping_type != group) {
    GST_WARNING_OBJECT (qtdemux, "Unhandled grouping type: '%"
        GST_FOURCC_FORMAT "'", GST_FOURCC_ARGS (grouping_type));
    return FALSE;
  }

  version = (flags >> 24);
  if (version == 1) {
    if (!gst_byte_reader_get_uint32_be (br, &default_length))
      return FALSE;
  } else if (version > 1) {
    GST_WARNING_OBJECT (qtdemux, "Unsupported 'sgpd' box version: %hhu",
        version);
    return FALSE;
  }

  if (!gst_byte_reader_get_uint32_be (br, &count))
    return FALSE;

  GST_LOG_OBJECT (qtdemux, "flags: %08x, type: '%" GST_FOURCC_FORMAT
      "', count: %u", flags, GST_FOURCC_ARGS (grouping_type), count);

  if (count)
    *properties = g_ptr_array_sized_new (count);

  for (guint32 index = 0; index < count; index++) {
    GstStructure *props = NULL;
    guint32 length = default_length;
    const guint8 *entry_data = NULL;
    guint8 is_encrypted = 0;
    guint8 iv_size = 0;
    guint8 constant_iv_size = 0;
    const guint8 *kid = NULL;
    guint8 crypt_byte_block = 0;
    guint8 skip_byte_block = 0;
    const guint8 *constant_iv = NULL;
    GstBuffer *kid_buf;

    if (version == 1 && length == 0) {
      if (!gst_byte_reader_get_uint32_be (br, &length))
        goto error;
    }

    if (G_UNLIKELY (length < min_entry_size)) {
      GST_ERROR_OBJECT (qtdemux, "Invalid entry size: %u", length);
      goto error;
    }

    if (!gst_byte_reader_get_data (br, length, &entry_data))
      goto error;

    /* Follows tenc format... */
    is_encrypted = QT_UINT8 (entry_data + 2);
    iv_size = QT_UINT8 (entry_data + 3);
    kid = (entry_data + 4);

    if (stream->protection_scheme_type == FOURCC_cbcs) {
      guint8 possible_pattern_info;

      if (iv_size == 0) {
        if (G_UNLIKELY (length < min_entry_size + 1)) {
          GST_ERROR_OBJECT (qtdemux, "Invalid entry size: %u", length);
          goto error;
        }

        constant_iv_size = QT_UINT8 (entry_data + 20);
        if (G_UNLIKELY (constant_iv_size != 8 && constant_iv_size != 16)) {
          GST_ERROR_OBJECT (qtdemux,
              "constant IV size should be 8 or 16, not %hhu", constant_iv_size);
          goto error;
        }

        if (G_UNLIKELY (length < min_entry_size + 1 + constant_iv_size)) {
          GST_ERROR_OBJECT (qtdemux, "Invalid entry size: %u", length);
          goto error;
        }

        constant_iv = (entry_data + 21);
      }

      possible_pattern_info = QT_UINT8 (entry_data + 1);
      crypt_byte_block = (possible_pattern_info >> 4) & 0x0f;
      skip_byte_block = possible_pattern_info & 0x0f;
    }

    kid_buf = gst_buffer_new_memdup (kid, 16);

    props = gst_structure_new ("application/x-cenc",
        "iv_size", G_TYPE_UINT, iv_size,
        "encrypted", G_TYPE_BOOLEAN, is_encrypted == 1,
        "kid", GST_TYPE_BUFFER, kid_buf, NULL);

    gst_buffer_unref (kid_buf);

    if (stream->protection_scheme_type == FOURCC_cbcs) {
      if (crypt_byte_block != 0 || skip_byte_block != 0) {
        gst_structure_set (props,
            "crypt_byte_block", G_TYPE_UINT, crypt_byte_block,
            "skip_byte_block", G_TYPE_UINT, skip_byte_block, NULL);
      }

      if (constant_iv != NULL) {
        GstBuffer *constant_iv_buf = gst_buffer_new_memdup (
            (guint8 *) constant_iv, constant_iv_size);
        gst_structure_set (props,
            "constant_iv_size", G_TYPE_UINT, constant_iv_size,
            "iv", GST_TYPE_BUFFER, constant_iv_buf, NULL);
        gst_buffer_unref (constant_iv_buf);
      }

      gst_structure_set (props, "cipher-mode", G_TYPE_STRING, "cbcs", NULL);
    } else {
      gst_structure_set (props, "cipher-mode", G_TYPE_STRING, "cenc", NULL);
    }

    GST_INFO_OBJECT (qtdemux, "properties for group '%"
        GST_FOURCC_FORMAT "' at index %u: %" GST_PTR_FORMAT,
        GST_FOURCC_ARGS (grouping_type), index, props);

    g_ptr_array_add (*properties, props);
  }

  return TRUE;

error:
  g_ptr_array_free (*properties, TRUE);
  *properties = NULL;
  return FALSE;
}

/* Parses the sizes of sample auxiliary information contained within a stream,
 * as given in a saiz box. Returns array of sample_count guint8 size values,
 * or NULL on failure */
static guint8 *
qtdemux_parse_saiz (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstByteReader * br, guint32 * sample_count)
{
  guint32 flags = 0;
  guint8 *info_sizes;
  guint8 default_info_size;

  g_return_val_if_fail (qtdemux != NULL, NULL);
  g_return_val_if_fail (stream != NULL, NULL);
  g_return_val_if_fail (br != NULL, NULL);
  g_return_val_if_fail (sample_count != NULL, NULL);

  if (!gst_byte_reader_get_uint32_be (br, &flags))
    return NULL;

  if (flags & 0x1) {
    /* aux_info_type and aux_info_type_parameter are ignored */
    if (!gst_byte_reader_skip (br, 8))
      return NULL;
  }

  if (!gst_byte_reader_get_uint8 (br, &default_info_size))
    return NULL;
  GST_DEBUG_OBJECT (qtdemux, "default_info_size: %u", default_info_size);

  if (!gst_byte_reader_get_uint32_be (br, sample_count))
    return NULL;
  GST_DEBUG_OBJECT (qtdemux, "sample_count: %u", *sample_count);


  if (default_info_size == 0) {
    if (!gst_byte_reader_dup_data (br, *sample_count, &info_sizes)) {
      return NULL;
    }
  } else {
    info_sizes = g_new (guint8, *sample_count);
    memset (info_sizes, default_info_size, *sample_count);
  }

  return info_sizes;
}

/* Parses the offset of sample auxiliary information contained within a stream,
 * as given in a saio box. Returns TRUE if successful; FALSE otherwise. */
static gboolean
qtdemux_parse_saio (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstByteReader * br, guint32 * info_type, guint32 * info_type_parameter,
    guint64 * offset)
{
  guint8 version = 0;
  guint32 flags = 0;
  guint32 aux_info_type = 0;
  guint32 aux_info_type_parameter = 0;
  guint32 entry_count;
  guint32 off_32;
  guint64 off_64;
  const guint8 *aux_info_type_data = NULL;

  g_return_val_if_fail (qtdemux != NULL, FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (br != NULL, FALSE);
  g_return_val_if_fail (offset != NULL, FALSE);

  if (!gst_byte_reader_get_uint8 (br, &version))
    return FALSE;

  if (!gst_byte_reader_get_uint24_be (br, &flags))
    return FALSE;

  if (flags & 0x1) {

    if (!gst_byte_reader_get_data (br, 4, &aux_info_type_data))
      return FALSE;
    aux_info_type = QT_FOURCC (aux_info_type_data);

    if (!gst_byte_reader_get_uint32_be (br, &aux_info_type_parameter))
      return FALSE;
  } else if (stream->protected) {
    aux_info_type = stream->protection_scheme_type;
  } else {
    aux_info_type = CUR_STREAM (stream)->fourcc;
  }

  if (info_type)
    *info_type = aux_info_type;
  if (info_type_parameter)
    *info_type_parameter = aux_info_type_parameter;

  GST_DEBUG_OBJECT (qtdemux, "aux_info_type: '%" GST_FOURCC_FORMAT "', "
      "aux_info_type_parameter:  %#06x",
      GST_FOURCC_ARGS (aux_info_type), aux_info_type_parameter);

  if (!gst_byte_reader_get_uint32_be (br, &entry_count))
    return FALSE;

  if (entry_count != 1) {
    GST_ERROR_OBJECT (qtdemux, "multiple offsets are not supported");
    return FALSE;
  }

  if (version == 0) {
    if (!gst_byte_reader_get_uint32_be (br, &off_32))
      return FALSE;
    *offset = (guint64) off_32;
  } else {
    if (!gst_byte_reader_get_uint64_be (br, &off_64))
      return FALSE;
    *offset = off_64;
  }

  GST_DEBUG_OBJECT (qtdemux, "offset: %" G_GUINT64_FORMAT, *offset);
  return TRUE;
}

static void
qtdemux_gst_structure_free (GstStructure * gststructure)
{
  if (gststructure) {
    gst_structure_free (gststructure);
  }
}

/* Parses auxiliary information relating to samples protected using
 * Common Encryption (cenc); the format of this information
 * is defined in ISO/IEC 23001-7. Returns TRUE if successful; FALSE
 * otherwise. */
static gboolean
qtdemux_parse_cenc_aux_info (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstByteReader * br, guint8 * info_sizes, guint32 sample_count)
{
  QtDemuxCencSampleSetInfo *ss_info = NULL;
  guint8 size;
  gint i;
  GPtrArray *old_crypto_info = NULL;
  guint old_entries = 0;

  g_return_val_if_fail (qtdemux != NULL, FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (br != NULL, FALSE);
  g_return_val_if_fail (stream->protected, FALSE);
  g_return_val_if_fail (stream->protection_scheme_info != NULL, FALSE);

  ss_info = (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;

  if (ss_info->crypto_info) {
    old_crypto_info = ss_info->crypto_info;
    /* Count number of non-null entries remaining at the tail end */
    for (i = old_crypto_info->len - 1; i >= 0; i--) {
      if (g_ptr_array_index (old_crypto_info, i) == NULL)
        break;
      old_entries++;
    }
  }

  ss_info->crypto_info =
      g_ptr_array_new_full (sample_count + old_entries,
      (GDestroyNotify) qtdemux_gst_structure_free);

  /* We preserve old entries because we parse the next moof in advance
   * of consuming all samples from the previous moof, and otherwise
   * we'd discard the corresponding crypto info for the samples
   * from the previous fragment. */
  if (old_entries) {
    GST_DEBUG_OBJECT (qtdemux, "Preserving %d old crypto info entries",
        old_entries);
    for (i = old_crypto_info->len - old_entries; i < old_crypto_info->len; i++) {
      g_ptr_array_add (ss_info->crypto_info, g_ptr_array_index (old_crypto_info,
              i));
      g_ptr_array_index (old_crypto_info, i) = NULL;
    }
  }

  if (old_crypto_info) {
    /* Everything now belongs to the new array */
    g_ptr_array_free (old_crypto_info, TRUE);
  }

  for (i = 0; i < sample_count; ++i) {
    GstStructure *properties;
    guint16 n_subsamples = 0;
    guint8 *data;
    guint iv_size;
    GstBuffer *buf;
    gboolean could_read_iv;

    properties = qtdemux_get_cenc_sample_properties (qtdemux, stream, i);
    if (properties == NULL) {
      GST_ERROR_OBJECT (qtdemux, "failed to get properties for sample %u", i);
      return FALSE;
    }
    if (!gst_structure_get_uint (properties, "iv_size", &iv_size)) {
      GST_ERROR_OBJECT (qtdemux, "failed to get iv_size for sample %u", i);
      gst_structure_free (properties);
      return FALSE;
    }
    could_read_iv =
        iv_size > 0 ? gst_byte_reader_dup_data (br, iv_size, &data) : FALSE;
    if (could_read_iv) {
      buf = gst_buffer_new_wrapped (data, iv_size);
      gst_structure_set (properties, "iv", GST_TYPE_BUFFER, buf, NULL);
      gst_buffer_unref (buf);
    } else if (stream->protection_scheme_type == FOURCC_cbcs) {
      const GValue *constant_iv_size_value =
          gst_structure_get_value (properties, "constant_iv_size");
      const GValue *constant_iv_value =
          gst_structure_get_value (properties, "iv");
      if (constant_iv_size_value == NULL || constant_iv_value == NULL) {
        GST_ERROR_OBJECT (qtdemux, "failed to get constant_iv");
        gst_structure_free (properties);
        return FALSE;
      }
      gst_structure_set_value (properties, "iv_size", constant_iv_size_value);
      gst_structure_remove_field (properties, "constant_iv_size");
    } else if (stream->protection_scheme_type == FOURCC_cenc) {
      GST_ERROR_OBJECT (qtdemux, "failed to get IV for sample %u", i);
      gst_structure_free (properties);
      return FALSE;
    }
    size = info_sizes[i];
    if (size > iv_size) {
      if (!gst_byte_reader_get_uint16_be (br, &n_subsamples)
          || !(n_subsamples > 0)) {
        gst_structure_free (properties);
        GST_ERROR_OBJECT (qtdemux,
            "failed to get subsample count for sample %u", i);
        return FALSE;
      }
      GST_LOG_OBJECT (qtdemux, "subsample count: %u", n_subsamples);
      if (!gst_byte_reader_dup_data (br, n_subsamples * 6, &data)) {
        GST_ERROR_OBJECT (qtdemux, "failed to get subsample data for sample %u",
            i);
        gst_structure_free (properties);
        return FALSE;
      }
      buf = gst_buffer_new_wrapped (data, n_subsamples * 6);
      if (!buf) {
        gst_structure_free (properties);
        return FALSE;
      }
      gst_structure_set (properties,
          "subsample_count", G_TYPE_UINT, n_subsamples,
          "subsamples", GST_TYPE_BUFFER, buf, NULL);
      gst_buffer_unref (buf);
    } else {
      gst_structure_set (properties, "subsample_count", G_TYPE_UINT, 0, NULL);
    }
    g_ptr_array_add (ss_info->crypto_info, properties);
  }
  return TRUE;
}

/* Converts a UUID in raw byte form to a string representation, as defined in
 * RFC 4122. The caller takes ownership of the returned string and is
 * responsible for freeing it after use. */
static gchar *
qtdemux_uuid_bytes_to_string (gconstpointer uuid_bytes)
{
  const guint8 *uuid = (const guint8 *) uuid_bytes;

  return g_strdup_printf ("%02x%02x%02x%02x-%02x%02x-%02x%02x-"
      "%02x%02x-%02x%02x%02x%02x%02x%02x",
      uuid[0], uuid[1], uuid[2], uuid[3],
      uuid[4], uuid[5], uuid[6], uuid[7],
      uuid[8], uuid[9], uuid[10], uuid[11],
      uuid[12], uuid[13], uuid[14], uuid[15]);
}

/* Parses a Protection System Specific Header box (pssh), as defined in the
 * Common Encryption (cenc) standard (ISO/IEC 23001-7), which contains
 * information needed by a specific content protection system in order to
 * decrypt cenc-protected tracks. Returns TRUE if successful; FALSE
 * otherwise. */
static gboolean
qtdemux_parse_pssh (GstQTDemux * qtdemux, GNode * node)
{
  gchar *sysid_string;
  guint32 pssh_size = QT_UINT32 (node->data);
  GstBuffer *pssh = NULL;
  GstEvent *event = NULL;
  guint32 parent_box_type;
  gint i;

  if (G_UNLIKELY (pssh_size < 32U)) {
    GST_ERROR_OBJECT (qtdemux, "invalid box size");
    return FALSE;
  }

  sysid_string =
      qtdemux_uuid_bytes_to_string ((const guint8 *) node->data + 12);

  gst_qtdemux_append_protection_system_id (qtdemux, sysid_string);

  pssh = gst_buffer_new_memdup (node->data, pssh_size);
  GST_LOG_OBJECT (qtdemux, "cenc pssh size: %" G_GSIZE_FORMAT,
      gst_buffer_get_size (pssh));

  parent_box_type = QT_FOURCC ((const guint8 *) node->parent->data + 4);

  /* Push an event containing the pssh box onto the queues of all streams. */
  event = gst_event_new_protection (sysid_string, pssh,
      (parent_box_type == FOURCC_moov) ? "isobmff/moov" : "isobmff/moof");
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    GST_TRACE_OBJECT (qtdemux,
        "adding protection event for stream %s and system %s",
        stream->stream_id, sysid_string);
    g_queue_push_tail (&stream->protection_scheme_event_queue,
        gst_event_ref (event));
  }
  g_free (sysid_string);
  gst_event_unref (event);
  gst_buffer_unref (pssh);
  return TRUE;
}

static gboolean
qtdemux_parse_moof (GstQTDemux * qtdemux, const guint8 * buffer, guint length,
    guint64 moof_offset, QtDemuxStream * stream)
{
  GNode *moof_node, *traf_node, *tfhd_node, *trun_node, *tfdt_node, *mfhd_node;
  GNode *uuid_node;
  GstByteReader mfhd_data, trun_data, tfhd_data, tfdt_data;
  GNode *saiz_node, *saio_node, *pssh_node;
  GstByteReader saiz_data, saio_data;
  guint32 ds_size = 0, ds_duration = 0, ds_flags = 0;
  gint64 base_offset, running_offset;
  guint32 frag_num;
  GstClockTime min_dts = GST_CLOCK_TIME_NONE;

  /* NOTE @stream ignored */

  moof_node = g_node_new ((guint8 *) buffer);
  qtdemux_parse_node (qtdemux, moof_node, buffer, length);
  qtdemux_node_dump (qtdemux, moof_node);

  /* Get fragment number from mfhd and check it's valid */
  mfhd_node =
      qtdemux_tree_get_child_by_type_full (moof_node, FOURCC_mfhd, &mfhd_data);
  if (mfhd_node == NULL)
    goto missing_mfhd;
  if (!qtdemux_parse_mfhd (qtdemux, &mfhd_data, &frag_num))
    goto fail;
  GST_DEBUG_OBJECT (qtdemux, "Fragment #%d", frag_num);

  /* unknown base_offset to start with */
  base_offset = running_offset = -1;
  traf_node = qtdemux_tree_get_child_by_type (moof_node, FOURCC_traf);
  while (traf_node) {
    guint64 decode_time = 0;
    guint trun_node_total = 0;

    /* Fragment Header node */
    tfhd_node =
        qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_tfhd,
        &tfhd_data);
    if (!tfhd_node)
      goto missing_tfhd;
    if (!qtdemux_parse_tfhd (qtdemux, &tfhd_data, &stream, &ds_duration,
            &ds_size, &ds_flags, &base_offset))
      goto missing_tfhd;

    /* Sample grouping support */
    if (stream != NULL && stream->protected
        && (stream->protection_scheme_type == FOURCC_cenc
            || stream->protection_scheme_type == FOURCC_cbcs)) {
      QtDemuxCencSampleSetInfo *info = stream->protection_scheme_info;
      GNode *sgpd_node;
      GstByteReader sgpd_data;

      if (!info) {
        GST_ERROR_OBJECT (qtdemux, "Have no valid protection scheme info");
        goto fail;
      }

      if (info->fragment_group_properties) {
        g_ptr_array_free (info->fragment_group_properties, TRUE);
        info->fragment_group_properties = NULL;
      }

      if (info->sample_to_group_map) {
        g_ptr_array_free (info->sample_to_group_map, FALSE);
        info->sample_to_group_map = NULL;
      }

      /* Check if sample grouping information is present for this segment. */
      /* However look only for 'seig' (CENC encryption) grouping type... */
      sgpd_node = qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_sgpd,
          &sgpd_data);
      while (sgpd_node) {
        if (qtdemux_parse_sgpd (qtdemux, stream, &sgpd_data, FOURCC_seig,
                &info->fragment_group_properties)) {
          /* CENC encryption grouping found, don't look further. */
          break;
        }
        sgpd_node = qtdemux_tree_get_sibling_by_type_full (sgpd_node,
            FOURCC_sgpd, &sgpd_data);
      }

      if (info->fragment_group_properties) {
        GstByteReader sbgp_data;
        GNode *sbgp_node = qtdemux_tree_get_child_by_type_full (traf_node,
            FOURCC_sbgp, &sbgp_data);
        while (sbgp_node) {
          if (qtdemux_parse_sbgp (qtdemux, stream, &sbgp_data, FOURCC_seig,
                  &info->sample_to_group_map, info->default_properties,
                  info->track_group_properties,
                  info->fragment_group_properties)) {
            break;
          }
          sbgp_node = qtdemux_tree_get_sibling_by_type_full (sbgp_node,
              FOURCC_sbgp, &sbgp_data);
        }
      }
    }

    /* The following code assumes at most a single set of sample auxiliary
     * data in the fragment (consisting of a saiz box and a corresponding saio
     * box); in theory, however, there could be multiple sets of sample
     * auxiliary data in a fragment. */
    saiz_node =
        qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_saiz,
        &saiz_data);
    if (saiz_node) {
      guint32 info_type = 0;
      guint64 offset = 0;
      guint32 info_type_parameter = 0;

      g_free (qtdemux->cenc_aux_info_sizes);

      qtdemux->cenc_aux_info_sizes =
          qtdemux_parse_saiz (qtdemux, stream, &saiz_data,
          &qtdemux->cenc_aux_sample_count);
      if (qtdemux->cenc_aux_info_sizes == NULL) {
        GST_ERROR_OBJECT (qtdemux, "failed to parse saiz box");
        goto fail;
      }
      saio_node =
          qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_saio,
          &saio_data);
      if (!saio_node) {
        GST_ERROR_OBJECT (qtdemux, "saiz box without a corresponding saio box");
        g_free (qtdemux->cenc_aux_info_sizes);
        qtdemux->cenc_aux_info_sizes = NULL;
        goto fail;
      }

      if (G_UNLIKELY (!qtdemux_parse_saio (qtdemux, stream, &saio_data,
                  &info_type, &info_type_parameter, &offset))) {
        GST_ERROR_OBJECT (qtdemux, "failed to parse saio box");
        g_free (qtdemux->cenc_aux_info_sizes);
        qtdemux->cenc_aux_info_sizes = NULL;
        goto fail;
      }
      if (base_offset > -1 && base_offset > qtdemux->moof_offset)
        offset += (guint64) (base_offset - qtdemux->moof_offset);
      if ((info_type == FOURCC_cenc || info_type == FOURCC_cbcs)
          && info_type_parameter == 0U) {
        GstByteReader br;
        if (offset > length) {
          GST_DEBUG_OBJECT (qtdemux, "cenc auxiliary info stored out of moof");
          qtdemux->cenc_aux_info_offset = offset;
        } else {
          gst_byte_reader_init (&br, buffer + offset, length - offset);
          if (!qtdemux_parse_cenc_aux_info (qtdemux, stream, &br,
                  qtdemux->cenc_aux_info_sizes,
                  qtdemux->cenc_aux_sample_count)) {
            GST_ERROR_OBJECT (qtdemux, "failed to parse cenc auxiliary info");
            g_free (qtdemux->cenc_aux_info_sizes);
            qtdemux->cenc_aux_info_sizes = NULL;
            goto fail;
          }
        }
      }
    }

    tfdt_node =
        qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_tfdt,
        &tfdt_data);
    if (tfdt_node) {
      /* We'll use decode_time to interpolate timestamps
       * in case the input timestamps are missing */
      qtdemux_parse_tfdt (qtdemux, &tfdt_data, &decode_time);

      GST_DEBUG_OBJECT (qtdemux, "decode time %" G_GINT64_FORMAT
          " (%" GST_TIME_FORMAT ")", decode_time,
          GST_TIME_ARGS (stream ? QTSTREAMTIME_TO_GSTTIME (stream,
                  decode_time) : GST_CLOCK_TIME_NONE));

      /* Discard the fragment buffer timestamp info to avoid using it.
       * Rely on tfdt instead as it is more accurate than the timestamp
       * that is fetched from a manifest/playlist and is usually
       * less accurate. */
      qtdemux->fragment_start = -1;
    }

    if (G_UNLIKELY (!stream)) {
      /* we lost track of offset, we'll need to regain it,
       * but can delay complaining until later or avoid doing so altogether */
      base_offset = -2;
      goto next;
    }
    if (G_UNLIKELY (base_offset < -1))
      goto lost_offset;

    min_dts = MIN (min_dts, QTSTREAMTIME_TO_GSTTIME (stream, decode_time));

    if (!qtdemux->pullbased) {
      /* Sample tables can grow enough to be problematic if the system memory
       * is very low (e.g. embedded devices) and the videos very long
       * (~8 MiB/hour for 25-30 fps video + typical AAC audio frames).
       * Fortunately, we can easily discard them for each new fragment when
       * we know qtdemux will not receive seeks outside of the current fragment.
       * adaptivedemux honors this assumption.
       * This optimization is also useful for applications that use qtdemux as
       * a push-based simple demuxer, like Media Source Extensions. */
      gst_qtdemux_stream_flush_samples_data (stream);
    }

    /* initialise moof sample data */
    stream->n_samples_moof = 0;
    stream->duration_last_moof = stream->duration_moof;
    stream->duration_moof = 0;

    /* Count the number of trun nodes */
    if (qtdemux->variant == VARIANT_MSS_FRAGMENTED) {
      trun_node =
          qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_trun,
          &trun_data);
      while (trun_node) {
        trun_node_total++;
        trun_node =
            qtdemux_tree_get_sibling_by_type_full (trun_node, FOURCC_trun,
            &trun_data);
      }
      GST_LOG_OBJECT (qtdemux, "%u trun node(s) available", trun_node_total);
    }

    /* Track Run node */
    trun_node =
        qtdemux_tree_get_child_by_type_full (traf_node, FOURCC_trun,
        &trun_data);
    while (trun_node) {
      qtdemux_parse_trun (qtdemux, &trun_data, stream,
          ds_duration, ds_size, ds_flags, moof_offset, length, &base_offset,
          &running_offset, decode_time, (tfdt_node != NULL), trun_node_total);
      /* iterate all siblings */
      trun_node = qtdemux_tree_get_sibling_by_type_full (trun_node, FOURCC_trun,
          &trun_data);
      /* don't use tfdt for subsequent trun as it only refers to the first */
      tfdt_node = NULL;
    }

    uuid_node = qtdemux_tree_get_child_by_type (traf_node, FOURCC_uuid);
    if (uuid_node) {
      guint8 *uuid_buffer = (guint8 *) uuid_node->data;
      guint32 box_length = QT_UINT32 (uuid_buffer);

      qtdemux_parse_uuid (qtdemux, uuid_buffer, box_length);
    }

    /* if no new base_offset provided for next traf,
     * base is end of current traf */
    base_offset = running_offset;
    running_offset = -1;

    if (stream->n_samples_moof && stream->duration_moof)
      stream->new_caps = TRUE;

  next:
    /* iterate all siblings */
    traf_node = qtdemux_tree_get_sibling_by_type (traf_node, FOURCC_traf);
  }

  /* parse any protection system info */
  pssh_node = qtdemux_tree_get_child_by_type (moof_node, FOURCC_pssh);
  if (pssh_node) {
    /* Unref old protection events if we are going to receive new ones. */
    qtdemux_clear_protection_events_on_all_streams (qtdemux);
  }
  while (pssh_node) {
    GST_LOG_OBJECT (qtdemux, "Parsing pssh box.");
    qtdemux_parse_pssh (qtdemux, pssh_node);
    pssh_node = qtdemux_tree_get_sibling_by_type (pssh_node, FOURCC_pssh);
  }

  if (!qtdemux->upstream_format_is_time
      && qtdemux->variant != VARIANT_MSE_BYTESTREAM
      && !qtdemux->first_moof_already_parsed
      && !qtdemux->received_seek && GST_CLOCK_TIME_IS_VALID (min_dts)
      && min_dts != 0) {
    /* Unless the user has explicitly requested another seek, perform an
     * internal seek to the time specified in the tfdt.
     *
     * This way if the user opens a file where the first tfdt is 1 hour
     * into the presentation, they will not have to wait 1 hour for run
     * time to catch up and actual playback to start. */
    gint i;

    GST_DEBUG_OBJECT (qtdemux, "First fragment has a non-zero tfdt, "
        "performing an internal seek to %" GST_TIME_FORMAT,
        GST_TIME_ARGS (min_dts));

    qtdemux->segment.start = min_dts;
    qtdemux->segment.time = qtdemux->segment.position = min_dts;

    for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
      QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
      stream->time_position = min_dts;
    }

    /* Before this code was run a segment was already sent when the moov was
     * parsed... which is OK -- some apps (mostly tests) expect a segment to
     * be emitted after a moov, and we can emit a second segment anyway for
     * special cases like this. */
    qtdemux->need_segment = TRUE;
  }

  qtdemux->first_moof_already_parsed = TRUE;

  g_node_destroy (moof_node);
  return TRUE;

missing_tfhd:
  {
    GST_DEBUG_OBJECT (qtdemux, "missing tfhd box");
    goto fail;
  }
missing_mfhd:
  {
    GST_DEBUG_OBJECT (qtdemux, "Missing mfhd box");
    goto fail;
  }
lost_offset:
  {
    GST_DEBUG_OBJECT (qtdemux, "lost offset");
    goto fail;
  }
fail:
  {
    g_node_destroy (moof_node);
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")), (NULL));
    return FALSE;
  }
}

#if 0
/* might be used if some day we actually use mfra & co
 * for random access to fragments,
 * but that will require quite some modifications and much less relying
 * on a sample array */
#endif

static gboolean
qtdemux_parse_tfra (GstQTDemux * qtdemux, GNode * tfra_node)
{
  QtDemuxStream *stream;
  guint32 ver_flags, track_id, len, num_entries, i;
  guint value_size, traf_size, trun_size, sample_size;
  guint64 time = 0, moof_offset = 0;
#if 0
  GstBuffer *buf = NULL;
  GstFlowReturn ret;
#endif
  GstByteReader tfra;

  gst_byte_reader_init (&tfra, tfra_node->data, QT_UINT32 (tfra_node->data));

  if (!gst_byte_reader_skip (&tfra, 8))
    return FALSE;

  if (!gst_byte_reader_get_uint32_be (&tfra, &ver_flags))
    return FALSE;

  if (!gst_byte_reader_get_uint32_be (&tfra, &track_id)
      || !gst_byte_reader_get_uint32_be (&tfra, &len)
      || !gst_byte_reader_get_uint32_be (&tfra, &num_entries))
    return FALSE;

  GST_DEBUG_OBJECT (qtdemux, "parsing tfra box for track id %u", track_id);

  stream = qtdemux_find_stream (qtdemux, track_id);
  if (stream == NULL)
    goto unknown_trackid;

  value_size = ((ver_flags >> 24) == 1) ? sizeof (guint64) : sizeof (guint32);
  sample_size = (len & 3) + 1;
  trun_size = ((len & 12) >> 2) + 1;
  traf_size = ((len & 48) >> 4) + 1;

  GST_DEBUG_OBJECT (qtdemux, "%u entries, sizes: value %u, traf %u, trun %u, "
      "sample %u", num_entries, value_size, traf_size, trun_size, sample_size);

  if (num_entries == 0)
    goto no_samples;

  if (!qt_atom_parser_has_chunks (&tfra, num_entries,
          value_size + value_size + traf_size + trun_size + sample_size))
    goto corrupt_file;

  g_free (stream->ra_entries);
  stream->ra_entries = g_new (QtDemuxRandomAccessEntry, num_entries);
  stream->n_ra_entries = num_entries;

  for (i = 0; i < num_entries; i++) {
    qt_atom_parser_get_offset (&tfra, value_size, &time);
    qt_atom_parser_get_offset (&tfra, value_size, &moof_offset);
    qt_atom_parser_get_uint_with_size_unchecked (&tfra, traf_size);
    qt_atom_parser_get_uint_with_size_unchecked (&tfra, trun_size);
    qt_atom_parser_get_uint_with_size_unchecked (&tfra, sample_size);

    time = QTSTREAMTIME_TO_GSTTIME (stream, time);

    GST_LOG_OBJECT (qtdemux, "fragment time: %" GST_TIME_FORMAT ", "
        " moof_offset: %" G_GUINT64_FORMAT, GST_TIME_ARGS (time), moof_offset);

    stream->ra_entries[i].ts = time;
    stream->ra_entries[i].moof_offset = moof_offset;

    /* don't want to go through the entire file and read all moofs at startup */
#if 0
    ret = gst_qtdemux_pull_atom (qtdemux, moof_offset, 0, &buf);
    if (ret != GST_FLOW_OK)
      goto corrupt_file;
    qtdemux_parse_moof (qtdemux, GST_BUFFER_DATA (buf), GST_BUFFER_SIZE (buf),
        moof_offset, stream);
    gst_buffer_unref (buf);
#endif
  }

  check_update_duration (qtdemux, time);

  return TRUE;

/* ERRORS */
unknown_trackid:
  {
    GST_WARNING_OBJECT (qtdemux, "Couldn't find stream for track %u", track_id);
    return FALSE;
  }
corrupt_file:
  {
    GST_WARNING_OBJECT (qtdemux, "broken traf box, ignoring");
    return FALSE;
  }
no_samples:
  {
    GST_WARNING_OBJECT (qtdemux, "stream has no samples");
    return FALSE;
  }
}

static gboolean
qtdemux_pull_mfro_mfra (GstQTDemux * qtdemux)
{
  GstMapInfo mfro_map = GST_MAP_INFO_INIT;
  GstMapInfo mfra_map = GST_MAP_INFO_INIT;
  GstBuffer *mfro = NULL, *mfra = NULL;
  GstFlowReturn flow;
  gboolean ret = FALSE;
  GNode *mfra_node, *tfra_node;
  guint64 mfra_offset = 0;
  guint32 fourcc, mfra_size;
  gint64 len;

  /* query upstream size in bytes */
  if (!gst_pad_peer_query_duration (qtdemux->sinkpad, GST_FORMAT_BYTES, &len))
    goto size_query_failed;

  /* mfro box should be at the very end of the file */
  flow = gst_qtdemux_pull_atom (qtdemux, len - 16, 16, &mfro);
  if (flow != GST_FLOW_OK)
    goto exit;

  gst_buffer_map (mfro, &mfro_map, GST_MAP_READ);

  fourcc = QT_FOURCC (mfro_map.data + 4);
  if (fourcc != FOURCC_mfro)
    goto exit;

  GST_INFO_OBJECT (qtdemux, "Found mfro box");
  if (mfro_map.size < 16)
    goto invalid_mfro_size;

  mfra_size = QT_UINT32 (mfro_map.data + 12);
  if (mfra_size >= len)
    goto invalid_mfra_size;

  mfra_offset = len - mfra_size;

  GST_INFO_OBJECT (qtdemux, "mfra offset: %" G_GUINT64_FORMAT ", size %u",
      mfra_offset, mfra_size);

  /* now get and parse mfra box */
  flow = gst_qtdemux_pull_atom (qtdemux, mfra_offset, mfra_size, &mfra);
  if (flow != GST_FLOW_OK)
    goto broken_file;

  gst_buffer_map (mfra, &mfra_map, GST_MAP_READ);

  mfra_node = g_node_new ((guint8 *) mfra_map.data);
  qtdemux_parse_node (qtdemux, mfra_node, mfra_map.data, mfra_map.size);

  tfra_node = qtdemux_tree_get_child_by_type (mfra_node, FOURCC_tfra);

  while (tfra_node) {
    qtdemux_parse_tfra (qtdemux, tfra_node);
    /* iterate all siblings */
    tfra_node = qtdemux_tree_get_sibling_by_type (tfra_node, FOURCC_tfra);
  }
  g_node_destroy (mfra_node);

  GST_INFO_OBJECT (qtdemux, "parsed movie fragment random access box (mfra)");
  ret = TRUE;

exit:

  if (mfro) {
    if (mfro_map.memory != NULL)
      gst_buffer_unmap (mfro, &mfro_map);
    gst_buffer_unref (mfro);
  }
  if (mfra) {
    if (mfra_map.memory != NULL)
      gst_buffer_unmap (mfra, &mfra_map);
    gst_buffer_unref (mfra);
  }
  return ret;

/* ERRORS */
size_query_failed:
  {
    GST_WARNING_OBJECT (qtdemux, "could not query upstream size");
    goto exit;
  }
invalid_mfro_size:
  {
    GST_WARNING_OBJECT (qtdemux, "mfro size is too small");
    goto exit;
  }
invalid_mfra_size:
  {
    GST_WARNING_OBJECT (qtdemux, "mfra_size in mfro box is invalid");
    goto exit;
  }
broken_file:
  {
    GST_WARNING_OBJECT (qtdemux, "bogus mfra offset or size, broken file");
    goto exit;
  }
}

static guint64
add_offset (guint64 offset, guint64 advance)
{
  /* Avoid 64-bit overflow by clamping */
  if (offset > G_MAXUINT64 - advance)
    return G_MAXUINT64;
  return offset + advance;
}

static GstFlowReturn
gst_qtdemux_loop_state_header (GstQTDemux * qtdemux)
{
  guint64 length = 0;
  guint32 fourcc = 0;
  GstBuffer *buf = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  guint64 cur_offset = qtdemux->offset;
  GstMapInfo map;

  ret = gst_pad_pull_range (qtdemux->sinkpad, cur_offset, 16, &buf);
  if (G_UNLIKELY (ret != GST_FLOW_OK))
    goto beach;
  gst_buffer_map (buf, &map, GST_MAP_READ);
  if (G_LIKELY (map.size >= 8))
    extract_initial_length_and_fourcc (map.data, map.size, &length, &fourcc);
  gst_buffer_unmap (buf, &map);
  gst_buffer_unref (buf);

  /* maybe we already got most we needed, so only consider this eof */
  if (G_UNLIKELY (length == 0)) {
    GST_ELEMENT_WARNING (qtdemux, STREAM, DEMUX,
        (_("Invalid atom size.")),
        ("Header atom '%" GST_FOURCC_FORMAT "' has empty length",
            GST_FOURCC_ARGS (fourcc)));
    ret = GST_FLOW_EOS;
    goto beach;
  }

  switch (fourcc) {
    case FOURCC_moof:
      /* record for later parsing when needed */
      if (!qtdemux->moof_offset) {
        qtdemux->moof_offset = qtdemux->offset;
      }
      if (qtdemux_pull_mfro_mfra (qtdemux)) {
        /* FIXME */
      } else {
        qtdemux->offset += length;      /* skip moof and keep going */
      }
      if (qtdemux->got_moov) {
        GST_INFO_OBJECT (qtdemux, "moof header, got moov, done with headers");
        ret = GST_FLOW_EOS;
        goto beach;
      }
      break;
    case FOURCC_mdat:
    case FOURCC_free:
    case FOURCC_skip:
    case FOURCC_wide:
    case FOURCC_PICT:
    case FOURCC_pnot:
    {
      GST_LOG_OBJECT (qtdemux,
          "skipping atom '%" GST_FOURCC_FORMAT "' at %" G_GUINT64_FORMAT,
          GST_FOURCC_ARGS (fourcc), cur_offset);
      qtdemux->offset = add_offset (qtdemux->offset, length);
      break;
    }
    case FOURCC_moov:
    {
      GstBuffer *moov = NULL;

      if (qtdemux->got_moov) {
        GST_DEBUG_OBJECT (qtdemux, "Skipping moov atom as we have one already");
        qtdemux->offset = add_offset (qtdemux->offset, length);
        goto beach;
      }

      if (length == G_MAXUINT64) {
        /* Read until the end */
        gint64 duration;
        if (!gst_pad_peer_query_duration (qtdemux->sinkpad, GST_FORMAT_BYTES,
                &duration)) {
          GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
              (_("Cannot query file size")),
              ("Duration query on sink pad failed"));
          ret = GST_FLOW_ERROR;
          goto beach;
        }
        if (G_UNLIKELY (cur_offset > duration)) {
          GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
              (_("Cannot query file size")),
              ("Duration %" G_GINT64_FORMAT " < current offset %"
                  G_GUINT64_FORMAT, duration, cur_offset));
          ret = GST_FLOW_ERROR;
          goto beach;
        }
        length = duration - cur_offset;
        if (length > QTDEMUX_MAX_ATOM_SIZE) {
          GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
              (_("Cannot demux file")),
              ("Moov atom size %" G_GINT64_FORMAT " > maximum %d", length,
                  QTDEMUX_MAX_ATOM_SIZE));
          ret = GST_FLOW_ERROR;
          goto beach;
        }
      }

      ret = gst_pad_pull_range (qtdemux->sinkpad, cur_offset, length, &moov);
      if (ret != GST_FLOW_OK)
        goto beach;
      gst_buffer_map (moov, &map, GST_MAP_READ);

      if (length != map.size) {
        /* Some files have a 'moov' atom at the end of the file which contains
         * a terminal 'free' atom where the body of the atom is missing.
         * Check for, and permit, this special case.
         */
        if (map.size >= 8) {
          guint8 *final_data = map.data + (map.size - 8);
          guint32 final_length = QT_UINT32 (final_data);
          guint32 final_fourcc = QT_FOURCC (final_data + 4);

          if (final_fourcc == FOURCC_free
              && map.size + final_length - 8 == length) {
            /* Ok, we've found that special case. Allocate a new buffer with
             * that free atom actually present. */
            GstBuffer *newmoov = gst_buffer_new_and_alloc (length);
            gst_buffer_fill (newmoov, 0, map.data, map.size);
            gst_buffer_memset (newmoov, map.size, 0, final_length - 8);
            gst_buffer_unmap (moov, &map);
            gst_buffer_unref (moov);
            moov = newmoov;
            gst_buffer_map (moov, &map, GST_MAP_READ);
          }
        }
      }

      if (length != map.size) {
        GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
            (_("This file is incomplete and cannot be played.")),
            ("We got less than expected (received %" G_GSIZE_FORMAT
                ", wanted %" G_GUINT64_FORMAT ", offset %" G_GUINT64_FORMAT ")",
                map.size, length, cur_offset));
        gst_buffer_unmap (moov, &map);
        gst_buffer_unref (moov);
        ret = GST_FLOW_ERROR;
        goto beach;
      }
      qtdemux->offset += length;

      qtdemux_parse_moov (qtdemux, map.data, length);
      qtdemux_node_dump (qtdemux, qtdemux->moov_node);

      qtdemux_parse_tree (qtdemux);
      if (qtdemux->moov_node_compressed) {
        g_node_destroy (qtdemux->moov_node_compressed);
        g_free (qtdemux->moov_node->data);
      }
      qtdemux->moov_node_compressed = NULL;
      g_node_destroy (qtdemux->moov_node);
      qtdemux->moov_node = NULL;
      gst_buffer_unmap (moov, &map);
      gst_buffer_unref (moov);
      qtdemux->got_moov = TRUE;

      break;
    }
    case FOURCC_ftyp:
    {
      GstBuffer *ftyp = NULL;

      /* extract major brand; might come in handy for ISO vs QT issues */
      ret = gst_qtdemux_pull_atom (qtdemux, cur_offset, length, &ftyp);
      if (ret != GST_FLOW_OK)
        goto beach;
      qtdemux->offset += length;
      gst_buffer_map (ftyp, &map, GST_MAP_READ);
      qtdemux_parse_ftyp (qtdemux, map.data, map.size);
      gst_buffer_unmap (ftyp, &map);
      gst_buffer_unref (ftyp);
      break;
    }
    case FOURCC_styp:
    {
      GstBuffer *styp = NULL;

      ret = gst_qtdemux_pull_atom (qtdemux, cur_offset, length, &styp);
      if (ret != GST_FLOW_OK)
        goto beach;
      qtdemux->offset += length;
      gst_buffer_map (styp, &map, GST_MAP_READ);
      qtdemux_parse_styp (qtdemux, map.data, map.size);
      gst_buffer_unmap (styp, &map);
      gst_buffer_unref (styp);
      break;
    }
    case FOURCC_uuid:
    {
      GstBuffer *uuid = NULL;

      /* uuid are extension atoms */
      ret = gst_qtdemux_pull_atom (qtdemux, cur_offset, length, &uuid);
      if (ret != GST_FLOW_OK)
        goto beach;
      qtdemux->offset += length;
      gst_buffer_map (uuid, &map, GST_MAP_READ);
      qtdemux_parse_uuid (qtdemux, map.data, map.size);
      gst_buffer_unmap (uuid, &map);
      gst_buffer_unref (uuid);
      break;
    }
    case FOURCC_sidx:
    {
      GstBuffer *sidx = NULL;
      ret = gst_qtdemux_pull_atom (qtdemux, cur_offset, length, &sidx);
      if (ret != GST_FLOW_OK)
        goto beach;
      qtdemux->offset += length;
      gst_buffer_map (sidx, &map, GST_MAP_READ);
      qtdemux_parse_sidx (qtdemux, map.data, map.size);
      gst_buffer_unmap (sidx, &map);
      gst_buffer_unref (sidx);
      break;
    }
    case FOURCC_meta:
    {
      GstBuffer *meta = NULL;
      GNode *node, *child;
      GstByteReader child_data;
      ret = gst_qtdemux_pull_atom (qtdemux, cur_offset, length, &meta);
      if (ret != GST_FLOW_OK)
        goto beach;
      qtdemux->offset += length;
      gst_buffer_map (meta, &map, GST_MAP_READ);

      node = g_node_new (map.data);

      qtdemux_parse_node (qtdemux, node, map.data, map.size);

      /* Parse ONVIF Export File Format CorrectStartTime box if available */
      if ((child =
              qtdemux_tree_get_child_by_type_full (node, FOURCC_cstb,
                  &child_data))) {
        qtdemux_parse_cstb (qtdemux, &child_data);
      }

      g_node_destroy (node);

      gst_buffer_unmap (meta, &map);
      gst_buffer_unref (meta);
      break;
    }
    default:
    {
      GstBuffer *unknown = NULL;

      GST_LOG_OBJECT (qtdemux,
          "unknown %08x '%" GST_FOURCC_FORMAT "' of size %" G_GUINT64_FORMAT
          " at %" G_GUINT64_FORMAT, fourcc, GST_FOURCC_ARGS (fourcc), length,
          cur_offset);
      ret = gst_qtdemux_pull_atom (qtdemux, cur_offset, length, &unknown);
      if (ret != GST_FLOW_OK)
        goto beach;
      gst_buffer_map (unknown, &map, GST_MAP_READ);
      GST_MEMDUMP ("Unknown tag", map.data, map.size);
      gst_buffer_unmap (unknown, &map);
      gst_buffer_unref (unknown);
      qtdemux->offset += length;
      break;
    }
  }

beach:
  if (ret == GST_FLOW_EOS && (qtdemux->got_moov || qtdemux->media_caps)) {
    /* digested all data, show what we have */
    ret = qtdemux_prepare_streams (qtdemux);
    if (ret != GST_FLOW_OK)
      return ret;

    QTDEMUX_EXPOSE_LOCK (qtdemux);
    ret = qtdemux_expose_streams (qtdemux);
    QTDEMUX_EXPOSE_UNLOCK (qtdemux);
    if (ret != GST_FLOW_OK)
      return ret;

    qtdemux->state = QTDEMUX_STATE_MOVIE;
    GST_DEBUG_OBJECT (qtdemux, "switching state to STATE_MOVIE (%d)",
        qtdemux->state);
    return ret;
  }
  return ret;
}

/* Seeks to the previous keyframe of the indexed stream and
 * aligns other streams with respect to the keyframe timestamp
 * of indexed stream. Only called in case of Reverse Playback
 */
static GstFlowReturn
gst_qtdemux_seek_to_previous_keyframe (GstQTDemux * qtdemux)
{
  guint32 seg_idx = 0, k_index = 0;
  guint32 ref_seg_idx, ref_k_index;
  GstClockTime k_pos = 0, last_stop = 0;
  QtDemuxSegment *seg = NULL;
  QtDemuxStream *ref_str = NULL;
  guint64 seg_media_start_mov;  /* segment media start time in mov format */
  guint64 target_ts;
  gint i;

  /* Now we choose an arbitrary stream, get the previous keyframe timestamp
   * and finally align all the other streams on that timestamp with their
   * respective keyframes */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *str = QTDEMUX_NTH_STREAM (qtdemux, i);

    /* No candidate yet, take the first stream */
    if (!ref_str) {
      ref_str = str;
      continue;
    }

    /* So that stream has a segment, we prefer video streams */
    if (str->subtype == FOURCC_vide) {
      ref_str = str;
      break;
    }
  }

  if (G_UNLIKELY (!ref_str)) {
    GST_DEBUG_OBJECT (qtdemux, "couldn't find any stream");
    goto eos;
  }

  if (G_UNLIKELY (!ref_str->from_sample)) {
    GST_DEBUG_OBJECT (qtdemux, "reached the beginning of the file");
    goto eos;
  }

  /* So that stream has been playing from from_sample to to_sample. We will
   * get the timestamp of the previous sample and search for a keyframe before
   * that. For audio streams we do an arbitrary jump in the past (10 samples) */
  if (ref_str->subtype == FOURCC_vide) {
    k_index = gst_qtdemux_find_keyframe (qtdemux, ref_str,
        ref_str->from_sample - 1, FALSE);
  } else {
    if (ref_str->from_sample >= 10)
      k_index = ref_str->from_sample - 10;
    else
      k_index = 0;
  }

  target_ts =
      ref_str->samples[k_index].timestamp +
      ref_str->samples[k_index].pts_offset;

  /* get current segment for that stream */
  seg = &ref_str->segments[ref_str->segment_index];
  /* Use segment start in original timescale for comparisons */
  seg_media_start_mov = seg->trak_media_start;

  GST_LOG_OBJECT (qtdemux, "keyframe index %u ts %" G_GUINT64_FORMAT
      " seg start %" G_GUINT64_FORMAT " %" GST_TIME_FORMAT,
      k_index, target_ts, seg_media_start_mov,
      GST_TIME_ARGS (seg->media_start));

  /* Crawl back through segments to find the one containing this I frame */
  while (target_ts < seg_media_start_mov) {
    GST_DEBUG_OBJECT (qtdemux,
        "keyframe position (sample %u) is out of segment %u " " target %"
        G_GUINT64_FORMAT " seg start %" G_GUINT64_FORMAT, k_index,
        ref_str->segment_index, target_ts, seg_media_start_mov);

    if (G_UNLIKELY (!ref_str->segment_index)) {
      /* Reached first segment, let's consider it's EOS */
      goto eos;
    }
    ref_str->segment_index--;
    seg = &ref_str->segments[ref_str->segment_index];
    /* Use segment start in original timescale for comparisons */
    seg_media_start_mov = seg->trak_media_start;
  }
  /* Calculate time position of the keyframe and where we should stop */
  k_pos =
      QTSTREAMTIME_TO_GSTTIME (ref_str,
      target_ts - seg->trak_media_start) + seg->time;
  last_stop =
      QTSTREAMTIME_TO_GSTTIME (ref_str,
      ref_str->samples[ref_str->from_sample].timestamp -
      seg->trak_media_start) + seg->time;

  GST_DEBUG_OBJECT (qtdemux, "preferred stream played from sample %u, "
      "now going to sample %u (pts %" GST_TIME_FORMAT ")", ref_str->from_sample,
      k_index, GST_TIME_ARGS (k_pos));

  /* Set last_stop with the keyframe timestamp we pushed of that stream */
  qtdemux->segment.position = last_stop;
  GST_DEBUG_OBJECT (qtdemux, "last_stop now is %" GST_TIME_FORMAT,
      GST_TIME_ARGS (last_stop));

  if (G_UNLIKELY (last_stop < qtdemux->segment.start)) {
    GST_DEBUG_OBJECT (qtdemux, "reached the beginning of segment");
    goto eos;
  }

  ref_seg_idx = ref_str->segment_index;
  ref_k_index = k_index;

  /* Align them all on this */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    guint32 index = 0;
    GstClockTime seg_time = 0;
    QtDemuxStream *str = QTDEMUX_NTH_STREAM (qtdemux, i);

    /* aligning reference stream again might lead to backing up to yet another
     * keyframe (due to timestamp rounding issues),
     * potentially putting more load on downstream; so let's try to avoid */
    if (str == ref_str) {
      seg_idx = ref_seg_idx;
      seg = &str->segments[seg_idx];
      k_index = ref_k_index;
      GST_DEBUG_OBJECT (qtdemux, "reference track-id %u segment %d, "
          "sample at index %d", str->track_id, ref_str->segment_index, k_index);
    } else {
      seg_idx = gst_qtdemux_find_segment (qtdemux, str, k_pos);
      GST_DEBUG_OBJECT (qtdemux,
          "track-id %u align segment %d for keyframe pos %" GST_TIME_FORMAT,
          str->track_id, seg_idx, GST_TIME_ARGS (k_pos));

      /* get segment and time in the segment */
      seg = &str->segments[seg_idx];
      seg_time = k_pos - seg->time;

      /* get the media time in the segment.
       * No adjustment for empty "filler" segments */
      if (seg->media_start != GST_CLOCK_TIME_NONE)
        seg_time += seg->media_start;

      /* get the index of the sample with media time */
      index = gst_qtdemux_find_index_linear (qtdemux, str, seg_time);
      GST_DEBUG_OBJECT (qtdemux,
          "track-id %u sample for %" GST_TIME_FORMAT " at %u", str->track_id,
          GST_TIME_ARGS (seg_time), index);

      /* find previous keyframe */
      k_index = gst_qtdemux_find_keyframe (qtdemux, str, index, FALSE);
    }

    /* Remember until where we want to go */
    if (str->from_sample == 0) {
      GST_LOG_OBJECT (qtdemux, "already at sample 0");
      str->to_sample = 0;
    } else {
      str->to_sample = str->from_sample - 1;
    }
    /* Define our time position */
    target_ts =
        str->samples[k_index].timestamp + str->samples[k_index].pts_offset;
    str->time_position = QTSTREAMTIME_TO_GSTTIME (str, target_ts) + seg->time;
    if (seg->media_start != GST_CLOCK_TIME_NONE)
      str->time_position -= seg->media_start;

    /* Now seek back in time */
    gst_qtdemux_move_stream (qtdemux, str, k_index);
    GST_DEBUG_OBJECT (qtdemux, "track-id %u keyframe at %u, time position %"
        GST_TIME_FORMAT " playing from sample %u to %u", str->track_id, k_index,
        GST_TIME_ARGS (str->time_position), str->from_sample, str->to_sample);
  }

  return GST_FLOW_OK;

eos:
  return GST_FLOW_EOS;
}

/*
 * Gets the current qt segment start, stop and position for the
 * given time offset. This is used in update_segment()
 */
static void
gst_qtdemux_stream_segment_get_boundaries (GstQTDemux * qtdemux,
    QtDemuxStream * stream, GstClockTime offset,
    GstClockTime * _start, GstClockTime * _stop, GstClockTime * _time)
{
  GstClockTime seg_time;
  GstClockTime start, stop, time;
  QtDemuxSegment *segment;

  segment = &stream->segments[stream->segment_index];

  /* get time in this segment */
  seg_time = (offset - segment->time) * segment->rate;

  GST_LOG_OBJECT (stream->pad, "seg_time %" GST_TIME_FORMAT,
      GST_TIME_ARGS (seg_time));

  if (G_UNLIKELY (seg_time > segment->duration)) {
    GST_LOG_OBJECT (stream->pad,
        "seg_time > segment->duration %" GST_TIME_FORMAT,
        GST_TIME_ARGS (segment->duration));
    seg_time = segment->duration;
  }

  /* qtdemux->segment.stop is in outside-time-realm, whereas
   * segment->media_stop is in track-time-realm.
   *
   * In order to compare the two, we need to bring segment.stop
   * into the track-time-realm
   *
   * FIXME - does this comment still hold? Don't see any conversion here */

  stop = qtdemux->segment.stop;
  if (stop == GST_CLOCK_TIME_NONE)
    stop = qtdemux->segment.duration;
  if (stop == GST_CLOCK_TIME_NONE)
    stop = segment->media_stop;
  else
    stop =
        MIN (segment->media_stop, stop - segment->time + segment->media_start);

  if (G_UNLIKELY (QTSEGMENT_IS_EMPTY (segment))) {
    start = segment->time + seg_time;
    time = offset;
    stop = start - seg_time + segment->duration;
  } else if (qtdemux->segment.rate >= 0) {
    start = MIN (segment->media_start + seg_time, stop);
    time = offset;
  } else {
    if (segment->media_start >= qtdemux->segment.start) {
      time = segment->time;
    } else {
      time = segment->time + (qtdemux->segment.start - segment->media_start);
    }

    start = MAX (segment->media_start, qtdemux->segment.start);
    stop = MIN (segment->media_start + seg_time, stop);
  }

  *_start = start;
  *_stop = stop;
  *_time = time;
}

/*
 * Updates the qt segment used for the stream and pushes a new segment event
 * downstream on this stream's pad.
 */
static gboolean
gst_qtdemux_stream_update_segment (GstQTDemux * qtdemux, QtDemuxStream * stream,
    gint seg_idx, GstClockTime offset, GstClockTime * _start,
    GstClockTime * _stop)
{
  QtDemuxSegment *segment;
  GstClockTime start = 0, stop = GST_CLOCK_TIME_NONE, time = 0;
  gdouble rate;
  GstEvent *event;

  /* update the current segment */
  stream->segment_index = seg_idx;

  /* get the segment */
  segment = &stream->segments[seg_idx];

  if (G_UNLIKELY (offset < segment->time)) {
    GST_WARNING_OBJECT (stream->pad, "offset < segment->time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (segment->time));
    return FALSE;
  }

  /* segment lies beyond total indicated duration */
  if (G_UNLIKELY (qtdemux->segment.duration != GST_CLOCK_TIME_NONE &&
          segment->time > qtdemux->segment.duration)) {
    GST_WARNING_OBJECT (stream->pad, "file duration %" GST_TIME_FORMAT
        " < segment->time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (qtdemux->segment.duration),
        GST_TIME_ARGS (segment->time));
    return FALSE;
  }

  gst_qtdemux_stream_segment_get_boundaries (qtdemux, stream, offset,
      &start, &stop, &time);

  GST_DEBUG_OBJECT (stream->pad, "new segment %d from %" GST_TIME_FORMAT
      " to %" GST_TIME_FORMAT ", time %" GST_TIME_FORMAT, seg_idx,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop), GST_TIME_ARGS (time));

  /* combine global rate with that of the segment */
  rate = segment->rate * qtdemux->segment.rate;

  /* Copy flags from main segment */
  stream->segment.flags = qtdemux->segment.flags;

  /* update the segment values used for clipping */
  stream->segment.offset = qtdemux->segment.offset;
  stream->segment.base = qtdemux->segment.base + stream->accumulated_base;
  stream->segment.applied_rate = qtdemux->segment.applied_rate;
  stream->segment.rate = rate;
  stream->segment.start = start + QTSTREAMTIME_TO_GSTTIME (stream,
      stream->cslg_shift);
  if (stop != -1)
    stream->segment.stop = stop + QTSTREAMTIME_TO_GSTTIME (stream,
        stream->cslg_shift);
  else
    stream->segment.stop = stop;
  stream->segment.time = time;
  stream->segment.position = stream->segment.start;

  /* Gapless audio requires adjustments to the segment
   * to reflect the actual playtime length. In
   * particular, this must exclude padding data. */
  if (qtdemux->gapless_audio_info.type != GAPLESS_AUDIO_INFO_TYPE_NONE) {
    stream->segment.stop = stream->segment.start +
        qtdemux->gapless_audio_info.valid_duration;
  }

  GST_DEBUG_OBJECT (stream->pad, "New segment: %" GST_SEGMENT_FORMAT,
      &stream->segment);

  /* now prepare and send the segment */
  if (stream->pad) {
    event = gst_event_new_segment (&stream->segment);
    if (qtdemux->segment_seqnum != GST_SEQNUM_INVALID) {
      gst_event_set_seqnum (event, qtdemux->segment_seqnum);
    }
    gst_pad_push_event (stream->pad, event);
    /* assume we can send more data now */
    GST_PAD_LAST_FLOW_RETURN (stream->pad) = GST_FLOW_OK;
    /* clear to send tags on this pad now */
    gst_qtdemux_push_tags (qtdemux, stream);
  }

  if (_start)
    *_start = start;
  if (_stop)
    *_stop = stop;

  return TRUE;
}

/* activate the given segment number @seg_idx of @stream at time @offset.
 * @offset is an absolute global position over all the segments.
 *
 * This will push out a NEWSEGMENT event with the right values and
 * position the stream index to the first decodable sample before
 * @offset.
 */
static gboolean
gst_qtdemux_activate_segment (GstQTDemux * qtdemux, QtDemuxStream * stream,
    guint32 seg_idx, GstClockTime offset)
{
  QtDemuxSegment *segment;
  guint32 index, kf_index;
  GstClockTime start = 0, stop = GST_CLOCK_TIME_NONE;

  GST_LOG_OBJECT (stream->pad, "activate segment %d, offset %" GST_TIME_FORMAT,
      seg_idx, GST_TIME_ARGS (offset));

  if (!gst_qtdemux_stream_update_segment (qtdemux, stream, seg_idx, offset,
          &start, &stop))
    return FALSE;

  segment = &stream->segments[stream->segment_index];

  /* in the fragmented case, we pick a fragment that starts before our
   * desired position and rely on downstream to wait for a keyframe
   * (FIXME: doesn't seem to work so well with ismv and wmv, as no parser; the
   * tfra entries tells us which trun/sample the key unit is in, but we don't
   * make use of this additional information at the moment) */
  if (qtdemux->fragmented && !qtdemux->fragmented_seek_pending) {
    stream->to_sample = G_MAXUINT32;
    return TRUE;
  } else {
    /* well, it will be taken care of below */
    qtdemux->fragmented_seek_pending = FALSE;
    /* FIXME ideally the do_fragmented_seek can be done right here,
     * rather than at loop level
     * (which might even allow handling edit lists in a fragmented file) */
  }

  /* We don't need to look for a sample in push-based */
  if (!qtdemux->pullbased)
    return TRUE;

  /* and move to the keyframe before the indicated media time of the
   * segment */
  if (G_LIKELY (!QTSEGMENT_IS_EMPTY (segment))) {
    if (qtdemux->segment.rate >= 0) {
      index = gst_qtdemux_find_index_linear (qtdemux, stream, start);
      stream->to_sample = G_MAXUINT32;
      GST_DEBUG_OBJECT (stream->pad,
          "moving data pointer to %" GST_TIME_FORMAT ", index: %u, pts %"
          GST_TIME_FORMAT, GST_TIME_ARGS (start), index,
          GST_TIME_ARGS (QTSAMPLE_PTS (stream, &stream->samples[index])));
    } else {
      index = gst_qtdemux_find_index_linear (qtdemux, stream, stop);
      stream->to_sample = index;
      GST_DEBUG_OBJECT (stream->pad,
          "moving data pointer to %" GST_TIME_FORMAT ", index: %u, pts %"
          GST_TIME_FORMAT, GST_TIME_ARGS (stop), index,
          GST_TIME_ARGS (QTSAMPLE_PTS (stream, &stream->samples[index])));
    }
  } else {
    GST_DEBUG_OBJECT (stream->pad, "No need to look for keyframe, "
        "this is an empty segment");
    return TRUE;
  }

  /* gst_qtdemux_parse_sample () called from gst_qtdemux_find_index_linear ()
   * encountered an error and printed a message so we return appropriately */
  if (index == -1)
    return FALSE;

  /* we're at the right spot */
  if (index == stream->sample_index) {
    GST_DEBUG_OBJECT (stream->pad, "we are at the right index");
    return TRUE;
  }

  /* find keyframe of the target index */
  kf_index = gst_qtdemux_find_keyframe (qtdemux, stream, index, FALSE);

  /* go back two frames to provide lead-in for non-raw audio decoders */
  if (stream->subtype == FOURCC_soun && !stream->need_clip) {
    guint32 lead_in = 2;
    guint32 old_index = kf_index;
    GstStructure *s = gst_caps_get_structure (CUR_STREAM (stream)->caps, 0);

    if (gst_structure_has_name (s, "audio/mpeg")) {
      gint mpegversion;
      if (gst_structure_get_int (s, "mpegversion", &mpegversion)
          && mpegversion == 1) {
        /* mp3 could need up to 30 frames of lead-in per mpegaudioparse */
        lead_in = 30;
      }
    }

    kf_index = MAX (kf_index, lead_in) - lead_in;
    if (qtdemux_parse_samples (qtdemux, stream, kf_index)) {
      GST_DEBUG_OBJECT (stream->pad,
          "Moving backwards %u frames to ensure sufficient sound lead-in",
          old_index - kf_index);
    } else {
      kf_index = old_index;
    }
  }

  /* if we move forwards, we don't have to go back to the previous
   * keyframe since we already sent that. We can also just jump to
   * the keyframe right before the target index if there is one. */
  if (index > stream->sample_index) {
    /* moving forwards check if we move past a keyframe */
    if (kf_index > stream->sample_index) {
      GST_DEBUG_OBJECT (stream->pad,
          "moving forwards to keyframe at %u "
          "(pts %" GST_TIME_FORMAT " dts %" GST_TIME_FORMAT " )",
          kf_index,
          GST_TIME_ARGS (QTSAMPLE_PTS (stream, &stream->samples[kf_index])),
          GST_TIME_ARGS (QTSAMPLE_DTS (stream, &stream->samples[kf_index])));
      gst_qtdemux_move_stream (qtdemux, stream, kf_index);
    } else {
      GST_DEBUG_OBJECT (stream->pad,
          "moving forwards, keyframe at %u "
          "(pts %" GST_TIME_FORMAT " dts %" GST_TIME_FORMAT " ) already sent",
          kf_index,
          GST_TIME_ARGS (QTSAMPLE_PTS (stream, &stream->samples[kf_index])),
          GST_TIME_ARGS (QTSAMPLE_DTS (stream, &stream->samples[kf_index])));
    }
  } else {
    GST_DEBUG_OBJECT (stream->pad,
        "moving backwards to %sframe at %u "
        "(pts %" GST_TIME_FORMAT " dts %" GST_TIME_FORMAT " )",
        (stream->subtype == FOURCC_soun) ? "audio " : "key", kf_index,
        GST_TIME_ARGS (QTSAMPLE_PTS (stream, &stream->samples[kf_index])),
        GST_TIME_ARGS (QTSAMPLE_DTS (stream, &stream->samples[kf_index])));
    gst_qtdemux_move_stream (qtdemux, stream, kf_index);
  }

  return TRUE;
}

/* prepare to get the current sample of @stream, getting essential values.
 *
 * This function will also prepare and send the segment when needed.
 *
 * Return FALSE if the stream is EOS.
 *
 * PULL-BASED
 */
static gboolean
gst_qtdemux_prepare_current_sample (GstQTDemux * qtdemux,
    QtDemuxStream * stream, gboolean * empty, guint64 * offset, guint * size,
    GstClockTime * dts, GstClockTime * pts, GstClockTime * duration,
    gboolean * keyframe)
{
  QtDemuxSample *sample;
  GstClockTime time_position;
  guint32 seg_idx;

  g_return_val_if_fail (stream != NULL, FALSE);

  time_position = stream->time_position;
  if (G_UNLIKELY (time_position == GST_CLOCK_TIME_NONE))
    goto eos;

  seg_idx = stream->segment_index;
  if (G_UNLIKELY (seg_idx == -1)) {
    /* find segment corresponding to time_position if we are looking
     * for a segment. */
    seg_idx = gst_qtdemux_find_segment (qtdemux, stream, time_position);
  }

  /* different segment, activate it, sample_index will be set. */
  if (G_UNLIKELY (stream->segment_index != seg_idx))
    gst_qtdemux_activate_segment (qtdemux, stream, seg_idx, time_position);

  if (G_UNLIKELY (QTSEGMENT_IS_EMPTY (&stream->
              segments[stream->segment_index]))) {
    QtDemuxSegment *seg = &stream->segments[stream->segment_index];

    GST_LOG_OBJECT (qtdemux, "Empty segment activated,"
        " prepare empty sample");

    *empty = TRUE;
    *pts = *dts = time_position;
    *duration = seg->duration - (time_position - seg->time);

    return TRUE;
  }

  *empty = FALSE;

  if (stream->sample_index == -1)
    stream->sample_index = 0;

  GST_LOG_OBJECT (qtdemux, "segment active, index = %u of %u",
      stream->sample_index, stream->n_samples);

  if (G_UNLIKELY (stream->sample_index >= stream->n_samples)) {
    if (!qtdemux->fragmented)
      goto eos;

    GST_INFO_OBJECT (qtdemux, "out of samples, trying to add more");
    do {
      GstFlowReturn flow;

      GST_OBJECT_LOCK (qtdemux);
      flow = qtdemux_add_fragmented_samples (qtdemux);
      GST_OBJECT_UNLOCK (qtdemux);

      if (flow != GST_FLOW_OK)
        goto eos;
    }
    while (stream->sample_index >= stream->n_samples);
  }

  if (!qtdemux_parse_samples (qtdemux, stream, stream->sample_index)) {
    GST_LOG_OBJECT (qtdemux, "Parsing of index %u failed!",
        stream->sample_index);
    return FALSE;
  }

  /* now get the info for the sample we're at */
  sample = &stream->samples[stream->sample_index];

  *dts = QTSAMPLE_DTS (stream, sample);
  *pts = QTSAMPLE_PTS (stream, sample);
  *offset = sample->offset;
  *size = sample->size;
  *duration = QTSAMPLE_DUR_DTS (stream, sample, *dts);
  *keyframe = QTSAMPLE_KEYFRAME (stream, sample);

  return TRUE;

  /* special cases */
eos:
  {
    stream->time_position = GST_CLOCK_TIME_NONE;
    return FALSE;
  }
}

/* move to the next sample in @stream.
 *
 * Moves to the next segment when needed.
 */
static void
gst_qtdemux_advance_sample (GstQTDemux * qtdemux, QtDemuxStream * stream)
{
  QtDemuxSample *sample;
  QtDemuxSegment *segment;

  /* get current segment */
  segment = &stream->segments[stream->segment_index];

  if (G_UNLIKELY (QTSEGMENT_IS_EMPTY (segment))) {
    GST_DEBUG_OBJECT (qtdemux, "Empty segment, no samples to advance");
    goto next_segment;
  }

  if (G_UNLIKELY (stream->sample_index >= stream->to_sample)) {
    /* Mark the stream as EOS */
    GST_DEBUG_OBJECT (qtdemux,
        "reached max allowed sample %u, mark EOS", stream->to_sample);
    stream->time_position = GST_CLOCK_TIME_NONE;
    return;
  }

  /* move to next sample */
  stream->sample_index++;
  stream->offset_in_sample = 0;

  GST_TRACE_OBJECT (qtdemux, "advance to sample %u/%u", stream->sample_index,
      stream->n_samples);

  /* reached the last sample, we need the next segment */
  if (G_UNLIKELY (stream->sample_index >= stream->n_samples))
    goto next_segment;

  if (!qtdemux_parse_samples (qtdemux, stream, stream->sample_index)) {
    GST_LOG_OBJECT (qtdemux, "Parsing of index %u failed!",
        stream->sample_index);
    return;
  }

  /* get next sample */
  sample = &stream->samples[stream->sample_index];

  GST_TRACE_OBJECT (qtdemux, "sample dts %" GST_TIME_FORMAT " media_stop: %"
      GST_TIME_FORMAT, GST_TIME_ARGS (QTSAMPLE_DTS (stream, sample)),
      GST_TIME_ARGS (segment->media_stop));

  /* see if we are past the segment */
  if (G_UNLIKELY (QTSAMPLE_DTS (stream, sample) >= segment->media_stop))
    goto next_segment;

  if (QTSAMPLE_DTS (stream, sample) >= segment->media_start) {
    /* inside the segment, update time_position, looks very familiar to
     * GStreamer segments, doesn't it? */
    stream->time_position =
        QTSAMPLE_DTS (stream, sample) - segment->media_start + segment->time;
  } else {
    /* not yet in segment, time does not yet increment. This means
     * that we are still prerolling keyframes to the decoder so it can
     * decode the first sample of the segment. */
    stream->time_position = segment->time;
  }
  return;

  /* move to the next segment */
next_segment:
  {
    GST_DEBUG_OBJECT (qtdemux, "segment %d ended ", stream->segment_index);

    if (stream->segment_index == stream->n_segments - 1) {
      /* are we at the end of the last segment, we're EOS */
      stream->time_position = GST_CLOCK_TIME_NONE;
    } else {
      /* else we're only at the end of the current segment */
      stream->time_position = segment->stop_time;
    }
    /* make sure we select a new segment */

    /* accumulate previous segments */
    if (GST_CLOCK_TIME_IS_VALID (stream->segment.stop))
      stream->accumulated_base +=
          (stream->segment.stop -
          stream->segment.start) / ABS (stream->segment.rate);

    stream->segment_index = -1;
  }
}

static void
gst_qtdemux_sync_streams (GstQTDemux * demux)
{
  gint i;

  if (QTDEMUX_N_STREAMS (demux) <= 1)
    return;

  for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
    QtDemuxStream *stream;
    GstClockTime end_time;

    stream = QTDEMUX_NTH_STREAM (demux, i);

    if (!stream->pad)
      continue;

    /* TODO advance time on subtitle streams here, if any some day */

    /* some clips/trailers may have unbalanced streams at the end,
     * so send EOS on shorter stream to prevent stalling others */

    /* do not mess with EOS if SEGMENT seeking */
    if (demux->segment.flags & GST_SEEK_FLAG_SEGMENT)
      continue;

    if (demux->pullbased) {
      /* loop mode is sample time based */
      if (!STREAM_IS_EOS (stream))
        continue;
    } else {
      /* push mode is byte position based */
      if (stream->n_samples &&
          stream->samples[stream->n_samples - 1].offset >= demux->offset)
        continue;
    }

    if (stream->sent_eos)
      continue;

    /* only act if some gap */
    end_time = stream->segments[stream->n_segments - 1].stop_time;
    GST_LOG_OBJECT (demux, "current position: %" GST_TIME_FORMAT
        ", stream end: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (demux->segment.position), GST_TIME_ARGS (end_time));
    if (GST_CLOCK_TIME_IS_VALID (end_time)
        && (end_time + 2 * GST_SECOND < demux->segment.position)) {
      GstEvent *event;

      GST_DEBUG_OBJECT (demux, "sending EOS for stream %s",
          GST_PAD_NAME (stream->pad));
      stream->sent_eos = TRUE;
      event = gst_event_new_eos ();
      if (demux->segment_seqnum != GST_SEQNUM_INVALID)
        gst_event_set_seqnum (event, demux->segment_seqnum);
      gst_pad_push_event (stream->pad, event);
    }
  }
}

/* EOS and NOT_LINKED need to be combined. This means that we return:
 *
 *  GST_FLOW_NOT_LINKED: when all pads NOT_LINKED.
 *  GST_FLOW_EOS: when all pads EOS or NOT_LINKED.
 */
static GstFlowReturn
gst_qtdemux_combine_flows (GstQTDemux * demux, QtDemuxStream * stream,
    GstFlowReturn ret)
{
  GST_LOG_OBJECT (demux, "flow return: %s", gst_flow_get_name (ret));

  if (stream->pad)
    ret = gst_flow_combiner_update_pad_flow (demux->flowcombiner, stream->pad,
        ret);
  else
    ret = gst_flow_combiner_update_flow (demux->flowcombiner, ret);

  GST_LOG_OBJECT (demux, "combined flow return: %s", gst_flow_get_name (ret));
  return ret;
}

/* the input buffer metadata must be writable. Returns NULL when the buffer is
 * completely clipped
 *
 * Should be used only with raw buffers */
static GstBuffer *
gst_qtdemux_clip_buffer (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  guint64 start, stop, cstart, cstop, diff;
  GstClockTime pts, duration;
  gsize size, osize;
  gint num_rate, denom_rate;
  gint frame_size;
  gboolean clip_data;
  guint offset;

  osize = size = gst_buffer_get_size (buf);
  offset = 0;

  /* depending on the type, setup the clip parameters */
  if (stream->subtype == FOURCC_soun) {
    frame_size = CUR_STREAM (stream)->bytes_per_frame;
    num_rate = GST_SECOND;
    denom_rate = (gint) CUR_STREAM (stream)->rate;
    clip_data = TRUE;
  } else if (stream->subtype == FOURCC_vide) {
    frame_size = size;
    num_rate = CUR_STREAM (stream)->fps_n;
    denom_rate = CUR_STREAM (stream)->fps_d;
    clip_data = FALSE;
  } else
    goto wrong_type;

  if (frame_size <= 0)
    goto bad_frame_size;

  /* we can only clip if we have a valid pts */
  pts = GST_BUFFER_PTS (buf);
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (pts)))
    goto no_pts;

  duration = GST_BUFFER_DURATION (buf);

  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (duration))) {
    duration =
        gst_util_uint64_scale_int (size / frame_size, num_rate, denom_rate);
  }

  start = pts;
  stop = start + duration;

  if (G_UNLIKELY (!gst_segment_clip (&stream->segment,
              GST_FORMAT_TIME, start, stop, &cstart, &cstop)))
    goto clipped;

  /* see if some clipping happened */
  diff = cstart - start;
  if (diff > 0) {
    pts += diff;
    duration -= diff;

    if (clip_data) {
      /* bring clipped time to samples and to bytes */
      diff = gst_util_uint64_scale_int (diff, denom_rate, num_rate);
      diff *= frame_size;

      GST_DEBUG_OBJECT (qtdemux,
          "clipping start to %" GST_TIME_FORMAT " %"
          G_GUINT64_FORMAT " bytes", GST_TIME_ARGS (cstart), diff);

      offset = diff;
      size -= diff;
    }
  }
  diff = stop - cstop;
  if (diff > 0) {
    duration -= diff;

    if (clip_data) {
      /* bring clipped time to samples and then to bytes */
      diff = gst_util_uint64_scale_int (diff, denom_rate, num_rate);
      diff *= frame_size;
      GST_DEBUG_OBJECT (qtdemux,
          "clipping stop to %" GST_TIME_FORMAT " %" G_GUINT64_FORMAT
          " bytes", GST_TIME_ARGS (cstop), diff);
      size -= diff;
    }
  }

  if (offset != 0 || size != osize)
    gst_buffer_resize (buf, offset, size);

  GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_PTS (buf) = pts;
  GST_BUFFER_DURATION (buf) = duration;

  return buf;

  /* dropped buffer */
wrong_type:
  {
    GST_DEBUG_OBJECT (qtdemux, "unknown stream type");
    return buf;
  }
bad_frame_size:
  {
    GST_DEBUG_OBJECT (qtdemux, "bad frame size");
    return buf;
  }
no_pts:
  {
    GST_DEBUG_OBJECT (qtdemux, "no pts on buffer");
    return buf;
  }
clipped:
  {
    GST_DEBUG_OBJECT (qtdemux, "clipped buffer");
    gst_buffer_unref (buf);
    return NULL;
  }
}

static GstBuffer *
gst_qtdemux_reorder_audio_channels (GstQTDemux * demux,
    QtDemuxStream * stream, GstBuffer * buffer)
{
  buffer = gst_buffer_make_writable (buffer);

  GstMapInfo map;

  if (!gst_buffer_map (buffer, &map, GST_MAP_READWRITE))
    return buffer;

  if (map.size % (CUR_STREAM (stream)->bytes_per_sample *
          CUR_STREAM (stream)->n_channels) != 0) {
    GST_WARNING_OBJECT (demux,
        "Odd number of frames in raw audio buffer of length %" G_GSIZE_FORMAT
        " with %u bps and %u channels", map.size,
        CUR_STREAM (stream)->bytes_per_sample, CUR_STREAM (stream)->n_channels);
    goto out;
  }

  gst_audio_reorder_channels_with_reorder_map (map.data, map.size,
      CUR_STREAM (stream)->bytes_per_sample, CUR_STREAM (stream)->n_channels,
      CUR_STREAM (stream)->reorder_map);

out:
  gst_buffer_unmap (buffer, &map);

  return buffer;
}

static GstBuffer *
gst_qtdemux_align_buffer (GstQTDemux * demux,
    GstBuffer * buffer, gsize alignment)
{
  GstMapInfo map;

  gst_buffer_map (buffer, &map, GST_MAP_READ);

  if (map.size < sizeof (guintptr)) {
    gst_buffer_unmap (buffer, &map);
    return buffer;
  }

  if (((guintptr) map.data) & (alignment - 1)) {
    GstBuffer *new_buffer;
    GstAllocationParams params = { 0, alignment - 1, 0, 0, };

    new_buffer = gst_buffer_new_allocate (NULL,
        gst_buffer_get_size (buffer), &params);

    /* Copy data "by hand", so ensure alignment is kept: */
    gst_buffer_fill (new_buffer, 0, map.data, map.size);

    gst_buffer_copy_into (new_buffer, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    GST_DEBUG_OBJECT (demux,
        "We want output aligned on %" G_GSIZE_FORMAT ", reallocated",
        alignment);

    gst_buffer_unmap (buffer, &map);
    gst_buffer_unref (buffer);

    return new_buffer;
  }

  gst_buffer_unmap (buffer, &map);
  return buffer;
}

/* Adds padding to the end of each row to achieve byte-alignment
 *
 * Returns NULL if failed
 */
static GstBuffer *
gst_qtdemux_row_align_buffer (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * pre_buffer)
{
  GstVideoFrame pre_frame;
  GstVideoFrame new_frame;
  GstVideoInfo pre_info = stream->pre_info;
  GstVideoInfo new_info = stream->info;
  GstBuffer *new_buffer = NULL;
  gboolean pre_frame_mapped = FALSE;
  gboolean new_frame_mapped = FALSE;

  /* Map Buffer to Frame */
  pre_frame_mapped =
      gst_video_frame_map (&pre_frame, &pre_info, pre_buffer, GST_MAP_READ);
  if (!pre_frame_mapped) {
    GST_ERROR_OBJECT (qtdemux, "Failed to map video frame.");
    goto error;
  }

  /* Allocate New Buffer */
  GstAllocationParams params = { 0, stream->alignment - 1, 0, 0, };
  new_buffer = gst_buffer_new_allocate (NULL, new_info.size, &params);
  if (!new_buffer) {
    GST_ERROR_OBJECT (qtdemux, "Failed to allocate new buffer.");
    goto error;
  }

  /* Map New Frame */
  new_frame_mapped =
      gst_video_frame_map (&new_frame, &new_info, new_buffer, GST_MAP_WRITE);
  if (!new_frame_mapped) {
    GST_ERROR_OBJECT (qtdemux, "Failed to map new video frame.");
    goto error;
  }

  /* Copying the frame will automatically row-align the buffer */
  if (!gst_video_frame_copy (&new_frame, &pre_frame)) {
    GST_ERROR_OBJECT (qtdemux, "Failed to copy video frame.");
    goto error;
  }

  /* Cleanup before returning */
  gst_video_frame_unmap (&pre_frame);
  gst_video_frame_unmap (&new_frame);
  gst_buffer_unref (pre_buffer);
  return new_buffer;

error:
  if (new_frame_mapped) {
    gst_video_frame_unmap (&new_frame);
  }
  if (pre_frame_mapped) {
    gst_video_frame_unmap (&pre_frame);
  }
  if (new_buffer) {
    gst_buffer_unref (new_buffer);
  }
  return NULL;
}

static guint8 *
convert_to_s334_1a (const guint8 * ccpair, guint8 ccpair_size, guint field,
    gsize * res)
{
  guint8 *storage;
  gsize i;

  /* Strip off any leftover odd bytes and assume everything before is valid */
  if (ccpair_size % 2 != 0) {
    ccpair_size -= 1;
  }

  /* We are converting from pairs to triplets */
  *res = ccpair_size / 2 * 3;
  storage = g_malloc (*res);
  for (i = 0; i * 2 < ccpair_size; i += 1) {
    /* FIXME: Use line offset 0 as we simply can't know here */
    if (field == 1)
      storage[i * 3] = 0x80 | 0x00;
    else
      storage[i * 3] = 0x00 | 0x00;
    storage[i * 3 + 1] = ccpair[i * 2];
    storage[i * 3 + 2] = ccpair[i * 2 + 1];
  }

  return storage;
}

static guint8 *
extract_cc_from_data (QtDemuxStream * stream, const guint8 * data, gsize size,
    gsize * cclen)
{
  guint8 *res = NULL;
  guint32 atom_length, fourcc;
  QtDemuxStreamStsdEntry *stsd_entry;

  GST_MEMDUMP ("caption atom", data, size);

  /* There might be multiple atoms */

  *cclen = 0;
  if (size < 8)
    goto invalid_cdat;
  atom_length = QT_UINT32 (data);
  fourcc = QT_FOURCC (data + 4);
  if (G_UNLIKELY (atom_length > size || atom_length <= 8))
    goto invalid_cdat;

  GST_DEBUG_OBJECT (stream->pad, "here");

  /* Check if we have something compatible */
  stsd_entry = CUR_STREAM (stream);
  switch (stsd_entry->fourcc) {
    case FOURCC_c608:{
      guint8 *cdat = NULL, *cdt2 = NULL;
      gsize cdat_size = 0, cdt2_size = 0;
      /* Should be cdat or cdt2 */
      if (fourcc != FOURCC_cdat && fourcc != FOURCC_cdt2) {
        GST_WARNING_OBJECT (stream->pad,
            "Unknown data atom (%" GST_FOURCC_FORMAT ") for CEA608",
            GST_FOURCC_ARGS (fourcc));
        goto invalid_cdat;
      }

      /* Convert to S334-1 Annex A byte triplet */
      if (fourcc == FOURCC_cdat)
        cdat = convert_to_s334_1a (data + 8, atom_length - 8, 1, &cdat_size);
      else
        cdt2 = convert_to_s334_1a (data + 8, atom_length - 8, 2, &cdt2_size);
      GST_DEBUG_OBJECT (stream->pad, "size:%" G_GSIZE_FORMAT " atom_length:%u",
          size, atom_length);

      /* Check for another atom ? */
      if (size > atom_length + 8) {
        guint32 new_atom_length = QT_UINT32 (data + atom_length);
        if (size >= atom_length + new_atom_length) {
          fourcc = QT_FOURCC (data + atom_length + 4);
          if (fourcc == FOURCC_cdat) {
            if (cdat == NULL)
              cdat =
                  convert_to_s334_1a (data + atom_length + 8,
                  new_atom_length - 8, 1, &cdat_size);
            else
              GST_WARNING_OBJECT (stream->pad,
                  "Got multiple [cdat] atoms in a c608 sample. This is unsupported for now. Please file a bug");
          } else if (fourcc == FOURCC_cdt2) {
            if (cdt2 == NULL)
              cdt2 =
                  convert_to_s334_1a (data + atom_length + 8,
                  new_atom_length - 8, 2, &cdt2_size);
            else
              GST_WARNING_OBJECT (stream->pad,
                  "Got multiple [cdt2] atoms in a c608 sample. This is unsupported for now. Please file a bug");
          } else {
            GST_WARNING_OBJECT (stream->pad,
                "Unknown second data atom (%" GST_FOURCC_FORMAT ") for CEA608",
                GST_FOURCC_ARGS (fourcc));
          }
        }
      }

      *cclen = cdat_size + cdt2_size;
      res = g_malloc (*cclen);
      if (cdat_size)
        memcpy (res, cdat, cdat_size);
      if (cdt2_size)
        memcpy (res + cdat_size, cdt2, cdt2_size);
      g_free (cdat);
      g_free (cdt2);
    }
      break;
    case FOURCC_c708:
      if (fourcc != FOURCC_ccdp) {
        GST_WARNING_OBJECT (stream->pad,
            "Unknown data atom (%" GST_FOURCC_FORMAT ") for CEA708",
            GST_FOURCC_ARGS (fourcc));
        goto invalid_cdat;
      }
      *cclen = atom_length - 8;
      res = g_memdup2 (data + 8, *cclen);
      break;
    default:
      /* Keep this here in case other closed caption formats are added */
      g_assert_not_reached ();
      break;
  }

  GST_MEMDUMP ("Output", res, *cclen);
  return res;

  /* Errors */
invalid_cdat:
  GST_WARNING ("[cdat] atom is too small or invalid");
  return NULL;
}

/* Handle Closed Caption sample buffers.
 * The input buffer metadata must be writable,
 * but time/duration etc not yet set and need not be preserved */
static GstBuffer *
gst_qtdemux_process_buffer_clcp (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  GstBuffer *outbuf = NULL;
  GstMapInfo map;
  guint8 *cc;
  gsize cclen = 0;

  gst_buffer_map (buf, &map, GST_MAP_READ);

  /* empty buffer is sent to terminate previous subtitle */
  if (map.size <= 2) {
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);
    return NULL;
  }

  /* For closed caption, we need to extract the information from the
   * [cdat],[cdt2] or [ccdp] atom */
  cc = extract_cc_from_data (stream, map.data, map.size, &cclen);
  gst_buffer_unmap (buf, &map);
  if (cc) {
    outbuf = _gst_buffer_new_wrapped (cc, cclen, g_free);
    gst_buffer_copy_into (outbuf, buf, GST_BUFFER_COPY_METADATA, 0, -1);
  } else {
    /* Conversion failed or there's nothing */
  }
  gst_buffer_unref (buf);

  return outbuf;
}

/* DVD subpicture specific sample handling.
 * the input buffer metadata must be writable,
 * but time/duration etc not yet set and need not be preserved */
static GstBuffer *
gst_qtdemux_process_buffer_dvd (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  /* send a one time dvd clut event */
  if (stream->pending_event && stream->pad)
    gst_pad_push_event (stream->pad, stream->pending_event);
  stream->pending_event = NULL;

  /* empty buffer is sent to terminate previous subtitle */
  if (gst_buffer_get_size (buf) <= 2) {
    gst_buffer_unref (buf);
    return NULL;
  }

  /* That's all the processing needed for subpictures */
  return buf;
}

/* Timed text formats
 * the input buffer metadata must be writable,
 * but time/duration etc not yet set and need not be preserved */
static GstBuffer *
gst_qtdemux_process_buffer_text (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  GstBuffer *outbuf = NULL;
  GstMapInfo map;
  guint nsize = 0;
  gchar *str;

  /* not many cases for now */
  if (G_UNLIKELY (stream->subtype != FOURCC_text &&
          stream->subtype != FOURCC_sbtl)) {
    return buf;
  }

  gst_buffer_map (buf, &map, GST_MAP_READ);

  /* empty buffer is sent to terminate previous subtitle */
  if (map.size <= 2) {
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);
    return NULL;
  }

  nsize = GST_READ_UINT16_BE (map.data);
  nsize = MIN (nsize, map.size - 2);

  GST_LOG_OBJECT (qtdemux, "3GPP timed text subtitle: %d/%" G_GSIZE_FORMAT "",
      nsize, map.size);

  /* takes care of UTF-8 validation or UTF-16 recognition,
   * no other encoding expected */
  str = gst_tag_freeform_string_to_utf8 ((gchar *) map.data + 2, nsize, NULL);
  gst_buffer_unmap (buf, &map);

  if (str) {
    outbuf = _gst_buffer_new_wrapped (str, strlen (str), g_free);
    gst_buffer_copy_into (outbuf, buf, GST_BUFFER_COPY_METADATA, 0, -1);
  } else {
    /* this should not really happen unless the subtitle is corrupted */
  }
  gst_buffer_unref (buf);

  /* FIXME ? convert optional subsequent style info to markup */

  return outbuf;
}

/* WebVTT sample handling according to 14496-30 */
static GstBuffer *
gst_qtdemux_process_buffer_wvtt (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  GstBuffer *outbuf = NULL;
  GstMapInfo map;

  if (!gst_buffer_map (buf, &map, GST_MAP_READ)) {
    g_assert_not_reached ();    /* The buffer must be mappable */
  }

  if (qtdemux_webvtt_is_empty (qtdemux, map.data, map.size)) {
    GstEvent *gap = NULL;
    /* Push a gap event */
    stream->segment.position = GST_BUFFER_PTS (buf);
    gap =
        gst_event_new_gap (stream->segment.position, GST_BUFFER_DURATION (buf));
    gst_pad_push_event (stream->pad, gap);

    if (GST_BUFFER_DURATION_IS_VALID (buf))
      stream->segment.position += GST_BUFFER_DURATION (buf);
  } else {
    outbuf =
        qtdemux_webvtt_decode (qtdemux, GST_BUFFER_PTS (buf),
        GST_BUFFER_DURATION (buf), map.data, map.size);
    gst_buffer_copy_into (outbuf, buf, GST_BUFFER_COPY_METADATA, 0, -1);
  }

  gst_buffer_unmap (buf, &map);
  gst_buffer_unref (buf);

  return outbuf;
}

static GstFlowReturn
gst_qtdemux_push_buffer (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstClockTime pts, duration;

  if (stream->need_clip)
    buf = gst_qtdemux_clip_buffer (qtdemux, stream, buf);

  if (G_UNLIKELY (buf == NULL))
    goto exit;

  if (G_UNLIKELY (stream->discont)) {
    GST_LOG_OBJECT (qtdemux, "marking discont buffer");
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    stream->discont = FALSE;
  } else {
    GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  GST_LOG_OBJECT (qtdemux,
      "Pushing buffer with dts %" GST_TIME_FORMAT ", pts %" GST_TIME_FORMAT
      ", duration %" GST_TIME_FORMAT " on pad %s",
      GST_TIME_ARGS (GST_BUFFER_DTS (buf)),
      GST_TIME_ARGS (GST_BUFFER_PTS (buf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buf)), GST_PAD_NAME (stream->pad));

  if (stream->protected && stream->protection_scheme_type == FOURCC_aavd) {
    GstStructure *crypto_info;
    QtDemuxAavdEncryptionInfo *info =
        (QtDemuxAavdEncryptionInfo *) stream->protection_scheme_info;

    crypto_info = gst_structure_copy (info->default_properties);
    if (!crypto_info || !gst_buffer_add_protection_meta (buf, crypto_info))
      GST_ERROR_OBJECT (qtdemux, "failed to attach aavd metadata to buffer");
  }

  if (qtdemux->gapless_audio_info.type != GAPLESS_AUDIO_INFO_TYPE_NONE) {
    guint64 num_start_padding_pcm_frames;
    guint64 audio_sample_offset;
    guint64 audio_sample_offset_end;
    guint64 start_of_trailing_padding;
    guint64 start_clip = 0, end_clip = 0;
    guint64 total_num_clipped_samples;
    GstClockTime timestamp_decrement;

    /* Attach GstAudioClippingMeta to exclude padding data. */

    num_start_padding_pcm_frames =
        qtdemux->gapless_audio_info.num_start_padding_pcm_frames;

    audio_sample_offset = stream->sample_index * stream->stts_duration;
    audio_sample_offset_end = audio_sample_offset + stream->stts_duration;
    start_of_trailing_padding = num_start_padding_pcm_frames +
        qtdemux->gapless_audio_info.num_valid_pcm_frames;

    if (audio_sample_offset < num_start_padding_pcm_frames) {
      guint64 num_padding_audio_samples =
          num_start_padding_pcm_frames - audio_sample_offset;
      start_clip = MIN (num_padding_audio_samples, stream->stts_duration);
    }

    timestamp_decrement = qtdemux->gapless_audio_info.start_padding_duration;

    if (audio_sample_offset >= start_of_trailing_padding) {
      /* This case happens when the buffer is located fully past
       * the beginning of the padding area at the end of the stream.
       * Add the end padding to the decrement amount to ensure
       * continuous timestamps when transitioning from gapless
       * media to gapless media. */
      end_clip = stream->stts_duration;
      timestamp_decrement += qtdemux->gapless_audio_info.end_padding_duration;
    } else if (audio_sample_offset_end >= start_of_trailing_padding) {
      /* This case happens when the beginning of the padding area that
       * is located at the end of the stream intersects the buffer. */
      end_clip = audio_sample_offset_end - start_of_trailing_padding;
    }

    total_num_clipped_samples = start_clip + end_clip;

    if (total_num_clipped_samples != 0) {
      GST_DEBUG_OBJECT (qtdemux, "adding audio clipping meta: start / "
          "end clip: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT,
          start_clip, end_clip);
      gst_buffer_add_audio_clipping_meta (buf, GST_FORMAT_DEFAULT,
          start_clip, end_clip);

      if (total_num_clipped_samples >= stream->stts_duration) {
        GST_BUFFER_DURATION (buf) = 0;
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DECODE_ONLY);
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DROPPABLE);
      } else {
        guint64 num_valid_samples =
            stream->stts_duration - total_num_clipped_samples;
        GST_BUFFER_DURATION (buf) =
            QTSTREAMTIME_TO_GSTTIME (stream, num_valid_samples);
      }
    }

    /* The timestamps need to be shifted to factor in the skipped padding data. */

    if (GST_BUFFER_PTS_IS_VALID (buf)) {
      GstClockTime ts = GST_BUFFER_PTS (buf);
      GST_BUFFER_PTS (buf) =
          (ts >= timestamp_decrement) ? (ts - timestamp_decrement) : 0;
    }

    if (GST_BUFFER_DTS_IS_VALID (buf)) {
      GstClockTime ts = GST_BUFFER_DTS (buf);
      GST_BUFFER_DTS (buf) =
          (ts >= timestamp_decrement) ? (ts - timestamp_decrement) : 0;
    }
  }

  if (stream->protected && (stream->protection_scheme_type == FOURCC_cenc
          || stream->protection_scheme_type == FOURCC_cbcs)) {
    GstStructure *crypto_info;
    QtDemuxCencSampleSetInfo *info =
        (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;
    gint index;
    GstEvent *event;

    while ((event = g_queue_pop_head (&stream->protection_scheme_event_queue))) {
      GST_TRACE_OBJECT (stream->pad, "pushing protection event: %"
          GST_PTR_FORMAT, event);
      gst_pad_push_event (stream->pad, event);
    }

    if (info->crypto_info == NULL) {
      if (stream->protection_scheme_type == FOURCC_cbcs) {
        if (CUR_STREAM (stream)->fourcc == FOURCC_enca ||
            CUR_STREAM (stream)->fourcc == FOURCC_encs ||
            CUR_STREAM (stream)->fourcc == FOURCC_enct ||
            CUR_STREAM (stream)->fourcc == FOURCC_encv) {
          crypto_info = qtdemux_get_cenc_sample_properties (qtdemux, stream, 0);
          if (!crypto_info
              || !gst_buffer_add_protection_meta (buf, crypto_info)) {
            GST_ERROR_OBJECT (qtdemux,
                "failed to attach cbcs metadata to buffer");
            qtdemux_gst_structure_free (crypto_info);
          } else {
            GST_TRACE_OBJECT (qtdemux, "added cbcs protection metadata");
          }
        } else {
          GST_TRACE_OBJECT (qtdemux,
              "cbcs stream is not encrypted yet, not adding protection metadata");
        }
      } else {
        GST_DEBUG_OBJECT (qtdemux,
            "cenc metadata hasn't been parsed yet, pushing buffer as if it wasn't encrypted");
      }
    } else {
      /* The end of the crypto_info array matches our n_samples position,
       * so count backward from there */
      index = stream->sample_index - stream->n_samples + info->crypto_info->len;
      if (G_LIKELY (index >= 0 && index < info->crypto_info->len)) {
        /* steal structure from array */
        crypto_info = g_ptr_array_index (info->crypto_info, index);
        g_ptr_array_index (info->crypto_info, index) = NULL;
        GST_LOG_OBJECT (qtdemux, "attaching cenc metadata [%u/%u]", index,
            info->crypto_info->len);
        if (!crypto_info || !gst_buffer_add_protection_meta (buf, crypto_info))
          GST_ERROR_OBJECT (qtdemux,
              "failed to attach cenc metadata to buffer");
      } else {
        GST_INFO_OBJECT (qtdemux, "No crypto info with index %d and sample %d",
            index, stream->sample_index);
      }
    }
  }

  /* Copy buffer to ensure alignment */
  if (stream->needs_row_alignment) {
    buf = gst_qtdemux_row_align_buffer (qtdemux, stream, buf);
    if (buf == NULL) {
      ret = GST_FLOW_ERROR;
      goto exit;
    }
  } else if (stream->alignment > 1) {
    buf = gst_qtdemux_align_buffer (qtdemux, buf, stream->alignment);
  }

  if (CUR_STREAM (stream)->needs_reorder)
    buf = gst_qtdemux_reorder_audio_channels (qtdemux, stream, buf);

  pts = GST_BUFFER_PTS (buf);
  duration = GST_BUFFER_DURATION (buf);

  ret = gst_pad_push (stream->pad, buf);

  if (GST_CLOCK_TIME_IS_VALID (pts) && GST_CLOCK_TIME_IS_VALID (duration)) {
    /* mark position in stream, we'll need this to know when to send GAP event */
    stream->segment.position = pts + duration;
  }

exit:

  return ret;
}

static GstFlowReturn
gst_qtdemux_split_and_push_buffer (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;

  if (stream->subtype == FOURCC_clcp
      && CUR_STREAM (stream)->fourcc == FOURCC_c608 && stream->need_split) {
    GstMapInfo map;
    guint n_output_buffers, n_field1 = 0, n_field2 = 0;
    guint n_triplets, i;
    guint field1_off = 0, field2_off = 0;

    /* We have to split CEA608 buffers so that each outgoing buffer contains
     * one byte pair per field according to the framerate of the video track.
     *
     * If there is only a single byte pair per field we don't have to do
     * anything
     */

    gst_buffer_map (buf, &map, GST_MAP_READ);

    n_triplets = map.size / 3;
    for (i = 0; i < n_triplets; i++) {
      if (map.data[3 * i] & 0x80)
        n_field1++;
      else
        n_field2++;
    }

    g_assert (n_field1 || n_field2);

    /* If there's more than 1 frame we have to split, otherwise we can just
     * pass through */
    if (n_field1 > 1 || n_field2 > 1) {
      n_output_buffers =
          gst_util_uint64_scale (GST_BUFFER_DURATION (buf),
          CUR_STREAM (stream)->fps_n, GST_SECOND * CUR_STREAM (stream)->fps_d);

      for (i = 0; i < n_output_buffers; i++) {
        GstBuffer *outbuf =
            gst_buffer_new_and_alloc ((n_field1 ? 3 : 0) + (n_field2 ? 3 : 0));
        GstMapInfo outmap;
        guint8 *outptr;

        gst_buffer_map (outbuf, &outmap, GST_MAP_WRITE);
        outptr = outmap.data;

        if (n_field1) {
          gboolean found = FALSE;

          while (map.data + field1_off < map.data + map.size) {
            if (map.data[field1_off] & 0x80) {
              memcpy (outptr, &map.data[field1_off], 3);
              field1_off += 3;
              found = TRUE;
              break;
            }
            field1_off += 3;
          }

          if (!found) {
            const guint8 empty[] = { 0x80, 0x80, 0x80 };

            memcpy (outptr, empty, 3);
          }

          outptr += 3;
        }

        if (n_field2) {
          gboolean found = FALSE;

          while (map.data + field2_off < map.data + map.size) {
            if ((map.data[field2_off] & 0x80) == 0) {
              memcpy (outptr, &map.data[field2_off], 3);
              field2_off += 3;
              found = TRUE;
              break;
            }
            field2_off += 3;
          }

          if (!found) {
            const guint8 empty[] = { 0x00, 0x80, 0x80 };

            memcpy (outptr, empty, 3);
          }

          outptr += 3;
        }

        gst_buffer_unmap (outbuf, &outmap);

        GST_BUFFER_PTS (outbuf) =
            GST_BUFFER_PTS (buf) + gst_util_uint64_scale (i,
            GST_SECOND * CUR_STREAM (stream)->fps_d,
            CUR_STREAM (stream)->fps_n);
        GST_BUFFER_DURATION (outbuf) =
            gst_util_uint64_scale (GST_SECOND, CUR_STREAM (stream)->fps_d,
            CUR_STREAM (stream)->fps_n);
        GST_BUFFER_OFFSET (outbuf) = -1;
        GST_BUFFER_OFFSET_END (outbuf) = -1;

        ret = gst_qtdemux_push_buffer (qtdemux, stream, outbuf);

        if (ret != GST_FLOW_OK && ret != GST_FLOW_NOT_LINKED)
          break;
      }
      gst_buffer_unmap (buf, &map);
      gst_buffer_unref (buf);
    } else {
      gst_buffer_unmap (buf, &map);
      ret = gst_qtdemux_push_buffer (qtdemux, stream, buf);
    }
  } else {
    ret = gst_qtdemux_push_buffer (qtdemux, stream, buf);
  }

  return ret;
}

/* Sets a buffer's attributes properly and pushes it downstream.
 * Also checks for additional actions and custom processing that may
 * need to be done first.
 */
static GstFlowReturn
gst_qtdemux_decorate_and_push_buffer (GstQTDemux * qtdemux,
    QtDemuxStream * stream, GstBuffer * buf,
    GstClockTime dts, GstClockTime pts, GstClockTime duration,
    gboolean keyframe, GstClockTime position, guint64 byte_position)
{
  GstFlowReturn ret = GST_FLOW_OK;

  /* offset the timestamps according to the edit list */

  if (G_UNLIKELY (CUR_STREAM (stream)->fourcc == FOURCC_rtsp)) {
    gchar *url;
    GstMapInfo map;

    gst_buffer_map (buf, &map, GST_MAP_READ);
    url = g_strndup ((gchar *) map.data, map.size);
    gst_buffer_unmap (buf, &map);
    if (url != NULL && strlen (url) != 0) {
      /* we have RTSP redirect now */
      g_free (qtdemux->redirect_location);
      qtdemux->redirect_location = g_strdup (url);
      gst_element_post_message (GST_ELEMENT_CAST (qtdemux),
          gst_message_new_element (GST_OBJECT_CAST (qtdemux),
              gst_structure_new ("redirect",
                  "new-location", G_TYPE_STRING, url, NULL)));
    } else {
      GST_WARNING_OBJECT (qtdemux, "Redirect URI of stream is empty, not "
          "posting");
    }
    g_free (url);
  }

  /* position reporting */
  if (qtdemux->segment.rate >= 0) {
    qtdemux->segment.position = position;
    gst_qtdemux_sync_streams (qtdemux);
  }

  if (G_UNLIKELY (!stream->pad)) {
    GST_DEBUG_OBJECT (qtdemux, "No output pad for stream, ignoring");
    gst_buffer_unref (buf);
    goto exit;
  }

  /* send out pending buffers */
  while (stream->buffers) {
    GstBuffer *buffer = (GstBuffer *) stream->buffers->data;

    if (G_UNLIKELY (stream->discont)) {
      GST_LOG_OBJECT (qtdemux, "marking discont buffer");
      GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
      stream->discont = FALSE;
    } else {
      GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
    }

    if (stream->alignment > 1)
      buffer = gst_qtdemux_align_buffer (qtdemux, buffer, stream->alignment);
    if (CUR_STREAM (stream)->needs_reorder)
      buffer = gst_qtdemux_reorder_audio_channels (qtdemux, stream, buffer);

    gst_pad_push (stream->pad, buffer);

    stream->buffers = g_slist_delete_link (stream->buffers, stream->buffers);
  }

  /* we're going to modify the metadata */
  buf = gst_buffer_make_writable (buf);

  if (qtdemux->start_utc_time != GST_CLOCK_TIME_NONE) {
    static GstStaticCaps unix_caps = GST_STATIC_CAPS ("timestamp/x-unix");
    GstCaps *caps = gst_static_caps_get (&unix_caps);
    gst_buffer_add_reference_timestamp_meta (buf, caps,
        pts + qtdemux->start_utc_time - stream->cslg_shift,
        GST_CLOCK_TIME_NONE);
    gst_caps_unref (caps);
  }

  GST_BUFFER_DTS (buf) = dts;
  GST_BUFFER_PTS (buf) = pts;
  GST_BUFFER_DURATION (buf) = duration;
  GST_BUFFER_OFFSET (buf) = -1;
  GST_BUFFER_OFFSET_END (buf) = -1;

  if (G_UNLIKELY (stream->process_func))
    buf = stream->process_func (qtdemux, stream, buf);

  if (!buf) {
    goto exit;
  }

  if (!keyframe) {
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    stream->on_keyframe = FALSE;
  } else {
    stream->on_keyframe = TRUE;
  }

  if (G_UNLIKELY (CUR_STREAM (stream)->rgb8_palette))
    gst_buffer_append_memory (buf,
        gst_memory_ref (CUR_STREAM (stream)->rgb8_palette));

  if (G_UNLIKELY (CUR_STREAM (stream)->padding)) {
    gst_buffer_resize (buf, CUR_STREAM (stream)->padding, -1);
  }
#if 0
  if (G_UNLIKELY (qtdemux->element_index)) {
    GstClockTime stream_time;

    stream_time =
        gst_segment_to_stream_time (&stream->segment, GST_FORMAT_TIME,
        timestamp);
    if (GST_CLOCK_TIME_IS_VALID (stream_time)) {
      GST_LOG_OBJECT (qtdemux,
          "adding association %" GST_TIME_FORMAT "-> %"
          G_GUINT64_FORMAT, GST_TIME_ARGS (stream_time), byte_position);
      gst_index_add_association (qtdemux->element_index,
          qtdemux->index_id,
          keyframe ? GST_ASSOCIATION_FLAG_KEY_UNIT :
          GST_ASSOCIATION_FLAG_DELTA_UNIT, GST_FORMAT_TIME, stream_time,
          GST_FORMAT_BYTES, byte_position, NULL);
    }
  }
#endif

  ret = gst_qtdemux_split_and_push_buffer (qtdemux, stream, buf);

exit:
  return ret;
}

static const QtDemuxRandomAccessEntry *
gst_qtdemux_stream_seek_fragment (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GstClockTime pos, gboolean after)
{
  QtDemuxRandomAccessEntry *entries = stream->ra_entries;
  guint n_entries = stream->n_ra_entries;
  guint i;

  /* we assume the table is sorted */
  for (i = 0; i < n_entries; ++i) {
    if (entries[i].ts > pos)
      break;
  }

  /* FIXME: maybe save first moof_offset somewhere instead, but for now it's
   * probably okay to assume that the index lists the very first fragment */
  if (i == 0)
    return &entries[0];

  if (after)
    return &entries[i];
  else
    return &entries[i - 1];
}

static gboolean
gst_qtdemux_do_fragmented_seek (GstQTDemux * qtdemux)
{
  const QtDemuxRandomAccessEntry *best_entry = NULL;
  gint i;

  GST_OBJECT_LOCK (qtdemux);

  g_assert (QTDEMUX_N_STREAMS (qtdemux) > 0);

  /* first see if we can determine where to go to using mfra,
   * before we start clearing things */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    const QtDemuxRandomAccessEntry *entry;
    QtDemuxStream *stream;
    gboolean is_audio_or_video;

    stream = QTDEMUX_NTH_STREAM (qtdemux, i);

    if (stream->ra_entries == NULL)
      continue;

    if (stream->subtype == FOURCC_vide || stream->subtype == FOURCC_soun)
      is_audio_or_video = TRUE;
    else
      is_audio_or_video = FALSE;

    entry =
        gst_qtdemux_stream_seek_fragment (qtdemux, stream,
        stream->time_position, !is_audio_or_video);

    GST_INFO_OBJECT (stream->pad, "%" GST_TIME_FORMAT " at offset "
        "%" G_GUINT64_FORMAT, GST_TIME_ARGS (entry->ts), entry->moof_offset);

    stream->pending_seek = entry;

    /* decide position to jump to just based on audio/video tracks, not subs */
    if (!is_audio_or_video)
      continue;

    if (best_entry == NULL || entry->moof_offset < best_entry->moof_offset)
      best_entry = entry;
  }

  /* no luck, will handle seek otherwise */
  if (best_entry == NULL) {
    GST_OBJECT_UNLOCK (qtdemux);
    return FALSE;
  }

  /* ok, now we can prepare for processing as of located moof */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream;

    stream = QTDEMUX_NTH_STREAM (qtdemux, i);

    g_free (stream->samples);
    stream->samples = NULL;
    stream->n_samples = 0;
    stream->stbl_index = -1;    /* no samples have yet been parsed */
    stream->sample_index = -1;

    if (stream->protection_scheme_info) {
      /* Clear out any old cenc crypto info entries as we'll move to a new moof */
      if (stream->protection_scheme_type == FOURCC_cenc
          || stream->protection_scheme_type == FOURCC_cbcs) {
        QtDemuxCencSampleSetInfo *info =
            (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;
        if (info->crypto_info) {
          g_ptr_array_free (info->crypto_info, TRUE);
          info->crypto_info = NULL;
        }
      }
    }
  }

  GST_INFO_OBJECT (qtdemux, "seek to %" GST_TIME_FORMAT ", best fragment "
      "moof offset: %" G_GUINT64_FORMAT ", ts %" GST_TIME_FORMAT,
      GST_TIME_ARGS (QTDEMUX_NTH_STREAM (qtdemux, 0)->time_position),
      best_entry->moof_offset, GST_TIME_ARGS (best_entry->ts));

  qtdemux->moof_offset = best_entry->moof_offset;

  qtdemux_add_fragmented_samples (qtdemux);

  GST_OBJECT_UNLOCK (qtdemux);
  return TRUE;
}

static GstFlowReturn
gst_qtdemux_loop_state_movie (GstQTDemux * qtdemux)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *buf = NULL;
  QtDemuxStream *stream, *target_stream = NULL;
  GstClockTime min_time;
  guint64 offset = 0;
  GstClockTime dts = GST_CLOCK_TIME_NONE;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  GstClockTime duration = 0;
  gboolean keyframe = FALSE;
  guint sample_size = 0;
  guint num_samples = 1;
  gboolean empty = 0;
  guint size;
  gint i;

  if (qtdemux->fragmented_seek_pending) {
    GST_INFO_OBJECT (qtdemux, "pending fragmented seek");
    if (gst_qtdemux_do_fragmented_seek (qtdemux)) {
      GST_INFO_OBJECT (qtdemux, "fragmented seek done!");
      qtdemux->fragmented_seek_pending = FALSE;
    } else {
      GST_INFO_OBJECT (qtdemux, "fragmented seek still pending");
    }
  }

  /* Figure out the next stream sample to output, min_time is expressed in
   * global time and runs over the edit list segments. */
  min_time = G_MAXUINT64;
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    GstClockTime position;

    stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    position = stream->time_position;

    /* position of -1 is EOS */
    if (position != GST_CLOCK_TIME_NONE && position < min_time) {
      min_time = position;
      target_stream = stream;
    }
  }
  /* all are EOS */
  if (G_UNLIKELY (target_stream == NULL)) {
    GST_DEBUG_OBJECT (qtdemux, "all streams are EOS");
    goto eos;
  }

  /* check for segment end */
  if (G_UNLIKELY (qtdemux->segment.stop != -1
          && qtdemux->segment.rate >= 0
          && qtdemux->segment.stop <= min_time && target_stream->on_keyframe)) {
    GST_DEBUG_OBJECT (qtdemux, "we reached the end of our segment.");
    target_stream->time_position = GST_CLOCK_TIME_NONE;
    goto eos_stream;
  }

  /* fetch info for the current sample of this stream */
  if (G_UNLIKELY (!gst_qtdemux_prepare_current_sample (qtdemux, target_stream,
              &empty, &offset, &sample_size, &dts, &pts, &duration, &keyframe)))
    goto eos_stream;

  /* Send catche-up GAP event for each other stream if required.
   * This logic will be applied only for positive rate */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux) &&
      qtdemux->segment.rate >= 0; i++) {
    stream = QTDEMUX_NTH_STREAM (qtdemux, i);

    if (stream == target_stream ||
        !GST_CLOCK_TIME_IS_VALID (stream->segment.stop) ||
        !GST_CLOCK_TIME_IS_VALID (stream->segment.position))
      continue;

    if (stream->pad) {
      GstClockTime gap_threshold;
      /* kind of running time with offset segment.base and segment.start */
      GstClockTime pseudo_target_time = target_stream->segment.base;
      GstClockTime pseudo_cur_time = stream->segment.base;

      /* make sure positive offset, segment.position can be smallr than
       * segment.start for some reasons */
      if (target_stream->segment.position >= target_stream->segment.start) {
        pseudo_target_time +=
            (target_stream->segment.position - target_stream->segment.start);
      }

      if (stream->segment.position >= stream->segment.start)
        pseudo_cur_time += (stream->segment.position - stream->segment.start);

      /* Only send gap events on non-subtitle streams if lagging way behind. */
      if (stream->subtype == FOURCC_subp
          || stream->subtype == FOURCC_text || stream->subtype == FOURCC_sbtl ||
          stream->subtype == FOURCC_wvtt)
        gap_threshold = 1 * GST_SECOND;
      else
        gap_threshold = 3 * GST_SECOND;

      /* send gap events until the stream catches up */
      /* gaps can only be sent after segment is activated (segment.stop is no longer -1) */
      while (GST_CLOCK_TIME_IS_VALID (stream->segment.position) &&
          pseudo_cur_time < (G_MAXUINT64 - gap_threshold) &&
          pseudo_cur_time + gap_threshold < pseudo_target_time) {
        GstEvent *gap =
            gst_event_new_gap (stream->segment.position, gap_threshold);
        GST_LOG_OBJECT (stream->pad, "Sending %" GST_PTR_FORMAT, gap);

        gst_pad_push_event (stream->pad, gap);
        stream->segment.position += gap_threshold;
        pseudo_cur_time += gap_threshold;
      }
    }
  }

  stream = target_stream;

  gst_qtdemux_stream_check_and_change_stsd_index (qtdemux, stream);
  if (stream->new_caps) {
    gst_qtdemux_configure_stream (qtdemux, stream);
    qtdemux_do_allocation (stream, qtdemux);
  }

  /* If we're doing a keyframe-only trickmode, only push keyframes on video streams */
  if (G_UNLIKELY (qtdemux->segment.
          flags & GST_SEGMENT_FLAG_TRICKMODE_KEY_UNITS)) {
    if (stream->subtype == FOURCC_vide) {
      if (!keyframe) {
        GST_LOG_OBJECT (qtdemux, "Skipping non-keyframe on track-id %u",
            stream->track_id);
        goto next;
      } else if (qtdemux->trickmode_interval > 0) {
        GstClockTimeDiff interval;

        if (qtdemux->segment.rate > 0)
          interval = stream->time_position - stream->last_keyframe_dts;
        else
          interval = stream->last_keyframe_dts - stream->time_position;

        if (GST_CLOCK_TIME_IS_VALID (stream->last_keyframe_dts)
            && interval < qtdemux->trickmode_interval) {
          GST_LOG_OBJECT (qtdemux,
              "Skipping keyframe within interval on track-id %u",
              stream->track_id);
          goto next;
        } else {
          stream->last_keyframe_dts = stream->time_position;
        }
      }
    }
  }

  GST_DEBUG_OBJECT (qtdemux,
      "pushing from track-id %u, empty %d offset %" G_GUINT64_FORMAT
      ", size %d, dts=%" GST_TIME_FORMAT ", pts=%" GST_TIME_FORMAT
      ", duration %" GST_TIME_FORMAT, stream->track_id, empty, offset,
      sample_size, GST_TIME_ARGS (dts), GST_TIME_ARGS (pts),
      GST_TIME_ARGS (duration));

  if (G_UNLIKELY (empty)) {
    /* empty segment, push a gap if there's a second or more
     * difference and move to the next one */
    if ((pts + duration - stream->segment.position) >= GST_SECOND)
      gst_pad_push_event (stream->pad, gst_event_new_gap (pts, duration));
    stream->segment.position = pts + duration;
    goto next;
  }

  /* hmm, empty sample, skip and move to next sample */
  if (G_UNLIKELY (sample_size <= 0))
    goto next;

  /* last pushed sample was out of boundary, goto next sample */
  if (G_UNLIKELY (GST_PAD_LAST_FLOW_RETURN (stream->pad) == GST_FLOW_EOS))
    goto next;

  if (stream->max_buffer_size != 0 && sample_size > stream->max_buffer_size) {
    GST_DEBUG_OBJECT (qtdemux,
        "size %d larger than stream max_buffer_size %d, trimming",
        sample_size, stream->max_buffer_size);
    size =
        MIN (sample_size - stream->offset_in_sample, stream->max_buffer_size);
  } else if (stream->min_buffer_size != 0 && stream->offset_in_sample == 0
      && sample_size < stream->min_buffer_size) {
    guint start_sample_index = stream->sample_index;
    guint accumulated_size = sample_size;
    guint64 expected_next_offset = offset + sample_size;

    GST_DEBUG_OBJECT (qtdemux,
        "size %d smaller than stream min_buffer_size %d, combining with the next",
        sample_size, stream->min_buffer_size);

    while (stream->sample_index < stream->to_sample
        && stream->sample_index + 1 < stream->n_samples) {
      const QtDemuxSample *next_sample;

      /* Increment temporarily */
      stream->sample_index++;

      /* Failed to parse sample so let's go back to the previous one that was
       * still successful */
      if (!qtdemux_parse_samples (qtdemux, stream, stream->sample_index)) {
        stream->sample_index--;
        break;
      }

      next_sample = &stream->samples[stream->sample_index];

      /* Not contiguous with the previous sample so let's go back to the
       * previous one that was still successful */
      if (next_sample->offset != expected_next_offset) {
        stream->sample_index--;
        break;
      }

      accumulated_size += next_sample->size;
      expected_next_offset += next_sample->size;
      if (accumulated_size >= stream->min_buffer_size)
        break;
    }

    num_samples = stream->sample_index + 1 - start_sample_index;
    stream->sample_index = start_sample_index;
    GST_DEBUG_OBJECT (qtdemux, "Pulling %u samples of size %u at once",
        num_samples, accumulated_size);
    size = accumulated_size;
  } else {
    size = sample_size;
  }

  if (qtdemux->cenc_aux_info_offset > 0) {
    GstMapInfo map;
    GstByteReader br;
    GstBuffer *aux_info = NULL;

    /* pull the data stored before the sample */
    ret =
        gst_qtdemux_pull_atom (qtdemux, qtdemux->offset,
        offset + stream->offset_in_sample - qtdemux->offset, &aux_info);
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto beach;
    gst_buffer_map (aux_info, &map, GST_MAP_READ);
    GST_DEBUG_OBJECT (qtdemux, "parsing cenc auxiliary info");
    gst_byte_reader_init (&br, map.data + 8, map.size);
    if (!qtdemux_parse_cenc_aux_info (qtdemux, stream, &br,
            qtdemux->cenc_aux_info_sizes, qtdemux->cenc_aux_sample_count)) {
      GST_ERROR_OBJECT (qtdemux, "failed to parse cenc auxiliary info");
      gst_buffer_unmap (aux_info, &map);
      gst_buffer_unref (aux_info);
      ret = GST_FLOW_ERROR;
      goto beach;
    }
    gst_buffer_unmap (aux_info, &map);
    gst_buffer_unref (aux_info);
  }

  GST_LOG_OBJECT (qtdemux, "reading %d bytes @ %" G_GUINT64_FORMAT, size,
      offset);

  if (stream->use_allocator) {
    /* if we have a per-stream allocator, use it */
    buf = gst_buffer_new_allocate (stream->allocator, size, &stream->params);
  }

  ret = gst_qtdemux_pull_atom (qtdemux, offset + stream->offset_in_sample,
      size, &buf);
  if (G_UNLIKELY (ret != GST_FLOW_OK))
    goto beach;

  /* Update for both splitting and combining of samples */
  if (size != sample_size) {
    pts += gst_util_uint64_scale_int (GST_SECOND,
        stream->offset_in_sample / CUR_STREAM (stream)->bytes_per_frame,
        stream->timescale);
    dts +=
        gst_util_uint64_scale_int (GST_SECOND,
        stream->offset_in_sample / CUR_STREAM (stream)->bytes_per_frame,
        stream->timescale);
    duration =
        gst_util_uint64_scale_int (GST_SECOND,
        size / CUR_STREAM (stream)->bytes_per_frame, stream->timescale);
  }

  ret = gst_qtdemux_decorate_and_push_buffer (qtdemux, stream, buf,
      dts, pts, duration, keyframe, min_time, offset);

  if (size < sample_size) {
    QtDemuxSample *sample = &stream->samples[stream->sample_index];
    QtDemuxSegment *segment = &stream->segments[stream->segment_index];

    GstClockTime time_position = QTSTREAMTIME_TO_GSTTIME (stream,
        sample->timestamp +
        stream->offset_in_sample / CUR_STREAM (stream)->bytes_per_frame);
    if (time_position >= segment->media_start) {
      /* inside the segment, update time_position, looks very familiar to
       * GStreamer segments, doesn't it? */
      stream->time_position = (time_position - segment->media_start) +
          segment->time;
    } else {
      /* not yet in segment, time does not yet increment. This means
       * that we are still prerolling keyframes to the decoder so it can
       * decode the first sample of the segment. */
      stream->time_position = segment->time;
    }
  } else if (size > sample_size) {
    /* Increase to the last sample we already pulled so that advancing
     * below brings us to the next sample we need to pull */
    stream->sample_index += num_samples - 1;
  }

  /* combine flows */
  GST_OBJECT_LOCK (qtdemux);
  ret = gst_qtdemux_combine_flows (qtdemux, stream, ret);
  GST_OBJECT_UNLOCK (qtdemux);
  /* ignore unlinked, we will not push on the pad anymore and we will EOS when
   * we have no more data for the pad to push */
  if (ret == GST_FLOW_EOS)
    ret = GST_FLOW_OK;

  stream->offset_in_sample += size;
  if (stream->offset_in_sample >= sample_size) {
    gst_qtdemux_advance_sample (qtdemux, stream);
  }
  goto beach;

next:
  gst_qtdemux_advance_sample (qtdemux, stream);

beach:
  return ret;

  /* special cases */
eos:
  {
    GST_DEBUG_OBJECT (qtdemux, "No samples left for any streams - EOS");
    ret = GST_FLOW_EOS;
    goto beach;
  }
eos_stream:
  {
    GST_DEBUG_OBJECT (qtdemux, "No samples left for stream");
    /* EOS will be raised if all are EOS */
    ret = GST_FLOW_OK;
    goto beach;
  }
}

static void
gst_qtdemux_loop (GstPad * pad)
{
  GstQTDemux *qtdemux;
  guint64 cur_offset;
  GstFlowReturn ret;

  qtdemux = GST_QTDEMUX (gst_pad_get_parent (pad));

  cur_offset = qtdemux->offset;
  GST_LOG_OBJECT (qtdemux, "loop at position %" G_GUINT64_FORMAT ", state %s",
      cur_offset, qt_demux_state_string (qtdemux->state));

  switch (qtdemux->state) {
    case QTDEMUX_STATE_INITIAL:
    case QTDEMUX_STATE_HEADER:
      ret = gst_qtdemux_loop_state_header (qtdemux);
      break;
    case QTDEMUX_STATE_MOVIE:
      ret = gst_qtdemux_loop_state_movie (qtdemux);
      if (qtdemux->segment.rate < 0 && ret == GST_FLOW_EOS) {
        ret = gst_qtdemux_seek_to_previous_keyframe (qtdemux);
      }
      break;
    default:
      /* ouch */
      goto invalid_state;
  }

  /* if something went wrong, pause */
  if (ret != GST_FLOW_OK)
    goto pause;

done:
  gst_object_unref (qtdemux);
  return;

  /* ERRORS */
invalid_state:
  {
    GST_ELEMENT_ERROR (qtdemux, STREAM, FAILED,
        (NULL), ("streaming stopped, invalid state"));
    gst_pad_pause_task (pad);
    gst_qtdemux_push_event (qtdemux, gst_event_new_eos ());
    goto done;
  }
pause:
  {
    const gchar *reason = gst_flow_get_name (ret);

    GST_LOG_OBJECT (qtdemux, "pausing task, reason %s", reason);

    gst_pad_pause_task (pad);

    /* fatal errors need special actions */
    /* check EOS */
    if (ret == GST_FLOW_EOS) {
      if (QTDEMUX_N_STREAMS (qtdemux) == 0) {
        /* we have no streams, post an error */
        gst_qtdemux_post_no_playable_stream_error (qtdemux);
      }
      if (qtdemux->segment.flags & GST_SEEK_FLAG_SEGMENT) {
        gint64 stop;

        if ((stop = qtdemux->segment.stop) == -1)
          stop = qtdemux->segment.duration;

        if (qtdemux->segment.rate >= 0) {
          GstMessage *message;
          GstEvent *event;

          /* segment.position will still be at the last timestamp and won't always
           * include the duration of the last packet. Expand that to the segment
           * duration so that segment.base is increased correctly to include the
           * length of the last packet when doing segment seeks. We need to do
           * this before the segment-done event goes out so everything's ready
           * for the next seek request coming in. */
          if (GST_CLOCK_TIME_IS_VALID (stop)) {
            GST_DEBUG_OBJECT (qtdemux,
                "End of segment, updating segment.position from %"
                GST_TIME_FORMAT " to stop %" GST_TIME_FORMAT,
                GST_TIME_ARGS (qtdemux->segment.position),
                GST_TIME_ARGS (stop));
            qtdemux->segment.position = stop;
          }

          GST_LOG_OBJECT (qtdemux, "Sending segment done, at end of segment");
          message = gst_message_new_segment_done (GST_OBJECT_CAST (qtdemux),
              GST_FORMAT_TIME, stop);
          event = gst_event_new_segment_done (GST_FORMAT_TIME, stop);
          if (qtdemux->segment_seqnum != GST_SEQNUM_INVALID) {
            gst_message_set_seqnum (message, qtdemux->segment_seqnum);
            gst_event_set_seqnum (event, qtdemux->segment_seqnum);
          }
          gst_element_post_message (GST_ELEMENT_CAST (qtdemux), message);
          gst_qtdemux_push_event (qtdemux, event);
        } else {
          GstMessage *message;
          GstEvent *event;

          /*  For Reverse Playback */
          GST_LOG_OBJECT (qtdemux, "Sending segment done, at start of segment");
          message = gst_message_new_segment_done (GST_OBJECT_CAST (qtdemux),
              GST_FORMAT_TIME, qtdemux->segment.start);
          event = gst_event_new_segment_done (GST_FORMAT_TIME,
              qtdemux->segment.start);
          if (qtdemux->segment_seqnum != GST_SEQNUM_INVALID) {
            gst_message_set_seqnum (message, qtdemux->segment_seqnum);
            gst_event_set_seqnum (event, qtdemux->segment_seqnum);
          }
          gst_element_post_message (GST_ELEMENT_CAST (qtdemux), message);
          gst_qtdemux_push_event (qtdemux, event);
        }
      } else {
        GstEvent *event;

        GST_LOG_OBJECT (qtdemux, "Sending EOS at end of segment");
        event = gst_event_new_eos ();
        if (qtdemux->segment_seqnum != GST_SEQNUM_INVALID)
          gst_event_set_seqnum (event, qtdemux->segment_seqnum);
        gst_qtdemux_push_event (qtdemux, event);
      }
    } else if (ret == GST_FLOW_NOT_LINKED || ret < GST_FLOW_EOS) {
      GST_ELEMENT_FLOW_ERROR (qtdemux, ret);
      gst_qtdemux_push_event (qtdemux, gst_event_new_eos ());
    }
    goto done;
  }
}

/*
 * has_next_entry
 *
 * Returns if there are samples to be played.
 */
static gboolean
has_next_entry (GstQTDemux * demux)
{
  QtDemuxStream *stream;
  gint i;

  GST_DEBUG_OBJECT (demux, "Checking if there are samples not played yet");

  for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
    stream = QTDEMUX_NTH_STREAM (demux, i);

    if (stream->sample_index == -1) {
      stream->sample_index = 0;
      stream->offset_in_sample = 0;
    }

    if (stream->sample_index >= stream->n_samples) {
      GST_LOG_OBJECT (demux, "track-id %u samples exhausted", stream->track_id);
      continue;
    }
    GST_DEBUG_OBJECT (demux, "Found a sample");
    return TRUE;
  }

  GST_DEBUG_OBJECT (demux, "There wasn't any next sample");
  return FALSE;
}

/*
 * next_entry_size
 *
 * Returns the size of the first entry at the current offset.
 * If -1, there are none (which means EOS or empty file).
 */
static guint64
next_entry_size (GstQTDemux * demux)
{
  QtDemuxStream *stream, *target_stream = NULL;
  guint64 smalloffs = (guint64) - 1;
  QtDemuxSample *sample;
  gint i;

  GST_LOG_OBJECT (demux, "Finding entry at offset %" G_GUINT64_FORMAT,
      demux->offset);

  for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
    stream = QTDEMUX_NTH_STREAM (demux, i);

    if (stream->sample_index == -1) {
      stream->sample_index = 0;
      stream->offset_in_sample = 0;
    }

    if (stream->sample_index >= stream->n_samples) {
      GST_LOG_OBJECT (demux, "track-id %u samples exhausted", stream->track_id);
      continue;
    }

    if (!qtdemux_parse_samples (demux, stream, stream->sample_index)) {
      GST_LOG_OBJECT (demux, "Parsing of index %u from stbl atom failed!",
          stream->sample_index);
      return -1;
    }

    sample = &stream->samples[stream->sample_index];

    GST_LOG_OBJECT (demux,
        "Checking track-id %u (sample_index:%d / offset:%" G_GUINT64_FORMAT
        " / size:%" G_GUINT32_FORMAT ")", stream->track_id,
        stream->sample_index, sample->offset, sample->size);

    if (((smalloffs == -1)
            || (sample->offset < smalloffs)) && (sample->size)) {
      smalloffs = sample->offset;
      target_stream = stream;
    }
  }

  if (!target_stream)
    return -1;

  GST_LOG_OBJECT (demux,
      "track-id %u offset %" G_GUINT64_FORMAT " demux->offset :%"
      G_GUINT64_FORMAT, target_stream->track_id, smalloffs, demux->offset);

  stream = target_stream;
  sample = &stream->samples[stream->sample_index];

  if (sample->offset >= demux->offset) {
    demux->todrop = sample->offset - demux->offset;
    return sample->size + demux->todrop;
  }

  GST_DEBUG_OBJECT (demux,
      "There wasn't any entry at offset %" G_GUINT64_FORMAT, demux->offset);
  return -1;
}

static void
gst_qtdemux_post_progress (GstQTDemux * demux, gint num, gint denom)
{
  gint perc = (gint) ((gdouble) num * 100.0 / (gdouble) denom);

  gst_element_post_message (GST_ELEMENT_CAST (demux),
      gst_message_new_element (GST_OBJECT_CAST (demux),
          gst_structure_new ("progress", "percent", G_TYPE_INT, perc, NULL)));
}

static gboolean
qtdemux_seek_offset (GstQTDemux * demux, guint64 offset)
{
  GstEvent *event;
  gboolean res = 0;

  GST_DEBUG_OBJECT (demux, "Seeking to %" G_GUINT64_FORMAT, offset);

  event =
      gst_event_new_seek (1.0, GST_FORMAT_BYTES,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SEEK_TYPE_SET, offset,
      GST_SEEK_TYPE_NONE, -1);

  /* store seqnum to drop flush events, they don't need to reach downstream */
  demux->offset_seek_seqnum = gst_event_get_seqnum (event);
  res = gst_pad_push_event (demux->sinkpad, event);
  demux->offset_seek_seqnum = GST_SEQNUM_INVALID;

  return res;
}

/* check for seekable upstream, above and beyond a mere query */
static void
gst_qtdemux_check_seekability (GstQTDemux * demux)
{
  GstQuery *query;
  gboolean seekable = FALSE;
  gint64 start = -1, stop = -1;

  if (demux->upstream_size)
    return;

  if (demux->upstream_format_is_time)
    return;

  query = gst_query_new_seeking (GST_FORMAT_BYTES);
  if (!gst_pad_peer_query (demux->sinkpad, query)) {
    GST_DEBUG_OBJECT (demux, "seeking query failed");
    goto done;
  }

  gst_query_parse_seeking (query, NULL, &seekable, &start, &stop);

  /* try harder to query upstream size if we didn't get it the first time */
  if (seekable && stop == -1) {
    GST_DEBUG_OBJECT (demux, "doing duration query to fix up unset stop");
    gst_pad_peer_query_duration (demux->sinkpad, GST_FORMAT_BYTES, &stop);
  }

  /* if upstream doesn't know the size, it's likely that it's not seekable in
   * practice even if it technically may be seekable */
  if (seekable && (start != 0 || stop <= start)) {
    GST_DEBUG_OBJECT (demux, "seekable but unknown start/stop -> disable");
    seekable = FALSE;
  }

done:
  gst_query_unref (query);

  GST_DEBUG_OBJECT (demux, "seekable: %d (%" G_GUINT64_FORMAT " - %"
      G_GUINT64_FORMAT ")", seekable, start, stop);
  demux->upstream_seekable = seekable;
  demux->upstream_size = seekable ? stop : -1;
}

static void
gst_qtdemux_drop_data (GstQTDemux * demux, gint bytes)
{
  g_return_if_fail (bytes <= demux->todrop);

  GST_LOG_OBJECT (demux, "Dropping %d bytes", bytes);
  gst_adapter_flush (demux->adapter, bytes);
  demux->neededbytes -= bytes;
  demux->offset += bytes;
  demux->todrop -= bytes;
}

/* PUSH-MODE only: Send a segment, if not done already. */
static void
gst_qtdemux_check_send_pending_segment (GstQTDemux * demux)
{
  if (G_UNLIKELY (demux->need_segment)) {
    gint i;

    if (!demux->upstream_format_is_time) {
      gst_qtdemux_map_and_push_segments (demux, &demux->segment);
    } else {
      GstEvent *segment_event;
      segment_event = gst_event_new_segment (&demux->segment);
      if (demux->segment_seqnum != GST_SEQNUM_INVALID)
        gst_event_set_seqnum (segment_event, demux->segment_seqnum);
      gst_qtdemux_push_event (demux, segment_event);
    }

    demux->need_segment = FALSE;

    /* clear to send tags on all streams */
    for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
      QtDemuxStream *stream = QTDEMUX_NTH_STREAM (demux, i);
      gst_qtdemux_push_tags (demux, stream);
      if (CUR_STREAM (stream)->sparse) {
        GST_INFO_OBJECT (demux, "Sending gap event on stream %d", i);
        gst_pad_push_event (stream->pad,
            gst_event_new_gap (stream->segment.position, GST_CLOCK_TIME_NONE));
      }
    }
  }
}

/* Used for push mode only. */
static void
gst_qtdemux_send_gap_for_segment (GstQTDemux * demux,
    QtDemuxStream * stream, gint segment_index, GstClockTime pos)
{
  GstClockTime ts, dur;

  ts = pos;
  dur =
      stream->segments[segment_index].duration - (pos -
      stream->segments[segment_index].time);
  stream->time_position += dur;

  /* Only gaps with a duration of at least one second are propagated.
   * Same workaround as in pull mode.
   * (See 2e45926a96ec5298c6ef29bf912e5e6a06dc3e0e) */
  if (dur >= GST_SECOND) {
    GstEvent *gap;
    gap = gst_event_new_gap (ts, dur);

    GST_DEBUG_OBJECT (stream->pad, "Pushing gap for empty "
        "segment: %" GST_PTR_FORMAT, gap);
    gst_pad_push_event (stream->pad, gap);
  }
}

static void
qtdemux_check_if_is_gapless_audio (GstQTDemux * qtdemux)
{
  QtDemuxStream *stream;

  if (QTDEMUX_N_STREAMS (qtdemux) != 1)
    goto incompatible_stream;

  stream = QTDEMUX_NTH_STREAM (qtdemux, 0);

  if (stream->subtype != FOURCC_soun || stream->n_segments != 1)
    goto incompatible_stream;

  /* Gapless audio info from revdns tags (most notably iTunSMPB) is
   * detected in the main udta node. If it isn't present, try as
   * fallback to recognize the encoder name, and apply known priming
   * and padding quantities specific to the encoder. */
  if (qtdemux->gapless_audio_info.type == GAPLESS_AUDIO_INFO_TYPE_NONE) {
    const gchar *orig_encoder_name = NULL;

    if (gst_tag_list_peek_string_index (qtdemux->tag_list, GST_TAG_ENCODER, 0,
            &orig_encoder_name) && orig_encoder_name != NULL) {
      gchar *lowercase_encoder_name = g_ascii_strdown (orig_encoder_name, -1);

      if (strstr (lowercase_encoder_name, "nero") != NULL)
        qtdemux->gapless_audio_info.type = GAPLESS_AUDIO_INFO_TYPE_NERO;

      g_free (lowercase_encoder_name);

      switch (qtdemux->gapless_audio_info.type) {
        case GAPLESS_AUDIO_INFO_TYPE_NERO:{
          guint64 total_length;
          guint64 valid_length;
          guint64 start_padding;

          /* The Nero AAC encoder always uses a lead-in of 1600 PCM frames.
           * Also, in Nero AAC's case, stream->duration contains the number
           * of PCM frames with start padding but without end padding.
           * The decoder delay equals 1 frame length, which is covered by
           * factoring stream->stts_duration into the start padding. */
          start_padding = 1600 + stream->stts_duration;

          if (G_UNLIKELY (stream->duration < start_padding)) {
            GST_ERROR_OBJECT (qtdemux, "stream duration is %" G_GUINT64_FORMAT
                " but start_padding is %" G_GUINT64_FORMAT, stream->duration,
                start_padding);
            goto invalid_gapless_audio_info;
          }
          valid_length = stream->duration - start_padding;

          qtdemux->gapless_audio_info.num_start_padding_pcm_frames =
              start_padding;
          qtdemux->gapless_audio_info.num_valid_pcm_frames = valid_length;

          total_length = stream->n_samples * stream->stts_duration;

          if (G_LIKELY (total_length >= valid_length)) {
            guint64 total_padding = total_length - valid_length;
            if (G_UNLIKELY (total_padding < start_padding)) {
              GST_ERROR_OBJECT (qtdemux, "total_padding is %" G_GUINT64_FORMAT
                  " but start_padding is %" G_GUINT64_FORMAT, total_padding,
                  start_padding);
              goto invalid_gapless_audio_info;
            }

            qtdemux->gapless_audio_info.num_end_padding_pcm_frames =
                total_padding - start_padding;
          } else {
            qtdemux->gapless_audio_info.num_end_padding_pcm_frames = 0;
          }

          GST_DEBUG_OBJECT (qtdemux, "media was encoded with Nero AAC encoder; "
              "using encoder specific lead-in and padding figures");
        }

        default:
          break;
      }
    }
  }

  if (qtdemux->gapless_audio_info.type != GAPLESS_AUDIO_INFO_TYPE_NONE) {
    qtdemux->gapless_audio_info.start_padding_duration =
        QTSTREAMTIME_TO_GSTTIME (stream,
        qtdemux->gapless_audio_info.num_start_padding_pcm_frames);
    qtdemux->gapless_audio_info.end_padding_duration =
        QTSTREAMTIME_TO_GSTTIME (stream,
        qtdemux->gapless_audio_info.num_end_padding_pcm_frames);
    qtdemux->gapless_audio_info.valid_duration =
        QTSTREAMTIME_TO_GSTTIME (stream,
        qtdemux->gapless_audio_info.num_valid_pcm_frames);
  }

  GST_DEBUG_OBJECT (qtdemux, "found valid gapless audio info: num start / end "
      "PCM padding frames: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "; "
      "start / end padding durations: %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT
      "; num valid PCM frames: %" G_GUINT64_FORMAT "; valid duration: %"
      GST_TIME_FORMAT, qtdemux->gapless_audio_info.num_start_padding_pcm_frames,
      qtdemux->gapless_audio_info.num_end_padding_pcm_frames,
      GST_TIME_ARGS (qtdemux->gapless_audio_info.start_padding_duration),
      GST_TIME_ARGS (qtdemux->gapless_audio_info.end_padding_duration),
      qtdemux->gapless_audio_info.num_valid_pcm_frames,
      GST_TIME_ARGS (qtdemux->gapless_audio_info.valid_duration));

  return;

incompatible_stream:
  if (G_UNLIKELY (qtdemux->gapless_audio_info.type !=
          GAPLESS_AUDIO_INFO_TYPE_NONE)) {
    GST_WARNING_OBJECT (qtdemux,
        "media contains gapless audio info, but it is not suitable for "
        "gapless audio playback (media must be audio-only, single-stream, "
        "single-segment; ignoring unusable gapless info");
    qtdemux->gapless_audio_info.type = GAPLESS_AUDIO_INFO_TYPE_NONE;
  }
  return;

invalid_gapless_audio_info:
  GST_WARNING_OBJECT (qtdemux,
      "media contains invalid/unusable gapless audio info");
  return;
}

static GstFlowReturn
gst_qtdemux_chain (GstPad * sinkpad, GstObject * parent, GstBuffer * inbuf)
{
  GstQTDemux *demux;

  demux = GST_QTDEMUX (parent);

  GST_DEBUG_OBJECT (demux,
      "Received buffer pts:%" GST_TIME_FORMAT " dts:%" GST_TIME_FORMAT
      " offset:%" G_GUINT64_FORMAT " size:%" G_GSIZE_FORMAT " demux offset:%"
      G_GUINT64_FORMAT, GST_TIME_ARGS (GST_BUFFER_PTS (inbuf)),
      GST_TIME_ARGS (GST_BUFFER_DTS (inbuf)), GST_BUFFER_OFFSET (inbuf),
      gst_buffer_get_size (inbuf), demux->offset);

  if (GST_BUFFER_FLAG_IS_SET (inbuf, GST_BUFFER_FLAG_DISCONT)) {
    gboolean is_gap_input = FALSE;
    gint i;

    GST_DEBUG_OBJECT (demux, "Got DISCONT, marking all streams as DISCONT");

    for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
      QTDEMUX_NTH_STREAM (demux, i)->discont = TRUE;
    }

    /* Check if we can land back on our feet in the case where upstream is
     * handling the seeking/pushing of samples with gaps in between (like
     * in the case of trick-mode DASH for example) */
    if (demux->upstream_format_is_time
        && GST_BUFFER_OFFSET (inbuf) != GST_BUFFER_OFFSET_NONE) {
      for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
        guint32 res;
        QtDemuxStream *stream = QTDEMUX_NTH_STREAM (demux, i);
        GST_LOG_OBJECT (demux,
            "track-id #%u , checking if offset %" G_GUINT64_FORMAT
            " is a sample start", stream->track_id, GST_BUFFER_OFFSET (inbuf));
        res =
            gst_qtdemux_find_index_for_given_media_offset_linear (demux,
            stream, GST_BUFFER_OFFSET (inbuf));
        if (res != -1) {
          QtDemuxSample *sample = &stream->samples[res];
          GST_LOG_OBJECT (demux,
              "Checking if sample %d from track-id %u is valid (offset:%"
              G_GUINT64_FORMAT " size:%" G_GUINT32_FORMAT ")", res,
              stream->track_id, sample->offset, sample->size);
          if (sample->offset == GST_BUFFER_OFFSET (inbuf)) {
            GST_LOG_OBJECT (demux,
                "new buffer corresponds to a valid sample : %" G_GUINT32_FORMAT,
                res);
            is_gap_input = TRUE;
            /* We can go back to standard playback mode */
            demux->state = QTDEMUX_STATE_MOVIE;
            /* Remember which sample this stream is at */
            stream->sample_index = res;
            /* Finally update all push-based values to the expected values */
            demux->neededbytes = stream->samples[res].size;
            demux->offset = GST_BUFFER_OFFSET (inbuf);
            demux->mdatleft =
                demux->mdatsize - demux->offset + demux->mdatoffset;
            demux->todrop = 0;
          }
        }
      }
      if (!is_gap_input) {
        GST_DEBUG_OBJECT (demux, "Resetting, actual DISCONT");
        /* Reset state if it's a real discont */
        demux->neededbytes = 16;
        demux->state = QTDEMUX_STATE_INITIAL;
        demux->offset = GST_BUFFER_OFFSET (inbuf);
        gst_adapter_clear (demux->adapter);
      }
    }
    /* Reverse fragmented playback, need to flush all we have before
     * consuming a new fragment.
     * The samples array have the timestamps calculated by accumulating the
     * durations but this won't work for reverse playback of fragments as
     * the timestamps of a subsequent fragment should be smaller than the
     * previously received one. */
    if (!is_gap_input && demux->fragmented && demux->segment.rate < 0) {
      gst_qtdemux_process_adapter (demux, TRUE);
      g_ptr_array_foreach (demux->active_streams,
          (GFunc) gst_qtdemux_stream_flush_samples_data, NULL);
    }
  }

  gst_adapter_push (demux->adapter, inbuf);

  GST_DEBUG_OBJECT (demux,
      "pushing in inbuf %p, neededbytes:%u, available:%" G_GSIZE_FORMAT, inbuf,
      demux->neededbytes, gst_adapter_available (demux->adapter));

  return gst_qtdemux_process_adapter (demux, FALSE);
}

static guint64
gst_segment_to_stream_time_clamped (const GstSegment * segment,
    guint64 position)
{
  guint64 segment_stream_time_start;
  guint64 segment_stream_time_stop = GST_CLOCK_TIME_NONE;
  guint64 stream_pts_unsigned;
  int ret;

  g_return_val_if_fail (segment != NULL, GST_CLOCK_TIME_NONE);
  g_return_val_if_fail (segment->format == GST_FORMAT_TIME,
      GST_CLOCK_TIME_NONE);

  segment_stream_time_start = segment->time;
  if (segment->stop != GST_CLOCK_TIME_NONE)
    segment_stream_time_stop =
        gst_segment_to_stream_time (segment, GST_FORMAT_TIME, segment->stop);

  ret =
      gst_segment_to_stream_time_full (segment, GST_FORMAT_TIME, position,
      &stream_pts_unsigned);
  /* ret == 0 if the segment is invalid (either position, segment->time or the segment start are -1). */
  g_return_val_if_fail (ret != 0, GST_CLOCK_TIME_NONE);

  if (ret == -1 || stream_pts_unsigned < segment_stream_time_start) {
    /* Negative or prior to segment start stream time, clamp to segment start. */
    return segment_stream_time_start;
  } else if (segment_stream_time_stop != GST_CLOCK_TIME_NONE
      && stream_pts_unsigned > segment_stream_time_stop) {
    /* Clamp to segment end. */
    return segment_stream_time_stop;
  } else {
    return stream_pts_unsigned;
  }
}

static GstFlowReturn
gst_qtdemux_process_adapter (GstQTDemux * demux, gboolean force)
{
  GstFlowReturn ret = GST_FLOW_OK;

  /* we never really mean to buffer that much */
  if (demux->neededbytes == -1) {
    goto eos;
  }

  while (((gst_adapter_available (demux->adapter)) >= demux->neededbytes) &&
      (ret == GST_FLOW_OK || (ret == GST_FLOW_NOT_LINKED && force))) {

#ifndef GST_DISABLE_GST_DEBUG
    {
      guint64 discont_offset, distance_from_discont;

      discont_offset = gst_adapter_offset_at_discont (demux->adapter);
      distance_from_discont =
          gst_adapter_distance_from_discont (demux->adapter);

      GST_DEBUG_OBJECT (demux,
          "state:%s , demux->neededbytes:%d, demux->offset:%" G_GUINT64_FORMAT
          " adapter offset :%" G_GUINT64_FORMAT " (+ %" G_GUINT64_FORMAT
          " bytes)", qt_demux_state_string (demux->state), demux->neededbytes,
          demux->offset, discont_offset, distance_from_discont);
    }
#endif

    switch (demux->state) {
      case QTDEMUX_STATE_INITIAL:{
        const guint8 *data;
        guint32 fourcc;
        guint64 size;

        gst_qtdemux_check_seekability (demux);

        data = gst_adapter_map (demux->adapter, demux->neededbytes);

        /* get fourcc/length, set neededbytes */
        extract_initial_length_and_fourcc ((guint8 *) data, demux->neededbytes,
            &size, &fourcc);
        gst_adapter_unmap (demux->adapter);
        data = NULL;
        GST_DEBUG_OBJECT (demux, "Peeking found [%" GST_FOURCC_FORMAT "] "
            "size: %" G_GUINT64_FORMAT, GST_FOURCC_ARGS (fourcc), size);
        if (size == 0) {
          GST_ELEMENT_ERROR (demux, STREAM, DEMUX,
              (_("This file is invalid and cannot be played.")),
              ("initial atom '%" GST_FOURCC_FORMAT "' has empty length",
                  GST_FOURCC_ARGS (fourcc)));
          ret = GST_FLOW_ERROR;
          break;
        }
        if (fourcc == FOURCC_mdat) {
          gint next_entry = next_entry_size (demux);
          if (QTDEMUX_N_STREAMS (demux) > 0 && (next_entry != -1
                  || !demux->fragmented)) {
            /* we have the headers, start playback */
            demux->state = QTDEMUX_STATE_MOVIE;
            demux->neededbytes = next_entry;
            demux->mdatleft = size;
            demux->mdatsize = demux->mdatleft;
          } else {
            /* no headers yet, try to get them */
            guint bs;
            gboolean res;
            guint64 old, target;

          buffer_data:
            old = demux->offset;
            target = old + size;

            /* try to jump over the atom with a seek */
            /* only bother if it seems worth doing so,
             * and avoids possible upstream/server problems */
            if (demux->upstream_seekable &&
                demux->upstream_size > 4 * (1 << 20)) {
              res = qtdemux_seek_offset (demux, target);
            } else {
              GST_DEBUG_OBJECT (demux, "skipping seek");
              res = FALSE;
            }

            if (res) {
              GST_DEBUG_OBJECT (demux, "seek success");
              /* remember the offset fo the first mdat so we can seek back to it
               * after we have the headers */
              if (fourcc == FOURCC_mdat && demux->first_mdat == -1) {
                demux->first_mdat = old;
                GST_DEBUG_OBJECT (demux, "first mdat at %" G_GUINT64_FORMAT,
                    demux->first_mdat);
              }
              /* seek worked, continue reading */
              demux->offset = target;
              demux->neededbytes = 16;
              demux->state = QTDEMUX_STATE_INITIAL;
            } else {
              /* seek failed, need to buffer */
              demux->offset = old;
              GST_DEBUG_OBJECT (demux, "seek failed/skipped");
              /* there may be multiple mdat (or alike) buffers */
              /* sanity check */
              if (demux->mdatbuffer)
                bs = gst_buffer_get_size (demux->mdatbuffer);
              else
                bs = 0;
              if (size + bs > 10 * (1 << 20))
                goto no_moov;
              demux->state = QTDEMUX_STATE_BUFFER_MDAT;
              demux->neededbytes = size;
              if (!demux->mdatbuffer)
                demux->mdatoffset = demux->offset;
            }
          }
        } else if (G_UNLIKELY (size > QTDEMUX_MAX_ATOM_SIZE)) {
          GST_ELEMENT_ERROR (demux, STREAM, DEMUX,
              (_("This file is invalid and cannot be played.")),
              ("atom %" GST_FOURCC_FORMAT " has bogus size %" G_GUINT64_FORMAT,
                  GST_FOURCC_ARGS (fourcc), size));
          ret = GST_FLOW_ERROR;
          break;
        } else {
          /* this means we already started buffering and still no moov header,
           * let's continue buffering everything till we get moov */
          if (demux->mdatbuffer && !(fourcc == FOURCC_moov
                  || fourcc == FOURCC_moof))
            goto buffer_data;
          demux->neededbytes = size;
          demux->state = QTDEMUX_STATE_HEADER;
        }
        break;
      }
      case QTDEMUX_STATE_HEADER:{
        const guint8 *data;
        guint32 fourcc;

        GST_DEBUG_OBJECT (demux, "In header");

        data = gst_adapter_map (demux->adapter, demux->neededbytes);

        /* parse the header */
        extract_initial_length_and_fourcc (data, demux->neededbytes, NULL,
            &fourcc);
        if (fourcc == FOURCC_moov) {
          /* in usual fragmented setup we could try to scan for more
           * and end up at the the moov (after mdat) again */
          if (demux->got_moov && QTDEMUX_N_STREAMS (demux) > 0 &&
              (!demux->fragmented
                  || demux->last_moov_offset == demux->offset)) {
            GST_DEBUG_OBJECT (demux,
                "Skipping moov atom as we have (this) one already");
          } else {
            GST_DEBUG_OBJECT (demux, "Parsing [moov]");

            if (demux->got_moov && demux->fragmented) {
              GST_DEBUG_OBJECT (demux,
                  "Got a second moov, clean up data from old one");
              if (demux->moov_node_compressed) {
                g_node_destroy (demux->moov_node_compressed);
                if (demux->moov_node)
                  g_free (demux->moov_node->data);
              }
              demux->moov_node_compressed = NULL;
              if (demux->moov_node)
                g_node_destroy (demux->moov_node);
              demux->moov_node = NULL;
              demux->start_utc_time = GST_CLOCK_TIME_NONE;
            }

            demux->last_moov_offset = demux->offset;

            /* Update streams with new moov */
            gst_qtdemux_stream_concat (demux,
                demux->old_streams, demux->active_streams);

            if (!qtdemux_parse_moov (demux, data, demux->neededbytes)) {
              ret = GST_FLOW_ERROR;
              break;
            }
            qtdemux_node_dump (demux, demux->moov_node);
            qtdemux_parse_tree (demux);
            ret = qtdemux_prepare_streams (demux);
            if (ret != GST_FLOW_OK)
              break;

            QTDEMUX_EXPOSE_LOCK (demux);
            ret = qtdemux_expose_streams (demux);
            QTDEMUX_EXPOSE_UNLOCK (demux);
            if (ret != GST_FLOW_OK)
              break;

            demux->got_moov = TRUE;

            gst_qtdemux_check_send_pending_segment (demux);

            if (demux->moov_node_compressed) {
              g_node_destroy (demux->moov_node_compressed);
              g_free (demux->moov_node->data);
            }
            demux->moov_node_compressed = NULL;
            g_node_destroy (demux->moov_node);
            demux->moov_node = NULL;
            GST_DEBUG_OBJECT (demux, "Finished parsing the header");
          }
        } else if (fourcc == FOURCC_moof) {
          if ((demux->got_moov || demux->media_caps) && demux->fragmented) {
            guint64 dist = 0;
            GstClockTime prev_pts;
            guint64 prev_offset;
            guint64 adapter_discont_offset, adapter_discont_dist;

            GST_DEBUG_OBJECT (demux, "Parsing [moof]");

            /*
             * The timestamp of the moof buffer is relevant as some scenarios
             * won't have the initial timestamp in the atoms. Whenever a new
             * buffer has started, we get that buffer's PTS and use it as a base
             * timestamp for the trun entries.
             *
             * To keep track of the current buffer timestamp and starting point
             * we use gst_adapter_prev_pts that gives us the PTS and the distance
             * from the beginning of the buffer, with the distance and demux->offset
             * we know if it is still the same buffer or not.
             */
            prev_pts = gst_adapter_prev_pts (demux->adapter, &dist);
            prev_offset = demux->offset - dist;
            if (demux->fragment_start_offset == -1
                || prev_offset > demux->fragment_start_offset) {
              demux->fragment_start_offset = prev_offset;
              demux->fragment_start = prev_pts;
              GST_DEBUG_OBJECT (demux,
                  "New fragment start found at: %" G_GUINT64_FORMAT " : %"
                  GST_TIME_FORMAT, demux->fragment_start_offset,
                  GST_TIME_ARGS (demux->fragment_start));
            }

            /* We can't use prev_offset() here because this would require
             * upstream to set consistent and correct offsets on all buffers
             * since the discont. Nothing ever did that in the past and we
             * would break backwards compatibility here then.
             * Instead take the offset we had at the last discont and count
             * the bytes from there. This works with old code as there would
             * be no discont between moov and moof, and also works with
             * adaptivedemux which correctly sets offset and will set the
             * DISCONT flag accordingly when needed.
             *
             * We also only do this for upstream TIME segments as otherwise
             * there are potential backwards compatibility problems with
             * seeking in PUSH mode and upstream providing inconsistent
             * timestamps. */
            adapter_discont_offset =
                gst_adapter_offset_at_discont (demux->adapter);
            adapter_discont_dist =
                gst_adapter_distance_from_discont (demux->adapter);

            GST_DEBUG_OBJECT (demux,
                "demux offset %" G_GUINT64_FORMAT " adapter offset %"
                G_GUINT64_FORMAT " (+ %" G_GUINT64_FORMAT " bytes)",
                demux->offset, adapter_discont_offset, adapter_discont_dist);

            if (demux->upstream_format_is_time) {
              demux->moof_offset = adapter_discont_offset;
              if (demux->moof_offset != GST_BUFFER_OFFSET_NONE)
                demux->moof_offset += adapter_discont_dist;
              if (demux->moof_offset == GST_BUFFER_OFFSET_NONE)
                demux->moof_offset = demux->offset;
            } else {
              demux->moof_offset = demux->offset;
            }

            if (!qtdemux_parse_moof (demux, data, demux->neededbytes,
                    demux->moof_offset, NULL)) {
              gst_adapter_unmap (demux->adapter);
              ret = GST_FLOW_ERROR;
              goto done;
            }

            /* in MSS we need to expose the pads after the first moof as we won't get a moov */
            if (demux->variant == VARIANT_MSS_FRAGMENTED && !demux->exposed) {
              QTDEMUX_EXPOSE_LOCK (demux);
              ret = qtdemux_expose_streams (demux);
              QTDEMUX_EXPOSE_UNLOCK (demux);
              if (ret != GST_FLOW_OK)
                goto done;
            }

            gst_qtdemux_check_send_pending_segment (demux);
          } else {
            GST_DEBUG_OBJECT (demux, "Discarding [moof]");
          }
        } else if (fourcc == FOURCC_ftyp) {
          GST_DEBUG_OBJECT (demux, "Parsing [ftyp]");
          qtdemux_parse_ftyp (demux, data, demux->neededbytes);
        } else if (fourcc == FOURCC_uuid) {
          GST_DEBUG_OBJECT (demux, "Parsing [uuid]");
          qtdemux_parse_uuid (demux, data, demux->neededbytes);
        } else if (fourcc == FOURCC_sidx) {
          GST_DEBUG_OBJECT (demux, "Parsing [sidx]");
          qtdemux_parse_sidx (demux, data, demux->neededbytes);
        } else if (fourcc == FOURCC_meta) {
          GNode *node, *child;
          GstByteReader child_data;

          node = g_node_new ((gpointer) data);
          qtdemux_parse_node (demux, node, data, demux->neededbytes);

          /* Parse ONVIF Export File Format CorrectStartTime box if available */
          if ((child =
                  qtdemux_tree_get_child_by_type_full (node, FOURCC_cstb,
                      &child_data))) {
            qtdemux_parse_cstb (demux, &child_data);
          }

          g_node_destroy (node);
        } else {
          switch (fourcc) {
            case FOURCC_styp:
              /* [styp] is like a [ftyp], but in fragment header. We ignore it for now
               * FALLTHROUGH */
            case FOURCC_skip:
            case FOURCC_free:
              /* [free] and [skip] are padding atoms */
              GST_DEBUG_OBJECT (demux,
                  "Skipping fourcc while parsing header : %" GST_FOURCC_FORMAT,
                  GST_FOURCC_ARGS (fourcc));
              break;
            default:
              GST_WARNING_OBJECT (demux,
                  "Unknown fourcc while parsing header : %" GST_FOURCC_FORMAT,
                  GST_FOURCC_ARGS (fourcc));
              /* Let's jump that one and go back to initial state */
              break;
          }
        }
        gst_adapter_unmap (demux->adapter);
        data = NULL;

        if (demux->mdatbuffer && QTDEMUX_N_STREAMS (demux)) {
          gsize remaining_data_size = 0;

          /* the mdat was before the header */
          GST_DEBUG_OBJECT (demux, "We have n_streams:%d and mdatbuffer:%p",
              QTDEMUX_N_STREAMS (demux), demux->mdatbuffer);
          /* restore our adapter/offset view of things with upstream;
           * put preceding buffered data ahead of current moov data.
           * This should also handle evil mdat, moov, mdat cases and alike */
          gst_adapter_flush (demux->adapter, demux->neededbytes);

          /* Store any remaining data after the mdat for later usage */
          remaining_data_size = gst_adapter_available (demux->adapter);
          if (remaining_data_size > 0) {
            g_assert (demux->restoredata_buffer == NULL);
            demux->restoredata_buffer =
                gst_adapter_take_buffer (demux->adapter, remaining_data_size);
            demux->restoredata_offset = demux->offset + demux->neededbytes;
            GST_DEBUG_OBJECT (demux,
                "Stored %" G_GSIZE_FORMAT " post mdat bytes at offset %"
                G_GUINT64_FORMAT, remaining_data_size,
                demux->restoredata_offset);
          }

          gst_adapter_push (demux->adapter, demux->mdatbuffer);
          demux->mdatbuffer = NULL;
          demux->offset = demux->mdatoffset;
          demux->neededbytes = next_entry_size (demux);
          demux->state = QTDEMUX_STATE_MOVIE;
          demux->mdatleft = gst_adapter_available (demux->adapter);
          demux->mdatsize = demux->mdatleft;
        } else {
          GST_DEBUG_OBJECT (demux, "Carrying on normally");
          gst_adapter_flush (demux->adapter, demux->neededbytes);

          /* only go back to the mdat if there are samples to play */
          if (demux->got_moov && demux->first_mdat != -1
              && has_next_entry (demux)) {
            gboolean res;

            /* we need to seek back */
            res = qtdemux_seek_offset (demux, demux->first_mdat);
            if (res) {
              demux->offset = demux->first_mdat;
            } else {
              GST_DEBUG_OBJECT (demux, "Seek back failed");
            }
          } else {
            demux->offset += demux->neededbytes;
          }
          demux->neededbytes = 16;
          demux->state = QTDEMUX_STATE_INITIAL;
        }

        break;
      }
      case QTDEMUX_STATE_BUFFER_MDAT:{
        GstBuffer *buf;
        guint8 fourcc[4];

        GST_DEBUG_OBJECT (demux, "Got our buffer at offset %" G_GUINT64_FORMAT,
            demux->offset);
        buf = gst_adapter_take_buffer (demux->adapter, demux->neededbytes);
        gst_buffer_extract (buf, 0, fourcc, 4);
        GST_DEBUG_OBJECT (demux, "mdatbuffer starts with %" GST_FOURCC_FORMAT,
            GST_FOURCC_ARGS (QT_FOURCC (fourcc)));
        if (demux->mdatbuffer)
          demux->mdatbuffer = gst_buffer_append (demux->mdatbuffer, buf);
        else
          demux->mdatbuffer = buf;
        demux->offset += demux->neededbytes;
        demux->neededbytes = 16;
        demux->state = QTDEMUX_STATE_INITIAL;
        gst_qtdemux_post_progress (demux, 1, 1);

        break;
      }
      case QTDEMUX_STATE_MOVIE:{
        QtDemuxStream *stream = NULL;
        QtDemuxSample *sample;
        GstClockTime dts, pts, stream_pts, duration;
        gboolean keyframe;
        gint i;

        GST_DEBUG_OBJECT (demux,
            "BEGIN // in MOVIE for offset %" G_GUINT64_FORMAT, demux->offset);

        if (demux->fragmented) {
          GST_DEBUG_OBJECT (demux, "mdat remaining %" G_GUINT64_FORMAT,
              demux->mdatleft);
          if (G_LIKELY (demux->todrop < demux->mdatleft)) {
            /* if needed data starts within this atom,
             * then it should not exceed this atom */
            if (G_UNLIKELY (demux->neededbytes > demux->mdatleft)) {
              GST_ELEMENT_ERROR (demux, STREAM, DEMUX,
                  (_("This file is invalid and cannot be played.")),
                  ("sample data crosses atom boundary"));
              ret = GST_FLOW_ERROR;
              break;
            }
            demux->mdatleft -= demux->neededbytes;
          } else {
            GST_DEBUG_OBJECT (demux, "data atom emptied; resuming atom scan");
            /* so we are dropping more than left in this atom */
            gst_qtdemux_drop_data (demux, demux->mdatleft);
            demux->mdatleft = 0;

            /* need to resume atom parsing so we do not miss any other pieces */
            demux->state = QTDEMUX_STATE_INITIAL;
            demux->neededbytes = 16;

            /* check if there was any stored post mdat data from previous buffers */
            if (demux->restoredata_buffer) {
              g_assert (gst_adapter_available (demux->adapter) == 0);

              gst_adapter_push (demux->adapter, demux->restoredata_buffer);
              demux->restoredata_buffer = NULL;
              demux->offset = demux->restoredata_offset;
            }

            break;
          }
        }

        if (demux->todrop) {
          if (demux->cenc_aux_info_offset > 0) {
            GstByteReader br;
            const guint8 *data;

            GST_DEBUG_OBJECT (demux, "parsing cenc auxiliary info");
            data = gst_adapter_map (demux->adapter, demux->todrop);
            gst_byte_reader_init (&br, data + 8, demux->todrop);
            if (!qtdemux_parse_cenc_aux_info (demux,
                    QTDEMUX_NTH_STREAM (demux, 0), &br,
                    demux->cenc_aux_info_sizes, demux->cenc_aux_sample_count)) {
              GST_ERROR_OBJECT (demux, "failed to parse cenc auxiliary info");
              ret = GST_FLOW_ERROR;
              gst_adapter_unmap (demux->adapter);
              g_free (demux->cenc_aux_info_sizes);
              demux->cenc_aux_info_sizes = NULL;
              goto done;
            }
            demux->cenc_aux_info_offset = 0;
            g_free (demux->cenc_aux_info_sizes);
            demux->cenc_aux_info_sizes = NULL;
            gst_adapter_unmap (demux->adapter);
          }
          gst_qtdemux_drop_data (demux, demux->todrop);
        }

        /* first buffer? */
        /* initial newsegment sent here after having added pads,
         * possible others in sink_event */
        gst_qtdemux_check_send_pending_segment (demux);

        /* Figure out which stream this packet belongs to */
        for (i = 0; i < QTDEMUX_N_STREAMS (demux); i++) {
          stream = QTDEMUX_NTH_STREAM (demux, i);
          if (stream->sample_index >= stream->n_samples) {
            /* reset to be checked below G_UNLIKELY (stream == NULL) */
            stream = NULL;
            continue;
          }
          GST_LOG_OBJECT (demux,
              "Checking track-id %u (sample_index:%d / offset:%"
              G_GUINT64_FORMAT " / size:%d)", stream->track_id,
              stream->sample_index,
              stream->samples[stream->sample_index].offset,
              stream->samples[stream->sample_index].size);

          if (stream->samples[stream->sample_index].offset == demux->offset)
            break;
        }

        if (G_UNLIKELY (stream == NULL))
          goto unknown_stream;

        gst_qtdemux_stream_check_and_change_stsd_index (demux, stream);

        if (stream->new_caps) {
          gst_qtdemux_configure_stream (demux, stream);
        }

        /* Put data in a buffer, set timestamps, caps, ... */
        sample = &stream->samples[stream->sample_index];

        if (G_LIKELY (!(STREAM_IS_EOS (stream)))) {
          GST_DEBUG_OBJECT (demux, "stream : %" GST_FOURCC_FORMAT,
              GST_FOURCC_ARGS (CUR_STREAM (stream)->fourcc));

          dts = QTSAMPLE_DTS (stream, sample);
          pts = QTSAMPLE_PTS (stream, sample);
          stream_pts =
              gst_segment_to_stream_time_clamped (&stream->segment, pts);
          duration = QTSAMPLE_DUR_DTS (stream, sample, dts);
          keyframe = QTSAMPLE_KEYFRAME (stream, sample);

          /* check for segment end */
          if (G_UNLIKELY (stream->segment.stop != -1
                  && stream->segment.stop <= stream_pts && keyframe)
              && !(demux->upstream_format_is_time && stream->segment.rate < 0)) {
            GST_DEBUG_OBJECT (demux, "we reached the end of our segment.");
            stream->time_position = GST_CLOCK_TIME_NONE;        /* this means EOS */

            /* skip this data, stream is EOS */
            gst_adapter_flush (demux->adapter, demux->neededbytes);

            ret = GST_FLOW_EOS;
          } else if ((demux->segment.flags &
                  GST_SEGMENT_FLAG_TRICKMODE_KEY_UNITS) != 0 &&
              stream->subtype == FOURCC_vide && !keyframe) {
            GST_LOG_OBJECT (demux, "Skipping non-keyframe on track-id %u",
                stream->track_id);
            gst_adapter_flush (demux->adapter, demux->neededbytes);
            ret = GST_FLOW_OK;
          } else {
            GstBuffer *outbuf;

            outbuf =
                gst_adapter_take_buffer (demux->adapter, demux->neededbytes);

            /* FIXME: should either be an assert or a plain check */
            g_return_val_if_fail (outbuf != NULL, GST_FLOW_ERROR);

            ret = gst_qtdemux_decorate_and_push_buffer (demux, stream, outbuf,
                dts, pts, duration, keyframe, dts, demux->offset);
          }

          /* combine flows */
          GST_OBJECT_LOCK (demux);
          ret = gst_qtdemux_combine_flows (demux, stream, ret);
          GST_OBJECT_UNLOCK (demux);
        } else {
          /* skip this data, stream is EOS */
          gst_adapter_flush (demux->adapter, demux->neededbytes);
        }

        stream->sample_index++;
        stream->offset_in_sample = 0;

        /* update current offset and figure out size of next buffer */
        GST_LOG_OBJECT (demux, "increasing offset %" G_GUINT64_FORMAT " by %u",
            demux->offset, demux->neededbytes);
        demux->offset += demux->neededbytes;
        GST_LOG_OBJECT (demux, "offset is now %" G_GUINT64_FORMAT,
            demux->offset);


        if (ret == GST_FLOW_EOS) {
          GST_DEBUG_OBJECT (demux, "All streams are EOS, signal upstream");
          demux->neededbytes = -1;
          goto eos;
        }

        if ((demux->neededbytes = next_entry_size (demux)) == -1) {
          if (demux->fragmented) {
            GST_DEBUG_OBJECT (demux, "(temporarily) out of fragmented samples");
            /* there may be more to follow, only finish this atom */
            demux->todrop = demux->mdatleft;
            demux->neededbytes = demux->todrop;
            break;
          }
          goto eos;
        }
        if (ret != GST_FLOW_OK && ret != GST_FLOW_NOT_LINKED) {
          goto non_ok_unlinked_flow;
        }
        break;
      }
      default:
        goto invalid_state;
    }
  }

  /* when buffering movie data, at least show user something is happening */
  if (ret == GST_FLOW_OK && demux->state == QTDEMUX_STATE_BUFFER_MDAT &&
      gst_adapter_available (demux->adapter) <= demux->neededbytes) {
    gst_qtdemux_post_progress (demux, gst_adapter_available (demux->adapter),
        demux->neededbytes);
  }
done:

  return ret;

  /* ERRORS */
non_ok_unlinked_flow:
  {
    GST_DEBUG_OBJECT (demux, "Stopping, combined return flow %s",
        gst_flow_get_name (ret));
    return ret;
  }
unknown_stream:
  {
    GST_ELEMENT_ERROR (demux, STREAM, FAILED, (NULL), ("unknown stream found"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
eos:
  {
    GST_DEBUG_OBJECT (demux, "no next entry, EOS");
    ret = GST_FLOW_EOS;
    goto done;
  }
invalid_state:
  {
    GST_ELEMENT_ERROR (demux, STREAM, FAILED,
        (NULL), ("qtdemuxer invalid state %d", demux->state));
    ret = GST_FLOW_ERROR;
    goto done;
  }
no_moov:
  {
    GST_ELEMENT_ERROR (demux, STREAM, FAILED,
        (NULL), ("no 'moov' atom within the first 10 MB"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static gboolean
qtdemux_sink_activate (GstPad * sinkpad, GstObject * parent)
{
  GstQuery *query;
  gboolean pull_mode;

  query = gst_query_new_scheduling ();

  if (!gst_pad_peer_query (sinkpad, query)) {
    gst_query_unref (query);
    goto activate_push;
  }

  pull_mode = gst_query_has_scheduling_mode_with_flags (query,
      GST_PAD_MODE_PULL, GST_SCHEDULING_FLAG_SEEKABLE);
  gst_query_unref (query);

  if (!pull_mode)
    goto activate_push;

  GST_DEBUG_OBJECT (sinkpad, "activating pull");
  return gst_pad_activate_mode (sinkpad, GST_PAD_MODE_PULL, TRUE);

activate_push:
  {
    GST_DEBUG_OBJECT (sinkpad, "activating push");
    return gst_pad_activate_mode (sinkpad, GST_PAD_MODE_PUSH, TRUE);
  }
}

static gboolean
qtdemux_sink_activate_mode (GstPad * sinkpad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean res;
  GstQTDemux *demux = GST_QTDEMUX (parent);

  switch (mode) {
    case GST_PAD_MODE_PUSH:
      demux->pullbased = FALSE;
      res = TRUE;
      break;
    case GST_PAD_MODE_PULL:
      if (active) {
        demux->pullbased = TRUE;
        res = gst_pad_start_task (sinkpad, (GstTaskFunction) gst_qtdemux_loop,
            sinkpad, NULL);
      } else {
        res = gst_pad_stop_task (sinkpad);
      }
      break;
    default:
      res = FALSE;
      break;
  }
  return res;
}

#ifdef HAVE_ZLIB
static void *
qtdemux_inflate (void *z_buffer, guint z_length, guint * length)
{
  guint8 *buffer;
  z_stream z;
  int ret;

  memset (&z, 0, sizeof (z));
  z.zalloc = NULL;
  z.zfree = NULL;
  z.opaque = NULL;

  if ((ret = inflateInit (&z)) != Z_OK) {
    GST_ERROR ("inflateInit() returned %d", ret);
    return NULL;
  }

  z.next_in = z_buffer;
  z.avail_in = z_length;

  buffer = (guint8 *) g_malloc (*length);
  z.avail_out = *length;
  z.next_out = (Bytef *) buffer;
  do {
    ret = inflate (&z, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) {
      break;
    } else if (ret != Z_OK) {
      GST_WARNING ("inflate() returned %d", ret);
      break;
    }

    if (*length > G_MAXUINT - 4096 || *length > QTDEMUX_MAX_SAMPLE_INDEX_SIZE) {
      GST_WARNING ("too big decompressed data");
      ret = Z_MEM_ERROR;
      break;
    }

    *length += 4096;
    buffer = (guint8 *) g_realloc (buffer, *length);
    z.next_out = (Bytef *) (buffer + z.total_out);
    z.avail_out += *length - z.total_out;
  } while (z.avail_in > 0);

  if (ret != Z_STREAM_END) {
    g_free (buffer);
    buffer = NULL;
    *length = 0;
  } else {
    *length = z.total_out;
  }

  inflateEnd (&z);

  return buffer;
}
#endif /* HAVE_ZLIB */

static gboolean
qtdemux_parse_moov (GstQTDemux * qtdemux, const guint8 * buffer, guint length)
{
  GNode *cmov;

  qtdemux->moov_node = g_node_new ((guint8 *) buffer);

  /* counts as header data */
  qtdemux->header_size += length;

  GST_DEBUG_OBJECT (qtdemux, "parsing 'moov' atom");
  qtdemux_parse_node (qtdemux, qtdemux->moov_node, buffer, length);

  cmov = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_cmov);
  if (cmov) {
    guint32 method;
    GNode *dcom;
    GNode *cmvd;
    guint32 dcom_len;

    dcom = qtdemux_tree_get_child_by_type (cmov, FOURCC_dcom);
    cmvd = qtdemux_tree_get_child_by_type (cmov, FOURCC_cmvd);
    if (dcom == NULL || cmvd == NULL)
      goto invalid_compression;

    dcom_len = QT_UINT32 (dcom->data);
    if (dcom_len < 12)
      goto invalid_compression;

    method = QT_FOURCC ((guint8 *) dcom->data + 8);
    switch (method) {
#ifdef HAVE_ZLIB
      case FOURCC_zlib:{
        guint uncompressed_length;
        guint compressed_length;
        guint8 *buf;
        guint32 cmvd_len;

        cmvd_len = QT_UINT32 ((guint8 *) cmvd->data);
        if (cmvd_len < 12)
          goto invalid_compression;

        uncompressed_length = QT_UINT32 ((guint8 *) cmvd->data + 8);
        compressed_length = cmvd_len - 12;
        GST_LOG ("length = %u", uncompressed_length);

        buf =
            (guint8 *) qtdemux_inflate ((guint8 *) cmvd->data + 12,
            compressed_length, &uncompressed_length);

        if (buf) {
          qtdemux->moov_node_compressed = qtdemux->moov_node;
          qtdemux->moov_node = g_node_new (buf);

          qtdemux_parse_node (qtdemux, qtdemux->moov_node, buf,
              uncompressed_length);
        }
        break;
      }
#endif /* HAVE_ZLIB */
      default:
        GST_WARNING_OBJECT (qtdemux, "unknown or unhandled header compression "
            "type %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (method));
        break;
    }
  }
  return TRUE;

  /* ERRORS */
invalid_compression:
  {
    GST_ERROR_OBJECT (qtdemux, "invalid compressed header");
    return FALSE;
  }
}

static gboolean
qtdemux_parse_container (GstQTDemux * qtdemux, GNode * node, const guint8 * buf,
    const guint8 * end)
{
  while (G_UNLIKELY (buf < end)) {
    GNode *child;
    guint32 len;

    if (G_UNLIKELY (buf + 4 > end)) {
      GST_LOG_OBJECT (qtdemux, "buffer overrun");
      break;
    }
    len = QT_UINT32 (buf);
    if (G_UNLIKELY (len == 0)) {
      GST_LOG_OBJECT (qtdemux, "empty container");
      break;
    }
    if (G_UNLIKELY (len < 8)) {
      GST_WARNING_OBJECT (qtdemux, "length too short (%d < 8)", len);
      break;
    }
    if (G_UNLIKELY (len > (end - buf))) {
      GST_WARNING_OBJECT (qtdemux, "length too long (%d > %d)", len,
          (gint) (end - buf));
      break;
    }

    child = g_node_new ((guint8 *) buf);
    g_node_append (node, child);
    GST_LOG_OBJECT (qtdemux, "adding new node of len %d", len);
    qtdemux_parse_node (qtdemux, child, buf, len);

    buf += len;
  }
  return TRUE;
}

static gboolean
qtdemux_parse_node (GstQTDemux * qtdemux, GNode * node, const guint8 * buffer,
    guint length)
{
  guint32 fourcc = 0;
  guint32 node_length = 0;
  const QtNodeType *type;
  const guint8 *end;

  GST_LOG_OBJECT (qtdemux, "qtdemux_parse buffer %p length %u", buffer, length);

  if (G_UNLIKELY (length < 8))
    goto not_enough_data;

  node_length = QT_UINT32 (buffer);
  fourcc = QT_FOURCC (buffer + 4);

  /* ignore empty nodes */
  if (G_UNLIKELY (fourcc == 0 || node_length == 8))
    return TRUE;

  type = qtdemux_type_get (fourcc);

  end = buffer + length;

  GST_LOG_OBJECT (qtdemux,
      "parsing '%" GST_FOURCC_FORMAT "', length=%u, name '%s'",
      GST_FOURCC_ARGS (fourcc), node_length, type->name);

  if (node_length > length)
    goto broken_atom_size;

  if (type->flags & QT_FLAG_CONTAINER) {
    qtdemux_parse_container (qtdemux, node, buffer + 8, end);
  } else {
    switch (fourcc) {
      case FOURCC_stsd:
      {
        if (node_length < 20) {
          GST_LOG_OBJECT (qtdemux, "skipping small stsd box");
          break;
        }
        GST_DEBUG_OBJECT (qtdemux,
            "parsing stsd (sample table, sample description) atom");
        /* Skip over 8 byte atom hdr + 1 byte version, 3 bytes flags, 4 byte num_entries */
        qtdemux_parse_container (qtdemux, node, buffer + 16, end);
        break;
      }
      case FOURCC_mp4a:
      case FOURCC_alac:
      case FOURCC_fLaC:
      case FOURCC_aavd:
      case FOURCC_opus:
      case FOURCC_lpcm:
      case FOURCC_wma_:
      case FOURCC_owma:
      case FOURCC_sowt:
      case FOURCC_twos:
      case FOURCC_in24:
      case FOURCC_in32:
      case FOURCC_fl32:
      case FOURCC_fl64:
      case FOURCC_s16l:
      case FOURCC_ipcm:
      case FOURCC_fpcm:
      case FOURCC_samr:
      case FOURCC_sawb:
      case FOURCC_QDM2:
      case FOURCC_QDMC:
      case FOURCC_MAC6:
      case FOURCC_MAC3:
      case FOURCC_ima4:
      case FOURCC_ulaw:
      case FOURCC_alaw:
      case FOURCC_agsm:
      case 0x20736d:
      case GST_MAKE_FOURCC ('e', 'c', '-', '3'):
      case GST_MAKE_FOURCC ('s', 'a', 'c', '3'):       // Nero Recode
      case FOURCC_ac_3:
      case 0x0200736d:
      case 0x6d730002:
      case 0x1100736d:
      case 0x6d730011:
      case 0x1700736d:
      case 0x6d730017:
      case 0x5500736d:
      case 0x6d730055:
      case FOURCC__mp3:
      case FOURCC_mp3_:
      case GST_MAKE_FOURCC ('.', 'm', 'p', '2'):
      case GST_MAKE_FOURCC ('d', 't', 's', 'c'):
      case GST_MAKE_FOURCC ('D', 'T', 'S', ' '):
      case GST_MAKE_FOURCC ('O', 'g', 'g', 'V'):
      case GST_MAKE_FOURCC ('d', 'v', 'c', 'a'):
      case GST_MAKE_FOURCC ('Q', 'c', 'l', 'p'):
      case GST_MAKE_FOURCC ('a', 'c', '-', '4'):
      case FOURCC_enca:
        /* FIXME FOURCC_raw_ but that is used for video too */
      {
        guint8 stsd_version;
        guint32 version;
        guint32 offset;
        guint min_size;

        /* also read alac (or whatever) in stead of mp4a in the following,
         * since a similar layout is used in other cases as well */
        if (fourcc == FOURCC_mp4a)
          min_size = 20;
        else if (fourcc == FOURCC_fLaC)
          min_size = 86;
        else if (fourcc == FOURCC_opus)
          min_size = 55;
        else
          min_size = 40;

        /* There are two things we might encounter here: a true mp4a atom, and
           an mp4a entry in an stsd atom. The latter is what we're interested
           in, and it looks like an atom, but isn't really one. The true mp4a
           atom is short, so we detect it based on length here. */
        if (length < min_size) {
          GST_LOG_OBJECT (qtdemux, "skipping small %" GST_FOURCC_FORMAT " box",
              GST_FOURCC_ARGS (fourcc));
          break;
        }

        /* 'version' here is the sound sample description version. Types 0 and
           1 are documented in the QTFF reference, but type 2 is not: it's
           described in Apple header files instead (struct SoundDescriptionV2
           in Movies.h).

           For ISOBMFF there's only version 0 and 1, and both have the same size.
           The distinction between the two version 1 can be made via the stsd (parent)
           node version.
         */
        stsd_version = QT_UINT8 ((const guint8 *) node->data + 8);
        version = QT_UINT16 (buffer + 16);

        GST_DEBUG_OBJECT (qtdemux,
            "%" GST_FOURCC_FORMAT " stsd version %u version %u",
            GST_FOURCC_ARGS (fourcc), stsd_version, version);

        if (stsd_version == 0) {
          /* parse any esds descriptors and other optional boxes */
          switch (version) {
            case 0:
              offset = 36;
              break;
            case 1:
              offset = 52;
              break;
            case 2:
              offset = 72;
              break;
            default:
              GST_WARNING_OBJECT (qtdemux,
                  "unhandled %" GST_FOURCC_FORMAT " version %u",
                  GST_FOURCC_ARGS (fourcc), version);
              offset = 0;
              break;
          }
        } else if (stsd_version == 1) {
          switch (version) {
            case 0:
            case 1:
              offset = 36;
              break;
            default:
              GST_WARNING_OBJECT (qtdemux,
                  "unhandled %" GST_FOURCC_FORMAT " version %u",
                  GST_FOURCC_ARGS (fourcc), version);
              offset = 0;
              break;
          }
        } else {
          GST_WARNING_OBJECT (qtdemux,
              "unhandled stsd version %u", stsd_version);
          offset = 0;
        }

        if (offset)
          qtdemux_parse_container (qtdemux, node, buffer + offset, end);
        break;
      }
      case FOURCC_mp4v:
      case FOURCC_MP4V:
      case FOURCC_fmp4:
      case FOURCC_FMP4:
      case FOURCC_apcs:
      case FOURCC_apch:
      case FOURCC_apcn:
      case FOURCC_apco:
      case FOURCC_ap4h:
      case FOURCC_ap4x:
      case FOURCC_xvid:
      case FOURCC_XVID:
      case FOURCC_H264:
      case FOURCC_avc1:
      case FOURCC_dva1:
      case FOURCC_avc3:
      case FOURCC_dvav:
      case FOURCC_ai12:
      case FOURCC_ai13:
      case FOURCC_ai15:
      case FOURCC_ai16:
      case FOURCC_ai1p:
      case FOURCC_ai1q:
      case FOURCC_ai52:
      case FOURCC_ai53:
      case FOURCC_ai55:
      case FOURCC_ai56:
      case FOURCC_ai5p:
      case FOURCC_ai5q:
      case FOURCC_H265:
      case FOURCC_hvc1:
      case FOURCC_hev1:
      case FOURCC_dvh1:
      case FOURCC_dvhe:
      case FOURCC_mjp2:
      case FOURCC_encv:
      case FOURCC_H266:
      case FOURCC_vvc1:
      case FOURCC_vvi1:
      case FOURCC_av01:
      case FOURCC_uncv:
      case FOURCC_SVQ3:
      case FOURCC_VP31:
      case FOURCC_jpeg:
      case FOURCC_rle_:
      case FOURCC_WRLE:
      case FOURCC_ovc1:
      case FOURCC_vc_1:
      case FOURCC_VP80:
      case FOURCC_vp08:
      case FOURCC_vp09:
      case FOURCC_png:
      case GST_MAKE_FOURCC ('m', 'j', 'p', 'a'):
      case GST_MAKE_FOURCC ('A', 'V', 'D', 'J'):
      case GST_MAKE_FOURCC ('M', 'J', 'P', 'G'):
      case GST_MAKE_FOURCC ('d', 'm', 'b', '1'):
      case GST_MAKE_FOURCC ('m', 'j', 'p', 'b'):
      case GST_MAKE_FOURCC ('s', 'v', 'q', 'i'):
      case GST_MAKE_FOURCC ('S', 'V', 'Q', '1'):
      case GST_MAKE_FOURCC ('W', 'R', 'A', 'W'):
      case GST_MAKE_FOURCC ('y', 'v', '1', '2'):
      case GST_MAKE_FOURCC ('y', 'u', 'v', '2'):
      case GST_MAKE_FOURCC ('Y', 'u', 'v', '2'):
      case GST_MAKE_FOURCC ('2', 'V', 'u', 'y'):
      case GST_MAKE_FOURCC ('v', '3', '0', '8'):
      case GST_MAKE_FOURCC ('v', '2', '1', '6'):
      case FOURCC_v210:
      case GST_MAKE_FOURCC ('r', '2', '1', '0'):
      case GST_MAKE_FOURCC ('m', 'p', 'e', 'g'):
      case GST_MAKE_FOURCC ('m', 'p', 'g', '1'):
      case GST_MAKE_FOURCC ('m', '1', 'v', ' '):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '1'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '2'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '3'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '4'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '5'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '6'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '7'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '8'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', '9'):
      case GST_MAKE_FOURCC ('h', 'd', 'v', 'a'):
      case GST_MAKE_FOURCC ('m', 'x', '5', 'n'):
      case GST_MAKE_FOURCC ('m', 'x', '5', 'p'):
      case GST_MAKE_FOURCC ('m', 'x', '4', 'n'):
      case GST_MAKE_FOURCC ('m', 'x', '4', 'p'):
      case GST_MAKE_FOURCC ('m', 'x', '3', 'n'):
      case GST_MAKE_FOURCC ('m', 'x', '3', 'p'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '1'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '2'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '3'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '4'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '5'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '6'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '7'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '8'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', '9'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', 'a'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', 'b'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', 'c'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', 'd'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', 'e'):
      case GST_MAKE_FOURCC ('x', 'd', 'v', 'f'):
      case GST_MAKE_FOURCC ('x', 'd', '5', '1'):
      case GST_MAKE_FOURCC ('x', 'd', '5', '4'):
      case GST_MAKE_FOURCC ('x', 'd', '5', '5'):
      case GST_MAKE_FOURCC ('x', 'd', '5', '9'):
      case GST_MAKE_FOURCC ('x', 'd', '5', 'a'):
      case GST_MAKE_FOURCC ('x', 'd', '5', 'b'):
      case GST_MAKE_FOURCC ('x', 'd', '5', 'c'):
      case GST_MAKE_FOURCC ('x', 'd', '5', 'd'):
      case GST_MAKE_FOURCC ('x', 'd', '5', 'e'):
      case GST_MAKE_FOURCC ('x', 'd', '5', 'f'):
      case GST_MAKE_FOURCC ('x', 'd', 'h', 'd'):
      case GST_MAKE_FOURCC ('x', 'd', 'h', '2'):
      case GST_MAKE_FOURCC ('A', 'V', 'm', 'p'):
      case GST_MAKE_FOURCC ('m', 'p', 'g', '2'):
      case GST_MAKE_FOURCC ('m', 'p', '2', 'v'):
      case GST_MAKE_FOURCC ('m', '2', 'v', '1'):
      case GST_MAKE_FOURCC ('g', 'i', 'f', ' '):
      case FOURCC_h263:
      case GST_MAKE_FOURCC ('H', '2', '6', '3'):
      case FOURCC_s263:
      case GST_MAKE_FOURCC ('U', '2', '6', '3'):
      case GST_MAKE_FOURCC ('3', 'i', 'v', 'd'):
      case GST_MAKE_FOURCC ('3', 'I', 'V', 'D'):
      case GST_MAKE_FOURCC ('D', 'I', 'V', '3'):
      case GST_MAKE_FOURCC ('D', 'I', 'V', 'X'):
      case GST_MAKE_FOURCC ('d', 'i', 'v', 'x'):
      case GST_MAKE_FOURCC ('D', 'X', '5', '0'):
      case GST_MAKE_FOURCC ('F', 'F', 'V', '1'):
      case GST_MAKE_FOURCC ('3', 'I', 'V', '1'):
      case GST_MAKE_FOURCC ('3', 'I', 'V', '2'):
      case GST_MAKE_FOURCC ('U', 'M', 'P', '4'):
      case GST_MAKE_FOURCC ('c', 'v', 'i', 'd'):
      case GST_MAKE_FOURCC ('q', 'd', 'r', 'w'):
      case GST_MAKE_FOURCC ('r', 'p', 'z', 'a'):
      case GST_MAKE_FOURCC ('I', 'V', '3', '2'):
      case GST_MAKE_FOURCC ('i', 'v', '3', '2'):
      case GST_MAKE_FOURCC ('I', 'V', '4', '1'):
      case GST_MAKE_FOURCC ('i', 'v', '4', '1'):
      case FOURCC_dvcp:
      case FOURCC_dvc_:
      case GST_MAKE_FOURCC ('d', 'v', 's', 'd'):
      case GST_MAKE_FOURCC ('D', 'V', 'S', 'D'):
      case GST_MAKE_FOURCC ('d', 'v', 'c', 's'):
      case GST_MAKE_FOURCC ('D', 'V', 'C', 'S'):
      case GST_MAKE_FOURCC ('d', 'v', '2', '5'):
      case GST_MAKE_FOURCC ('d', 'v', 'p', 'p'):
      case FOURCC_dv5n:
      case FOURCC_dv5p:
      case GST_MAKE_FOURCC ('d', 'v', 'h', '5'):
      case GST_MAKE_FOURCC ('d', 'v', 'h', '6'):
      case GST_MAKE_FOURCC ('s', 'm', 'c', ' '):
      case GST_MAKE_FOURCC ('V', 'P', '6', 'F'):
      case FOURCC_drac:
      case GST_MAKE_FOURCC ('t', 'i', 'f', 'f'):
      case GST_MAKE_FOURCC ('i', 'c', 'o', 'd'):
      case GST_MAKE_FOURCC ('A', 'V', 'd', 'n'):
      case GST_MAKE_FOURCC ('A', 'V', 'd', 'h'):
      case FOURCC_cfhd:
      case FOURCC_SHQ0:
      case FOURCC_SHQ1:
      case FOURCC_SHQ2:
      case FOURCC_SHQ3:
      case FOURCC_SHQ4:
      case FOURCC_SHQ5:
      case FOURCC_SHQ6:
      case FOURCC_SHQ7:
      case FOURCC_SHQ8:
      case FOURCC_SHQ9:
      case FOURCC_LAGS:
      case FOURCC_Hap1:
      case FOURCC_Hap5:
      case FOURCC_HapY:
      case FOURCC_HapM:
      case FOURCC_HapA:
      case FOURCC_Hap7:
      case FOURCC_HapH:
        /* FIXME FOURCC_raw_ but that is used for audio too */
      {
        guint32 version;
        guint32 str_len;

        /* codec_data is contained inside these atoms, which all have
         * the same format. */
        /* video sample description size is 86 bytes without extension.
         * node_length have to be bigger than 86 bytes because video sample
         * description can include extensions such as esds, fiel, glbl, etc. */
        if (node_length < 86) {
          GST_WARNING_OBJECT (qtdemux, "%" GST_FOURCC_FORMAT
              " sample description length too short (%u < 86)",
              GST_FOURCC_ARGS (fourcc), node_length);
          break;
        }

        GST_DEBUG_OBJECT (qtdemux, "parsing in %" GST_FOURCC_FORMAT,
            GST_FOURCC_ARGS (fourcc));

        /* version (2 bytes) : this is set to 0, unless a compressor has changed
         *              its data format.
         * revision level (2 bytes) : must be set to 0. */
        version = QT_UINT32 (buffer + 16);
        GST_DEBUG_OBJECT (qtdemux, "version %08x", version);

        /* compressor name : PASCAL string and informative purposes
         * first byte : the number of bytes to be displayed.
         *              it has to be less than 32 because it is reserved
         *              space of 32 bytes total including itself. */
        str_len = QT_UINT8 (buffer + 50);
        if (str_len < 32)
          GST_DEBUG_OBJECT (qtdemux, "compressorname = %.*s", str_len,
              (char *) buffer + 51);
        else
          GST_WARNING_OBJECT (qtdemux,
              "compressorname length too big (%u > 31)", str_len);

        GST_MEMDUMP_OBJECT (qtdemux, "video sample description", buffer,
            end - buffer);
        qtdemux_parse_container (qtdemux, node, buffer + 86, end);
        break;
      }
      case FOURCC_meta:
      {
        GST_DEBUG_OBJECT (qtdemux, "parsing meta atom");

        /* You are reading this correctly. QTFF specifies that the
         * metadata atom is a short atom, whereas ISO BMFF specifies
         * it's a full atom. But since so many people are doing things
         * differently, we actually peek into the atom to see which
         * variant it is */
        if (length < 16) {
          GST_LOG_OBJECT (qtdemux, "skipping small %" GST_FOURCC_FORMAT " box",
              GST_FOURCC_ARGS (fourcc));
          break;
        }
        if (QT_FOURCC (buffer + 12) == FOURCC_hdlr) {
          /* Variant 1: What QTFF specifies. 'meta' is a short header which
           * starts with a 'hdlr' atom */
          qtdemux_parse_container (qtdemux, node, buffer + 8, end);
        } else if (QT_UINT32 (buffer + 8) == 0x00000000) {
          /* Variant 2: What ISO BMFF specifies. 'meta' is a _full_ atom
           * with version/flags both set to zero */
          qtdemux_parse_container (qtdemux, node, buffer + 12, end);
        } else
          GST_WARNING_OBJECT (qtdemux, "Unknown 'meta' atom format");
        break;
      }
      case FOURCC_mp4s:
      {
        GST_MEMDUMP_OBJECT (qtdemux, "mp4s", buffer, end - buffer);
        /* Skip 8 byte header, plus 8 byte version + flags + entry_count */
        qtdemux_parse_container (qtdemux, node, buffer + 16, end);
        break;
      }
      case FOURCC_XiTh:
      {
        guint32 version;
        guint32 offset;

        if (length < 16) {
          GST_LOG_OBJECT (qtdemux, "skipping small %" GST_FOURCC_FORMAT " box",
              GST_FOURCC_ARGS (fourcc));
          break;
        }

        version = QT_UINT32 (buffer + 12);
        GST_DEBUG_OBJECT (qtdemux, "parsing XiTh atom version 0x%08x", version);

        switch (version) {
          case 0x00000001:
            offset = 0x62;
            break;
          default:
            GST_DEBUG_OBJECT (qtdemux, "unknown version 0x%08x", version);
            offset = 0;
            break;
        }
        if (offset) {
          if (length < offset) {
            GST_WARNING_OBJECT (qtdemux,
                "skipping too small %" GST_FOURCC_FORMAT " box",
                GST_FOURCC_ARGS (fourcc));
            break;
          }
          qtdemux_parse_container (qtdemux, node, buffer + offset, end);
        }
        break;
      }
      case FOURCC_uuid:
      {
        qtdemux_parse_uuid (qtdemux, buffer, end - buffer);
        break;
      }
      default:
        if (!strcmp (type->name, "unknown"))
          GST_MEMDUMP ("Unknown tag", buffer + 4, end - buffer - 4);
        break;
    }
  }
  GST_LOG_OBJECT (qtdemux, "parsed '%" GST_FOURCC_FORMAT "'",
      GST_FOURCC_ARGS (fourcc));
  return TRUE;

/* ERRORS */
not_enough_data:
  {
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")),
        ("Not enough data for an atom header, got only %u bytes", length));
    return FALSE;
  }
broken_atom_size:
  {
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")),
        ("Atom '%" GST_FOURCC_FORMAT "' has size of %u bytes, but we have only "
            "%u bytes available.", GST_FOURCC_ARGS (fourcc), node_length,
            length));
    return FALSE;
  }
}

static void
qtdemux_do_allocation (QtDemuxStream * stream, GstQTDemux * qtdemux)
{
/* FIXME: This can only reliably work if demuxers have a
 * separate streaming thread per srcpad. This should be
 * done in a demuxer base class, which integrates parts
 * of multiqueue
 *
 * https://bugzilla.gnome.org/show_bug.cgi?id=701856
 */
#if 0
  GstQuery *query;

  query = gst_query_new_allocation (stream->caps, FALSE);

  if (!gst_pad_peer_query (stream->pad, query)) {
    /* not a problem, just debug a little */
    GST_DEBUG_OBJECT (qtdemux, "peer ALLOCATION query failed");
  }

  if (stream->allocator)
    gst_object_unref (stream->allocator);

  if (gst_query_get_n_allocation_params (query) > 0) {
    /* try the allocator */
    gst_query_parse_nth_allocation_param (query, 0, &stream->allocator,
        &stream->params);
    stream->use_allocator = TRUE;
  } else {
    stream->allocator = NULL;
    gst_allocation_params_init (&stream->params);
    stream->use_allocator = FALSE;
  }
  gst_query_unref (query);
#endif
}

static gboolean
pad_query (const GValue * item, GValue * value, gpointer user_data)
{
  GstPad *pad = g_value_get_object (item);
  GstQuery *query = user_data;
  gboolean res;

  res = gst_pad_peer_query (pad, query);

  if (res) {
    g_value_set_boolean (value, TRUE);
    return FALSE;
  }

  GST_INFO_OBJECT (pad, "pad peer query failed");
  return TRUE;
}

static gboolean
gst_qtdemux_run_query (GstElement * element, GstQuery * query,
    GstPadDirection direction)
{
  GstIterator *it;
  GstIteratorFoldFunction func = pad_query;
  GValue res = { 0, };

  g_value_init (&res, G_TYPE_BOOLEAN);
  g_value_set_boolean (&res, FALSE);

  /* Ask neighbor */
  if (direction == GST_PAD_SRC)
    it = gst_element_iterate_src_pads (element);
  else
    it = gst_element_iterate_sink_pads (element);

  while (gst_iterator_fold (it, func, &res, query) == GST_ITERATOR_RESYNC)
    gst_iterator_resync (it);

  gst_iterator_free (it);

  return g_value_get_boolean (&res);
}

static void
gst_qtdemux_request_protection_context (GstQTDemux * qtdemux,
    QtDemuxStream * stream)
{
  GstQuery *query;
  GstContext *ctxt;
  GstElement *element = GST_ELEMENT (qtdemux);
  GstStructure *st;
  gchar **filtered_sys_ids;
  GValue event_list = G_VALUE_INIT;
  GList *walk;

  /* 1. Check if we already have the context. */
  if (qtdemux->preferred_protection_system_id != NULL) {
    GST_LOG_OBJECT (element,
        "already have the protection context, no need to request it again");
    return;
  }

  g_ptr_array_add (qtdemux->protection_system_ids, NULL);
  filtered_sys_ids = gst_protection_filter_systems_by_available_decryptors (
      (const gchar **) qtdemux->protection_system_ids->pdata);

  g_ptr_array_remove_index (qtdemux->protection_system_ids,
      qtdemux->protection_system_ids->len - 1);
  GST_TRACE_OBJECT (qtdemux, "detected %u protection systems, we have "
      "decryptors for %u of them, running context request",
      qtdemux->protection_system_ids->len,
      filtered_sys_ids ? g_strv_length (filtered_sys_ids) : 0);


  if (stream->protection_scheme_event_queue.length) {
    GST_TRACE_OBJECT (qtdemux, "using stream event queue, length %u",
        stream->protection_scheme_event_queue.length);
    walk = stream->protection_scheme_event_queue.tail;
  } else {
    GST_TRACE_OBJECT (qtdemux, "using demuxer event queue, length %u",
        qtdemux->protection_event_queue.length);
    walk = qtdemux->protection_event_queue.tail;
  }

  g_value_init (&event_list, GST_TYPE_LIST);
  for (; walk; walk = g_list_previous (walk)) {
    GValue event_value = G_VALUE_INIT;
    g_value_init (&event_value, GST_TYPE_EVENT);
    g_value_set_boxed (&event_value, walk->data);
    gst_value_list_append_and_take_value (&event_list, &event_value);
  }

  /*  2a) Query downstream with GST_QUERY_CONTEXT for the context and
   *      check if downstream already has a context of the specific type
   *  2b) Query upstream as above.
   */
  query = gst_query_new_context ("drm-preferred-decryption-system-id");
  st = gst_query_writable_structure (query);
  gst_structure_set (st, "track-id", G_TYPE_UINT, stream->track_id,
      "available-stream-encryption-systems", G_TYPE_STRV, filtered_sys_ids,
      NULL);
  gst_structure_set_value (st, "stream-encryption-events", &event_list);
  if (gst_qtdemux_run_query (element, query, GST_PAD_SRC)) {
    gst_query_parse_context (query, &ctxt);
    GST_INFO_OBJECT (element, "found context (%p) in downstream query", ctxt);
    gst_element_set_context (element, ctxt);
  } else if (gst_qtdemux_run_query (element, query, GST_PAD_SINK)) {
    gst_query_parse_context (query, &ctxt);
    GST_INFO_OBJECT (element, "found context (%p) in upstream query", ctxt);
    gst_element_set_context (element, ctxt);
  } else {
    /* 3) Post a GST_MESSAGE_NEED_CONTEXT message on the bus with
     *    the required context type and afterwards check if a
     *    usable context was set now as in 1). The message could
     *    be handled by the parent bins of the element and the
     *    application.
     */
    GstMessage *msg;

    GST_INFO_OBJECT (element, "posting need context message");
    msg = gst_message_new_need_context (GST_OBJECT_CAST (element),
        "drm-preferred-decryption-system-id");
    st = (GstStructure *) gst_message_get_structure (msg);
    gst_structure_set (st, "track-id", G_TYPE_UINT, stream->track_id,
        "available-stream-encryption-systems", G_TYPE_STRV, filtered_sys_ids,
        NULL);

    gst_structure_set_value (st, "stream-encryption-events", &event_list);
    gst_element_post_message (element, msg);
  }

  g_strfreev (filtered_sys_ids);
  g_value_unset (&event_list);
  gst_query_unref (query);
}

static gboolean
gst_qtdemux_configure_protected_caps (GstQTDemux * qtdemux,
    QtDemuxStream * stream)
{
  GstStructure *s;
  const gchar *selected_system = NULL;

  g_return_val_if_fail (qtdemux != NULL, FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (gst_caps_get_size (CUR_STREAM (stream)->caps) == 1,
      FALSE);

  if (stream->protection_scheme_type == FOURCC_aavd) {
    s = gst_caps_get_structure (CUR_STREAM (stream)->caps, 0);
    if (!gst_structure_has_name (s, "application/x-aavd")) {
      gst_structure_set (s,
          "original-media-type", G_TYPE_STRING, gst_structure_get_name (s),
          NULL);
      gst_structure_set_name (s, "application/x-aavd");
    }
    return TRUE;
  }

  if (stream->protection_scheme_type != FOURCC_cenc
      && stream->protection_scheme_type != FOURCC_cbcs) {
    GST_ERROR_OBJECT (qtdemux,
        "unsupported protection scheme: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (stream->protection_scheme_type));
    return FALSE;
  }

  s = gst_caps_get_structure (CUR_STREAM (stream)->caps, 0);
  if (!gst_structure_has_name (s, "application/x-cenc")) {
    gst_structure_set (s,
        "original-media-type", G_TYPE_STRING, gst_structure_get_name (s), NULL);
    gst_structure_set (s, "cipher-mode", G_TYPE_STRING,
        (stream->protection_scheme_type == FOURCC_cbcs) ? "cbcs" : "cenc",
        NULL);
    gst_structure_set_name (s, "application/x-cenc");
  }

  if (qtdemux->protection_system_ids == NULL) {
    GST_DEBUG_OBJECT (qtdemux, "stream is protected using cenc, but no "
        "cenc protection system information has been found, not setting a "
        "protection system UUID");
    return TRUE;
  }

  gst_qtdemux_request_protection_context (qtdemux, stream);
  if (qtdemux->preferred_protection_system_id != NULL) {
    const gchar *preferred_system_array[] =
        { qtdemux->preferred_protection_system_id, NULL };

    selected_system = gst_protection_select_system (preferred_system_array);

    if (selected_system) {
      GST_TRACE_OBJECT (qtdemux, "selected preferred system %s",
          qtdemux->preferred_protection_system_id);
    } else {
      GST_WARNING_OBJECT (qtdemux, "could not select preferred system %s "
          "because there is no available decryptor",
          qtdemux->preferred_protection_system_id);
    }
  }

  if (!selected_system) {
    g_ptr_array_add (qtdemux->protection_system_ids, NULL);
    selected_system = gst_protection_select_system ((const gchar **)
        qtdemux->protection_system_ids->pdata);
    g_ptr_array_remove_index (qtdemux->protection_system_ids,
        qtdemux->protection_system_ids->len - 1);
  }

  if (!selected_system) {
    GST_ERROR_OBJECT (qtdemux, "stream is protected, but no "
        "suitable decryptor element has been found");
    return FALSE;
  }

  GST_DEBUG_OBJECT (qtdemux, "selected protection system is %s",
      selected_system);

  gst_structure_set (s,
      GST_PROTECTION_SYSTEM_ID_CAPS_FIELD, G_TYPE_STRING, selected_system,
      NULL);

  return TRUE;
}

static gboolean
gst_qtdemux_guess_framerate (GstQTDemux * qtdemux, QtDemuxStream * stream)
{
  /* fps is calculated base on the duration of the average framerate since
   * qt does not have a fixed framerate. */
  gboolean fps_available = TRUE;
  guint32 first_duration = 0;

  if (stream->n_samples > 0)
    first_duration = stream->samples[0].duration;

  if ((stream->n_samples == 1 && first_duration == 0)
      || (qtdemux->fragmented && stream->n_samples_moof == 1)) {
    /* still frame */
    CUR_STREAM (stream)->fps_n = 0;
    CUR_STREAM (stream)->fps_d = 1;
  } else {
    if (stream->duration == 0 || stream->n_samples < 2) {
      CUR_STREAM (stream)->fps_n = stream->timescale;
      CUR_STREAM (stream)->fps_d = 1;
      fps_available = FALSE;
    } else {
      GstClockTime avg_duration;
      guint64 duration;
      guint32 n_samples;

      /* duration and n_samples can be updated for fragmented format
       * so, framerate of fragmented format is calculated using data in a moof */
      if (qtdemux->fragmented && stream->n_samples_moof > 0
          && stream->duration_moof > 0) {
        n_samples = stream->n_samples_moof;
        duration = stream->duration_moof;
      } else {
        n_samples = stream->n_samples;
        duration = stream->duration;
      }

      /* Calculate a framerate, ignoring the first sample which is sometimes truncated */
      /* stream->duration is guint64, timescale, n_samples are guint32 */
      avg_duration =
          gst_util_uint64_scale_round (duration -
          first_duration, GST_SECOND,
          (guint64) (stream->timescale) * (n_samples - 1));

      GST_LOG_OBJECT (qtdemux,
          "Calculating avg sample duration based on stream (or moof) duration %"
          G_GUINT64_FORMAT
          " minus first sample %u, leaving %d samples gives %"
          GST_TIME_FORMAT, duration, first_duration,
          n_samples - 1, GST_TIME_ARGS (avg_duration));

      fps_available =
          gst_video_guess_framerate (avg_duration,
          &CUR_STREAM (stream)->fps_n, &CUR_STREAM (stream)->fps_d);

      GST_DEBUG_OBJECT (qtdemux,
          "Calculating framerate, timescale %u gave fps_n %d fps_d %d",
          stream->timescale, CUR_STREAM (stream)->fps_n,
          CUR_STREAM (stream)->fps_d);
    }
  }

  return fps_available;
}

static gboolean
gst_qtdemux_configure_stream (GstQTDemux * qtdemux, QtDemuxStream * stream)
{
  if (stream->subtype == FOURCC_vide) {
    gboolean fps_available = gst_qtdemux_guess_framerate (qtdemux, stream);

    if (CUR_STREAM (stream)->caps) {
      CUR_STREAM (stream)->caps =
          gst_caps_make_writable (CUR_STREAM (stream)->caps);

      if (CUR_STREAM (stream)->width && CUR_STREAM (stream)->height)
        gst_caps_set_simple (CUR_STREAM (stream)->caps,
            "width", G_TYPE_INT, CUR_STREAM (stream)->width,
            "height", G_TYPE_INT, CUR_STREAM (stream)->height, NULL);

      /* set framerate if calculated framerate is reliable */
      if (fps_available) {
        gst_caps_set_simple (CUR_STREAM (stream)->caps,
            "framerate", GST_TYPE_FRACTION, CUR_STREAM (stream)->fps_n,
            CUR_STREAM (stream)->fps_d, NULL);
      }

      /* calculate pixel-aspect-ratio using display width and height */
      GST_DEBUG_OBJECT (qtdemux,
          "video size %dx%d, target display size %dx%d",
          CUR_STREAM (stream)->width, CUR_STREAM (stream)->height,
          stream->display_width, stream->display_height);
      /* qt file might have pasp atom */
      if (CUR_STREAM (stream)->par_w > 0 && CUR_STREAM (stream)->par_h > 0) {
        GST_DEBUG_OBJECT (qtdemux, "par %d:%d", CUR_STREAM (stream)->par_w,
            CUR_STREAM (stream)->par_h);
        gst_caps_set_simple (CUR_STREAM (stream)->caps, "pixel-aspect-ratio",
            GST_TYPE_FRACTION, CUR_STREAM (stream)->par_w,
            CUR_STREAM (stream)->par_h, NULL);
      } else if (stream->display_width > 0 && stream->display_height > 0
          && CUR_STREAM (stream)->width > 0
          && CUR_STREAM (stream)->height > 0) {
        gint n, d;

        /* calculate the pixel aspect ratio using the display and pixel w/h */
        n = stream->display_width * CUR_STREAM (stream)->height;
        d = stream->display_height * CUR_STREAM (stream)->width;
        if (n == d)
          n = d = 1;
        GST_DEBUG_OBJECT (qtdemux, "setting PAR to %d/%d", n, d);
        CUR_STREAM (stream)->par_w = n;
        CUR_STREAM (stream)->par_h = d;
        gst_caps_set_simple (CUR_STREAM (stream)->caps, "pixel-aspect-ratio",
            GST_TYPE_FRACTION, CUR_STREAM (stream)->par_w,
            CUR_STREAM (stream)->par_h, NULL);
      }

      if (CUR_STREAM (stream)->interlace_mode > 0) {
        if (CUR_STREAM (stream)->interlace_mode == 1) {
          gst_caps_set_simple (CUR_STREAM (stream)->caps, "interlace-mode",
              G_TYPE_STRING, "progressive", NULL);
        } else if (CUR_STREAM (stream)->interlace_mode == 2) {
          gst_caps_set_simple (CUR_STREAM (stream)->caps, "interlace-mode",
              G_TYPE_STRING, "interleaved", NULL);
          if (CUR_STREAM (stream)->field_order == 9) {
            gst_caps_set_simple (CUR_STREAM (stream)->caps, "field-order",
                G_TYPE_STRING, "top-field-first", NULL);
          } else if (CUR_STREAM (stream)->field_order == 14) {
            gst_caps_set_simple (CUR_STREAM (stream)->caps, "field-order",
                G_TYPE_STRING, "bottom-field-first", NULL);
          }
        }
      }

      /* Create incomplete colorimetry here if needed */
      if (CUR_STREAM (stream)->colorimetry.range ||
          CUR_STREAM (stream)->colorimetry.matrix ||
          CUR_STREAM (stream)->colorimetry.transfer
          || CUR_STREAM (stream)->colorimetry.primaries) {
        gchar *colorimetry =
            gst_video_colorimetry_to_string (&CUR_STREAM (stream)->colorimetry);
        gst_caps_set_simple (CUR_STREAM (stream)->caps, "colorimetry",
            G_TYPE_STRING, colorimetry, NULL);
        g_free (colorimetry);
      }

      if (CUR_STREAM (stream)->content_light_level_set) {
        gst_video_content_light_level_add_to_caps (&CUR_STREAM
            (stream)->content_light_level, CUR_STREAM (stream)->caps);
      }

      if (CUR_STREAM (stream)->mastering_display_info_set) {
        gst_video_mastering_display_info_add_to_caps (&CUR_STREAM
            (stream)->mastering_display_info, CUR_STREAM (stream)->caps);
      }

      if (stream->multiview_mode != GST_VIDEO_MULTIVIEW_MODE_NONE) {
        guint par_w = 1, par_h = 1;

        if (CUR_STREAM (stream)->par_w > 0 && CUR_STREAM (stream)->par_h > 0) {
          par_w = CUR_STREAM (stream)->par_w;
          par_h = CUR_STREAM (stream)->par_h;
        }

        if (gst_video_multiview_guess_half_aspect (stream->multiview_mode,
                CUR_STREAM (stream)->width, CUR_STREAM (stream)->height, par_w,
                par_h)) {
          stream->multiview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT;
        }

        gst_caps_set_simple (CUR_STREAM (stream)->caps,
            "multiview-mode", G_TYPE_STRING,
            gst_video_multiview_mode_to_caps_string (stream->multiview_mode),
            "multiview-flags", GST_TYPE_VIDEO_MULTIVIEW_FLAGSET,
            stream->multiview_flags, GST_FLAG_SET_MASK_EXACT, NULL);
      }
    }
  }

  else if (stream->subtype == FOURCC_soun) {
    if (CUR_STREAM (stream)->caps) {
      CUR_STREAM (stream)->caps =
          gst_caps_make_writable (CUR_STREAM (stream)->caps);
      if (CUR_STREAM (stream)->rate > 0)
        gst_caps_set_simple (CUR_STREAM (stream)->caps,
            "rate", G_TYPE_INT, (int) CUR_STREAM (stream)->rate, NULL);
      if (CUR_STREAM (stream)->n_channels > 0)
        gst_caps_set_simple (CUR_STREAM (stream)->caps,
            "channels", G_TYPE_INT, CUR_STREAM (stream)->n_channels, NULL);
      /* FIXME: Need to parse the 'chan' atom to get channel layouts
       * correctly. */
    }
  }

  else if (stream->subtype == FOURCC_clcp && CUR_STREAM (stream)->caps) {
    const GstStructure *s;
    QtDemuxStream *fps_stream = NULL;
    gboolean fps_available = FALSE;

    /* CEA608 closed caption tracks are a bit special in that each sample
     * can contain CCs for multiple frames, and CCs can be omitted and have to
     * be inferred from the duration of the sample then.
     *
     * As such we take the framerate from the (first) video track here for
     * CEA608 as there must be one CC byte pair for every video frame
     * according to the spec.
     *
     * For CEA708 all is fine and there is one sample per frame.
     */

    s = gst_caps_get_structure (CUR_STREAM (stream)->caps, 0);
    if (gst_structure_has_name (s, "closedcaption/x-cea-608")) {
      gint i;

      for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
        QtDemuxStream *tmp = QTDEMUX_NTH_STREAM (qtdemux, i);

        if (tmp->subtype == FOURCC_vide) {
          fps_stream = tmp;
          break;
        }
      }

      if (fps_stream) {
        fps_available = gst_qtdemux_guess_framerate (qtdemux, fps_stream);
        CUR_STREAM (stream)->fps_n = CUR_STREAM (fps_stream)->fps_n;
        CUR_STREAM (stream)->fps_d = CUR_STREAM (fps_stream)->fps_d;
      }
    } else {
      fps_available = gst_qtdemux_guess_framerate (qtdemux, stream);
      fps_stream = stream;
    }

    CUR_STREAM (stream)->caps =
        gst_caps_make_writable (CUR_STREAM (stream)->caps);

    /* set framerate if calculated framerate is reliable */
    if (fps_available) {
      gst_caps_set_simple (CUR_STREAM (stream)->caps,
          "framerate", GST_TYPE_FRACTION, CUR_STREAM (stream)->fps_n,
          CUR_STREAM (stream)->fps_d, NULL);
    }
  }

  if (stream->pad) {
    gboolean forward_collection = FALSE;
    GstCaps *prev_caps = NULL;

    GST_PAD_ELEMENT_PRIVATE (stream->pad) = stream;
    gst_pad_set_event_function (stream->pad, gst_qtdemux_handle_src_event);
    gst_pad_set_query_function (stream->pad, gst_qtdemux_handle_src_query);
    gst_pad_set_active (stream->pad, TRUE);

    gst_pad_use_fixed_caps (stream->pad);

    if (stream->protected) {
      if (!gst_qtdemux_configure_protected_caps (qtdemux, stream)) {
        GST_ERROR_OBJECT (qtdemux,
            "Failed to configure protected stream caps.");
        return FALSE;
      }
    }

    if (stream->new_stream) {
      GstEvent *event;
      GstStreamFlags stream_flags = GST_STREAM_FLAG_NONE;

      event =
          gst_pad_get_sticky_event (qtdemux->sinkpad, GST_EVENT_STREAM_START,
          0);
      if (event) {
        gst_event_parse_stream_flags (event, &stream_flags);
        if (gst_event_parse_group_id (event, &qtdemux->group_id))
          qtdemux->have_group_id = TRUE;
        else
          qtdemux->have_group_id = FALSE;
        gst_event_unref (event);
      } else if (!qtdemux->have_group_id) {
        qtdemux->have_group_id = TRUE;
        qtdemux->group_id = gst_util_group_id_next ();
      }

      stream->new_stream = FALSE;
      event = gst_event_new_stream_start (stream->stream_id);
      if (qtdemux->have_group_id)
        gst_event_set_group_id (event, qtdemux->group_id);
      if (stream->disabled)
        stream_flags |= GST_STREAM_FLAG_UNSELECT;
      if (CUR_STREAM (stream)->sparse) {
        stream_flags |= GST_STREAM_FLAG_SPARSE;
      } else {
        stream_flags &= ~GST_STREAM_FLAG_SPARSE;
      }
      gst_event_set_stream_flags (event, stream_flags);
      gst_pad_push_event (stream->pad, event);

      forward_collection = TRUE;
    }

    prev_caps = gst_pad_get_current_caps (stream->pad);

    if (CUR_STREAM (stream)->caps) {
      if (!prev_caps
          || !gst_caps_is_equal_fixed (prev_caps, CUR_STREAM (stream)->caps)) {
        GST_DEBUG_OBJECT (qtdemux, "setting caps %" GST_PTR_FORMAT,
            CUR_STREAM (stream)->caps);
        gst_pad_set_caps (stream->pad, CUR_STREAM (stream)->caps);
      } else {
        GST_DEBUG_OBJECT (qtdemux, "ignore duplicated caps");
      }
    } else {
      GST_WARNING_OBJECT (qtdemux, "stream without caps");
    }

    if (prev_caps)
      gst_caps_unref (prev_caps);
    stream->new_caps = FALSE;

    if (forward_collection) {
      /* Forward upstream collection and selection if any */
      GstEvent *upstream_event = gst_pad_get_sticky_event (qtdemux->sinkpad,
          GST_EVENT_STREAM_COLLECTION, 0);
      if (upstream_event)
        gst_pad_push_event (stream->pad, upstream_event);
    }
  }
  return TRUE;
}

static void
gst_qtdemux_stream_check_and_change_stsd_index (GstQTDemux * demux,
    QtDemuxStream * stream)
{
  if (stream->cur_stsd_entry_index == stream->stsd_sample_description_id)
    return;

  GST_DEBUG_OBJECT (stream->pad, "Changing stsd index from '%u' to '%u'",
      stream->cur_stsd_entry_index, stream->stsd_sample_description_id);
  if (G_UNLIKELY (stream->stsd_sample_description_id >=
          stream->stsd_entries_length)) {
    GST_ELEMENT_ERROR (demux, STREAM, DEMUX,
        (_("This file is invalid and cannot be played.")),
        ("New sample description id is out of bounds (%d >= %d)",
            stream->stsd_sample_description_id, stream->stsd_entries_length));
  } else {
    stream->cur_stsd_entry_index = stream->stsd_sample_description_id;
    stream->new_caps = TRUE;
  }
}

static gboolean
gst_qtdemux_add_stream (GstQTDemux * qtdemux,
    QtDemuxStream * stream, GstTagList * list)
{
  gboolean ret = TRUE;

  if (stream->subtype == FOURCC_vide) {
    gchar *name = g_strdup_printf ("video_%u", qtdemux->n_video_streams);

    stream->pad =
        gst_pad_new_from_static_template (&gst_qtdemux_videosrc_template, name);
    g_free (name);

    if (!gst_qtdemux_configure_stream (qtdemux, stream)) {
      gst_object_unref (stream->pad);
      stream->pad = NULL;
      ret = FALSE;
      goto done;
    }

    qtdemux->n_video_streams++;
  } else if (stream->subtype == FOURCC_soun) {
    gchar *name = g_strdup_printf ("audio_%u", qtdemux->n_audio_streams);

    stream->pad =
        gst_pad_new_from_static_template (&gst_qtdemux_audiosrc_template, name);
    g_free (name);
    if (!gst_qtdemux_configure_stream (qtdemux, stream)) {
      gst_object_unref (stream->pad);
      stream->pad = NULL;
      ret = FALSE;
      goto done;
    }
    qtdemux->n_audio_streams++;
  } else if (stream->subtype == FOURCC_strm) {
    GST_DEBUG_OBJECT (qtdemux, "stream type, not creating pad");
  } else if (stream->subtype == FOURCC_subp || stream->subtype == FOURCC_text
      || stream->subtype == FOURCC_sbtl || stream->subtype == FOURCC_subt
      || stream->subtype == FOURCC_clcp || stream->subtype == FOURCC_wvtt) {
    gchar *name = g_strdup_printf ("subtitle_%u", qtdemux->n_sub_streams);

    stream->pad =
        gst_pad_new_from_static_template (&gst_qtdemux_subsrc_template, name);
    g_free (name);
    if (!gst_qtdemux_configure_stream (qtdemux, stream)) {
      gst_object_unref (stream->pad);
      stream->pad = NULL;
      ret = FALSE;
      goto done;
    }
    qtdemux->n_sub_streams++;
  } else if (stream->subtype == FOURCC_meta) {
    gchar *name = g_strdup_printf ("meta_%u", qtdemux->n_meta_streams);

    stream->pad =
        gst_pad_new_from_static_template (&gst_qtdemux_metasrc_template, name);
    g_free (name);
    if (!gst_qtdemux_configure_stream (qtdemux, stream)) {
      gst_object_unref (stream->pad);
      stream->pad = NULL;
      ret = FALSE;
      goto done;
    }
    qtdemux->n_meta_streams++;
  } else if (CUR_STREAM (stream)->caps) {
    gchar *name = g_strdup_printf ("video_%u", qtdemux->n_video_streams);

    stream->pad =
        gst_pad_new_from_static_template (&gst_qtdemux_videosrc_template, name);
    g_free (name);
    if (!gst_qtdemux_configure_stream (qtdemux, stream)) {
      gst_object_unref (stream->pad);
      stream->pad = NULL;
      ret = FALSE;
      goto done;
    }
    qtdemux->n_video_streams++;
  } else {
    GST_DEBUG_OBJECT (qtdemux, "unknown stream type");
    goto done;
  }

  if (stream->pad) {
    GList *l;

    GST_DEBUG_OBJECT (qtdemux, "adding pad %s %p to qtdemux %p",
        GST_OBJECT_NAME (stream->pad), stream->pad, qtdemux);
    gst_element_add_pad (GST_ELEMENT_CAST (qtdemux), stream->pad);
    GST_OBJECT_LOCK (qtdemux);
    gst_flow_combiner_add_pad (qtdemux->flowcombiner, stream->pad);
    GST_OBJECT_UNLOCK (qtdemux);

    if (stream->stream_tags)
      gst_tag_list_unref (stream->stream_tags);
    stream->stream_tags = list;
    list = NULL;
    /* global tags go on each pad anyway */
    stream->send_global_tags = TRUE;
    /* send upstream GST_EVENT_PROTECTION events that were received before
       this source pad was created */
    for (l = qtdemux->protection_event_queue.head; l != NULL; l = l->next)
      gst_pad_push_event (stream->pad, gst_event_ref (l->data));
  }
done:
  if (list)
    gst_tag_list_unref (list);
  return ret;
}

/* find next atom with @fourcc starting at @offset */
static GstFlowReturn
qtdemux_find_atom (GstQTDemux * qtdemux, guint64 * offset,
    guint64 * length, guint32 fourcc)
{
  GstFlowReturn ret;
  guint32 lfourcc;
  GstBuffer *buf;

  GST_LOG_OBJECT (qtdemux, "finding fourcc %" GST_FOURCC_FORMAT " at offset %"
      G_GUINT64_FORMAT, GST_FOURCC_ARGS (fourcc), *offset);

  while (TRUE) {
    GstMapInfo map;

    buf = NULL;
    ret = gst_pad_pull_range (qtdemux->sinkpad, *offset, 16, &buf);
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto locate_failed;
    if (G_UNLIKELY (gst_buffer_get_size (buf) != 16)) {
      /* likely EOF */
      ret = GST_FLOW_EOS;
      gst_buffer_unref (buf);
      goto locate_failed;
    }
    gst_buffer_map (buf, &map, GST_MAP_READ);
    extract_initial_length_and_fourcc (map.data, 16, length, &lfourcc);
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);

    if (G_UNLIKELY (*length == 0)) {
      GST_DEBUG_OBJECT (qtdemux, "invalid length 0");
      ret = GST_FLOW_ERROR;
      goto locate_failed;
    }

    if (lfourcc == fourcc) {
      GST_DEBUG_OBJECT (qtdemux, "found '%" GST_FOURCC_FORMAT " at offset %"
          G_GUINT64_FORMAT, GST_FOURCC_ARGS (fourcc), *offset);
      break;
    } else {
      GST_LOG_OBJECT (qtdemux,
          "skipping atom '%" GST_FOURCC_FORMAT "' at %" G_GUINT64_FORMAT,
          GST_FOURCC_ARGS (lfourcc), *offset);
      if (*offset == G_MAXUINT64)
        goto locate_failed;
      *offset += *length;
    }
  }

  return GST_FLOW_OK;

locate_failed:
  {
    /* might simply have had last one */
    GST_DEBUG_OBJECT (qtdemux, "fourcc not found");
    return ret;
  }
}

/* should only do something in pull mode */
/* call with OBJECT lock */
static GstFlowReturn
qtdemux_add_fragmented_samples (GstQTDemux * qtdemux)
{
  guint64 length, offset;
  GstBuffer *buf = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  GstFlowReturn res = GST_FLOW_OK;
  GstMapInfo map;

  offset = qtdemux->moof_offset;
  GST_DEBUG_OBJECT (qtdemux, "next moof at offset %" G_GUINT64_FORMAT, offset);

  if (!offset) {
    GST_DEBUG_OBJECT (qtdemux, "no next moof");
    return GST_FLOW_EOS;
  }

  /* best not do pull etc with lock held */
  GST_OBJECT_UNLOCK (qtdemux);

  ret = qtdemux_find_atom (qtdemux, &offset, &length, FOURCC_moof);
  if (ret != GST_FLOW_OK)
    goto flow_failed;

  ret = gst_qtdemux_pull_atom (qtdemux, offset, length, &buf);
  if (G_UNLIKELY (ret != GST_FLOW_OK))
    goto flow_failed;
  gst_buffer_map (buf, &map, GST_MAP_READ);
  if (!qtdemux_parse_moof (qtdemux, map.data, map.size, offset, NULL)) {
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);
    buf = NULL;
    goto parse_failed;
  }

  gst_buffer_unmap (buf, &map);
  gst_buffer_unref (buf);
  buf = NULL;

  offset += length;
  /* look for next moof */
  ret = qtdemux_find_atom (qtdemux, &offset, &length, FOURCC_moof);
  if (G_UNLIKELY (ret != GST_FLOW_OK))
    goto flow_failed;

exit:
  GST_OBJECT_LOCK (qtdemux);

  qtdemux->moof_offset = offset;

  return res;

parse_failed:
  {
    GST_DEBUG_OBJECT (qtdemux, "failed to parse moof");
    offset = 0;
    res = GST_FLOW_ERROR;
    goto exit;
  }
flow_failed:
  {
    /* maybe upstream temporarily flushing */
    if (ret != GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (qtdemux, "no next moof");
      offset = 0;
    } else {
      GST_DEBUG_OBJECT (qtdemux, "upstream WRONG_STATE");
      /* resume at current position next time */
    }
    res = ret;
    goto exit;
  }
}

static void
qtdemux_merge_sample_table (GstQTDemux * qtdemux, QtDemuxStream * stream)
{
  guint i;
  guint32 num_chunks;
  gint32 stts_duration;
  GstByteWriter stsc, stts, stsz;

  /* Each sample has a different size, which we don't support for merging */
  if (stream->sample_size == 0) {
    GST_DEBUG_OBJECT (qtdemux,
        "Not all samples have the same size, not merging");
    return;
  }

  /* The stream has a ctts table, we don't support that */
  if (stream->ctts_present) {
    GST_DEBUG_OBJECT (qtdemux, "Have ctts, not merging");
    return;
  }

  /* If there's a sync sample table also ignore this stream */
  if (stream->stps_present || stream->stss_present) {
    GST_DEBUG_OBJECT (qtdemux, "Have stss/stps, not merging");
    return;
  }

  /* If chunks are considered samples already ignore this stream */
  if (stream->chunks_are_samples) {
    GST_DEBUG_OBJECT (qtdemux, "Chunks are samples, not merging");
    return;
  }

  /* Require that all samples have the same duration */
  if (stream->n_sample_times > 1) {
    GST_DEBUG_OBJECT (qtdemux, "Not all samples have the same duration");
    return;
  }

  if (gst_byte_reader_get_remaining (&stream->stts) < 8) {
    GST_DEBUG_OBJECT (qtdemux, "Too small stts");
    return;
  }

  if (stream->stco.size < 8) {
    GST_DEBUG_OBJECT (qtdemux, "Too small stco");
    return;
  }

  if (stream->n_samples_per_chunk == 0) {
    GST_DEBUG_OBJECT (qtdemux, "No samples per chunk");
    return;
  }

  /* Parse the stts to get the sample duration and number of samples */
  gst_byte_reader_skip_unchecked (&stream->stts, 4);
  stts_duration = gst_byte_reader_get_uint32_be_unchecked (&stream->stts);

  /* Parse the number of chunks from the stco manually because the
   * reader is already behind that */
  num_chunks = GST_READ_UINT32_BE (stream->stco.data + 4);

  GST_DEBUG_OBJECT (qtdemux, "sample_duration %d, num_chunks %u", stts_duration,
      num_chunks);

  if (gst_byte_reader_get_remaining (&stream->stsc) <
      stream->n_samples_per_chunk * 3 * 4) {
    GST_DEBUG_OBJECT (qtdemux, "Too small stsc");
    return;
  }

  /* Now parse stsc, convert chunks into single samples and generate a
   * new stsc, stts and stsz from this information */
  gst_byte_writer_init (&stsc);
  gst_byte_writer_init (&stts);
  gst_byte_writer_init (&stsz);

  /* Note: we skip fourccs, size, version, flags and other fields of the new
   * atoms as the byte readers with them are already behind that position
   * anyway and only update the values of those inside the stream directly.
   */
  stream->n_sample_times = 0;
  stream->n_samples = 0;
  for (i = 0; i < stream->n_samples_per_chunk; i++) {
    guint j;
    guint32 first_chunk, last_chunk, samples_per_chunk, sample_description_id;

    first_chunk = gst_byte_reader_get_uint32_be_unchecked (&stream->stsc);
    samples_per_chunk = gst_byte_reader_get_uint32_be_unchecked (&stream->stsc);
    sample_description_id =
        gst_byte_reader_get_uint32_be_unchecked (&stream->stsc);

    if (i == stream->n_samples_per_chunk - 1) {
      /* +1 because first_chunk is 1-based */
      last_chunk = num_chunks + 1;
    } else {
      last_chunk = gst_byte_reader_peek_uint32_be_unchecked (&stream->stsc);
    }

    GST_DEBUG_OBJECT (qtdemux,
        "Merging first_chunk: %u, last_chunk: %u, samples_per_chunk: %u, sample_description_id: %u",
        first_chunk, last_chunk, samples_per_chunk, sample_description_id);

    gst_byte_writer_put_uint32_be (&stsc, first_chunk);
    /* One sample in this chunk */
    gst_byte_writer_put_uint32_be (&stsc, 1);
    gst_byte_writer_put_uint32_be (&stsc, sample_description_id);

    /* For each chunk write a stts and stsz entry now */
    gst_byte_writer_put_uint32_be (&stts, last_chunk - first_chunk);
    gst_byte_writer_put_uint32_be (&stts, stts_duration * samples_per_chunk);
    for (j = first_chunk; j < last_chunk; j++) {
      gst_byte_writer_put_uint32_be (&stsz,
          stream->sample_size * samples_per_chunk);
    }

    stream->n_sample_times += 1;
    stream->n_samples += last_chunk - first_chunk;
  }

  g_assert_cmpint (stream->n_samples, ==, num_chunks);

  GST_DEBUG_OBJECT (qtdemux, "Have %u samples and %u sample times",
      stream->n_samples, stream->n_sample_times);

  /* We don't have a fixed sample size anymore */
  stream->sample_size = 0;

  /* Free old data for the atoms */
  g_free ((gpointer) stream->stsz.data);
  stream->stsz.data = NULL;
  g_free ((gpointer) stream->stsc.data);
  stream->stsc.data = NULL;
  g_free ((gpointer) stream->stts.data);
  stream->stts.data = NULL;

  /* Store new data and replace byte readers */
  stream->stsz.size = gst_byte_writer_get_size (&stsz);
  stream->stsz.data = gst_byte_writer_reset_and_get_data (&stsz);
  gst_byte_reader_init (&stream->stsz, stream->stsz.data, stream->stsz.size);
  stream->stts.size = gst_byte_writer_get_size (&stts);
  stream->stts.data = gst_byte_writer_reset_and_get_data (&stts);
  gst_byte_reader_init (&stream->stts, stream->stts.data, stream->stts.size);
  stream->stsc.size = gst_byte_writer_get_size (&stsc);
  stream->stsc.data = gst_byte_writer_reset_and_get_data (&stsc);
  gst_byte_reader_init (&stream->stsc, stream->stsc.data, stream->stsc.size);
}

/* initialise bytereaders for stbl sub-atoms */
static gboolean
qtdemux_stbl_init (GstQTDemux * qtdemux, QtDemuxStream * stream, GNode * stbl)
{
  stream->stbl_index = -1;      /* no samples have yet been parsed */
  stream->sample_index = -1;

  /* time-to-sample atom */
  if (!qtdemux_tree_get_child_by_type_full (stbl, FOURCC_stts, &stream->stts))
    goto corrupt_file;

  /* copy atom data into a new buffer for later use */
  stream->stts.data = g_memdup2 (stream->stts.data, stream->stts.size);

  /* skip version + flags */
  if (!gst_byte_reader_skip (&stream->stts, 1 + 3) ||
      !gst_byte_reader_get_uint32_be (&stream->stts, &stream->n_sample_times))
    goto corrupt_file;
  GST_LOG_OBJECT (qtdemux, "%u timestamp blocks", stream->n_sample_times);

  /* make sure there's enough data */
  if (!qt_atom_parser_has_chunks (&stream->stts, stream->n_sample_times, 8)) {
    stream->n_sample_times = gst_byte_reader_get_remaining (&stream->stts) / 8;
    GST_LOG_OBJECT (qtdemux, "overriding to %u timestamp blocks",
        stream->n_sample_times);
    if (!stream->n_sample_times)
      goto corrupt_file;
  }

  /* sync sample atom */
  stream->stps_present = FALSE;
  if ((stream->stss_present =
          !!qtdemux_tree_get_child_by_type_full (stbl, FOURCC_stss,
              &stream->stss) ? TRUE : FALSE) == TRUE) {
    /* copy atom data into a new buffer for later use */
    stream->stss.data = g_memdup2 (stream->stss.data, stream->stss.size);

    /* skip version + flags */
    if (!gst_byte_reader_skip (&stream->stss, 1 + 3) ||
        !gst_byte_reader_get_uint32_be (&stream->stss, &stream->n_sample_syncs))
      goto corrupt_file;

    if (stream->n_sample_syncs) {
      /* make sure there's enough data */
      if (!qt_atom_parser_has_chunks (&stream->stss, stream->n_sample_syncs, 4))
        goto corrupt_file;
    }

    /* partial sync sample atom */
    if ((stream->stps_present =
            !!qtdemux_tree_get_child_by_type_full (stbl, FOURCC_stps,
                &stream->stps) ? TRUE : FALSE) == TRUE) {
      /* copy atom data into a new buffer for later use */
      stream->stps.data = g_memdup2 (stream->stps.data, stream->stps.size);

      /* skip version + flags */
      if (!gst_byte_reader_skip (&stream->stps, 1 + 3) ||
          !gst_byte_reader_get_uint32_be (&stream->stps,
              &stream->n_sample_partial_syncs))
        goto corrupt_file;

      /* if there are no entries, the stss table contains the real
       * sync samples */
      if (stream->n_sample_partial_syncs) {
        /* make sure there's enough data */
        if (!qt_atom_parser_has_chunks (&stream->stps,
                stream->n_sample_partial_syncs, 4))
          goto corrupt_file;
      }
    }
  }

  /* sample size */
  if (!qtdemux_tree_get_child_by_type_full (stbl, FOURCC_stsz, &stream->stsz))
    goto no_samples;

  /* copy atom data into a new buffer for later use */
  stream->stsz.data = g_memdup2 (stream->stsz.data, stream->stsz.size);

  /* skip version + flags */
  if (!gst_byte_reader_skip (&stream->stsz, 1 + 3) ||
      !gst_byte_reader_get_uint32_be (&stream->stsz, &stream->sample_size))
    goto corrupt_file;

  if (!gst_byte_reader_get_uint32_be (&stream->stsz, &stream->n_samples))
    goto corrupt_file;

  if (!stream->n_samples)
    goto no_samples;

  /* sample-to-chunk atom */
  if (!qtdemux_tree_get_child_by_type_full (stbl, FOURCC_stsc, &stream->stsc))
    goto corrupt_file;

  /* copy atom data into a new buffer for later use */
  stream->stsc.data = g_memdup2 (stream->stsc.data, stream->stsc.size);

  /* skip version + flags */
  if (!gst_byte_reader_skip (&stream->stsc, 1 + 3) ||
      !gst_byte_reader_get_uint32_be (&stream->stsc,
          &stream->n_samples_per_chunk))
    goto corrupt_file;

  GST_DEBUG_OBJECT (qtdemux, "n_samples_per_chunk %u",
      stream->n_samples_per_chunk);

  /* make sure there's enough data */
  if (!qt_atom_parser_has_chunks (&stream->stsc, stream->n_samples_per_chunk,
          12))
    goto corrupt_file;


  /* chunk offset */
  if (qtdemux_tree_get_child_by_type_full (stbl, FOURCC_stco, &stream->stco))
    stream->co_size = sizeof (guint32);
  else if (qtdemux_tree_get_child_by_type_full (stbl, FOURCC_co64,
          &stream->stco))
    stream->co_size = sizeof (guint64);
  else
    goto corrupt_file;

  /* copy atom data into a new buffer for later use */
  stream->stco.data = g_memdup2 (stream->stco.data, stream->stco.size);

  /* skip version + flags */
  if (!gst_byte_reader_skip (&stream->stco, 1 + 3))
    goto corrupt_file;

  /* chunks_are_samples == TRUE means treat chunks as samples */
  stream->chunks_are_samples = stream->sample_size
      && !CUR_STREAM (stream)->sampled;
  if (stream->chunks_are_samples) {
    /* treat chunks as samples */
    if (!gst_byte_reader_get_uint32_be (&stream->stco, &stream->n_samples))
      goto corrupt_file;
  } else {
    /* skip number of entries */
    if (!gst_byte_reader_skip (&stream->stco, 4))
      goto corrupt_file;

    /* make sure there are enough data in the stsz atom */
    if (!stream->sample_size) {
      /* different sizes for each sample */
      if (!qt_atom_parser_has_chunks (&stream->stsz, stream->n_samples, 4))
        goto corrupt_file;
    }
  }

  /* composition time-to-sample */
  if ((stream->ctts_present =
          !!qtdemux_tree_get_child_by_type_full (stbl, FOURCC_ctts,
              &stream->ctts) ? TRUE : FALSE) == TRUE) {
    GstByteReader cslg = GST_BYTE_READER_INIT (NULL, 0);
    guint8 ctts_version;
    gboolean checked_ctts = FALSE;

    /* copy atom data into a new buffer for later use */
    stream->ctts.data = g_memdup2 (stream->ctts.data, stream->ctts.size);

    /* version 1 has signed offsets */
    if (!gst_byte_reader_get_uint8 (&stream->ctts, &ctts_version))
      goto corrupt_file;

    /* flags */
    if (!gst_byte_reader_skip (&stream->ctts, 3)
        || !gst_byte_reader_get_uint32_be (&stream->ctts,
            &stream->n_composition_times))
      goto corrupt_file;

    /* make sure there's enough data */
    if (!qt_atom_parser_has_chunks (&stream->ctts, stream->n_composition_times,
            4 + 4))
      goto corrupt_file;

    /* This is optional, if missing we iterate the ctts */
    if (qtdemux_tree_get_child_by_type_full (stbl, FOURCC_cslg, &cslg)) {
      guint8 cslg_version;

      /* cslg version 1 has 64 bit fields */
      if (!gst_byte_reader_get_uint8 (&cslg, &cslg_version))
        goto corrupt_file;

      /* skip flags */
      if (!gst_byte_reader_skip (&cslg, 3))
        goto corrupt_file;

      if (cslg_version == 0) {
        gint32 composition_to_dts_shift;

        if (!gst_byte_reader_get_int32_be (&cslg, &composition_to_dts_shift))
          goto corrupt_file;

        stream->cslg_shift = MAX (0, composition_to_dts_shift);
      } else {
        gint64 composition_to_dts_shift;

        if (!gst_byte_reader_get_int64_be (&cslg, &composition_to_dts_shift))
          goto corrupt_file;

        stream->cslg_shift = MAX (0, composition_to_dts_shift);
      }
    } else {
      gint32 cslg_least = 0;
      guint num_entries, pos;
      gint i;

      pos = gst_byte_reader_get_pos (&stream->ctts);
      num_entries = stream->n_composition_times;

      checked_ctts = TRUE;

      stream->cslg_shift = 0;

      for (i = 0; i < num_entries; i++) {
        gint32 offset;

        gst_byte_reader_skip_unchecked (&stream->ctts, 4);
        offset = gst_byte_reader_get_int32_be_unchecked (&stream->ctts);
        /* HACK: if sample_offset is larger than 2 * duration, ignore the box.
         * slightly inaccurate PTS could be more usable than corrupted one */
        if (G_UNLIKELY ((ctts_version == 0 || offset != G_MININT32)
                && ABS (offset) / 2 > stream->duration)) {
          GST_WARNING_OBJECT (qtdemux,
              "Ignore corrupted ctts, sample_offset %" G_GINT32_FORMAT
              " larger than duration %" G_GUINT64_FORMAT, offset,
              stream->duration);

          stream->cslg_shift = 0;
          stream->ctts_present = FALSE;
          goto done;
        }

        /* Don't consider "no decode samples" with offset G_MININT32
         * for the DTS/PTS shift */
        if (offset != G_MININT32 && offset < cslg_least)
          cslg_least = offset;
      }

      if (cslg_least < 0)
        stream->cslg_shift = -cslg_least;
      else
        stream->cslg_shift = 0;

      /* reset the reader so we can generate sample table */
      gst_byte_reader_set_pos (&stream->ctts, pos);
    }

    /* Check if ctts values are looking reasonable if that didn't happen above */
    if (!checked_ctts) {
      guint num_entries, pos;
      gint i;

      pos = gst_byte_reader_get_pos (&stream->ctts);
      num_entries = stream->n_composition_times;

      for (i = 0; i < num_entries; i++) {
        gint32 offset;

        gst_byte_reader_skip_unchecked (&stream->ctts, 4);
        offset = gst_byte_reader_get_int32_be_unchecked (&stream->ctts);
        /* HACK: if sample_offset is larger than 2 * duration, ignore the box.
         * slightly inaccurate PTS could be more usable than corrupted one */
        if (G_UNLIKELY ((ctts_version == 0 || offset != G_MININT32)
                && ABS (offset) / 2 > stream->duration)) {
          GST_WARNING_OBJECT (qtdemux,
              "Ignore corrupted ctts, sample_offset %" G_GINT32_FORMAT
              " larger than duration %" G_GUINT64_FORMAT, offset,
              stream->duration);

          stream->cslg_shift = 0;
          stream->ctts_present = FALSE;
          goto done;
        }
      }

      /* reset the reader so we can generate sample table */
      gst_byte_reader_set_pos (&stream->ctts, pos);
    }
  } else {
    /* Ensure the cslg_shift value is consistent so we can use it
     * unconditionally to produce TS and Segment */
    stream->cslg_shift = 0;
  }

  GST_DEBUG_OBJECT (qtdemux, "Using cslg_shift %" G_GUINT64_FORMAT,
      stream->cslg_shift);

  /* For raw audio streams especially we might want to merge the samples
   * to not output one audio sample per buffer. We're doing this here
   * before allocating the sample tables so that from this point onwards
   * the number of container samples are static */
  if (stream->min_buffer_size > 0) {
    qtdemux_merge_sample_table (qtdemux, stream);
  }

done:
  GST_DEBUG_OBJECT (qtdemux, "allocating n_samples %u * %u (%.2f MB)",
      stream->n_samples, (guint) sizeof (QtDemuxSample),
      stream->n_samples * sizeof (QtDemuxSample) / (1024.0 * 1024.0));

  if (stream->n_samples >=
      QTDEMUX_MAX_SAMPLE_INDEX_SIZE / sizeof (QtDemuxSample)) {
    GST_WARNING_OBJECT (qtdemux, "not allocating index of %d samples, would "
        "be larger than %uMB (broken file?)", stream->n_samples,
        QTDEMUX_MAX_SAMPLE_INDEX_SIZE >> 20);
    return FALSE;
  }

  g_assert (stream->samples == NULL);
  stream->samples = g_try_new0 (QtDemuxSample, stream->n_samples);
  if (!stream->samples) {
    GST_WARNING_OBJECT (qtdemux, "failed to allocate %d samples",
        stream->n_samples);
    return FALSE;
  }

  return TRUE;

corrupt_file:
  {
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")), (NULL));
    return FALSE;
  }
no_samples:
  {
    gst_qtdemux_stbl_free (stream);
    if (!qtdemux->fragmented) {
      /* not quite good */
      GST_WARNING_OBJECT (qtdemux, "stream has no samples");
      return FALSE;
    } else {
      /* may pick up samples elsewhere */
      return TRUE;
    }
  }
}

/* collect samples from the next sample to be parsed up to sample @n for @stream
 * by reading the info from @stbl
 *
 * This code can be executed from both the streaming thread and the seeking
 * thread so it takes the object lock to protect itself
 */
static gboolean
qtdemux_parse_samples (GstQTDemux * qtdemux, QtDemuxStream * stream, guint32 n)
{
  gint i, j, k;
  QtDemuxSample *samples, *first, *cur, *last;
  guint32 n_samples_per_chunk;
  guint32 n_samples;

  GST_LOG_OBJECT (qtdemux, "parsing samples for stream fourcc %"
      GST_FOURCC_FORMAT ", pad %s",
      GST_FOURCC_ARGS (CUR_STREAM (stream)->fourcc),
      stream->pad ? GST_PAD_NAME (stream->pad) : "(NULL)");

  n_samples = stream->n_samples;

  if (n >= n_samples)
    goto out_of_samples;

  GST_OBJECT_LOCK (qtdemux);
  if (n <= stream->stbl_index)
    goto already_parsed;

  GST_DEBUG_OBJECT (qtdemux, "parsing up to sample %u", n);

  if (!stream->stsz.data) {
    /* so we already parsed and passed all the moov samples;
     * onto fragmented ones */
    g_assert (qtdemux->fragmented);
    goto done;
  }

  /* pointer to the sample table */
  samples = stream->samples;

  /* starts from -1, moves to the next sample index to parse */
  stream->stbl_index++;

  /* keep track of the first and last sample to fill */
  first = &samples[stream->stbl_index];
  last = &samples[n];

  if (!stream->chunks_are_samples) {
    /* set the sample sizes */
    if (stream->sample_size == 0) {
      /* different sizes for each sample */
      for (cur = first; cur <= last; cur++) {
        cur->size = gst_byte_reader_get_uint32_be_unchecked (&stream->stsz);
        GST_LOG_OBJECT (qtdemux, "sample %d has size %u",
            (guint) (cur - samples), cur->size);
      }
    } else {
      /* samples have the same size */
      GST_LOG_OBJECT (qtdemux, "all samples have size %u", stream->sample_size);
      for (cur = first; cur <= last; cur++)
        cur->size = stream->sample_size;
    }
  }

  n_samples_per_chunk = stream->n_samples_per_chunk;
  cur = first;

  for (i = stream->stsc_index; i < n_samples_per_chunk; i++) {
    guint32 last_chunk;

    if (stream->stsc_chunk_index >= stream->last_chunk
        || stream->stsc_chunk_index < stream->first_chunk) {
      stream->first_chunk =
          gst_byte_reader_get_uint32_be_unchecked (&stream->stsc);
      stream->samples_per_chunk =
          gst_byte_reader_get_uint32_be_unchecked (&stream->stsc);
      /* starts from 1 */
      stream->stsd_sample_description_id =
          gst_byte_reader_get_uint32_be_unchecked (&stream->stsc) - 1;

      /* chunk numbers are counted from 1 it seems */
      if (G_UNLIKELY (stream->first_chunk == 0))
        goto corrupt_file;

      --stream->first_chunk;

      /* the last chunk of each entry is calculated by taking the first chunk
       * of the next entry; except if there is no next, where we fake it with
       * INT_MAX */
      if (G_UNLIKELY (i == (stream->n_samples_per_chunk - 1))) {
        stream->last_chunk = G_MAXUINT32;
      } else {
        stream->last_chunk =
            gst_byte_reader_peek_uint32_be_unchecked (&stream->stsc);
        if (G_UNLIKELY (stream->last_chunk == 0))
          goto corrupt_file;

        --stream->last_chunk;
      }

      GST_LOG_OBJECT (qtdemux,
          "entry %d has first_chunk %d, last_chunk %d, samples_per_chunk %d"
          "sample desc ID: %d", i, stream->first_chunk, stream->last_chunk,
          stream->samples_per_chunk, stream->stsd_sample_description_id);

      if (G_UNLIKELY (stream->last_chunk < stream->first_chunk))
        goto corrupt_file;

      if (stream->last_chunk != G_MAXUINT32) {
        if (!qt_atom_parser_peek_sub (&stream->stco,
                stream->first_chunk * stream->co_size,
                (stream->last_chunk - stream->first_chunk) * stream->co_size,
                &stream->co_chunk))
          goto corrupt_file;

      } else {
        stream->co_chunk = stream->stco;
        if (!gst_byte_reader_skip (&stream->co_chunk,
                stream->first_chunk * stream->co_size))
          goto corrupt_file;
      }

      stream->stsc_chunk_index = stream->first_chunk;
    }

    last_chunk = stream->last_chunk;

    if (stream->chunks_are_samples) {
      cur = &samples[stream->stsc_chunk_index];

      for (j = stream->stsc_chunk_index; j < last_chunk; j++) {
        if (j > n) {
          /* save state */
          stream->stsc_chunk_index = j;
          goto done;
        }

        if (!qt_atom_parser_get_offset (&stream->co_chunk,
                stream->co_size, &cur->offset))
          goto corrupt_file;

        GST_LOG_OBJECT (qtdemux, "Created entry %d with offset "
            "%" G_GUINT64_FORMAT, j, cur->offset);

        if (CUR_STREAM (stream)->samples_per_frame > 0 &&
            CUR_STREAM (stream)->bytes_per_frame > 0) {
          cur->size =
              (stream->samples_per_chunk * CUR_STREAM (stream)->n_channels) /
              CUR_STREAM (stream)->samples_per_frame *
              CUR_STREAM (stream)->bytes_per_frame;
        } else {
          cur->size = stream->samples_per_chunk;
        }

        GST_DEBUG_OBJECT (qtdemux,
            "keyframe sample %d: timestamp %" GST_TIME_FORMAT ", size %u",
            j, GST_TIME_ARGS (QTSTREAMTIME_TO_GSTTIME (stream,
                    stream->stco_sample_index)), cur->size);

        cur->timestamp = stream->stco_sample_index;
        cur->duration = stream->samples_per_chunk;
        cur->keyframe = TRUE;
        cur++;

        stream->stco_sample_index += stream->samples_per_chunk;
      }
      stream->stsc_chunk_index = j;
    } else {
      for (j = stream->stsc_chunk_index; j < last_chunk; j++) {
        guint32 samples_per_chunk;
        guint64 chunk_offset;

        if (!stream->stsc_sample_index
            && !qt_atom_parser_get_offset (&stream->co_chunk, stream->co_size,
                &stream->chunk_offset))
          goto corrupt_file;

        samples_per_chunk = stream->samples_per_chunk;
        chunk_offset = stream->chunk_offset;

        for (k = stream->stsc_sample_index; k < samples_per_chunk; k++) {
          GST_LOG_OBJECT (qtdemux, "creating entry %d with offset %"
              G_GUINT64_FORMAT " and size %d",
              (guint) (cur - samples), chunk_offset, cur->size);

          cur->offset = chunk_offset;
          chunk_offset += cur->size;
          cur++;

          if (G_UNLIKELY (cur > last)) {
            /* save state */
            stream->stsc_sample_index = k + 1;
            stream->chunk_offset = chunk_offset;
            stream->stsc_chunk_index = j;
            goto done2;
          }
        }
        stream->stsc_sample_index = 0;
      }
      stream->stsc_chunk_index = j;
    }
    stream->stsc_index++;
  }

  if (stream->chunks_are_samples)
    goto ctts;
done2:
  {
    guint32 n_sample_times;

    n_sample_times = stream->n_sample_times;
    cur = first;

    for (i = stream->stts_index; i < n_sample_times; i++) {
      guint32 stts_samples;
      gint32 stts_duration;
      gint64 stts_time;

      if (stream->stts_sample_index >= stream->stts_samples
          || !stream->stts_sample_index) {

        stream->stts_samples =
            gst_byte_reader_get_uint32_be_unchecked (&stream->stts);
        stream->stts_duration =
            gst_byte_reader_get_uint32_be_unchecked (&stream->stts);

        GST_LOG_OBJECT (qtdemux, "block %d, %u timestamps, duration %u",
            i, stream->stts_samples, stream->stts_duration);

        stream->stts_sample_index = 0;
      }

      stts_samples = stream->stts_samples;
      stts_duration = stream->stts_duration;
      stts_time = stream->stts_time;

      for (j = stream->stts_sample_index; j < stts_samples; j++) {
        GST_DEBUG_OBJECT (qtdemux,
            "sample %d: index %d, timestamp %" GST_TIME_FORMAT,
            (guint) (cur - samples), j,
            GST_TIME_ARGS (QTSTREAMTIME_TO_GSTTIME (stream, stts_time)));

        cur->timestamp = stts_time;
        cur->duration = stts_duration;

        /* avoid 32-bit wrap-around,
         * but still mind possible 'negative' duration */
        stts_time += (gint64) stts_duration;
        cur++;

        if (G_UNLIKELY (cur > last)) {
          /* save values */
          stream->stts_time = stts_time;
          stream->stts_sample_index = j + 1;
          if (stream->stts_sample_index >= stream->stts_samples)
            stream->stts_index++;
          goto done3;
        }
      }
      stream->stts_sample_index = 0;
      stream->stts_time = stts_time;
      stream->stts_index++;
    }
    /* fill up empty timestamps with the last timestamp, this can happen when
     * the last samples do not decode and so we don't have timestamps for them.
     * We however look at the last timestamp to estimate the track length so we
     * need something in here. */
    for (; cur < last; cur++) {
      GST_DEBUG_OBJECT (qtdemux,
          "fill sample %d: timestamp %" GST_TIME_FORMAT,
          (guint) (cur - samples),
          GST_TIME_ARGS (QTSTREAMTIME_TO_GSTTIME (stream, stream->stts_time)));
      cur->timestamp = stream->stts_time;
      cur->duration = -1;
    }
  }
done3:
  {
    /* sample sync, can be NULL */
    if (stream->stss_present == TRUE) {
      guint32 n_sample_syncs;

      n_sample_syncs = stream->n_sample_syncs;

      if (!n_sample_syncs) {
        GST_DEBUG_OBJECT (qtdemux, "all samples are keyframes");
        stream->all_keyframe = TRUE;
      } else {
        for (i = stream->stss_index; i < n_sample_syncs; i++) {
          /* note that the first sample is index 1, not 0 */
          guint32 index;

          index = gst_byte_reader_get_uint32_be_unchecked (&stream->stss);

          if (G_LIKELY (index > 0 && index <= n_samples)) {
            index -= 1;
            samples[index].keyframe = TRUE;
            GST_DEBUG_OBJECT (qtdemux, "samples at %u is keyframe", index);
            /* and exit if we have enough samples */
            if (G_UNLIKELY (index >= n)) {
              i++;
              break;
            }
          }
        }
        /* save state */
        stream->stss_index = i;
      }

      /* stps marks partial sync frames like open GOP I-Frames */
      if (stream->stps_present == TRUE) {
        guint32 n_sample_partial_syncs;

        n_sample_partial_syncs = stream->n_sample_partial_syncs;

        /* if there are no entries, the stss table contains the real
         * sync samples */
        if (n_sample_partial_syncs) {
          for (i = stream->stps_index; i < n_sample_partial_syncs; i++) {
            /* note that the first sample is index 1, not 0 */
            guint32 index;

            index = gst_byte_reader_get_uint32_be_unchecked (&stream->stps);

            if (G_LIKELY (index > 0 && index <= n_samples)) {
              index -= 1;
              samples[index].keyframe = TRUE;
              GST_DEBUG_OBJECT (qtdemux, "samples at %u is keyframe", index);
              /* and exit if we have enough samples */
              if (G_UNLIKELY (index >= n)) {
                i++;
                break;
              }
            }
          }
          /* save state */
          stream->stps_index = i;
        }
      }
    } else {
      /* no stss, all samples are keyframes */
      stream->all_keyframe = TRUE;
      GST_DEBUG_OBJECT (qtdemux, "setting all keyframes");
    }
  }

ctts:
  /* composition time to sample */
  if (stream->ctts_present == TRUE) {
    guint32 n_composition_times;
    guint32 ctts_count;
    gint32 ctts_soffset;

    /* Fill in the pts_offsets */
    cur = first;
    n_composition_times = stream->n_composition_times;

    for (i = stream->ctts_index; i < n_composition_times; i++) {
      if (stream->ctts_sample_index >= stream->ctts_count
          || !stream->ctts_sample_index) {
        stream->ctts_count =
            gst_byte_reader_get_uint32_be_unchecked (&stream->ctts);
        stream->ctts_soffset =
            gst_byte_reader_get_int32_be_unchecked (&stream->ctts);
        stream->ctts_sample_index = 0;
      }

      ctts_count = stream->ctts_count;
      ctts_soffset = stream->ctts_soffset;

      /* FIXME: Set offset to 0 for "no decode samples". This needs
       * to be handled in a codec specific manner ideally. */
      if (ctts_soffset == G_MININT32)
        ctts_soffset = 0;

      for (j = stream->ctts_sample_index; j < ctts_count; j++) {
        cur->pts_offset = ctts_soffset;
        cur++;

        if (G_UNLIKELY (cur > last)) {
          /* save state */
          stream->ctts_sample_index = j + 1;
          goto done;
        }
      }
      stream->ctts_sample_index = 0;
      stream->ctts_index++;
    }
  }
done:
  stream->stbl_index = n;
  /* if index has been completely parsed, free data that is no-longer needed */
  if (n + 1 == stream->n_samples) {
    gst_qtdemux_stbl_free (stream);
    GST_DEBUG_OBJECT (qtdemux, "parsed all available samples;");
    if (qtdemux->pullbased) {
      GST_DEBUG_OBJECT (qtdemux, "checking for more samples");
      while (n + 1 == stream->n_samples)
        if (qtdemux_add_fragmented_samples (qtdemux) != GST_FLOW_OK)
          break;
    }
  }
  GST_OBJECT_UNLOCK (qtdemux);

  return TRUE;

  /* SUCCESS */
already_parsed:
  {
    GST_LOG_OBJECT (qtdemux,
        "Tried to parse up to sample %u but this sample has already been parsed",
        n);
    /* if fragmented, there may be more */
    if (qtdemux->fragmented && n == stream->stbl_index)
      goto done;
    GST_OBJECT_UNLOCK (qtdemux);
    return TRUE;
  }
  /* ERRORS */
out_of_samples:
  {
    GST_LOG_OBJECT (qtdemux,
        "Tried to parse up to sample %u but there are only %u samples", n + 1,
        stream->n_samples);
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")), (NULL));
    return FALSE;
  }
corrupt_file:
  {
    GST_OBJECT_UNLOCK (qtdemux);
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")), (NULL));
    return FALSE;
  }
}

/* collect all segment info for @stream.
 */
static gboolean
qtdemux_parse_segments (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GNode * trak)
{
  GNode *edts;
  /* accept edts if they contain gaps at start and there is only
   * one media segment */
  gboolean allow_pushbased_edts = TRUE;
  gint media_segments_count = 0;

  /* parse and prepare segment info from the edit list */
  GST_DEBUG_OBJECT (qtdemux, "looking for edit list container");
  stream->n_segments = 0;
  stream->segments = NULL;
  if ((edts = qtdemux_tree_get_child_by_type (trak, FOURCC_edts))) {
    GNode *elst;
    guint n_segments;
    guint segment_number, entry_size;
    guint64 time;
    GstClockTime stime;
    const guint8 *buffer;
    guint8 version;
    guint32 size;

    GST_DEBUG_OBJECT (qtdemux, "looking for edit list");
    if (!(elst = qtdemux_tree_get_child_by_type (edts, FOURCC_elst)))
      goto done;

    buffer = elst->data;

    size = QT_UINT32 (buffer);
    /* version, flags, n_segments */
    if (size < 16) {
      GST_WARNING_OBJECT (qtdemux, "Invalid edit list");
      goto done;
    }
    version = QT_UINT8 (buffer + 8);
    entry_size = (version == 1) ? 20 : 12;

    n_segments = QT_UINT32 (buffer + 12);

    if (n_segments > 100000 || size < 16 + n_segments * entry_size) {
      GST_WARNING_OBJECT (qtdemux, "Invalid edit list");
      goto done;
    }

    /* we might allocate a bit too much, at least allocate 1 segment */
    stream->segments = g_new (QtDemuxSegment, MAX (n_segments, 1));

    /* segments always start from 0 */
    time = 0;
    stime = 0;
    buffer += 16;
    for (segment_number = 0; segment_number < n_segments; segment_number++) {
      guint64 duration;
      guint64 media_time;
      gboolean empty_edit = FALSE;
      QtDemuxSegment *segment;
      guint32 rate_int;
      GstClockTime media_start = GST_CLOCK_TIME_NONE;

      if (version == 1) {
        media_time = QT_UINT64 (buffer + 8);
        duration = QT_UINT64 (buffer);
        if (media_time == G_MAXUINT64)
          empty_edit = TRUE;
      } else {
        media_time = QT_UINT32 (buffer + 4);
        duration = QT_UINT32 (buffer);
        if (media_time == G_MAXUINT32)
          empty_edit = TRUE;
      }

      if (!empty_edit)
        media_start = QTSTREAMTIME_TO_GSTTIME (stream, media_time);

      segment = &stream->segments[segment_number];

      /* time and duration expressed in global timescale */
      segment->time = stime;
      if (duration != 0 || empty_edit) {
        /* edge case: empty edits with duration=zero are treated here.
         * (files should not have these anyway). */

        /* add non scaled values so we don't cause roundoff errors */
        time += duration;
        stime = QTTIME_TO_GSTTIME (qtdemux, time);
        segment->duration = stime - segment->time;
      } else {
        /* zero duration does not imply media_start == media_stop
         * but, only specify media_start. The edit ends with the track. */
        stime = segment->duration = GST_CLOCK_TIME_NONE;
        /* Don't allow more edits after this one. */
        n_segments = segment_number + 1;
      }
      segment->stop_time = stime;

      segment->trak_media_start = media_time;
      /* media_time expressed in stream timescale */
      if (!empty_edit) {
        segment->media_start = media_start;
        segment->media_stop = GST_CLOCK_TIME_IS_VALID (segment->duration)
            ? segment->media_start + segment->duration : GST_CLOCK_TIME_NONE;
        media_segments_count++;
      } else {
        segment->media_start = GST_CLOCK_TIME_NONE;
        segment->media_stop = GST_CLOCK_TIME_NONE;
      }
      rate_int = QT_UINT32 (buffer + ((version == 1) ? 16 : 8));

      if (rate_int <= 1) {
        /* 0 is not allowed, some programs write 1 instead of the floating point
         * value */
        GST_WARNING_OBJECT (qtdemux, "found suspicious rate %" G_GUINT32_FORMAT,
            rate_int);
        segment->rate = 1;
      } else {
        segment->rate = rate_int / 65536.0;
      }

      GST_DEBUG_OBJECT (qtdemux, "created segment %d time %" GST_TIME_FORMAT
          ", duration %" GST_TIME_FORMAT ", media_start %" GST_TIME_FORMAT
          " (%" G_GUINT64_FORMAT ") , media_stop %" GST_TIME_FORMAT
          " stop_time %" GST_TIME_FORMAT " rate %g, (%d) timescale %u",
          segment_number, GST_TIME_ARGS (segment->time),
          GST_TIME_ARGS (segment->duration),
          GST_TIME_ARGS (segment->media_start), media_time,
          GST_TIME_ARGS (segment->media_stop),
          GST_TIME_ARGS (segment->stop_time), segment->rate, rate_int,
          stream->timescale);
      if (segment->stop_time > qtdemux->segment.stop &&
          !qtdemux->upstream_format_is_time) {
        GST_WARNING_OBJECT (qtdemux, "Segment %d "
            " extends to %" GST_TIME_FORMAT
            " past the end of the declared movie duration %" GST_TIME_FORMAT
            " movie segment will be extended", segment_number,
            GST_TIME_ARGS (segment->stop_time),
            GST_TIME_ARGS (qtdemux->segment.stop));
        qtdemux->segment.stop = qtdemux->segment.duration = segment->stop_time;
      }

      buffer += entry_size;
    }
    GST_DEBUG_OBJECT (qtdemux, "found %d segments", n_segments);
    stream->n_segments = n_segments;
    if (media_segments_count != 1)
      allow_pushbased_edts = FALSE;
  }
done:

  /* push based does not handle segments, so act accordingly here,
   * and warn if applicable */
  if (!qtdemux->pullbased && !allow_pushbased_edts) {
    GST_WARNING_OBJECT (qtdemux, "streaming; discarding edit list segments");
    /* remove and use default one below, we stream like it anyway */
    g_free (stream->segments);
    stream->segments = NULL;
    stream->n_segments = 0;
  }

  /* no segments, create one to play the complete trak */
  if (stream->n_segments == 0) {
    GstClockTime stream_duration =
        QTSTREAMTIME_TO_GSTTIME (stream, stream->duration);

    if (stream->segments == NULL)
      stream->segments = g_new (QtDemuxSegment, 1);

    /* represent unknown our way */
    if (stream_duration == 0)
      stream_duration = GST_CLOCK_TIME_NONE;

    stream->segments[0].time = 0;
    stream->segments[0].stop_time = stream_duration;
    stream->segments[0].duration = stream_duration;
    stream->segments[0].media_start = 0;
    stream->segments[0].media_stop = stream_duration;
    stream->segments[0].rate = 1.0;
    stream->segments[0].trak_media_start = 0;

    GST_DEBUG_OBJECT (qtdemux, "created dummy segment %" GST_TIME_FORMAT,
        GST_TIME_ARGS (stream_duration));
    stream->n_segments = 1;
    stream->dummy_segment = TRUE;
  }
  GST_DEBUG_OBJECT (qtdemux, "using %d segments", stream->n_segments);

  return TRUE;
}

static gchar *
qtdemux_get_rtsp_uri_from_hndl (GstQTDemux * qtdemux, GNode * minf)
{
  GNode *dinf;
  GstByteReader dref;
  gchar *uri = NULL;

  /*
   * Get 'dinf', to get its child 'dref', that might contain a 'hndl'
   * atom that might contain a 'data' atom with the rtsp uri.
   * This case was reported in bug #597497, some info about
   * the hndl atom can be found in TN1195
   */
  dinf = qtdemux_tree_get_child_by_type (minf, FOURCC_dinf);
  GST_DEBUG_OBJECT (qtdemux, "Trying to obtain rtsp URI for stream trak");

  if (dinf) {
    guint32 dref_num_entries = 0;
    if (qtdemux_tree_get_child_by_type_full (dinf, FOURCC_dref, &dref) &&
        gst_byte_reader_skip (&dref, 4) &&
        gst_byte_reader_get_uint32_be (&dref, &dref_num_entries)) {
      gint i;

      /* search dref entries for hndl atom */
      for (i = 0; i < dref_num_entries; i++) {
        guint32 size = 0, type;
        guint8 string_len = 0;
        if (gst_byte_reader_get_uint32_be (&dref, &size) &&
            qt_atom_parser_get_fourcc (&dref, &type)) {
          if (type == FOURCC_hndl) {
            GST_DEBUG_OBJECT (qtdemux, "Found hndl atom");

            /* skip data reference handle bytes and the
             * following pascal string and some extra 4
             * bytes I have no idea what are */
            if (!gst_byte_reader_skip (&dref, 4) ||
                !gst_byte_reader_get_uint8 (&dref, &string_len) ||
                !gst_byte_reader_skip (&dref, string_len + 4)) {
              GST_WARNING_OBJECT (qtdemux, "Failed to parse hndl atom");
              break;
            }

            /* iterate over the atoms to find the data atom */
            while (gst_byte_reader_get_remaining (&dref) >= 8) {
              guint32 atom_size;
              guint32 atom_type;

              if (gst_byte_reader_get_uint32_be (&dref, &atom_size) &&
                  qt_atom_parser_get_fourcc (&dref, &atom_type)) {
                if (atom_type == FOURCC_data) {
                  const guint8 *uri_aux = NULL;

                  /* found the data atom that might contain the rtsp uri */
                  GST_DEBUG_OBJECT (qtdemux, "Found data atom inside "
                      "hndl atom, interpreting it as an URI");
                  if (gst_byte_reader_peek_data (&dref, atom_size - 8,
                          &uri_aux)) {
                    if (g_strstr_len ((gchar *) uri_aux, 7, "rtsp://") != NULL)
                      uri = g_strndup ((gchar *) uri_aux, atom_size - 8);
                    else
                      GST_WARNING_OBJECT (qtdemux, "Data atom in hndl atom "
                          "didn't contain a rtsp address");
                  } else {
                    GST_WARNING_OBJECT (qtdemux, "Failed to get the data "
                        "atom contents");
                  }
                  break;
                }
                /* skipping to the next entry */
                if (!gst_byte_reader_skip (&dref, atom_size - 8))
                  break;
              } else {
                GST_WARNING_OBJECT (qtdemux, "Failed to parse hndl child "
                    "atom header");
                break;
              }
            }
            break;
          }
          /* skip to the next entry */
          if (!gst_byte_reader_skip (&dref, size - 8))
            break;
        } else {
          GST_WARNING_OBJECT (qtdemux, "Error parsing dref atom");
        }
      }
      GST_DEBUG_OBJECT (qtdemux, "Finished parsing dref atom");
    }
  }
  return uri;
}

#define AMR_NB_ALL_MODES        0x81ff
#define AMR_WB_ALL_MODES        0x83ff
static guint
qtdemux_parse_amr_bitrate (const guint8 * data, guint32 len, gboolean wb)
{
  /* The 'damr' atom is of the form:
   *
   * | vendor | decoder_ver | mode_set | mode_change_period | frames/sample |
   *    32 b       8 b          16 b           8 b                 8 b
   *
   * The highest set bit of the first 7 (AMR-NB) or 8 (AMR-WB) bits of mode_set
   * represents the highest mode used in the stream (and thus the maximum
   * bitrate), with a couple of special cases as seen below.
   */

  /* Map of frame type ID -> bitrate */
  static const guint nb_bitrates[] = {
    4750, 5150, 5900, 6700, 7400, 7950, 10200, 12200
  };
  static const guint wb_bitrates[] = {
    6600, 8850, 12650, 14250, 15850, 18250, 19850, 23050, 23850
  };
  gsize max_mode;
  guint16 mode_set;

  if (len != 0x11) {
    GST_DEBUG ("Atom should have size 0x11, not %u", len);
    goto bad_data;
  }

  if (QT_FOURCC (data + 4) != FOURCC_damr) {
    GST_DEBUG ("Unknown atom in %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (QT_UINT32 (data + 4)));
    goto bad_data;
  }

  mode_set = QT_UINT16 (data + 13);

  if (mode_set == (wb ? AMR_WB_ALL_MODES : AMR_NB_ALL_MODES))
    max_mode = 7 + (wb ? 1 : 0);
  else
    /* AMR-NB modes fo from 0-7, and AMR-WB modes go from 0-8 */
    max_mode = g_bit_nth_msf ((gulong) mode_set & (wb ? 0x1ff : 0xff), -1);

  if (max_mode == -1) {
    GST_DEBUG ("No mode indication was found (mode set) = %x",
        (guint) mode_set);
    goto bad_data;
  }

  return wb ? wb_bitrates[max_mode] : nb_bitrates[max_mode];

bad_data:
  return 0;
}

static gboolean
qtdemux_parse_transformation_matrix (GstQTDemux * qtdemux,
    GstByteReader * reader, guint32 * matrix, const gchar * atom)
{
  /*
   * 9 values of 32 bits (fixed point 16.16, except 2 5 and 8 that are 2.30)
   * [0 1 2]
   * [3 4 5]
   * [6 7 8]
   */

  if (gst_byte_reader_get_remaining (reader) < 36)
    return FALSE;

  matrix[0] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[1] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[2] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[3] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[4] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[5] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[6] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[7] = gst_byte_reader_get_uint32_be_unchecked (reader);
  matrix[8] = gst_byte_reader_get_uint32_be_unchecked (reader);

  /* The 2.30 value conversion does not work for negative values */
  GST_DEBUG_OBJECT (qtdemux, "Transformation matrix from atom %s", atom);
  GST_DEBUG_OBJECT (qtdemux, "%i.%u %i.%u %u.%u", (gint16) (matrix[0] >> 16),
      matrix[0] & 0xFFFF, (gint16) (matrix[1] >> 16), matrix[1] & 0xFFFF,
      matrix[2] >> 30, matrix[2] & 0x3FFFFFFF);
  GST_DEBUG_OBJECT (qtdemux, "%i.%u %i.%u %u.%u", (gint16) (matrix[3] >> 16),
      matrix[3] & 0xFFFF, (gint16) (matrix[4] >> 16), matrix[4] & 0xFFFF,
      matrix[5] >> 30, matrix[5] & 0x3FFFFFFF);
  GST_DEBUG_OBJECT (qtdemux, "%i.%u %i.%u %u.%u", (gint16) (matrix[6] >> 16),
      matrix[6] & 0xFFFF, (gint16) (matrix[7] >> 16), matrix[7] & 0xFFFF,
      matrix[8] >> 30, matrix[8] & 0x3FFFFFFF);

  return TRUE;
}

/* Check if all matrix elements are either 0, 1 or -1 */
static gboolean
qtdemux_transformation_matrix_is_simple (GstQTDemux * qtdemux, guint32 * m)
{
  int i;

  for (i = 0; i < 9; i++) {
    switch (i) {
      case 2:
      case 5:
        /* 2.30 */
        if (m[i] != 0U)
          GST_INFO_OBJECT (qtdemux, "Matrix non-zero UV values ignored");
        break;
      case 6:
      case 7:
        /* 16.16 */
        if (m[i] != 0U)
          GST_INFO_OBJECT (qtdemux, "Matrix non-zero XY values ignored");
        break;
      case 8:
        /* 2.30 */
        if (m[i] != 0U && m[i] != (1U << 30) && m[i] != (3U << 30))
          return FALSE;
        break;
      default:
        /* 16.16 */
        if (m[i] != 0U && m[i] != (1U << 16) && m[i] != (G_MAXUINT16 << 16))
          return FALSE;
        break;
    }
  }

  return TRUE;
}

static void
qtdemux_mul_transformation_matrix (GstQTDemux * qtdemux,
    guint32 * a, guint32 * b, guint32 * c)
{
#define QTMUL_MATRIX(_a,_b) (((_a) == 0 || (_b) == 0) ? 0 : \
      ((_a) == (_b) ? 1 : -1))
#define QTADD_MATRIX(_a,_b) ((_a) + (_b) > 0 ? (1U << 16) : \
      ((_a) + (_b) < 0) ? (G_MAXUINT16 << 16) : 0u)

  if (!qtdemux_transformation_matrix_is_simple (qtdemux, a) ||
      !qtdemux_transformation_matrix_is_simple (qtdemux, b)) {
    GST_WARNING_OBJECT (qtdemux,
        "Cannot handle transform matrix with element values other than 0, 1 or -1");
    /* Pretend to have an identity matrix in this case */
    c[1] = c[2] = c[3] = c[5] = c[6] = c[7] = 0;
    c[0] = c[4] = (1U << 16);
    c[8] = (1 << 30);
  } else {
    c[2] = c[5] = c[6] = c[7] = 0;
    c[0] = QTADD_MATRIX (QTMUL_MATRIX (a[0], b[0]), QTMUL_MATRIX (a[1], b[3]));
    c[1] = QTADD_MATRIX (QTMUL_MATRIX (a[0], b[1]), QTMUL_MATRIX (a[1], b[4]));
    c[3] = QTADD_MATRIX (QTMUL_MATRIX (a[3], b[0]), QTMUL_MATRIX (a[4], b[3]));
    c[4] = QTADD_MATRIX (QTMUL_MATRIX (a[3], b[1]), QTMUL_MATRIX (a[4], b[4]));
    c[8] = a[8];
  }

#undef QTMUL_MATRIX
#undef QTADD_MATRIX
}

static void
qtdemux_inspect_transformation_matrix (GstQTDemux * qtdemux,
    QtDemuxStream * stream, guint32 * matrix, GstTagList ** taglist)
{

/* [a b c]
 * [d e f]
 * [g h i]
 *
 * This macro will only compare value abde, it expects cfi to have already
 * been checked
 */
#define QTCHECK_MATRIX(m,a,b,d,e) ((m)[0] == (a << 16) && (m)[1] == (b << 16) && \
                                   (m)[3] == (d << 16) && (m)[4] == (e << 16))

  /* only handle the cases where the last column has standard values */
  if (matrix[2] == 0 && matrix[5] == 0 && matrix[8] == 1 << 30) {
    const gchar *rotation_tag = NULL;

    /* no rotation needed */
    if (QTCHECK_MATRIX (matrix, 1, 0, 0, 1)) {
      /* NOP */
    } else if (QTCHECK_MATRIX (matrix, 0, 1, G_MAXUINT16, 0)) {
      rotation_tag = "rotate-90";
    } else if (QTCHECK_MATRIX (matrix, G_MAXUINT16, 0, 0, G_MAXUINT16)) {
      rotation_tag = "rotate-180";
    } else if (QTCHECK_MATRIX (matrix, 0, G_MAXUINT16, 1, 0)) {
      rotation_tag = "rotate-270";
    } else if (QTCHECK_MATRIX (matrix, G_MAXUINT16, 0, 0, 1)) {
      rotation_tag = "flip-rotate-0";
    } else if (QTCHECK_MATRIX (matrix, 0, G_MAXUINT16, G_MAXUINT16, 0)) {
      rotation_tag = "flip-rotate-90";
    } else if (QTCHECK_MATRIX (matrix, 1, 0, 0, G_MAXUINT16)) {
      rotation_tag = "flip-rotate-180";
    } else if (QTCHECK_MATRIX (matrix, 0, 1, 1, 0)) {
      rotation_tag = "flip-rotate-270";
    } else {
      GST_FIXME_OBJECT (qtdemux, "Unhandled transformation matrix values");
    }

    GST_DEBUG_OBJECT (qtdemux, "Transformation matrix rotation %s",
        GST_STR_NULL (rotation_tag));
    if (rotation_tag != NULL) {
      if (*taglist == NULL)
        *taglist = gst_tag_list_new_empty ();
      gst_tag_list_add (*taglist, GST_TAG_MERGE_REPLACE,
          GST_TAG_IMAGE_ORIENTATION, rotation_tag, NULL);
    }
  } else {
    GST_FIXME_OBJECT (qtdemux, "Unhandled transformation matrix values");
  }
}

static gboolean
qtdemux_parse_protection_aavd (GstQTDemux * qtdemux,
    QtDemuxStream * stream, GNode * container, guint32 * original_fmt)
{
  GNode *adrm;
  guint32 adrm_size;
  GstBuffer *adrm_buf = NULL;
  QtDemuxAavdEncryptionInfo *info;

  adrm = qtdemux_tree_get_child_by_type (container, FOURCC_adrm);
  if (G_UNLIKELY (!adrm)) {
    GST_ERROR_OBJECT (qtdemux, "aavd box does not contain mandatory adrm box");
    return FALSE;
  }
  adrm_size = QT_UINT32 (adrm->data);
  adrm_buf = gst_buffer_new_memdup (adrm->data, adrm_size);

  stream->protection_scheme_type = FOURCC_aavd;

  if (!stream->protection_scheme_info)
    stream->protection_scheme_info = g_new0 (QtDemuxAavdEncryptionInfo, 1);

  info = (QtDemuxAavdEncryptionInfo *) stream->protection_scheme_info;

  if (info->default_properties)
    gst_structure_free (info->default_properties);
  info->default_properties = gst_structure_new ("application/x-aavd",
      "encrypted", G_TYPE_BOOLEAN, TRUE,
      "adrm", GST_TYPE_BUFFER, adrm_buf, NULL);
  gst_buffer_unref (adrm_buf);

  *original_fmt = FOURCC_mp4a;
  return TRUE;
}

/* Parses the boxes defined in ISO/IEC 14496-12 that enable support for
 * protected streams (sinf, frma, schm and schi); if the protection scheme is
 * Common Encryption (cenc), the function will also parse the tenc box (defined
 * in ISO/IEC 23001-7). @container points to the node that contains these boxes
 * (typically an enc[v|a|t|s] sample entry); the function will set
 * @original_fmt to the fourcc of the original unencrypted stream format.
 * Returns TRUE if successful; FALSE otherwise. */
static gboolean
qtdemux_parse_protection_scheme_info (GstQTDemux * qtdemux,
    QtDemuxStream * stream, GNode * container, guint32 * original_fmt)
{
  GNode *sinf;
  GNode *frma;
  GNode *schm;
  GNode *schi;
  QtDemuxCencSampleSetInfo *info;
  GNode *tenc;
  const guint8 *tenc_data;

  g_return_val_if_fail (qtdemux != NULL, FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (container != NULL, FALSE);
  g_return_val_if_fail (original_fmt != NULL, FALSE);

  sinf = qtdemux_tree_get_child_by_type (container, FOURCC_sinf);
  if (G_UNLIKELY (!sinf)) {
    if (stream->protection_scheme_type == FOURCC_cenc
        || stream->protection_scheme_type == FOURCC_cbcs) {
      GST_ERROR_OBJECT (qtdemux, "sinf box does not contain schi box, which is "
          "mandatory for Common Encryption");
      return FALSE;
    }
    return TRUE;
  }

  frma = qtdemux_tree_get_child_by_type (sinf, FOURCC_frma);
  if (G_UNLIKELY (!frma)) {
    GST_ERROR_OBJECT (qtdemux, "sinf box does not contain mandatory frma box");
    return FALSE;
  }

  *original_fmt = QT_FOURCC ((const guint8 *) frma->data + 8);
  GST_DEBUG_OBJECT (qtdemux, "original stream format: '%" GST_FOURCC_FORMAT "'",
      GST_FOURCC_ARGS (*original_fmt));

  schm = qtdemux_tree_get_child_by_type (sinf, FOURCC_schm);
  if (!schm) {
    GST_DEBUG_OBJECT (qtdemux, "sinf box does not contain schm box");
    return FALSE;
  }
  stream->protection_scheme_type = QT_FOURCC ((const guint8 *) schm->data + 12);
  stream->protection_scheme_version =
      QT_UINT32 ((const guint8 *) schm->data + 16);

  GST_DEBUG_OBJECT (qtdemux,
      "protection_scheme_type: %" GST_FOURCC_FORMAT ", "
      "protection_scheme_version: %#010x",
      GST_FOURCC_ARGS (stream->protection_scheme_type),
      stream->protection_scheme_version);

  schi = qtdemux_tree_get_child_by_type (sinf, FOURCC_schi);
  if (!schi) {
    GST_DEBUG_OBJECT (qtdemux, "sinf box does not contain schi box");
    return FALSE;
  }
  if (stream->protection_scheme_type != FOURCC_cenc &&
      stream->protection_scheme_type != FOURCC_piff &&
      stream->protection_scheme_type != FOURCC_cbcs) {
    GST_ERROR_OBJECT (qtdemux,
        "Invalid protection_scheme_type: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (stream->protection_scheme_type));
    return FALSE;
  }

  if (G_UNLIKELY (!stream->protection_scheme_info))
    stream->protection_scheme_info =
        g_malloc0 (sizeof (QtDemuxCencSampleSetInfo));

  info = (QtDemuxCencSampleSetInfo *) stream->protection_scheme_info;

  if (stream->protection_scheme_type == FOURCC_cenc
      || stream->protection_scheme_type == FOURCC_cbcs) {
    guint8 is_encrypted;
    guint8 iv_size;
    guint8 constant_iv_size = 0;
    const guint8 *default_kid;
    guint8 crypt_byte_block = 0;
    guint8 skip_byte_block = 0;
    const guint8 *constant_iv = NULL;

    tenc = qtdemux_tree_get_child_by_type (schi, FOURCC_tenc);
    if (!tenc) {
      GST_ERROR_OBJECT (qtdemux, "schi box does not contain tenc box, "
          "which is mandatory for Common Encryption");
      return FALSE;
    }
    tenc_data = (const guint8 *) tenc->data + 12;
    is_encrypted = QT_UINT8 (tenc_data + 2);
    iv_size = QT_UINT8 (tenc_data + 3);
    default_kid = (tenc_data + 4);
    if (stream->protection_scheme_type == FOURCC_cbcs) {
      guint8 possible_pattern_info;
      if (iv_size == 0) {
        constant_iv_size = QT_UINT8 (tenc_data + 20);
        if (constant_iv_size != 8 && constant_iv_size != 16) {
          GST_ERROR_OBJECT (qtdemux,
              "constant IV size should be 8 or 16, not %hhu", constant_iv_size);
          return FALSE;
        }
        constant_iv = (tenc_data + 21);
      }
      possible_pattern_info = QT_UINT8 (tenc_data + 1);
      crypt_byte_block = (possible_pattern_info >> 4) & 0x0f;
      skip_byte_block = possible_pattern_info & 0x0f;
    }
    qtdemux_update_default_sample_cenc_settings (qtdemux, info,
        is_encrypted, stream->protection_scheme_type, iv_size, default_kid,
        crypt_byte_block, skip_byte_block, constant_iv_size, constant_iv);
  } else if (stream->protection_scheme_type == FOURCC_piff) {
    GstByteReader br;
    static const guint8 piff_track_encryption_uuid[] = {
      0x89, 0x74, 0xdb, 0xce, 0x7b, 0xe7, 0x4c, 0x51,
      0x84, 0xf9, 0x71, 0x48, 0xf9, 0x88, 0x25, 0x54
    };

    tenc = qtdemux_tree_get_child_by_type (schi, FOURCC_uuid);
    if (!tenc) {
      GST_ERROR_OBJECT (qtdemux, "schi box does not contain tenc box, "
          "which is mandatory for Common Encryption");
      return FALSE;
    }

    tenc_data = (const guint8 *) tenc->data + 8;
    if (memcmp (tenc_data, piff_track_encryption_uuid, 16) != 0) {
      gchar *box_uuid = qtdemux_uuid_bytes_to_string (tenc_data);
      GST_ERROR_OBJECT (qtdemux,
          "Unsupported track encryption box with uuid: %s", box_uuid);
      g_free (box_uuid);
      return FALSE;
    }
    tenc_data = (const guint8 *) tenc->data + 16 + 12;
    gst_byte_reader_init (&br, tenc_data, 20);
    if (!qtdemux_update_default_piff_encryption_settings (qtdemux, info, &br)) {
      GST_ERROR_OBJECT (qtdemux, "PIFF track box parsing error");
      return FALSE;
    }
    stream->protection_scheme_type = FOURCC_cenc;
  }

  return TRUE;
}

static gint
qtdemux_track_id_compare_func (QtDemuxStream ** stream1,
    QtDemuxStream ** stream2)
{
  return (gint) (*stream1)->track_id - (gint) (*stream2)->track_id;
}

static gboolean
qtdemux_parse_stereo_svmi_atom (GstQTDemux * qtdemux, QtDemuxStream * stream,
    GNode * stbl)
{
  GNode *svmi;

  /*parse svmi header if existing */
  svmi = qtdemux_tree_get_child_by_type (stbl, FOURCC_svmi);
  if (svmi) {
    guint32 len = QT_UINT32 ((guint8 *) svmi->data);
    guint32 version = QT_UINT32 ((guint8 *) svmi->data + 8);
    if (!version) {
      GstVideoMultiviewMode mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
      GstVideoMultiviewFlags flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;
      guint8 frame_type, frame_layout;
      guint32 stereo_mono_change_count;

      if (len < 18)
        return FALSE;

      /* MPEG-A stereo video */
      if (qtdemux->major_brand == FOURCC_ss02)
        flags |= GST_VIDEO_MULTIVIEW_FLAGS_MIXED_MONO;

      frame_type = QT_UINT8 ((guint8 *) svmi->data + 12);
      frame_layout = QT_UINT8 ((guint8 *) svmi->data + 13) & 0x01;
      stereo_mono_change_count = QT_UINT32 ((guint8 *) svmi->data + 14);

      switch (frame_type) {
        case 0:
          mode = GST_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE;
          break;
        case 1:
          mode = GST_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED;
          break;
        case 2:
          mode = GST_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME;
          break;
        case 3:
          /* mode 3 is primary/secondary view sequence, ie
           * left/right views in separate tracks. See section 7.2
           * of ISO/IEC 23000-11:2009 */
          /* In the future this might be supported using related
           * streams, like an enhancement track - if files like this
           * ever exist */
          GST_FIXME_OBJECT (qtdemux,
              "Implement stereo video in separate streams");
      }

      if ((frame_layout & 0x1) == 0)
        flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;

      GST_LOG_OBJECT (qtdemux,
          "StereoVideo: composition type: %u, is_left_first: %u",
          frame_type, frame_layout);

      if (stereo_mono_change_count > 1) {
        GST_FIXME_OBJECT (qtdemux,
            "Mixed-mono flags are not yet supported in qtdemux.");
      }

      stream->multiview_mode = mode;
      stream->multiview_flags = flags;
    }
  }

  return TRUE;
}

typedef enum
{
  // ISO/IEC 23001-17 Table 1 - Component Types
  COMPONENT_MONOCHROME = 0,     // Gray
  COMPONENT_Y = 1,              // Luma: Y
  COMPONENT_U = 2,              // Chroma: Cb or U
  COMPONENT_V = 3,              // Chroma: Cr or V
  COMPONENT_RED = 4,            // R
  COMPONENT_GREEN = 5,          // G
  COMPONENT_BLUE = 6,           // B
  COMPONENT_ALPHA = 7,          // A
  COMPONENT_DEPTH = 8,          //
  COMPONENT_DISPARITY = 9,      //
  COMPONENT_PALETTE = 10,       // The component_format value for this component shall be 0.
  COMPONENT_FILTER_ARRAY = 11,  // Bayer, RGBW, etc.
  COMPONENT_PADDING = 12,       // unused bit/bytes
  COMPONENT_CYAN = 13,          //
  COMPONENT_MAGENTA = 14,       //
  COMPONENT_YELLOW = 15,        //
  COMPONENT_KEY = 16,           // Black
  // Values 17 to 0x7FFF are reserved
  // Values 0x8000 to 0xFFFF are user-defined
} ComponentType;

typedef enum
{
  // ISO/IEC 23001-17 Table 4 - Interleave Types
  INTERLEAVE_COMPONENT = 0,     // Planar: RRR... GGG... BBB...
  INTERLEAVE_PIXEL = 1,         // Packed: RGB RGB RGB ...
  INTERLEAVE_MIXED = 2,         // All Y components followed by interleaved U and V components.
  INTERLEAVE_ROW = 3,           // Interleaved by rows
  INTERLEAVE_TILE = 4,          // Interleaved by tiles
  INTERLEAVE_MULTI_Y = 5,       // Multiple Y components with a single UV pair
} InterleaveType;

typedef enum
{
  // ISO/IEC 23001-17 Table 3 - Sampling Types
  SAMPLING_444 = 0,             // 4:4:4 (no subsampling)
  SAMPLING_422 = 1,             // 4:2:2
  SAMPLING_420 = 2,             // 4:2:0
  SAMPLING_411 = 3,             // 4:1:1
} SamplingType;

typedef struct ComponentDefinitionBox
{
  // ComponentDefinitionBox
  guint32 component_count;      // Should match the uncC component_count
  ComponentType *types;         // The type of component (R,G,B,A,Y,U,V, etc.)
  const gchar **type_uris;      // Describes a user-defined component type
} ComponentDefinitionBox;

typedef struct UncompressedFrameConfigComponent
{
  guint16 index;                // Index associated with the cmpd box
  guint8 bit_depth;             // The number of bits to store a component value.
  guint8 format;                // 0: int, 1: float, 2: complex
  guint8 align_size;            // The number of bytes used to store a component value. If 0, refer to bit_depth.
} UncompressedFrameConfigComponent;

typedef struct UncompressedFrameConfigBox
{
  guint8 version;
  guint32 flags;
  guint32 profile;              // indicates a predefined configuration
  guint32 component_count;      // Should match the cmpd component_count
  UncompressedFrameConfigComponent *components; // Array of Components
  guint8 sampling_type;         // 0=4:4:4, 1=4:2:2, 2=4:2:0, 3=4:1:1
  guint8 interleave_type;       // Planar, interleaved, etc.
  guint8 block_size;            // Stores data in fixed-sized blocks
  gboolean components_little_endian;    // indicates that components are stored as little endian
  gboolean block_pad_lsb;       // Padding bits location
  gboolean block_little_endian; // Block Endianness
  gboolean block_reversed;      // Indicates if component order is reversed within the block
  gboolean pad_unknown;         // the value of padded bits is unknown
  guint32 pixel_size;           // number of bytes used to store all components for a single pixel
  guint32 row_align_size;       // Padding between rows
  guint32 tile_align_size;      // Padding between tiles
  guint32 num_tile_cols;        // Number of horizontal tiles
  guint32 num_tile_rows;        // Number of vertical tiles
} UncompressedFrameConfigBox;

static void
qtdemux_clear_uncC (UncompressedFrameConfigBox * uncC)
{
  if (!uncC)
    return;

  g_free (uncC->components);
}

static void
qtdemux_clear_cmpd (ComponentDefinitionBox * cmpd)
{
  if (!cmpd)
    return;

  g_free (cmpd->types);
  g_free (cmpd->type_uris);
}

static gboolean
qtdemux_parse_cmpd (GstQTDemux * qtdemux, GstByteReader * reader,
    ComponentDefinitionBox * cmpd)
{
  /* There should be enough to parse the component_count (4) */
  if (gst_byte_reader_get_remaining (reader) < 4) {
    GST_ERROR_OBJECT (qtdemux, "cmpd is too short");
    goto error;
  }

  cmpd->component_count = gst_byte_reader_get_uint32_be_unchecked (reader);

  guint32 minimum_size = cmpd->component_count * 2 + 4; // assuming type_uris are not used
  if (gst_byte_reader_get_size (reader) < minimum_size) {
    GST_ERROR_OBJECT (qtdemux, "cmpd size is too short");
    goto error;
  }

  cmpd->types = g_new0 (ComponentType, cmpd->component_count);
  cmpd->type_uris = g_new0 (const gchar *, cmpd->component_count);

  guint16 type = 0;
  for (guint32 i = 0; i < cmpd->component_count; i++) {
    if (!gst_byte_reader_get_uint16_be (reader, &type)) {
      GST_ERROR_OBJECT (qtdemux, "Failed to read component type");
      goto error;
    }
    if (type >= 0x8000) {
      if (!gst_byte_reader_get_string (reader, &cmpd->type_uris[i])) {
        GST_ERROR_OBJECT (qtdemux, "Failed to read component type URI");
        goto error;
      }
    }
    cmpd->types[i] = (ComponentType) type;
  }

  /* Success */
  return TRUE;

error:
  return FALSE;
}

static gboolean
qtdemux_parse_uncC (GstQTDemux * qtdemux, GstByteReader * reader,
    UncompressedFrameConfigBox * uncC)
{
  /* There should be enough to parse the version/flags (4) & profile (4) */
  if (gst_byte_reader_get_remaining (reader) < 8) {
    GST_ERROR_OBJECT (qtdemux, "uncC is too short");
    goto error;
  }

  uncC->version = gst_byte_reader_get_uint8_unchecked (reader);
  uncC->flags = gst_byte_reader_get_uint24_be_unchecked (reader);

  uncC->profile = gst_byte_reader_get_uint32_le_unchecked (reader);
  if (uncC->version == 1) {
    /* Use the profile for predetermined settings */
    goto success;
  } else if (uncC->version != 0) {
    GST_ERROR_OBJECT (qtdemux, "Unsupported uncC version");
    goto error;
  }

  if (!gst_byte_reader_get_uint32_be (reader, &uncC->component_count)) {
    GST_ERROR_OBJECT (qtdemux, "Failed to read component count");
    goto error;
  }

  guint32 expected_size = uncC->component_count * 5 + 36;
  if (gst_byte_reader_get_size (reader) != expected_size) {
    GST_ERROR_OBJECT (qtdemux, "uncC size is incorrect");
    goto error;
  }

  guint32 expected_remaining = uncC->component_count * 5 + 24;
  if (gst_byte_reader_get_remaining (reader) < expected_remaining) {
    GST_ERROR_OBJECT (qtdemux, "uncC is too short");
    goto error;
  }

  uncC->components =
      g_new0 (UncompressedFrameConfigComponent, uncC->component_count);

  for (guint32 i = 0; i < uncC->component_count; i++) {
    UncompressedFrameConfigComponent *component = &uncC->components[i];
    component->index = gst_byte_reader_get_uint16_be_unchecked (reader);
    component->bit_depth = gst_byte_reader_get_uint8_unchecked (reader) + 1;
    component->format = gst_byte_reader_get_uint8_unchecked (reader);
    component->align_size = gst_byte_reader_get_uint8_unchecked (reader);
  }

  uncC->sampling_type = gst_byte_reader_get_uint8_unchecked (reader);
  uncC->interleave_type = gst_byte_reader_get_uint8_unchecked (reader);
  uncC->block_size = gst_byte_reader_get_uint8_unchecked (reader);

  guint8 block_flags = gst_byte_reader_get_uint8_unchecked (reader);
  uncC->components_little_endian = (block_flags >> 7) & 0x01;
  uncC->block_pad_lsb = (block_flags >> 6) & 0x01;
  uncC->block_little_endian = (block_flags >> 5) & 0x01;
  uncC->block_reversed = (block_flags >> 4) & 0x01;
  uncC->pad_unknown = (block_flags >> 3) & 0x01;

  uncC->pixel_size = gst_byte_reader_get_uint32_be_unchecked (reader);
  uncC->row_align_size = gst_byte_reader_get_uint32_be_unchecked (reader);
  uncC->tile_align_size = gst_byte_reader_get_uint32_be_unchecked (reader);
  uncC->num_tile_cols = gst_byte_reader_get_uint32_be_unchecked (reader) + 1;
  uncC->num_tile_rows = gst_byte_reader_get_uint32_be_unchecked (reader) + 1;

success:
  return TRUE;

error:
  return FALSE;
}

typedef struct ComponentFormatMapping
{
  GstVideoFormat format;
  UncompressedFrameConfigBox uncC;
  UncompressedFrameConfigComponent component_config;    // All components are assumed to have the same config
  guint16 component_types[4];   // From cmpd
} ComponentFormatMapping;

static const ComponentFormatMapping component_lookup[] = {
  {
        GST_VIDEO_FORMAT_GRAY8,
        {.component_count = 1,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 1},
        {.bit_depth = 8,.format = 0,.align_size = 1},
      {COMPONENT_MONOCHROME}},
  {
        GST_VIDEO_FORMAT_GRAY8,
        {.component_count = 1,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_444,.pixel_size =
              1},
        {.bit_depth = 8,.format = 0,.align_size = 1},
      {COMPONENT_MONOCHROME}},
  {
        GST_VIDEO_FORMAT_GRAY16_BE,
        {.component_count = 1,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_444,.pixel_size =
              2,.components_little_endian = FALSE},
        {.bit_depth = 16,.format = 0,.align_size = 2},
      {COMPONENT_MONOCHROME}},
  {
        GST_VIDEO_FORMAT_RGB,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
      {COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE}},
  {
        GST_VIDEO_FORMAT_RGBP,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_444,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
      {COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE}},
  {
        GST_VIDEO_FORMAT_BGRP,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_444,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
      {COMPONENT_BLUE, COMPONENT_GREEN, COMPONENT_RED}},
  {
        GST_VIDEO_FORMAT_RGBx,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE}
      },
  {
        GST_VIDEO_FORMAT_GBR,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_444,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_GREEN, COMPONENT_BLUE, COMPONENT_RED}
      },
  {
        GST_VIDEO_FORMAT_BGR,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_BLUE, COMPONENT_GREEN, COMPONENT_RED}
      },
  {
        GST_VIDEO_FORMAT_BGRx,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_BLUE, COMPONENT_GREEN, COMPONENT_RED}
      },
  {
        GST_VIDEO_FORMAT_r210,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size =
              4,.block_size = 4},
        {.bit_depth = 10,.format = 0,.align_size = 0},
        {COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE}
      },
  {
        GST_VIDEO_FORMAT_Y444,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_444,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_v308,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_Y42B,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_422,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_I420,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_420,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_YV12,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_420,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_V, COMPONENT_U}
      },
  {
        GST_VIDEO_FORMAT_IYU2,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_U, COMPONENT_Y, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_NV12,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_MIXED,.sampling_type = SAMPLING_420,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_NV21,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_MIXED,.sampling_type = SAMPLING_420,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_V, COMPONENT_U}
      },
  {
        GST_VIDEO_FORMAT_NV16,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_MIXED,.sampling_type = SAMPLING_422,.pixel_size = 3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_Y41B,
        {.component_count = 3,.interleave_type =
              INTERLEAVE_COMPONENT,.sampling_type = SAMPLING_411,.pixel_size =
              3},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_AYUV,
        {.component_count = 4,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_ALPHA, COMPONENT_Y, COMPONENT_U, COMPONENT_V}
      },
  {
        GST_VIDEO_FORMAT_ARGB,
        {.component_count = 4,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_ALPHA, COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE}
      },
  {
        GST_VIDEO_FORMAT_BGRA,
        {.component_count = 4,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_BLUE, COMPONENT_GREEN, COMPONENT_RED, COMPONENT_ALPHA}
      },
  {
        GST_VIDEO_FORMAT_RGBA,
        {.component_count = 4,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE, COMPONENT_ALPHA}
      },
  {
        GST_VIDEO_FORMAT_RGBx,
        {.component_count = 4,.interleave_type =
              INTERLEAVE_PIXEL,.sampling_type = SAMPLING_444,.pixel_size = 4},
        {.bit_depth = 8,.format = 0,.align_size = 1},
        {COMPONENT_RED, COMPONENT_GREEN, COMPONENT_BLUE, COMPONENT_PADDING}
      },
};


static GstVideoFormat
qtdemux_get_format_from_uncv (GstQTDemux * qtdemux,
    UncompressedFrameConfigBox * uncC, ComponentDefinitionBox * cmpd)
{
  guint32 num_components = uncC->component_count;
  guint16 component_types[4];


  if (uncC->version == 1) {
    // Determine format with profile
    // The only permitted profiles for version 1 are `rgb3`, `rgba`, and `abgr`
    switch (uncC->profile) {
      case GST_MAKE_FOURCC ('r', 'g', 'b', '3'):       // RGB 24 bits packed
        return GST_VIDEO_FORMAT_RGB;
        break;

      case GST_MAKE_FOURCC ('r', 'g', 'b', 'a'):       // RGBA 32 bits packed
        return GST_VIDEO_FORMAT_RGBA;
        break;

      case GST_MAKE_FOURCC ('a', 'b', 'g', 'r'):       // RGBA 32 bits packed
        return GST_VIDEO_FORMAT_ABGR;
        break;

      default:
        goto unsupported_feature;
    }

  } else if (uncC->version == 0) {
    // Determine format with uncC & cmpd boxes
  } else {
    GST_WARNING_OBJECT (qtdemux, "Unsupported uncv version: %u", uncC->version);
    goto unsupported_feature;
  }


  /* Assert that components are similar */
  UncompressedFrameConfigComponent *first_comp = &uncC->components[0];
  guint8 align_size = first_comp->align_size;
  for (guint32 i = 0; i < num_components; i++) {
    // For now, assert that each component has the same bit depth
    UncompressedFrameConfigComponent *comp = &uncC->components[i];
    if (comp->bit_depth != first_comp->bit_depth) {
      GST_WARNING_OBJECT (qtdemux,
          "Unsupported bit_depth combination for uncompressed track: %u != %u",
          comp->bit_depth, first_comp->bit_depth);
      goto unsupported_feature;
    }
    // For now, assert that each component has the same align size
    if (comp->align_size != first_comp->align_size) {
      GST_WARNING_OBJECT (qtdemux,
          "Unsupported component_align_size for uncompressed track: %u != %u",
          comp->align_size, first_comp->align_size);
      goto unsupported_feature;
    }
  }


  /* Unsupported Features */
  if (align_size) {
    // If component_align_size is 0, the component value
    // is coded on component_bit_depth bits exactly
    GST_WARNING_OBJECT (qtdemux,
        "Unsupported align_size for uncompressed track: %u", align_size);
    goto unsupported_feature;
  } else if (uncC->tile_align_size) {
    // tile_align_size indicates the padding between tiles
    GST_WARNING_OBJECT (qtdemux,
        "Unsupported tile_align_size for uncompressed track: %u",
        uncC->tile_align_size);
    goto unsupported_feature;
  }

  // Get Component Types
  for (guint32 i = 0; i < num_components; i++) {
    guint16 component_index = uncC->components[i].index;
    component_types[i] = cmpd->types[component_index];
  }

  // Lookup Format
  const ComponentFormatMapping *lut = component_lookup;
  for (guint i = 0; i < G_N_ELEMENTS (component_lookup); i++) {
    // Component Count
    if (num_components != lut[i].uncC.component_count) {
      continue;
    }

    // Component Bit Depth
    if (first_comp->bit_depth != lut[i].component_config.bit_depth) {
      continue;
    }

    // Component Align Size
    if (align_size && align_size != lut[i].component_config.align_size) {
      continue;                 // If set, the align size must match
    }

    // Interleave Types
    if (uncC->interleave_type != lut[i].uncC.interleave_type) {
      continue;
    }

    // Sampling Types
    if (uncC->sampling_type != lut[i].uncC.sampling_type) {
      continue;
    }

    // Pixel Size
    if (uncC->pixel_size && uncC->pixel_size != lut[i].uncC.pixel_size) {
      continue;                 // If set, the pixel size must match
    }

    // Block Size
    if (uncC->block_size && uncC->block_size != lut[i].uncC.block_size) {
      continue;                 // If set, the block size must match
    }

    // Endian
    if (uncC->components_little_endian != lut[i].uncC.components_little_endian) {
      continue;
    }

    if (memcmp (component_types, lut[i].component_types,
            num_components * sizeof (guint16))) {
      continue;
    }

    /* success */
    return lut[i].format;
  }

unsupported_feature:
  GST_WARNING_OBJECT (qtdemux, "Unsupported uncv format");
  return GST_VIDEO_FORMAT_UNKNOWN;
}

static void
qtdemux_set_info_from_uncv (GstQTDemux * qtdemux,
    QtDemuxStreamStsdEntry * entry, UncompressedFrameConfigBox * uncC,
    GstVideoInfo * info)
{
  guint32 num_components = uncC->component_count;
  guint32 row_align_size = uncC->row_align_size;
  gint height = entry->height;

  if (uncC->version == 1) {
    switch (uncC->profile) {
      case GST_MAKE_FOURCC ('r', 'g', 'b', '3'):
        num_components = 3;
        break;
      case GST_MAKE_FOURCC ('r', 'g', 'b', 'a'):
      case GST_MAKE_FOURCC ('a', 'b', 'g', 'r'):
        num_components = 4;
        break;
      default:
        GST_WARNING_OBJECT (qtdemux, "Unsupported uncv profile: %u",
            uncC->profile);
        return;
    }
    info->stride[0] = entry->width * num_components;
    info->size = info->stride[0] * height;
    return;
  }

  gint default_stride = 0;
  if (row_align_size) {
    default_stride = row_align_size;
  } else {
    default_stride = entry->width;
  }

  switch (uncC->sampling_type) {
    case SAMPLING_444:
      if (uncC->interleave_type == INTERLEAVE_PIXEL) {
        if (row_align_size) {
          info->stride[0] = row_align_size;
        } else {
          info->stride[0] = entry->width * num_components;
        }
        info->size = info->stride[0] * height;
      } else {
        for (gint i = 0; i < num_components; i++) {
          info->stride[i] = default_stride;
        }
        info->size = info->stride[0] * height * num_components;
      }
      break;

    case SAMPLING_422:
      info->stride[0] = default_stride;
      switch (uncC->interleave_type) {
        case INTERLEAVE_COMPONENT:
          info->stride[1] = info->stride[0] / 2;
          info->stride[2] = info->stride[1];
          break;
        case INTERLEAVE_MIXED:
          info->stride[1] = info->stride[0];
          break;
        case INTERLEAVE_MULTI_Y:
          // TODO
          break;
        default:
          break;                // Error
      }
      info->size = info->stride[0] * height * 2;
      break;

    case SAMPLING_420:
      info->stride[0] = default_stride;
      switch (uncC->interleave_type) {
        case INTERLEAVE_COMPONENT:
          info->stride[1] = info->stride[0] / 2;
          info->stride[2] = info->stride[1];
          break;
        case INTERLEAVE_MIXED:
          info->stride[1] = info->stride[0];
          break;
        default:
          break;                // Error
      }
      info->size = info->stride[0] * height * 3 / 2;
      break;

    case SAMPLING_411:
      info->stride[0] = default_stride;
      switch (uncC->interleave_type) {
        case INTERLEAVE_COMPONENT:
          info->stride[1] = info->stride[0] / 4;
          info->stride[2] = info->stride[1];
          break;
        case INTERLEAVE_MIXED:
          info->stride[1] = info->stride[0];
          break;
        case INTERLEAVE_MULTI_Y:
          // TODO
        default:
          break;                // Error
      }
      info->size = info->stride[0] * height * 3 / 2;
      break;
    default:
      break;
  }

}

/* *INDENT-OFF* */

// ISO/IEC 23091-3
static const GstAudioChannelPosition chnl_positions[] = {
  // 0
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
  GST_AUDIO_CHANNEL_POSITION_LFE1,
  GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
  GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
  GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
  GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
  // 10
  GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
  GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT,
  GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
  GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT,
  GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
  // 20
  GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT,
  GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER,
  GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT,
  GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
  GST_AUDIO_CHANNEL_POSITION_LFE2,
  GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_CENTER,
  // 30
  GST_AUDIO_CHANNEL_POSITION_TOP_SURROUND_LEFT,
  GST_AUDIO_CHANNEL_POSITION_TOP_SURROUND_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_INVALID, // reserved
  GST_AUDIO_CHANNEL_POSITION_INVALID, // reserved
  GST_AUDIO_CHANNEL_POSITION_INVALID, // reserved
  GST_AUDIO_CHANNEL_POSITION_INVALID, // reserved
  GST_AUDIO_CHANNEL_POSITION_INVALID, // low frequency enhancement 3
  GST_AUDIO_CHANNEL_POSITION_INVALID, // left edge of screen
  GST_AUDIO_CHANNEL_POSITION_INVALID, // right edge of screen
  GST_AUDIO_CHANNEL_POSITION_INVALID, // half-way between centre of screen and
                                      // left edge of screen
  // 40
  GST_AUDIO_CHANNEL_POSITION_INVALID, // half-way between centre of screen and
                                      // right edge of screen
  GST_AUDIO_CHANNEL_POSITION_INVALID, // left back surround
  GST_AUDIO_CHANNEL_POSITION_INVALID, // right back surround
  // 43-125 reserved
  // 126 explicit position
  // 127 unknown / undefined
};

// Pre-defined channel layouts
//
// Each layout is terminated by INVALID to allow counting the number of
// channels in the layout.
static const GstAudioChannelPosition chnl_layouts[][25] = {
 // 0
 { GST_AUDIO_CHANNEL_POSITION_INVALID, },
 // 1
 { GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID, },
 // 2
 { GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_INVALID, },
 // 3
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 4
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 5
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 6
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_LFE1,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 7
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_LFE1,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 8
 { GST_AUDIO_CHANNEL_POSITION_INVALID, },
 // 9
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 10
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 11
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
   GST_AUDIO_CHANNEL_POSITION_LFE1,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 12
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_LFE1,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 13
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT, GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
   GST_AUDIO_CHANNEL_POSITION_LFE1, GST_AUDIO_CHANNEL_POSITION_LFE2,
   GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
   GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER,
   GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },
 // 14
 {
   GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
   GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_LFE1,
   GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
   GST_AUDIO_CHANNEL_POSITION_INVALID,
 },

};
/* *INDENT-ON* */

static void
qtdemux_parse_chnl (GstQTDemux * qtdemux, GstByteReader * br,
    QtDemuxStream * stream, QtDemuxStreamStsdEntry * entry)
{
  GstAudioChannelPosition positions[64];
  guint n_channels = 0;

  guint8 version = gst_byte_reader_get_uint8_unchecked (br);
  guint32 flags = gst_byte_reader_get_uint24_be_unchecked (br);

  if (version == 0 && flags == 0) {
    guint8 stream_structure;
    if (!gst_byte_reader_get_uint8 (br, &stream_structure)) {
      GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
      goto error;
    }

    // stream carries channels
    if (stream_structure & 1) {
      guint8 defined_layout;

      if (!gst_byte_reader_get_uint8 (br, &defined_layout)) {
        GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
        goto error;
      }

      n_channels = entry->n_channels;

      if (defined_layout == 0) {
        for (unsigned int i = 0; i < n_channels; i++) {
          guint8 speaker_position;

          if (!gst_byte_reader_get_uint8 (br, &speaker_position)) {
            GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
            goto error;
          }

          // explicit position
          if (speaker_position == 126) {
            GST_WARNING_OBJECT (qtdemux,
                "Explicit speaker position not supported");
            goto error;
          }

          if (speaker_position >= G_N_ELEMENTS (chnl_positions) ||
              chnl_positions[speaker_position] ==
              GST_AUDIO_CHANNEL_POSITION_INVALID) {
            GST_WARNING_OBJECT (qtdemux,
                "Unsupported speaker channel %u position %u", i,
                speaker_position);
            goto error;
          }

          positions[i] = chnl_positions[speaker_position];
        }
      } else {
        guint64 omitted_channels_map;

        if (!gst_byte_reader_get_uint64_be (br, &omitted_channels_map)) {
          GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
          goto error;
        }

        if (defined_layout >= G_N_ELEMENTS (chnl_layouts) ||
            chnl_layouts[defined_layout][0] ==
            GST_AUDIO_CHANNEL_POSITION_INVALID) {
          GST_WARNING_OBJECT (qtdemux, "Unsupported defined layout %u",
              defined_layout);
          goto error;
        }

        const GstAudioChannelPosition *layout = chnl_layouts[defined_layout];

        // The omitted channel map defines which of the channels of the
        // pre-defined layout are *not* included.
        for (unsigned int c = 0; c < n_channels; c++) {
          // Find c-th channel in layout that is not omitted
          unsigned int l_c = 0;
          for (unsigned int i = 0; i < 64; i++) {
            // If there are not enough non-omitted channels in the layout we end
            // up here and return
            if (layout[i] == GST_AUDIO_CHANNEL_POSITION_INVALID) {
              GST_WARNING_OBJECT (qtdemux,
                  "Invalid defined layout %u with %u channels and omitted channels map %016"
                  G_GINT64_MODIFIER "x", defined_layout, n_channels,
                  omitted_channels_map);
              goto error;
            }

            // The i-th channel of the layout is included
            if (((omitted_channels_map >> i) & 1) == 0) {
              // The channel we're looking for
              if (l_c == c) {
                positions[c] = layout[l_c];
                break;
              }
              l_c += 1;
            }
          }

          // If there are not enough non-omitted channels in the omitted
          // channels map then return here
          if (positions[c] == GST_AUDIO_CHANNEL_POSITION_INVALID) {
            GST_WARNING_OBJECT (qtdemux,
                "Invalid defined layout %u with %u channels and omitted channels map %016"
                G_GINT64_MODIFIER "x", defined_layout, n_channels,
                omitted_channels_map);
            goto error;
          }
        }
      }
    }

    // stream carries objects
    if (stream_structure & 2) {
      guint8 object_count;

      if (!gst_byte_reader_get_uint8 (br, &object_count)) {
        GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
        goto error;
      }

      GST_WARNING_OBJECT (qtdemux, "Stream carries %u objects", object_count);
      goto error;
    }
  } else if (version == 1 && flags == 0) {
    guint8 b;

    if (!gst_byte_reader_get_uint8 (br, &b)) {
      GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
      goto error;
    }

    guint8 stream_structure = b >> 4;
    // guint8 format_ordering = b & 0x0f;

    guint8 base_channel_count;
    if (!gst_byte_reader_get_uint8 (br, &base_channel_count)) {
      GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
      goto error;
    }

    // stream carries channels
    if (stream_structure & 1) {
      guint8 defined_layout;

      if (!gst_byte_reader_get_uint8 (br, &defined_layout)) {
        GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
        goto error;
      }

      if (defined_layout == 0) {
        guint8 layout_channel_count;

        if (!gst_byte_reader_get_uint8 (br, &layout_channel_count)) {
          GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
          goto error;
        }

        if (layout_channel_count == 0) {
          // Not present so configure a default based on the sample entry
          goto error;
        }

        n_channels = layout_channel_count;
        for (unsigned int i = 0; i < layout_channel_count; i++) {
          guint8 speaker_position;

          if (!gst_byte_reader_get_uint8 (br, &speaker_position)) {
            GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
            goto error;
          }

          // explicit position
          if (speaker_position == 126) {
            GST_WARNING_OBJECT (qtdemux,
                "Explicit speaker position not supported");
            goto error;
          }

          if (speaker_position >= G_N_ELEMENTS (chnl_positions) ||
              chnl_positions[speaker_position] ==
              GST_AUDIO_CHANNEL_POSITION_INVALID) {
            GST_WARNING_OBJECT (qtdemux,
                "Unsupported speaker channel %u position %u", i,
                speaker_position);
            goto error;
          }

          positions[i] = chnl_positions[speaker_position];
        }
      } else {
        if (!gst_byte_reader_get_uint8 (br, &b)) {
          GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
          goto error;
        }

        guint8 channel_order_definition = (b >> 1) & 0x07;
        guint8 omitted_channels_present = b & 0x01;

        if (channel_order_definition != 0) {
          GST_WARNING_OBJECT (qtdemux,
              "Channel order definition %u not supported",
              channel_order_definition);
          goto error;
        }

        guint64 omitted_channels_map = 0;
        if (omitted_channels_present) {
          if (!gst_byte_reader_get_uint64_be (br, &omitted_channels_map)) {
            GST_WARNING_OBJECT (qtdemux, "Too short chnl box");
            goto error;
          }
        }

        const GstAudioChannelPosition *layout = chnl_layouts[defined_layout];

        // Calculate number of channels: number of channels in the layout
        // minus number of omitted channels
        n_channels = 0;
        for (unsigned int i = 0; i < G_N_ELEMENTS (chnl_layouts[0]); i++) {
          if (layout[i] == GST_AUDIO_CHANNEL_POSITION_INVALID)
            break;

          n_channels += 1;
        }
        for (unsigned int i = 0; i < 64; i++) {
          if ((omitted_channels_map >> i) == 1) {
            n_channels -= 1;
          }
          // No channels present
          if (n_channels == 0) {
            goto error;
          }
        }

        // The omitted channel map defines which of the channels of the
        // pre-defined layout are *not* included.
        for (unsigned int c = 0; c < n_channels; c++) {
          // Find c-th channel in layout that is not omitted
          unsigned int l_c = 0;
          for (unsigned int i = 0; i < 64; i++) {
            // If there are not enough non-omitted channels in the layout we end
            // up here and return
            if (layout[i] == GST_AUDIO_CHANNEL_POSITION_INVALID) {
              GST_WARNING_OBJECT (qtdemux,
                  "Invalid defined layout %u with %u channels and omitted channels map %016"
                  G_GINT64_MODIFIER "x", defined_layout, n_channels,
                  omitted_channels_map);
              goto error;
            }

            // The i-th channel of the layout is included
            if (((omitted_channels_map >> i) & 1) == 0) {
              // The channel we're looking for
              if (l_c == c) {
                positions[c] = layout[l_c];
                break;
              }
              l_c += 1;
            }
          }

          // If there are not enough non-omitted channels in the omitted
          // channels map then return here
          if (positions[c] == GST_AUDIO_CHANNEL_POSITION_INVALID) {
            GST_WARNING_OBJECT (qtdemux,
                "Invalid defined layout %u with %u channels and omitted channels map %016"
                G_GINT64_MODIFIER "x", defined_layout, n_channels,
                omitted_channels_map);
            goto error;
          }
        }
      }
    }

    // stream carries objects
    if (stream_structure & 2) {
      guint8 object_count = base_channel_count - n_channels;
      GST_WARNING_OBJECT (qtdemux, "Stream carries %u objects", object_count);
      goto error;
    }
  } else {
    GST_WARNING_OBJECT (qtdemux,
        "Unsupported chnl version %u flags %06x", version, flags);

    goto error;
  }

#ifndef GST_DISABLE_GST_DEBUG
  {
    gchar *s = gst_audio_channel_positions_to_string (positions, n_channels);

    GST_DEBUG_OBJECT (qtdemux, "Retrieved channel positions %s", s);

    g_free (s);
  }
#endif

  guint64 channel_mask;
  GstAudioChannelPosition valid_positions[64];

  if (!gst_audio_channel_positions_to_mask (positions, n_channels, FALSE,
          &channel_mask)) {
    GST_WARNING_OBJECT (qtdemux, "Can't convert channel positions to mask");
    goto error;
  }

  memcpy (valid_positions, positions, sizeof (positions[0]) * n_channels);
  if (!gst_audio_channel_positions_to_valid_order (valid_positions, n_channels)) {
    GST_WARNING_OBJECT (qtdemux,
        "Can't convert channel positions to GStreamer channel order");
    goto error;
  }

  if (n_channels > 1) {
    if (!gst_audio_get_channel_reorder_map (n_channels, positions,
            valid_positions, entry->reorder_map)) {
      GST_WARNING_OBJECT (qtdemux, "Can't calculate channel reorder map");
      goto error;
    }
    entry->needs_reorder =
        memcmp (positions, valid_positions,
        sizeof (positions[0]) * n_channels) != 0;
  }

  gst_caps_set_simple (entry->caps, "channel-mask", GST_TYPE_BITMASK,
      channel_mask, NULL);

  // Update based on the actual channel count from this box
  entry->samples_per_frame = n_channels;
  entry->bytes_per_frame = n_channels * entry->bytes_per_sample;
  entry->samples_per_packet = entry->samples_per_frame;
  entry->bytes_per_packet = entry->bytes_per_sample;

  stream->min_buffer_size = 1024 * entry->bytes_per_frame;
  stream->max_buffer_size = entry->rate * entry->bytes_per_frame;
  GST_DEBUG ("setting min/max buffer sizes to %d/%d", stream->min_buffer_size,
      stream->max_buffer_size);

  return;

error:
  {
    GST_WARNING_OBJECT (qtdemux,
        "Configuring default channel mask for %u channels", entry->n_channels);

    if (entry->n_channels > 1) {
      // Set a default channel mask on errors
      guint64 default_mask =
          gst_audio_channel_get_fallback_mask (entry->n_channels);

      gst_caps_set_simple (entry->caps, "channel-mask", GST_TYPE_BITMASK,
          default_mask, NULL);
    }
  }
}

// See CoreAudioTypes.h and ffmpeg's mov_chan.h
typedef enum
{
  AUDIO_CHANNEL_LAYOUT_TAG_USECHANNELDESCRIPTIONS = (0 << 16) | 0,
  AUDIO_CHANNEL_LAYOUT_TAG_USECHANNELBITMAP = (1 << 16) | 0,

  AUDIO_CHANNEL_LAYOUT_TAG_MONO = (100 << 16) | 1,
  AUDIO_CHANNEL_LAYOUT_TAG_STEREO = (101 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_STEREOHEADPHONES = (102 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_MATRIXSTEREO = (103 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_MIDSIDE = (104 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_XY = (105 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_BINAURAL = (106 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_AMBISONIC_B_FORMAT = (107 << 16) | 4,

  AUDIO_CHANNEL_LAYOUT_TAG_QUADRAPHONIC = (108 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_PENTAGONAL = (109 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_HEXAGONAL = (110 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_OCTAGONAL = (111 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_CUBE = (112 << 16) | 8,

  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_3_0_A = (113 << 16) | 3,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_3_0_B = (114 << 16) | 3,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_4_0_A = (115 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_4_0_B = (116 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_A = (117 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_B = (118 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_C = (119 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_D = (120 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_A = (121 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_B = (122 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_C = (123 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_D = (124 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_6_1_A = (125 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_7_1_A = (126 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_7_1_B = (127 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_MPEG_7_1_C = (128 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EMAGIC_DEFAULT_7_1 = (129 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_SMPTE_DTV = (130 << 16) | 8,

  AUDIO_CHANNEL_LAYOUT_TAG_ITU_2_1 = (131 << 16) | 3,
  AUDIO_CHANNEL_LAYOUT_TAG_ITU_2_2 = (132 << 16) | 4,

  AUDIO_CHANNEL_LAYOUT_TAG_DVD_4 = (133 << 16) | 3,
  AUDIO_CHANNEL_LAYOUT_TAG_DVD_5 = (134 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_DVD_6 = (135 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_DVD_10 = (136 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_DVD_11 = (137 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_DVD_18 = (138 << 16) | 5,

  AUDIO_CHANNEL_LAYOUT_TAG_AUDIOUNIT_6_0 = (139 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_AUDIOUNIT_7_0 = (140 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_AUDIOUNIT_7_0_FRONT = (148 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_AAC_6_0 = (141 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_AAC_6_1 = (142 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_AAC_7_0 = (143 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_AAC_7_1_B = (183 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_AAC_OCTAGONAL = (144 << 16) | 8,

  AUDIO_CHANNEL_LAYOUT_TAG_TMH_10_2_STD = (145 << 16) | 16,
  AUDIO_CHANNEL_LAYOUT_TAG_TMH_10_2_FULL = (146 << 16) | 21,

  AUDIO_CHANNEL_LAYOUT_TAG_AC3_1_0_1 = (149 << 16) | 2,
  AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_0 = (150 << 16) | 3,
  AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_1 = (151 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_0_1 = (152 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_AC3_2_1_1 = (153 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_1_1 = (154 << 16) | 5,

  AUDIO_CHANNEL_LAYOUT_TAG_EAC_6_0_A = (155 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC_7_0_A = (156 << 16) | 7,

  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_6_1_A = (157 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_6_1_B = (158 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_6_1_C = (159 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_A = (160 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_B = (161 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_C = (162 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_D = (163 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_E = (164 << 16) | 8,

  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_F = (165 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_G = (166 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_H = (167 << 16) | 8,

  AUDIO_CHANNEL_LAYOUT_TAG_DTS_3_1 = (168 << 16) | 4,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_4_1 = (169 << 16) | 5,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_0_A = (170 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_0_B = (171 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_0_C = (172 << 16) | 6,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_A = (173 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_B = (174 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_C = (175 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_7_0 = (176 << 16) | 7,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_7_1 = (177 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_0_A = (178 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_0_B = (179 << 16) | 8,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_1_A = (180 << 16) | 9,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_1_B = (181 << 16) | 9,
  AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_D = (182 << 16) | 7,

  AUDIO_CHANNEL_LAYOUT_TAG_HOA_ACN_SN3D = (190U << 16) | 0,
  AUDIO_CHANNEL_LAYOUT_TAG_HOA_ACN_N3D = (191U << 16) | 0,

  AUDIO_CHANNEL_LAYOUT_TAG_ATMOS_7_1_4 = (192U << 16) | 12,
  AUDIO_CHANNEL_LAYOUT_TAG_ATMOS_9_1_6 = (193U << 16) | 16,
  AUDIO_CHANNEL_LAYOUT_TAG_ATMOS_5_1_2 = (194U << 16) | 8,

  AUDIO_CHANNEL_LAYOUT_TAG_DISCRETEINORDER = (147 << 16) | 0,
  AUDIO_CHANNEL_LAYOUT_TAG_UNKNOWN = 0xFFFF0000
} AudioChannelLayoutTag;

static const struct
{
  AudioChannelLayoutTag tag;
  const GstAudioChannelPosition *positions;
} chan_layout_map[] = {
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MONO,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_MONO,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_STEREO,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_STEREOHEADPHONES,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MATRIXSTEREO,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MIDSIDE,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_XY,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_BINAURAL,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_AMBISONIC_B_FORMAT
  {
        AUDIO_CHANNEL_LAYOUT_TAG_QUADRAPHONIC,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_PENTAGONAL,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_HEXAGONAL,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_OCTAGONAL,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_CUBE,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_3_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_3_0_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_4_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_4_0_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_0_D,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_5_1_D,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_6_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_7_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_7_1_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_MPEG_7_1_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EMAGIC_DEFAULT_7_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
            },
      },
  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_SMPTE_DTV
  {
        AUDIO_CHANNEL_LAYOUT_TAG_ITU_2_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_ITU_2_2,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DVD_4,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DVD_5,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DVD_6,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DVD_10,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DVD_11,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DVD_18,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AUDIOUNIT_6_0,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AUDIOUNIT_7_0,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AUDIOUNIT_7_0_FRONT,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AAC_6_0,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AAC_6_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AAC_7_0,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AAC_7_1_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AAC_OCTAGONAL,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },

// TODO: AUDIO_CHANNEL_LAYOUT_TAG_TMH_10_2_STD
// TODO: AUDIO_CHANNEL_LAYOUT_TAG_TMH_10_2_FULL
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AC3_1_0_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_0,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_0_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AC3_2_1_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_AC3_3_1_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },

  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC_6_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC_7_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },


  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_6_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },
// TODO: AUDIO_CHANNEL_LAYOUT_TAG_EAC3_6_1_B,
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_6_1_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_D,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_E,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_F,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_G,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_EAC3_7_1_H,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
              GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_3_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_4_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_0_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_0_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_C,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_7_0,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_7_1,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },

  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_0_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_0_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
            },
      },

  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_1_A,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },
  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_8_1_B,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
            },
      },

  {
        AUDIO_CHANNEL_LAYOUT_TAG_DTS_6_1_D,
        (const GstAudioChannelPosition[]) {
              GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
              GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
              GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
              GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
              GST_AUDIO_CHANNEL_POSITION_LFE1,
              GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
            },
      },

  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_HOA_ACN_SN3D
  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_HOA_ACN_N3D
  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_ATMOS_7_1_4
  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_ATMOS_9_1_6
  // TODO: AUDIO_CHANNEL_LAYOUT_TAG_ATMOS_5_1_2
};

// Mapping bit N to GstAudioChannelPosition
static const GstAudioChannelPosition audio_channel_bitmap_mapping[32] = {
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,        // 0
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
  GST_AUDIO_CHANNEL_POSITION_LFE1,
  GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
  GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
  GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
  GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT,
  GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT,    // 10
  GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
  GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
  GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT,
  GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER,
  GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT,    // 17
  GST_AUDIO_CHANNEL_POSITION_INVALID,
  GST_AUDIO_CHANNEL_POSITION_INVALID,
  GST_AUDIO_CHANNEL_POSITION_INVALID,   // 20
  GST_AUDIO_CHANNEL_POSITION_INVALID,   // LeftTopMiddle
  GST_AUDIO_CHANNEL_POSITION_INVALID,
  GST_AUDIO_CHANNEL_POSITION_INVALID,   // RightTopMiddle
  GST_AUDIO_CHANNEL_POSITION_INVALID,   // LeftTopRear
  GST_AUDIO_CHANNEL_POSITION_INVALID,   // CenterTopRear
  GST_AUDIO_CHANNEL_POSITION_INVALID,   // RightTopRear
};

typedef enum
{
  AUDIO_CHANNEL_LABEL_UNKNOWN = 0xFFFFFFFF,
  AUDIO_CHANNEL_LABEL_UNUSED = 0,
  AUDIO_CHANNEL_LABEL_USE_COORDINATES = 100,

  AUDIO_CHANNEL_LABEL_LEFT = 1,
  AUDIO_CHANNEL_LABEL_RIGHT = 2,
  AUDIO_CHANNEL_LABEL_CENTER = 3,
  AUDIO_CHANNEL_LABEL_LFE_SCREEN = 4,
  AUDIO_CHANNEL_LABEL_LEFT_SURROUND = 5,
  AUDIO_CHANNEL_LABEL_RIGHT_SURROUND = 6,
  AUDIO_CHANNEL_LABEL_LEFT_CENTER = 7,
  AUDIO_CHANNEL_LABEL_RIGHT_CENTER = 8,
  AUDIO_CHANNEL_LABEL_CENTER_SURROUND = 9,
  AUDIO_CHANNEL_LABEL_LEFT_SURROUND_DIRECT = 10,
  AUDIO_CHANNEL_LABEL_RIGHT_SURROUND_DIRECT = 11,
  AUDIO_CHANNEL_LABEL_TOP_CENTER_SURROUND = 12,
  AUDIO_CHANNEL_LABEL_VERTICAL_HEIGHT_LEFT = 13,
  AUDIO_CHANNEL_LABEL_VERTICAL_HEIGHT_CENTER = 14,
  AUDIO_CHANNEL_LABEL_VERTICAL_HEIGHT_RIGHT = 15,

  AUDIO_CHANNEL_LABEL_TOP_BACK_LEFT = 16,
  AUDIO_CHANNEL_LABEL_TOP_BACK_CENTER = 17,
  AUDIO_CHANNEL_LABEL_TOP_BACK_RIGHT = 18,

  AUDIO_CHANNEL_LABEL_REAR_SURROUND_LEFT = 33,
  AUDIO_CHANNEL_LABEL_REAR_SURROUND_RIGHT = 34,
  AUDIO_CHANNEL_LABEL_LEFT_WIDE = 35,
  AUDIO_CHANNEL_LABEL_RIGHT_WIDE = 36,
  AUDIO_CHANNEL_LABEL_LFE2 = 37,
  AUDIO_CHANNEL_LABEL_LEFT_TOTAL = 38,
  AUDIO_CHANNEL_LABEL_RIGHT_TOTAL = 39,
  AUDIO_CHANNEL_LABEL_HEARING_IMPAIRED = 40,
  AUDIO_CHANNEL_LABEL_NARRATION = 41,
  AUDIO_CHANNEL_LABEL_MONO = 42,
  AUDIO_CHANNEL_LABEL_DIALOG_CENTRIC_MIX = 43,

  AUDIO_CHANNEL_LABEL_CENTER_SURROUND_DIRECT = 44,

  AUDIO_CHANNEL_LABEL_HAPTIC = 45,

  AUDIO_CHANNEL_LABEL_LEFT_TOP_MIDDLE = 49,
  AUDIO_CHANNEL_LABEL_RIGHT_TOP_MIDDLE = 51,
  AUDIO_CHANNEL_LABEL_LEFT_TOP_REAR = 52,
  AUDIO_CHANNEL_LABEL_CENTER_TOP_REAR = 53,
  AUDIO_CHANNEL_LABEL_RIGHT_TOP_REAR = 54,

  AUDIO_CHANNEL_LABEL_AMBISONIC_W = 200,
  AUDIO_CHANNEL_LABEL_AMBISONIC_X = 201,
  AUDIO_CHANNEL_LABEL_AMBISONIC_Y = 202,
  AUDIO_CHANNEL_LABEL_AMBISONIC_Z = 203,

  AUDIO_CHANNEL_LABEL_MS_MID = 204,
  AUDIO_CHANNEL_LABEL_MS_SIDE = 205,

  AUDIO_CHANNEL_LABEL_XY_X = 206,
  AUDIO_CHANNEL_LABEL_XY_Y = 207,

  AUDIO_CHANNEL_LABEL_BINAURAL_LEFT = 208,
  AUDIO_CHANNEL_LABEL_BINAURAL_RIGHT = 209,

  AUDIO_CHANNEL_LABEL_HEADPHONES_LEFT = 301,
  AUDIO_CHANNEL_LABEL_HEADPHONES_RIGHT = 302,
  AUDIO_CHANNEL_LABEL_CLICK_TRACK = 304,
  AUDIO_CHANNEL_LABEL_FOREIGN_LANGUAGE = 305,
} AudioChannelLabel;

static const struct
{
  AudioChannelLabel label;
  GstAudioChannelPosition position;
} audio_channel_label_mapping[] = {
  {
        AUDIO_CHANNEL_LABEL_MONO,
      GST_AUDIO_CHANNEL_POSITION_MONO},
  {
        AUDIO_CHANNEL_LABEL_LEFT,
      GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT},
  {
        AUDIO_CHANNEL_LABEL_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
      },
  {
        AUDIO_CHANNEL_LABEL_LFE_SCREEN,
        GST_AUDIO_CHANNEL_POSITION_LFE1,
      },
  {
        AUDIO_CHANNEL_LABEL_LEFT_SURROUND,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_RIGHT_SURROUND,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_LEFT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
      },
  {
        AUDIO_CHANNEL_LABEL_RIGHT_CENTER,
      GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER},
  {
        AUDIO_CHANNEL_LABEL_CENTER_SURROUND,
        GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
      },
  {
        AUDIO_CHANNEL_LABEL_LEFT_SURROUND_DIRECT,
        GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_RIGHT_SURROUND_DIRECT,
        GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_TOP_CENTER_SURROUND,
        GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
      },
  {
        AUDIO_CHANNEL_LABEL_VERTICAL_HEIGHT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_VERTICAL_HEIGHT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
      },
  {
        AUDIO_CHANNEL_LABEL_VERTICAL_HEIGHT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_TOP_BACK_LEFT,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_TOP_BACK_CENTER,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER,
      },
  {
        AUDIO_CHANNEL_LABEL_TOP_BACK_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_REAR_SURROUND_LEFT,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_REAR_SURROUND_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_LEFT_WIDE,
        GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_RIGHT_WIDE,
        GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_LFE2,
        GST_AUDIO_CHANNEL_POSITION_LFE2,
      },
  {
        AUDIO_CHANNEL_LABEL_BINAURAL_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_BINAURAL_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
      },
  {
        AUDIO_CHANNEL_LABEL_HEADPHONES_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
      },
  {
        AUDIO_CHANNEL_LABEL_HEADPHONES_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
      }
};

static void
qtdemux_parse_chan (GstQTDemux * qtdemux, GstByteReader * br,
    QtDemuxStream * stream, QtDemuxStreamStsdEntry * entry)
{
  GstAudioChannelPosition positions[64];
  guint n_channels = 0;

  // Skip over version and flags
  gst_byte_reader_skip_unchecked (br, 4);

  if (gst_byte_reader_get_remaining (br) < 12)
    return;

  guint32 layout_tag = gst_byte_reader_get_uint32_be_unchecked (br);
  guint32 bitmap = gst_byte_reader_get_uint32_be_unchecked (br);
  guint32 num_channel_descs = gst_byte_reader_get_uint32_be_unchecked (br);

  if (gst_byte_reader_get_remaining (br) < num_channel_descs * 5 * 4)
    return;

  if (layout_tag == AUDIO_CHANNEL_LAYOUT_TAG_USECHANNELBITMAP) {
    // Invalid/unsupported positions are mapped to defaults later
    for (gsize i = 0; i < G_N_ELEMENTS (audio_channel_bitmap_mapping); i++) {
      if (bitmap & (1 << i)) {
        positions[n_channels] = audio_channel_bitmap_mapping[i];
        n_channels += 1;
      }
    }
  } else if (layout_tag == AUDIO_CHANNEL_LAYOUT_TAG_USECHANNELDESCRIPTIONS) {
    if (num_channel_descs < 64) {
      n_channels = num_channel_descs;

      for (guint32 i = 0; i < num_channel_descs; i++) {
        positions[i] = GST_AUDIO_CHANNEL_POSITION_INVALID;

        guint32 label = gst_byte_reader_get_uint32_be_unchecked (br);

        // Discrete channel
        if (label == 400 || (label >> 16) == 1) {
          positions[i] = GST_AUDIO_CHANNEL_POSITION_NONE;
        } else {
          // If a label is not found the channel stays invalid and is
          // handled as an error later with mapping to the defaults
          for (gsize j = 0; j < G_N_ELEMENTS (audio_channel_label_mapping); j++) {
            if (audio_channel_label_mapping[j].label == label) {
              positions[i] = audio_channel_label_mapping[j].position;
              break;
            }
          }
        }

        // Skip coordinates and flags
        gst_byte_reader_skip_unchecked (br, 4 * 4);
      }
    }
  } else if (layout_tag == AUDIO_CHANNEL_LAYOUT_TAG_DISCRETEINORDER) {
    // Unordered
    n_channels = entry->n_channels;
    for (gsize i = 0; i < n_channels; i++) {
      positions[i] = GST_AUDIO_CHANNEL_POSITION_NONE;
    }
  } else if (layout_tag & 0xffff) {
    for (gsize i = 0; i < G_N_ELEMENTS (chan_layout_map); i++) {
      if (chan_layout_map[i].tag == layout_tag) {
        n_channels = layout_tag & 0xffff;
        memcpy (positions, chan_layout_map[i].positions,
            n_channels * sizeof (GstAudioChannelPosition));
        break;
      }
    }
  }

  if (n_channels == 0) {
    GST_WARNING_OBJECT (qtdemux, "Unsupported channel layout tag 0x%08x",
        layout_tag);
    return;
  }

#ifndef GST_DISABLE_GST_DEBUG
  {
    gchar *s = gst_audio_channel_positions_to_string (positions, n_channels);

    GST_DEBUG_OBJECT (qtdemux, "Retrieved channel positions %s", s);

    g_free (s);
  }
#endif

  guint64 channel_mask;
  GstAudioChannelPosition valid_positions[64];

  if (!gst_audio_channel_positions_to_mask (positions, n_channels, FALSE,
          &channel_mask)) {
    GST_WARNING_OBJECT (qtdemux, "Can't convert channel positions to mask");
    goto error;
  }

  memcpy (valid_positions, positions, sizeof (positions[0]) * n_channels);
  if (!gst_audio_channel_positions_to_valid_order (valid_positions, n_channels)) {
    GST_WARNING_OBJECT (qtdemux,
        "Can't convert channel positions to GStreamer channel order");
    goto error;
  }

  if (n_channels > 1) {
    if (!gst_audio_get_channel_reorder_map (n_channels, positions,
            valid_positions, entry->reorder_map)) {
      GST_WARNING_OBJECT (qtdemux, "Can't calculate channel reorder map");
      goto error;
    }
    entry->needs_reorder =
        memcmp (positions, valid_positions,
        sizeof (positions[0]) * n_channels) != 0;
  }

  gst_caps_set_simple (entry->caps, "channel-mask", GST_TYPE_BITMASK,
      channel_mask, NULL);

  // Update based on the actual channel count from this box
  entry->samples_per_frame = n_channels;
  entry->bytes_per_frame = n_channels * entry->bytes_per_sample;
  entry->samples_per_packet = entry->samples_per_frame;
  entry->bytes_per_packet = entry->bytes_per_sample;

  stream->min_buffer_size = 1024 * entry->bytes_per_frame;
  stream->max_buffer_size = entry->rate * entry->bytes_per_frame;
  GST_DEBUG ("setting min/max buffer sizes to %d/%d", stream->min_buffer_size,
      stream->max_buffer_size);

  return;

error:
  {
    GST_WARNING_OBJECT (qtdemux,
        "Configuring default channel mask for %u channels", entry->n_channels);

    if (entry->n_channels > 1) {
      // Set a default channel mask on errors
      guint64 default_mask =
          gst_audio_channel_get_fallback_mask (entry->n_channels);

      gst_caps_set_simple (entry->caps, "channel-mask", GST_TYPE_BITMASK,
          default_mask, NULL);
    }
  }
}

/* parse the traks.
 * With each track we associate a new QtDemuxStream that contains all the info
 * about the trak.
 * traks that do not decode to something (like strm traks) will not have a pad.
 */
static gboolean
qtdemux_parse_trak (GstQTDemux * qtdemux, GNode * trak, guint32 * mvhd_matrix)
{
  GstByteReader tkhd;
  int offset;
  GNode *mdia;
  GNode *mdhd;
  GNode *hdlr;
  GNode *minf;
  GNode *stbl;
  GNode *stsd;
  GNode *esds;
  GNode *tref;
  GNode *udta;

  QtDemuxStream *stream = NULL;
  const guint8 *stsd_data;
  guint8 stsd_version;
  guint stsd_entry_count;
  guint stsd_index;
  guint16 lang_code;            /* quicktime lang code or packed iso code */
  guint32 version;
  guint32 tkhd_flags = 0;
  guint8 tkhd_version = 0;
  guint32 w = 0, h = 0;
  guint value_size, stsd_len, len;
  guint32 track_id;
  guint32 dummy;

  GST_DEBUG_OBJECT (qtdemux, "parse_trak");

  if (!qtdemux_tree_get_child_by_type_full (trak, FOURCC_tkhd, &tkhd)
      || !gst_byte_reader_get_uint8 (&tkhd, &tkhd_version)
      || !gst_byte_reader_get_uint24_be (&tkhd, &tkhd_flags))
    goto corrupt_file;

  /* pick between 64 or 32 bits */
  value_size = tkhd_version == 1 ? 8 : 4;
  if (!gst_byte_reader_skip (&tkhd, value_size * 2) ||
      !gst_byte_reader_get_uint32_be (&tkhd, &track_id))
    goto corrupt_file;

  /* Check if current moov has duplicated track_id */
  if (qtdemux_find_stream (qtdemux, track_id))
    goto existing_stream;

  stream = _create_stream (qtdemux, track_id);
  stream->stream_tags = gst_tag_list_make_writable (stream->stream_tags);

  /* need defaults for fragments */
  qtdemux_parse_trex (qtdemux, stream, &dummy, &dummy, &dummy);

  if ((tkhd_flags & 1) == 0)
    stream->disabled = TRUE;

  GST_LOG_OBJECT (qtdemux, "track[tkhd] version/flags/id: 0x%02x/%06x/%u",
      tkhd_version, tkhd_flags, stream->track_id);

  if (!(mdia = qtdemux_tree_get_child_by_type (trak, FOURCC_mdia)))
    goto corrupt_file;

  if (!(mdhd = qtdemux_tree_get_child_by_type (mdia, FOURCC_mdhd))) {
    /* be nice for some crooked mjp2 files that use mhdr for mdhd */
    if (qtdemux->major_brand != FOURCC_mjp2 ||
        !(mdhd = qtdemux_tree_get_child_by_type (mdia, FOURCC_mhdr)))
      goto corrupt_file;
  }

  len = QT_UINT32 ((guint8 *) mdhd->data);
  version = QT_UINT32 ((guint8 *) mdhd->data + 8);
  GST_LOG_OBJECT (qtdemux, "track version/flags: %08x", version);
  if (version == 0x01000000) {
    if (len < 42)
      goto corrupt_file;
    stream->timescale = QT_UINT32 ((guint8 *) mdhd->data + 28);
    stream->duration = QT_UINT64 ((guint8 *) mdhd->data + 32);
    lang_code = QT_UINT16 ((guint8 *) mdhd->data + 40);
  } else {
    if (len < 30)
      goto corrupt_file;
    stream->timescale = QT_UINT32 ((guint8 *) mdhd->data + 20);
    stream->duration = QT_UINT32 ((guint8 *) mdhd->data + 24);
    lang_code = QT_UINT16 ((guint8 *) mdhd->data + 28);
  }

  if (lang_code < 0x400) {
    qtdemux_lang_map_qt_code_to_iso (stream->lang_id, lang_code);
  } else if (lang_code == 0x7fff) {
    stream->lang_id[0] = 0;     /* unspecified */
  } else {
    stream->lang_id[0] = 0x60 + ((lang_code >> 10) & 0x1F);
    stream->lang_id[1] = 0x60 + ((lang_code >> 5) & 0x1F);
    stream->lang_id[2] = 0x60 + (lang_code & 0x1F);
    stream->lang_id[3] = 0;
  }

  GST_LOG_OBJECT (qtdemux, "track timescale: %" G_GUINT32_FORMAT,
      stream->timescale);
  GST_LOG_OBJECT (qtdemux, "track duration: %" G_GUINT64_FORMAT,
      stream->duration);
  GST_LOG_OBJECT (qtdemux, "track language code/id: 0x%04x/%s",
      lang_code, stream->lang_id);

  if (G_UNLIKELY (stream->timescale == 0 || qtdemux->timescale == 0))
    goto corrupt_file;

  if ((tref = qtdemux_tree_get_child_by_type (trak, FOURCC_tref))) {
    /* chapters track reference */
    GNode *chap = qtdemux_tree_get_child_by_type (tref, FOURCC_chap);
    if (chap) {
      gsize length = GST_READ_UINT32_BE (chap->data);
      if (qtdemux->chapters_track_id)
        GST_FIXME_OBJECT (qtdemux, "Multiple CHAP tracks");

      if (length >= 12) {
        qtdemux->chapters_track_id =
            GST_READ_UINT32_BE ((gint8 *) chap->data + 8);
      }
    }
  }

  /* fragmented files may have bogus duration in moov */
  if (!qtdemux->fragmented &&
      qtdemux->duration != G_MAXINT64 && stream->duration != G_MAXINT32) {
    guint64 tdur1, tdur2;

    /* don't overflow */
    tdur1 = stream->timescale * (guint64) qtdemux->duration;
    tdur2 = qtdemux->timescale * (guint64) stream->duration;

    /* HACK:
     * some of those trailers, nowadays, have prologue images that are
     * themselves video tracks as well. I haven't really found a way to
     * identify those yet, except for just looking at their duration. */
    if (tdur1 != 0 && (tdur2 * 10 / tdur1) < 2) {
      GST_WARNING_OBJECT (qtdemux,
          "Track shorter than 20%% (%" G_GUINT64_FORMAT "/%" G_GUINT32_FORMAT
          " vs. %" G_GUINT64_FORMAT "/%" G_GUINT32_FORMAT ") of the stream "
          "found, assuming preview image or something; skipping track",
          stream->duration, stream->timescale, qtdemux->duration,
          qtdemux->timescale);
      gst_qtdemux_stream_unref (stream);
      return TRUE;
    }
  }

  if (!(hdlr = qtdemux_tree_get_child_by_type (mdia, FOURCC_hdlr)))
    goto corrupt_file;

  GST_LOG_OBJECT (qtdemux, "track type: %" GST_FOURCC_FORMAT,
      GST_FOURCC_ARGS (QT_FOURCC ((guint8 *) hdlr->data + 12)));

  len = QT_UINT32 ((guint8 *) hdlr->data);
  if (len >= 20)
    stream->subtype = QT_FOURCC ((guint8 *) hdlr->data + 16);
  GST_LOG_OBJECT (qtdemux, "track subtype: %" GST_FOURCC_FORMAT,
      GST_FOURCC_ARGS (stream->subtype));

  if (!(minf = qtdemux_tree_get_child_by_type (mdia, FOURCC_minf)))
    goto corrupt_file;

  if (!(stbl = qtdemux_tree_get_child_by_type (minf, FOURCC_stbl)))
    goto corrupt_file;

  /* Parse out svmi (and later st3d/sv3d) atoms */
  if (!qtdemux_parse_stereo_svmi_atom (qtdemux, stream, stbl))
    goto corrupt_file;

  /* parse rest of tkhd */
  if (stream->subtype == FOURCC_vide) {
    guint32 tkhd_matrix[9];
    guint32 matrix[9];

    /* version 1 uses some 64-bit ints */
    if (!gst_byte_reader_skip (&tkhd, 20 + value_size))
      goto corrupt_file;

    if (!qtdemux_parse_transformation_matrix (qtdemux, &tkhd, tkhd_matrix,
            "tkhd"))
      goto corrupt_file;

    /* calculate the final matrix from the mvhd_matrix and the tkhd matrix */
    qtdemux_mul_transformation_matrix (qtdemux, mvhd_matrix, tkhd_matrix,
        matrix);

    if (!gst_byte_reader_get_uint32_be (&tkhd, &w)
        || !gst_byte_reader_get_uint32_be (&tkhd, &h))
      goto corrupt_file;

    qtdemux_inspect_transformation_matrix (qtdemux, stream, matrix,
        &stream->stream_tags);
  }

  /* parse stsd */
  if (!(stsd = qtdemux_tree_get_child_by_type (stbl, FOURCC_stsd)))
    goto corrupt_file;
  stsd_data = (const guint8 *) stsd->data;

  /* stsd should at least have one entry */
  stsd_len = QT_UINT32 (stsd_data);
  if (stsd_len < 24) {
    /* .. but skip stream with empty stsd produced by some Vivotek cameras */
    if (stream->subtype == FOURCC_vivo) {
      gst_qtdemux_stream_unref (stream);
      return TRUE;
    } else {
      goto corrupt_file;
    }
  }

  stsd_version = QT_UINT8 (stsd_data + 8);
  stream->stsd_entries_length = stsd_entry_count = QT_UINT32 (stsd_data + 12);
  /* each stsd entry must contain at least 8 bytes */
  if (stream->stsd_entries_length == 0
      || stream->stsd_entries_length > stsd_len / 8) {
    stream->stsd_entries_length = 0;
    goto corrupt_file;
  }
  stream->stsd_entries = g_new0 (QtDemuxStreamStsdEntry, stsd_entry_count);
  GST_LOG_OBJECT (qtdemux, "stsd len:           %d", stsd_len);
  GST_LOG_OBJECT (qtdemux, "stsd entry count:   %u", stsd_entry_count);

  for (stsd_index = 0; stsd_index < stsd_entry_count; stsd_index++) {
    GNode *stsd_entry;
    const guint8 *stsd_entry_data;
    guint32 fourcc;
    gchar *codec = NULL;
    QtDemuxStreamStsdEntry *entry = &stream->stsd_entries[stsd_index];

    stsd_entry = qtdemux_tree_get_child_by_index (stsd, stsd_index);
    if (!stsd_entry)
      goto corrupt_file;

    stsd_entry_data = stsd_entry->data;

    len = QT_UINT32 (stsd_entry_data);

    entry->fourcc = fourcc = QT_FOURCC (stsd_entry_data + 4);
    GST_LOG_OBJECT (qtdemux, "stsd type:          %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (entry->fourcc));
    GST_LOG_OBJECT (qtdemux, "stsd type len:      %d", len);

    if ((fourcc == FOURCC_drms) || (fourcc == FOURCC_drmi))
      goto error_encrypted;

    if (fourcc == FOURCC_aavd) {
      if (stream->subtype != FOURCC_soun) {
        GST_ERROR_OBJECT (qtdemux,
            "Unexpeced stsd type 'aavd' outside 'soun' track");
        goto corrupt_file;
      } else {
        /* encrypted audio with sound sample description v0 */
        GNode *enc = qtdemux_tree_get_child_by_type (stsd, fourcc);
        stream->protected = TRUE;
        if (!qtdemux_parse_protection_aavd (qtdemux, stream, enc, &fourcc)) {
          GST_ERROR_OBJECT (qtdemux, "Failed to parse protection scheme info");
          goto corrupt_file;
        }
      }
    }

    if (fourcc == FOURCC_encv || fourcc == FOURCC_enca) {
      stream->protected = TRUE;
      if (!qtdemux_parse_protection_scheme_info (qtdemux, stream, stsd_entry,
              &fourcc)) {
        GST_ERROR_OBJECT (qtdemux, "Failed to parse protection scheme info");
        goto corrupt_file;
      }
    }

    if (stream->subtype == FOURCC_vide) {
      GNode *colr;
      GNode *fiel;
      GNode *pasp;
      GNode *btrt;
      GNode *clli;
      GNode *mdcv;
      guint32 version;
      gboolean gray;
      gint depth, palette_size, palette_count;
      guint32 *palette_data = NULL;

      entry->sampled = TRUE;

      stream->display_width = w >> 16;
      stream->display_height = h >> 16;

      offset = 16;
      /* sample description entry (16) + visual sample description (70) */
      if (len < 86)
        goto corrupt_file;

      version = QT_UINT32 (stsd_entry_data + offset);
      entry->width = QT_UINT16 (stsd_entry_data + offset + 16);
      entry->height = QT_UINT16 (stsd_entry_data + offset + 18);
      entry->fps_n = 0;         /* this is filled in later */
      entry->fps_d = 0;         /* this is filled in later */
      entry->bits_per_sample = QT_UINT16 (stsd_entry_data + offset + 66);
      entry->color_table_id = QT_UINT16 (stsd_entry_data + offset + 68);

      /* if color_table_id is 0, ctab atom must follow; however some files
       * produced by TMPEGEnc have color_table_id = 0 and no ctab atom, so
       * if color table is not present we'll correct the value */
      if (entry->color_table_id == 0 &&
          (len < 90
              || QT_FOURCC (stsd_entry_data + offset + 70) != FOURCC_ctab)) {
        entry->color_table_id = -1;
      }

      GST_LOG_OBJECT (qtdemux, "width %d, height %d, bps %d, color table id %d",
          entry->width, entry->height, entry->bits_per_sample,
          entry->color_table_id);

      depth = entry->bits_per_sample;

      /* more than 32 bits means grayscale */
      gray = (depth > 32);
      /* low 32 bits specify the depth  */
      depth &= 0x1F;

      /* different number of palette entries is determined by depth. */
      palette_count = 0;
      if ((depth == 1) || (depth == 2) || (depth == 4) || (depth == 8))
        palette_count = (1 << depth);
      palette_size = palette_count * 4;

      if (entry->color_table_id) {
        switch (palette_count) {
          case 0:
            break;
          case 2:
            palette_data = g_memdup2 (ff_qt_default_palette_2, palette_size);
            break;
          case 4:
            palette_data = g_memdup2 (ff_qt_default_palette_4, palette_size);
            break;
          case 16:
            if (gray)
              palette_data =
                  g_memdup2 (ff_qt_grayscale_palette_16, palette_size);
            else
              palette_data = g_memdup2 (ff_qt_default_palette_16, palette_size);
            break;
          case 256:
            if (gray)
              palette_data =
                  g_memdup2 (ff_qt_grayscale_palette_256, palette_size);
            else
              palette_data =
                  g_memdup2 (ff_qt_default_palette_256, palette_size);
            break;
          default:
            GST_ELEMENT_WARNING (qtdemux, STREAM, DEMUX,
                (_("The video in this file might not play correctly.")),
                ("unsupported palette depth %d", depth));
            break;
        }
      } else {
        guint i, j, start, end;

        if (len < 94)
          goto corrupt_file;

        /* read table */
        start = QT_UINT32 (stsd_entry_data + offset + 70);
        palette_count = QT_UINT16 (stsd_entry_data + offset + 74);
        end = QT_UINT16 (stsd_entry_data + offset + 76);

        GST_LOG_OBJECT (qtdemux, "start %d, end %d, palette_count %d",
            start, end, palette_count);

        if (end > 255)
          end = 255;
        if (start > end)
          start = end;

        if (len < 94 + (end - start) * 8)
          goto corrupt_file;

        /* palette is always the same size */
        palette_data = g_malloc0 (256 * 4);
        palette_size = 256 * 4;

        for (j = 0, i = start; i <= end; j++, i++) {
          guint32 a, r, g, b;

          a = QT_UINT16 (stsd_entry_data + offset + 78 + (j * 8));
          r = QT_UINT16 (stsd_entry_data + offset + 80 + (j * 8));
          g = QT_UINT16 (stsd_entry_data + offset + 82 + (j * 8));
          b = QT_UINT16 (stsd_entry_data + offset + 84 + (j * 8));

          palette_data[i] = ((a & 0xff00) << 16) | ((r & 0xff00) << 8) |
              (g & 0xff00) | (b >> 8);
        }
      }

      if (entry->caps)
        gst_caps_unref (entry->caps);

      entry->caps =
          qtdemux_video_caps (qtdemux, stream, entry, fourcc, stsd_entry,
          &codec);
      if (G_UNLIKELY (!entry->caps)) {
        g_free (palette_data);
        goto unknown_stream;
      }

      if (codec) {
        gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
            GST_TAG_VIDEO_CODEC, codec, NULL);
        g_free (codec);
        codec = NULL;
      }

      if (palette_data) {
        GstStructure *s;

        if (entry->rgb8_palette)
          gst_memory_unref (entry->rgb8_palette);
        entry->rgb8_palette = gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
            palette_data, palette_size, 0, palette_size, palette_data, g_free);

        s = gst_caps_get_structure (entry->caps, 0);

        /* non-raw video has a palette_data property. raw video has the palette as
         * an extra plane that we append to the output buffers before we push
         * them*/
        if (!gst_structure_has_name (s, "video/x-raw")) {
          GstBuffer *palette;

          palette = gst_buffer_new ();
          gst_buffer_append_memory (palette, entry->rgb8_palette);
          entry->rgb8_palette = NULL;

          gst_caps_set_simple (entry->caps, "palette_data",
              GST_TYPE_BUFFER, palette, NULL);
          gst_buffer_unref (palette);
        }
      } else if (palette_count != 0) {
        GST_ELEMENT_WARNING (qtdemux, STREAM, NOT_IMPLEMENTED,
            (NULL), ("Unsupported palette depth %d", depth));
      }

      GST_LOG_OBJECT (qtdemux, "frame count:   %u",
          QT_UINT16 (stsd_entry_data + offset + 32));

      esds = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_esds);
      pasp = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_pasp);
      colr = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_colr);
      fiel = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_fiel);
      btrt = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_btrt);
      clli = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_clli);
      mdcv = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_mdcv);

      if (pasp) {
        const guint8 *pasp_data = (const guint8 *) pasp->data;
        guint len = QT_UINT32 (pasp_data);

        if (len == 16) {
          CUR_STREAM (stream)->par_w = QT_UINT32 (pasp_data + 8);
          CUR_STREAM (stream)->par_h = QT_UINT32 (pasp_data + 12);
        } else {
          CUR_STREAM (stream)->par_w = 0;
          CUR_STREAM (stream)->par_h = 0;
        }
      } else {
        CUR_STREAM (stream)->par_w = 0;
        CUR_STREAM (stream)->par_h = 0;
      }

      if (fiel) {
        const guint8 *fiel_data = (const guint8 *) fiel->data;
        guint len = QT_UINT32 (fiel_data);

        if (len == 10) {
          CUR_STREAM (stream)->interlace_mode = GST_READ_UINT8 (fiel_data + 8);
          CUR_STREAM (stream)->field_order = GST_READ_UINT8 (fiel_data + 9);
        }
      }

      if (colr) {
        const guint8 *colr_data = (const guint8 *) colr->data;
        guint len = QT_UINT32 (colr_data);

        if (len == 19 || len == 18) {
          guint32 color_type = GST_READ_UINT32_LE (colr_data + 8);

          if (color_type == FOURCC_nclx || color_type == FOURCC_nclc) {
            guint16 primaries = GST_READ_UINT16_BE (colr_data + 12);
            guint16 transfer_function = GST_READ_UINT16_BE (colr_data + 14);
            guint16 matrix = GST_READ_UINT16_BE (colr_data + 16);
            gboolean full_range = len == 19 ? colr_data[18] >> 7 : FALSE;

            CUR_STREAM (stream)->colorimetry.primaries =
                gst_video_color_primaries_from_iso (primaries);
            CUR_STREAM (stream)->colorimetry.transfer =
                gst_video_transfer_function_from_iso (transfer_function);
            CUR_STREAM (stream)->colorimetry.matrix =
                gst_video_color_matrix_from_iso (matrix);
            CUR_STREAM (stream)->colorimetry.range =
                full_range ? GST_VIDEO_COLOR_RANGE_0_255 :
                GST_VIDEO_COLOR_RANGE_16_235;
          } else {
            GST_DEBUG_OBJECT (qtdemux, "Unsupported color type");
          }
        } else {
          GST_WARNING_OBJECT (qtdemux, "Invalid colr atom size");
        }
      }

      if (clli) {
        const guint8 *clli_data = clli->data;
        guint32 len = QT_UINT32 (clli_data);

        if (len >= 8 + 2 * 2) {
          CUR_STREAM (stream)->content_light_level_set = TRUE;
          CUR_STREAM (stream)->content_light_level.max_content_light_level =
              QT_UINT16 (clli_data + 8);
          CUR_STREAM (stream)->
              content_light_level.max_frame_average_light_level =
              QT_UINT16 (clli_data + 10);
        }
      }

      if (mdcv) {
        const guint8 *mdcv_data = mdcv->data;
        guint32 len = QT_UINT32 (mdcv_data);

        if (len >= 8 + 3 * 2 * 2 + 2 * 2 + 2 * 4) {
          CUR_STREAM (stream)->mastering_display_info_set = TRUE;
          for (gsize c = 0; c < 3; c++) {
            CUR_STREAM (stream)->mastering_display_info.display_primaries[c].x =
                QT_UINT16 (mdcv_data + 8 + c * 2 * 2);
            CUR_STREAM (stream)->mastering_display_info.display_primaries[c].y =
                QT_UINT16 (mdcv_data + 8 + c * 2 * 2 + 2);
          }
          CUR_STREAM (stream)->mastering_display_info.white_point.x =
              QT_UINT16 (mdcv_data + 8 + 3 * 2 * 2);
          CUR_STREAM (stream)->mastering_display_info.white_point.y =
              QT_UINT16 (mdcv_data + 8 + 3 * 2 * 2 + 2);
          CUR_STREAM (stream)->
              mastering_display_info.max_display_mastering_luminance =
              QT_UINT16 (mdcv_data + 8 + 3 * 2 * 2 + 2 * 2);
          CUR_STREAM (stream)->
              mastering_display_info.min_display_mastering_luminance =
              QT_UINT16 (mdcv_data + 8 + 3 * 2 * 2 + 2 * 2 + 4);
        }
      }

      if (btrt) {
        const guint8 *data;
        guint32 size;

        data = btrt->data;
        size = QT_UINT32 (data);

        /* bufferSizeDB, maxBitrate and avgBitrate - 4 bytes each */
        if (size >= 8 + 12) {

          guint32 max_bitrate = QT_UINT32 (data + 8 + 4);
          guint32 avg_bitrate = QT_UINT32 (data + 8 + 8);

          /* Some muxers seem to swap the average and maximum bitrates
           * (I'm looking at you, YouTube), so we swap for sanity. */
          if (max_bitrate > 0 && max_bitrate < avg_bitrate) {
            guint temp = avg_bitrate;

            avg_bitrate = max_bitrate;
            max_bitrate = temp;
          }
          if (max_bitrate > 0 && max_bitrate < G_MAXUINT32) {
            gst_tag_list_add (stream->stream_tags,
                GST_TAG_MERGE_REPLACE, GST_TAG_MAXIMUM_BITRATE,
                max_bitrate, NULL);
          }
          if (avg_bitrate > 0 && avg_bitrate < G_MAXUINT32) {
            gst_tag_list_add (stream->stream_tags,
                GST_TAG_MERGE_REPLACE, GST_TAG_BITRATE, avg_bitrate, NULL);
          }
        }
      }

      if (esds) {
        gst_qtdemux_handle_esds (qtdemux, stream, entry, esds,
            stream->stream_tags);
      } else {
        switch (fourcc) {
          case FOURCC_H264:
          case FOURCC_avc1:
          case FOURCC_avc3:
          {
            GNode *avcC =
                qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_avcC);
            GNode *strf =
                qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_strf);

            if (avcC) {
              const guint8 *data;
              guint32 size;

              data = avcC->data;
              size = QT_UINT32 (data);
              if (size >= 8 + 1) {
                GstBuffer *buf;

                GST_DEBUG_OBJECT (qtdemux, "found avcC codec_data in stsd");

                /* First 4 bytes are the length of the atom, the next 4 bytes
                 * are the fourcc, the next 1 byte is the version, and the
                 * subsequent bytes are profile_tier_level structure like data. */
                gst_codec_utils_h264_caps_set_level_and_profile (entry->caps,
                    data + 8 + 1, size - 8 - 1);
                buf = gst_buffer_new_and_alloc (size - 8);
                gst_buffer_fill (buf, 0, data + 8, size - 8);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);
              }
            } else if (strf) {
              const guint8 *data;
              guint32 size;

              data = strf->data;
              size = QT_UINT32 (data);

              if (size >= 8 + 40 + 1) {
                GstBuffer *buf;


                GST_DEBUG_OBJECT (qtdemux, "found strf codec_data in stsd");

                /* First 4 bytes are the length of the atom, the next 4 bytes
                 * are the fourcc, next 40 bytes are BITMAPINFOHEADER,
                 * next 1 byte is the version, and the
                 * subsequent bytes are sequence parameter set like data. */

                gst_codec_utils_h264_caps_set_level_and_profile
                    (entry->caps, data + 8 + 40 + 1, size - 8 - 40 - 1);

                buf = gst_buffer_new_and_alloc (size - 8 - 40);
                gst_buffer_fill (buf, 0, data + 8 + 40, size - 8 - 40);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);
              }
            }

            break;
          }
          case FOURCC_H265:
          case FOURCC_hvc1:
          case FOURCC_hev1:
          case FOURCC_dvh1:
          case FOURCC_dvhe:
          {
            GNode *hvcC =
                qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_hvcC);

            if (hvcC) {
              const guint8 *data;
              guint32 size;

              data = hvcC->data;
              size = QT_UINT32 (data);

              if (size >= 8 + 1) {
                GstBuffer *buf;

                GST_DEBUG_OBJECT (qtdemux, "found hvcC codec_data in stsd");

                /* First 4 bytes are the length of the atom, the next 4 bytes
                 * are the fourcc, the next 1 byte is the version, and the
                 * subsequent bytes are sequence parameter set like data. */
                gst_codec_utils_h265_caps_set_level_tier_and_profile
                    (entry->caps, data + 8 + 1, size - 8 - 1);

                buf = gst_buffer_new_and_alloc (size - 8);
                gst_buffer_fill (buf, 0, data + 8, size - 8);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);
              }
            }
            break;
          }
          case FOURCC_H266:
          case FOURCC_vvc1:
          case FOURCC_vvi1:
          {
            GNode *vvcC =
                qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_vvcC);

            if (vvcC) {
              const guint8 *data;
              guint32 size;

              data = vvcC->data;
              size = QT_UINT32 (data);


              if (size >= 12 + 1) {
                GstBuffer *buf;
                guint8 version;

                GST_DEBUG_OBJECT (qtdemux, "found vvcC codec_data in stsd");

                /* First 4 bytes are the length of the atom, the next 4 bytes
                 * are the fourcc, the next 1 byte is the version, the next 3 bytes are flags and the
                 * subsequent bytes are the decoder configuration record. */
                version = data[8];
                if (version != 0) {
                  GST_ERROR_OBJECT (qtdemux,
                      "Unsupported vvcC version %u. Only version 0 is supported",
                      version);
                  break;
                }

                gst_codec_utils_h266_caps_set_level_tier_and_profile
                    (entry->caps, data + 12, size - 12);

                buf = gst_buffer_new_and_alloc (size - 12);
                gst_buffer_fill (buf, 0, data + 12, size - 12);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);
              }
            }
            break;
          }
          case FOURCC_mp4v:
          case FOURCC_MP4V:
          case FOURCC_fmp4:
          case FOURCC_FMP4:
          case FOURCC_xvid:
          case FOURCC_XVID:
          {
            GNode *glbl;

            GST_DEBUG_OBJECT (qtdemux, "found %" GST_FOURCC_FORMAT,
                GST_FOURCC_ARGS (fourcc));

            /* codec data might be in glbl extension atom */
            glbl = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_glbl);
            if (glbl) {
              guint8 *data;
              GstBuffer *buf;
              guint len;

              GST_DEBUG_OBJECT (qtdemux, "found glbl data in stsd");
              data = glbl->data;
              len = QT_UINT32 (data);
              if (len > 0x8) {
                len -= 0x8;
                buf = gst_buffer_new_and_alloc (len);
                gst_buffer_fill (buf, 0, data + 8, len);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);
              }
            }
            break;
          }
          case FOURCC_mjp2:
          {
            /* see annex I of the jpeg2000 spec */
            GNode *jp2h, *ihdr, *colr, *prefix, *cmap, *cdef;
            const guint8 *data;
            const gchar *colorspace = NULL;
            gint ncomp = 0;
            guint32 colr_len;
            guint32 ncomp_map = 0;
            gint32 *comp_map = NULL;
            guint32 nchan_def = 0;
            gint32 *chan_def = NULL;

            GST_DEBUG_OBJECT (qtdemux, "found mjp2");
            /* some required atoms */
            jp2h = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_jp2h);
            if (!jp2h)
              break;

            /* number of components; redundant with info in codestream, but useful
               to a muxer */
            ihdr = qtdemux_tree_get_child_by_type (jp2h, FOURCC_ihdr);
            if (!ihdr || QT_UINT32 (ihdr->data) != 22)
              break;
            ncomp = QT_UINT16 (((guint8 *) ihdr->data) + 16);

            colr = qtdemux_tree_get_child_by_type (jp2h, FOURCC_colr);
            if (!colr)
              break;
            colr_len = QT_UINT32 (colr->data);
            if (colr_len < 15)
              break;
            GST_DEBUG_OBJECT (qtdemux, "found colr");
            /* extract colour space info */
            if (QT_UINT8 ((guint8 *) colr->data + 8) == 1) {
              switch (QT_UINT32 ((guint8 *) colr->data + 11)) {
                case 16:
                  colorspace = "sRGB";
                  break;
                case 17:
                  colorspace = "GRAY";
                  break;
                case 18:
                  colorspace = "sYUV";
                  break;
                default:
                  colorspace = NULL;
                  break;
              }
            }
            if (!colorspace)
              /* colr is required, and only values 16, 17, and 18 are specified,
                 so error if we have no colorspace */
              break;

            /* extract component mapping */
            cmap = qtdemux_tree_get_child_by_type (jp2h, FOURCC_cmap);
            if (cmap) {
              guint32 cmap_len = 0;
              int i;
              cmap_len = QT_UINT32 (cmap->data);
              if (cmap_len >= 8) {
                /* normal box, subtract off header */
                cmap_len -= 8;
                /* cmap: { u16 cmp; u8 mtyp; u8 pcol; }* */
                if (cmap_len % 4 == 0) {
                  ncomp_map = (cmap_len / 4);
                  comp_map = g_new0 (gint32, ncomp_map);
                  for (i = 0; i < ncomp_map; i++) {
                    guint16 cmp;
                    guint8 mtyp, pcol;
                    cmp = QT_UINT16 (((guint8 *) cmap->data) + 8 + i * 4);
                    mtyp = QT_UINT8 (((guint8 *) cmap->data) + 8 + i * 4 + 2);
                    pcol = QT_UINT8 (((guint8 *) cmap->data) + 8 + i * 4 + 3);
                    comp_map[i] = (mtyp << 24) | (pcol << 16) | cmp;
                  }
                }
              }
            }
            /* extract channel definitions */
            cdef = qtdemux_tree_get_child_by_type (jp2h, FOURCC_cdef);
            if (cdef) {
              guint32 cdef_len = 0;
              int i;
              cdef_len = QT_UINT32 (cdef->data);
              if (cdef_len >= 10) {
                /* normal box, subtract off header and len */
                cdef_len -= 10;
                /* cdef: u16 n; { u16 cn; u16 typ; u16 asoc; }* */
                if (cdef_len % 6 == 0) {
                  nchan_def = (cdef_len / 6);
                  chan_def = g_new0 (gint32, nchan_def);
                  for (i = 0; i < nchan_def; i++)
                    chan_def[i] = -1;
                  for (i = 0; i < nchan_def; i++) {
                    guint16 cn, typ, asoc;
                    cn = QT_UINT16 (((guint8 *) cdef->data) + 10 + i * 6);
                    typ = QT_UINT16 (((guint8 *) cdef->data) + 10 + i * 6 + 2);
                    asoc = QT_UINT16 (((guint8 *) cdef->data) + 10 + i * 6 + 4);
                    if (cn < nchan_def) {
                      switch (typ) {
                        case 0:
                          chan_def[cn] = asoc;
                          break;
                        case 1:
                          chan_def[cn] = 0;     /* alpha */
                          break;
                        default:
                          chan_def[cn] = -typ;
                      }
                    }
                  }
                }
              }
            }

            gst_caps_set_simple (entry->caps,
                "num-components", G_TYPE_INT, ncomp, NULL);
            gst_caps_set_simple (entry->caps,
                "colorspace", G_TYPE_STRING, colorspace, NULL);

            if (comp_map) {
              GValue arr = { 0, };
              GValue elt = { 0, };
              int i;
              g_value_init (&arr, GST_TYPE_ARRAY);
              g_value_init (&elt, G_TYPE_INT);
              for (i = 0; i < ncomp_map; i++) {
                g_value_set_int (&elt, comp_map[i]);
                gst_value_array_append_value (&arr, &elt);
              }
              gst_structure_set_value (gst_caps_get_structure (entry->caps, 0),
                  "component-map", &arr);
              g_value_unset (&elt);
              g_value_unset (&arr);
              g_free (comp_map);
            }

            if (chan_def) {
              GValue arr = { 0, };
              GValue elt = { 0, };
              int i;
              g_value_init (&arr, GST_TYPE_ARRAY);
              g_value_init (&elt, G_TYPE_INT);
              for (i = 0; i < nchan_def; i++) {
                g_value_set_int (&elt, chan_def[i]);
                gst_value_array_append_value (&arr, &elt);
              }
              gst_structure_set_value (gst_caps_get_structure (entry->caps, 0),
                  "channel-definitions", &arr);
              g_value_unset (&elt);
              g_value_unset (&arr);
              g_free (chan_def);
            }

            /* indicate possible fields in caps */
            if (CUR_STREAM (stream)->interlace_mode != 1) {
              gst_caps_set_simple (entry->caps, "fields", G_TYPE_INT,
                  CUR_STREAM (stream)->interlace_mode, NULL);
            }

            /* some optional atoms */
            prefix = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_jp2x);

            /* add codec_data if provided */
            if (prefix) {
              GstBuffer *buf;
              guint len;

              GST_DEBUG_OBJECT (qtdemux, "found prefix data in stsd");
              data = prefix->data;
              len = QT_UINT32 (data);
              if (len > 0x8) {
                len -= 0x8;
                buf = gst_buffer_new_and_alloc (len);
                gst_buffer_fill (buf, 0, data + 8, len);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);
              }
            }
            break;
          }
          case FOURCC_SVQ3:
          case FOURCC_VP31:
          {
            if (version >> 16 == 3) {
              GNode *gama, *smi;

              gama = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_gama);
              if (gama) {
                guint32 size = QT_UINT32 (gama->data);

                if (size == 12) {
                  gdouble gamma = QT_FP32 ((const guint8 *) gama->data + 8);
                  gst_caps_set_simple (entry->caps, "applied-gamma",
                      G_TYPE_DOUBLE, gamma, NULL);
                }
              }

              smi = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_SMI_);
              if (smi) {
                const guint8 *data = smi->data;
                guint32 size = QT_UINT32 (data);

                // This has first a fourcc and then the size
                if (size > 16 && QT_FOURCC (data + 8) == FOURCC_SEQH) {
                  guint32 seqh_size = QT_UINT32 (data + 8 + 4);
                  if (seqh_size > 0 && seqh_size <= size - 8 - 8) {
                    GstBuffer *seqh = gst_buffer_new_and_alloc (seqh_size);
                    gst_buffer_fill (seqh, 0, data + 8 + 8, seqh_size);

                    /* sorry for the bad name, but we don't know what this is, other
                     * than its own fourcc */
                    gst_caps_set_simple (entry->caps, "seqh", GST_TYPE_BUFFER,
                        seqh, NULL);
                    gst_buffer_unref (seqh);
                  }
                }
              }
            }

            break;
          }
          case FOURCC_rle_:
          case FOURCC_WRLE:
          {
            gst_caps_set_simple (entry->caps,
                "depth", G_TYPE_INT, entry->bits_per_sample, NULL);
            break;
          }
          case FOURCC_XiTh:
          {
            GNode *xdxt;

            GST_DEBUG_OBJECT (qtdemux, "found XiTh");

            xdxt = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_XdxT);
            if (xdxt) {
              GNode *tcth, *tct, *tctc;

              GST_DEBUG_OBJECT (qtdemux, "found XdxT node");

              /* collect the headers and store them in a stream list so that we can
               * send them out first */

              tcth = qtdemux_tree_get_child_by_type (xdxt, FOURCC_tCtH);
              if (tcth) {
                guint32 size = QT_UINT32 (tcth->data);
                GstBuffer *buffer;

                buffer = gst_buffer_new_and_alloc (size);
                gst_buffer_fill (buffer, 0, tcth->data, size);
                stream->buffers = g_slist_append (stream->buffers, buffer);
                GST_LOG_OBJECT (qtdemux, "parsing theora header");
              }

              tct = qtdemux_tree_get_child_by_type (xdxt, FOURCC_tCt_);
              if (tct) {
                guint32 size = QT_UINT32 (tct->data);
                GstBuffer *buffer;

                buffer = gst_buffer_new_and_alloc (size);
                gst_buffer_fill (buffer, 0, tct->data, size);
                stream->buffers = g_slist_append (stream->buffers, buffer);
                GST_LOG_OBJECT (qtdemux, "parsing theora comment");
              }

              tctc = qtdemux_tree_get_child_by_type (xdxt, FOURCC_tCtC);
              if (tctc) {
                guint32 size = QT_UINT32 (tctc->data);
                GstBuffer *buffer;

                buffer = gst_buffer_new_and_alloc (size);
                gst_buffer_fill (buffer, 0, tctc->data, size);
                stream->buffers = g_slist_append (stream->buffers, buffer);
                GST_LOG_OBJECT (qtdemux, "parsing theora codebook");
              }
            }
            break;
          }
          case FOURCC_ovc1:
          {
            guint8 *ovc1_data;
            guint ovc1_len;
            GstBuffer *buf;

            GST_DEBUG_OBJECT (qtdemux, "parse ovc1 header");
            ovc1_data = stsd_entry->data;
            ovc1_len = QT_UINT32 (ovc1_data);
            if (ovc1_len <= 198) {
              GST_WARNING_OBJECT (qtdemux, "Too small ovc1 header, skipping");
              break;
            }
            buf = gst_buffer_new_and_alloc (ovc1_len - 198);
            gst_buffer_fill (buf, 0, ovc1_data + 198, ovc1_len - 198);
            gst_caps_set_simple (entry->caps,
                "codec_data", GST_TYPE_BUFFER, buf, NULL);
            gst_buffer_unref (buf);
            break;
          }
          case FOURCC_vc_1:
          {
            GNode *dvc1;

            dvc1 =
                qtdemux_tree_get_child_by_type (stsd_entry,
                GST_MAKE_FOURCC ('d', 'v', 'c', '1'));
            if (dvc1) {
              guint32 size = QT_UINT32 (dvc1->data);

              if (size >= 8) {
                GstBuffer *buf;

                GST_DEBUG_OBJECT (qtdemux, "found dvc1 codec_data in stsd");
                buf = gst_buffer_new_and_alloc (size - 8);
                gst_buffer_fill (buf, 0, (const guint8 *) dvc1->data + 8,
                    size - 8);
                gst_caps_set_simple (entry->caps, "codec_data", GST_TYPE_BUFFER,
                    buf, NULL);
                gst_buffer_unref (buf);
              }
            }
            break;
          }
          case FOURCC_av01:
          {
            GNode *av1C;

            av1C = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_av1C);
            if (av1C) {
              const guint8 *data = av1C->data;
              guint32 size = QT_UINT32 (data);

              GstBuffer *buf;

              GST_DEBUG_OBJECT (qtdemux,
                  "found av1C codec_data in stsd of size %d", size);

              /* not enough data, just ignore and hope for the best */
              if (size < 8 + 4)
                break;

              /* Content is:
               * 4 bytes: atom length
               * 4 bytes: fourcc
               *
               * version 1 (marker=1):
               *
               *  unsigned int (1) marker = 1;
               *  unsigned int (7) version = 1;
               *  unsigned int (3) seq_profile;
               *  unsigned int (5) seq_level_idx_0;
               *  unsigned int (1) seq_tier_0;
               *  unsigned int (1) high_bitdepth;
               *  unsigned int (1) twelve_bit;
               *  unsigned int (1) monochrome;
               *  unsigned int (1) chroma_subsampling_x;
               *  unsigned int (1) chroma_subsampling_y;
               *  unsigned int (2) chroma_sample_position;
               *  unsigned int (3) reserved = 0;
               *
               *  unsigned int (1) initial_presentation_delay_present;
               *  if (initial_presentation_delay_present) {
               *    unsigned int (4) initial_presentation_delay_minus_one;
               *  } else {
               *    unsigned int (4) reserved = 0;
               *  }
               *
               *  unsigned int (8) configOBUs[];
               *
               * rest: OBUs.
               */

              switch (data[8]) {
                case 0x81:{
                  guint8 pres_delay_field;

                  /* We let profile and the other parts be figured out by
                   * av1parse and only include the presentation delay here
                   * if present */
                  /* We skip initial_presentation_delay* for now */
                  pres_delay_field = *(data + 11);
                  if (pres_delay_field & (1 << 5)) {
                    gst_caps_set_simple (entry->caps,
                        "presentation-delay", G_TYPE_INT,
                        (gint) (pres_delay_field & 0x0F) + 1, NULL);
                  }

                  buf = gst_buffer_new_and_alloc (size - 8);
                  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_HEADER);
                  gst_buffer_fill (buf, 0, data + 8, size - 8);
                  gst_caps_set_simple (entry->caps,
                      "codec_data", GST_TYPE_BUFFER, buf, NULL);
                  gst_buffer_unref (buf);
                  break;
                }
                default:
                  GST_WARNING ("Unknown version 0x%02x of av1C box", data[8]);
                  break;
              }
            }

            break;
          }

          case FOURCC_vp08:
          case FOURCC_vp09:
          {
            GNode *vpcC;

            vpcC = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_vpcC);
            if (vpcC) {
              const guint8 *data = vpcC->data;
              guint32 size = QT_UINT32 (data);
              const gchar *profile_str = NULL;
              const gchar *chroma_format_str = NULL;
              guint8 profile;
              guint8 bitdepth;
              guint8 chroma_format;
              GstVideoColorimetry cinfo;

              /* parse, if found */
              GST_DEBUG_OBJECT (qtdemux,
                  "found vp codec_data in stsd of size %d", size);

              /* the meaning of "size" is length of the atom body, excluding
               * atom length and fourcc fields */
              if (size < 8 + 12)
                break;

              /* Content is:
               * 4 bytes: atom length
               * 4 bytes: fourcc
               * 1 byte: version
               * 3 bytes: flags
               * 1 byte: profile
               * 1 byte: level
               * 4 bits: bitDepth
               * 3 bits: chromaSubsampling
               * 1 bit: videoFullRangeFlag
               * 1 byte: colourPrimaries
               * 1 byte: transferCharacteristics
               * 1 byte: matrixCoefficients
               * 2 bytes: codecIntializationDataSize (should be zero for vp8 and vp9)
               * rest: codecIntializationData (not used for vp8 and vp9)
               */

              if (data[8] != 1) {
                GST_WARNING_OBJECT (qtdemux,
                    "unknown vpcC version %d", data[8]);
                break;
              }

              profile = data[12];
              switch (profile) {
                case 0:
                  profile_str = "0";
                  break;
                case 1:
                  profile_str = "1";
                  break;
                case 2:
                  profile_str = "2";
                  break;
                case 3:
                  profile_str = "3";
                  break;
                default:
                  break;
              }

              if (profile_str) {
                gst_caps_set_simple (entry->caps,
                    "profile", G_TYPE_STRING, profile_str, NULL);
              }

              /* skip level, the VP9 spec v0.6 defines only one level atm,
               * but webm spec define various ones. Add level to caps
               * if we really need it then */

              bitdepth = (data[14] & 0xf0) >> 4;
              if (bitdepth == 8 || bitdepth == 10 || bitdepth == 12) {
                gst_caps_set_simple (entry->caps,
                    "bit-depth-luma", G_TYPE_UINT, bitdepth,
                    "bit-depth-chroma", G_TYPE_UINT, bitdepth, NULL);
              }

              chroma_format = (data[14] & 0xe) >> 1;
              switch (chroma_format) {
                case 0:
                case 1:
                  chroma_format_str = "4:2:0";
                  break;
                case 2:
                  chroma_format_str = "4:2:2";
                  break;
                case 3:
                  chroma_format_str = "4:4:4";
                  break;
                default:
                  break;
              }

              if (chroma_format_str) {
                gst_caps_set_simple (entry->caps,
                    "chroma-format", G_TYPE_STRING, chroma_format_str, NULL);
              }

              if ((data[14] & 0x1) != 0)
                cinfo.range = GST_VIDEO_COLOR_RANGE_0_255;
              else
                cinfo.range = GST_VIDEO_COLOR_RANGE_16_235;
              cinfo.primaries = gst_video_color_primaries_from_iso (data[15]);
              cinfo.transfer = gst_video_transfer_function_from_iso (data[16]);
              cinfo.matrix = gst_video_color_matrix_from_iso (data[17]);

              if (cinfo.primaries != GST_VIDEO_COLOR_PRIMARIES_UNKNOWN &&
                  cinfo.transfer != GST_VIDEO_TRANSFER_UNKNOWN &&
                  cinfo.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
                /* set this only if all values are known, otherwise this
                 * might overwrite valid ones parsed from other color box */
                CUR_STREAM (stream)->colorimetry = cinfo;
              }
            }
            break;
          }
          case GST_MAKE_FOURCC ('A', 'V', 'd', 'h'):{
            GNode *adhr;

            adhr =
                qtdemux_tree_get_child_by_type (stsd_entry,
                GST_MAKE_FOURCC ('A', 'D', 'H', 'R'));
            if (adhr) {
              const guint8 *data = adhr->data;
              guint32 size = QT_UINT32 (data);

              if (size >= 8 + 4 + 4) {
                guint32 version = QT_FOURCC (data + 8);

                if (version == GST_MAKE_FOURCC ('0', '0', '0', '1') ||
                    version == GST_MAKE_FOURCC ('0', '0', '0', '2')) {
                  guint32 profile = QT_UINT32 (data + 12);
                  const gchar *profile_s = "dnxhr";

                  switch (profile) {
                    case 0x4f6:
                      profile_s = "dnxhr-444";
                      break;
                    case 0x4f7:
                      profile_s = "dnxhr-hqx";
                      break;
                    case 0x4f8:
                      profile_s = "dnxhr-hq";
                      break;
                    case 0x4f9:
                      profile_s = "dnxhr-sq";
                      break;
                    case 0x4fa:
                      profile_s = "dnxhr-lb";
                      break;
                    default:
                      GST_WARNING_OBJECT (qtdemux, "Unknown DNxHR profile %08x",
                          profile);
                      break;
                  }

                  gst_caps_set_simple (entry->caps, "profile", G_TYPE_STRING,
                      profile_s, NULL);
                }
              }
            }
            break;
          }
          default:
            break;
        }
      }

      GST_INFO_OBJECT (qtdemux,
          "type %" GST_FOURCC_FORMAT " caps %" GST_PTR_FORMAT,
          GST_FOURCC_ARGS (fourcc), entry->caps);

    } else if (stream->subtype == FOURCC_soun) {
      GNode *wave, *btrt;
      guint version, samplesize;
      guint16 compression_id;
      gboolean amrwb = FALSE;

      offset = 16;
      /* sample description entry (16) + sound sample description v0 (20) */
      if (len < 36)
        goto corrupt_file;

      version = QT_UINT32 (stsd_entry_data + offset);
      entry->n_channels = QT_UINT16 (stsd_entry_data + offset + 8);
      samplesize = QT_UINT16 (stsd_entry_data + offset + 10);
      compression_id = QT_UINT16 (stsd_entry_data + offset + 12);
      entry->rate = QT_FP32 (stsd_entry_data + offset + 16);

      GST_LOG_OBJECT (qtdemux, "version/rev:      %08x", version);
      GST_LOG_OBJECT (qtdemux, "vendor:           %08x",
          QT_UINT32 (stsd_entry_data + offset + 4));
      GST_LOG_OBJECT (qtdemux, "n_channels:       %d", entry->n_channels);
      GST_LOG_OBJECT (qtdemux, "sample_size:      %d", samplesize);
      GST_LOG_OBJECT (qtdemux, "compression_id:   %d", compression_id);
      GST_LOG_OBJECT (qtdemux, "packet size:      %d",
          QT_UINT16 (stsd_entry_data + offset + 14));
      GST_LOG_OBJECT (qtdemux, "sample rate:      %g", entry->rate);

      if (compression_id == 0xfffe)
        entry->sampled = TRUE;

      /* first assume uncompressed audio */
      entry->bytes_per_sample = samplesize / 8;
      entry->samples_per_frame = entry->n_channels;
      entry->bytes_per_frame = entry->n_channels * entry->bytes_per_sample;
      entry->samples_per_packet = entry->samples_per_frame;
      entry->bytes_per_packet = entry->bytes_per_sample;

      offset = 36;

      /* This is only valid in MOV files. To distinguish this from the
       * AudioSampleEntryV1 from ISOBMFF (which does not have the additional
       * fields but instead the exact same layout as AudioSampleEntry), the
       * latter requires a stsd of version 1 to be used.
       * The same goes for version 2 below, for which no equivalent in ISOBMFF
       * exists yet, fortunately
       */
      if (version == 0x00010000 && stsd_version == 0) {
        /* sample description entry (16) + sound sample description v1 (20+16) */
        if (len < 52)
          goto corrupt_file;

        /* take information from here over the normal sample description */
        entry->samples_per_packet = QT_UINT32 (stsd_entry_data + offset);
        entry->bytes_per_packet = QT_UINT32 (stsd_entry_data + offset + 4);
        entry->bytes_per_frame = QT_UINT32 (stsd_entry_data + offset + 8);
        entry->bytes_per_sample = QT_UINT32 (stsd_entry_data + offset + 12);

        GST_LOG_OBJECT (qtdemux, "Sound sample description Version 1");
        GST_LOG_OBJECT (qtdemux, "samples/packet:   %d",
            entry->samples_per_packet);
        GST_LOG_OBJECT (qtdemux, "bytes/packet:     %d",
            entry->bytes_per_packet);
        GST_LOG_OBJECT (qtdemux, "bytes/frame:      %d",
            entry->bytes_per_frame);
        GST_LOG_OBJECT (qtdemux, "bytes/sample:     %d",
            entry->bytes_per_sample);

        if (!entry->sampled && entry->bytes_per_packet) {
          entry->samples_per_frame = (entry->bytes_per_frame /
              entry->bytes_per_packet) * entry->samples_per_packet;
          GST_LOG_OBJECT (qtdemux, "samples/frame:    %d",
              entry->samples_per_frame);
        }
      } else if (version == 0x00020000 && stsd_version == 0) {
        /* sample description entry (16) + sound sample description v2 (56) */
        if (len < 72)
          goto corrupt_file;

        /* take information from here over the normal sample description */
        entry->rate = GST_READ_DOUBLE_BE (stsd_entry_data + offset + 4);
        entry->n_channels = QT_UINT32 (stsd_entry_data + offset + 12);
        entry->samples_per_frame = entry->n_channels;
        entry->bytes_per_sample = QT_UINT32 (stsd_entry_data + offset + 20) / 8;
        entry->bytes_per_packet = QT_UINT32 (stsd_entry_data + offset + 28);
        entry->samples_per_packet = QT_UINT32 (stsd_entry_data + offset + 32);
        entry->bytes_per_frame = entry->bytes_per_sample * entry->n_channels;

        GST_LOG_OBJECT (qtdemux, "Sound sample description Version 2");
        GST_LOG_OBJECT (qtdemux, "sample rate:        %g", entry->rate);
        GST_LOG_OBJECT (qtdemux, "n_channels:         %d", entry->n_channels);
        GST_LOG_OBJECT (qtdemux, "bits/channel:       %d",
            entry->bytes_per_sample * 8);
        GST_LOG_OBJECT (qtdemux, "format flags:       %X",
            QT_UINT32 (stsd_entry_data + offset + 24));
        GST_LOG_OBJECT (qtdemux, "bytes/packet:       %d",
            entry->bytes_per_packet);
        GST_LOG_OBJECT (qtdemux, "LPCM frames/packet: %d",
            entry->samples_per_packet);
      } else if (version != 0x00000) {
        GST_WARNING_OBJECT (qtdemux, "unknown audio STSD version %08x",
            version);
      }

      switch (fourcc) {
          /* Yes, these have to be hard-coded */
        case FOURCC_MAC6:
        {
          entry->samples_per_packet = 6;
          entry->bytes_per_packet = 1;
          entry->bytes_per_frame = 1 * entry->n_channels;
          entry->bytes_per_sample = 1;
          entry->samples_per_frame = 6 * entry->n_channels;
          break;
        }
        case FOURCC_MAC3:
        {
          entry->samples_per_packet = 3;
          entry->bytes_per_packet = 1;
          entry->bytes_per_frame = 1 * entry->n_channels;
          entry->bytes_per_sample = 1;
          entry->samples_per_frame = 3 * entry->n_channels;
          break;
        }
        case FOURCC_ima4:
        {
          entry->samples_per_packet = 64;
          entry->bytes_per_packet = 34;
          entry->bytes_per_frame = 34 * entry->n_channels;
          entry->bytes_per_sample = 2;
          entry->samples_per_frame = 64 * entry->n_channels;
          break;
        }
        case FOURCC_ulaw:
        case FOURCC_alaw:
        {
          entry->samples_per_packet = 1;
          entry->bytes_per_packet = 1;
          entry->bytes_per_frame = 1 * entry->n_channels;
          entry->bytes_per_sample = 1;
          entry->samples_per_frame = 1 * entry->n_channels;
          break;
        }
        case FOURCC_agsm:
        {
          entry->samples_per_packet = 160;
          entry->bytes_per_packet = 33;
          entry->bytes_per_frame = 33 * entry->n_channels;
          entry->bytes_per_sample = 2;
          entry->samples_per_frame = 160 * entry->n_channels;
          break;
        }
          /* fix up any invalid header information from above */
        case FOURCC_twos:
        case FOURCC_sowt:
        case FOURCC_raw_:
        case FOURCC_lpcm:
        case FOURCC_ipcm:
        case FOURCC_fpcm:
          /* Sometimes these are set to 0 in the sound sample descriptions so
           * let's try to infer useful values from the other information we
           * have available */
          if (entry->bytes_per_sample == 0)
            entry->bytes_per_sample =
                entry->bytes_per_frame / entry->n_channels;
          if (entry->bytes_per_sample == 0)
            entry->bytes_per_sample = samplesize / 8;

          if (entry->bytes_per_frame == 0)
            entry->bytes_per_frame =
                entry->bytes_per_sample * entry->n_channels;

          if (entry->bytes_per_packet == 0)
            entry->bytes_per_packet = entry->bytes_per_sample;

          if (entry->samples_per_frame == 0)
            entry->samples_per_frame = entry->n_channels;

          if (entry->samples_per_packet == 0)
            entry->samples_per_packet = entry->samples_per_frame;

          break;
        case FOURCC_in24:
        case FOURCC_in32:
        case FOURCC_fl32:
        case FOURCC_fl64:
        case FOURCC_s16l:{
          switch (fourcc) {
            case FOURCC_in24:
              entry->bytes_per_sample = 3;
              break;
            case FOURCC_in32:
            case FOURCC_fl32:
              entry->bytes_per_sample = 4;
              break;
            case FOURCC_fl64:
              entry->bytes_per_sample = 8;
              break;
            case FOURCC_s16l:
              entry->bytes_per_sample = 2;
              break;
            default:
              g_assert_not_reached ();
              break;
          }
          entry->samples_per_frame = entry->n_channels;
          entry->bytes_per_frame = entry->n_channels * entry->bytes_per_sample;
          entry->samples_per_packet = entry->samples_per_frame;
          entry->bytes_per_packet = entry->bytes_per_sample;
          break;
        }

          /* According to TS 102 366, the channel count in
           * a (E)AC3SampleEntry box is to be ignored */
        case 0x20736d:
        case GST_MAKE_FOURCC ('e', 'c', '-', '3'):
        case GST_MAKE_FOURCC ('s', 'a', 'c', '3'):     // Nero Recode
        case FOURCC_ac_3:
          entry->n_channels = 0;
          break;

        default:
          break;
      }

      if (entry->caps)
        gst_caps_unref (entry->caps);

      entry->caps = qtdemux_audio_caps (qtdemux, stream, entry, fourcc,
          stsd_version, version, stsd_entry, &codec);

      switch (fourcc) {
        case FOURCC_in24:
        case FOURCC_in32:
        case FOURCC_fl32:
        case FOURCC_fl64:
        {
          GNode *enda;

          enda = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_enda);
          if (!enda) {
            wave = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_wave);
            if (wave)
              enda = qtdemux_tree_get_child_by_type (wave, FOURCC_enda);
          }
          if (enda) {
            const gchar *format_str;
            guint32 enda_len = QT_UINT32 (enda->data);

            if (enda_len >= 9) {
              guint16 enda_value = QT_UINT16 ((guint8 *) enda->data + 8);

              switch (fourcc) {
                case FOURCC_in24:
                  format_str = (enda_value) ? "S24LE" : "S24BE";
                  break;
                case FOURCC_in32:
                  format_str = (enda_value) ? "S32LE" : "S32BE";
                  break;
                case FOURCC_fl32:
                  format_str = (enda_value) ? "F32LE" : "F32BE";
                  break;
                case FOURCC_fl64:
                  format_str = (enda_value) ? "F64LE" : "F64BE";
                  break;
                default:
                  g_assert_not_reached ();
                  break;
              }
              gst_caps_set_simple (entry->caps,
                  "format", G_TYPE_STRING, format_str, NULL);
            }
          }
          break;
        }
        case FOURCC_owma:
        {
          const guint8 *owma_data;
          const gchar *codec_name = NULL;
          guint owma_len;
          GstBuffer *buf;
          gint version = 1;
          /* from http://msdn.microsoft.com/en-us/library/dd757720(VS.85).aspx */
          /* FIXME this should also be gst_riff_strf_auds,
           * but the latter one is actually missing bits-per-sample :( */
          typedef struct
          {
            gint16 wFormatTag;
            gint16 nChannels;
            gint32 nSamplesPerSec;
            gint32 nAvgBytesPerSec;
            gint16 nBlockAlign;
            gint16 wBitsPerSample;
            gint16 cbSize;
          } WAVEFORMATEX;
          WAVEFORMATEX wfex;

          GST_DEBUG_OBJECT (qtdemux, "parse owma");
          owma_data = stsd_entry_data;
          owma_len = QT_UINT32 (owma_data);
          if (owma_len <= 54) {
            GST_WARNING_OBJECT (qtdemux, "Too small owma header, skipping");
            break;
          }
          wfex.wFormatTag = GST_READ_UINT16_LE (owma_data + 36 + 0);
          wfex.nChannels = GST_READ_UINT16_LE (owma_data + 36 + 2);
          wfex.nSamplesPerSec = GST_READ_UINT32_LE (owma_data + 36 + 4);
          wfex.nAvgBytesPerSec = GST_READ_UINT32_LE (owma_data + 36 + 8);
          wfex.nBlockAlign = GST_READ_UINT16_LE (owma_data + 36 + 12);
          wfex.wBitsPerSample = GST_READ_UINT16_LE (owma_data + 36 + 14);
          wfex.cbSize = GST_READ_UINT16_LE (owma_data + 36 + 16);
          buf = gst_buffer_new_and_alloc (owma_len - 54);
          gst_buffer_fill (buf, 0, owma_data + 54, owma_len - 54);
          if (wfex.wFormatTag == 0x0161) {
            codec_name = "Windows Media Audio";
            version = 2;
          } else if (wfex.wFormatTag == 0x0162) {
            codec_name = "Windows Media Audio 9 Pro";
            version = 3;
          } else if (wfex.wFormatTag == 0x0163) {
            codec_name = "Windows Media Audio 9 Lossless";
            /* is that correct? gstffmpegcodecmap.c is missing it, but
             * fluendo codec seems to support it */
            version = 4;
          }

          gst_caps_set_simple (entry->caps,
              "codec_data", GST_TYPE_BUFFER, buf,
              "wmaversion", G_TYPE_INT, version,
              "block_align", G_TYPE_INT,
              wfex.nBlockAlign, "bitrate", G_TYPE_INT,
              wfex.nAvgBytesPerSec, "width", G_TYPE_INT,
              wfex.wBitsPerSample, "depth", G_TYPE_INT,
              wfex.wBitsPerSample, NULL);
          gst_buffer_unref (buf);

          if (codec_name) {
            g_free (codec);
            codec = g_strdup (codec_name);
          }
          break;
        }
        case FOURCC_wma_:
        {
          const gchar *codec_name = NULL;
          gint version = 1;
          /* from http://msdn.microsoft.com/en-us/library/dd757720(VS.85).aspx */
          /* FIXME this should also be gst_riff_strf_auds,
           * but the latter one is actually missing bits-per-sample :( */
          typedef struct
          {
            gint16 wFormatTag;
            gint16 nChannels;
            gint32 nSamplesPerSec;
            gint32 nAvgBytesPerSec;
            gint16 nBlockAlign;
            gint16 wBitsPerSample;
            gint16 cbSize;
          } WAVEFORMATEX;
          WAVEFORMATEX wfex;
          GNode *wfex_node;

          /* FIXME: unify with similar wavformatex parsing code above */
          GST_DEBUG_OBJECT (qtdemux, "parse wma, looking for wfex");

          wfex_node =
              qtdemux_tree_get_child_by_type (stsd_entry, GST_MAKE_FOURCC ('w',
                  'f', 'e', 'x'));

          if (wfex_node) {
            const guint8 *wfex_data = wfex_node->data;
            guint32 wfex_size = QT_UINT32 (wfex_data);

            GST_DEBUG_OBJECT (qtdemux, "found wfex in stsd");

            if (wfex_size < 8 + 18)
              break;

            wfex.wFormatTag = GST_READ_UINT16_LE (wfex_data + 8 + 0);
            wfex.nChannels = GST_READ_UINT16_LE (wfex_data + 8 + 2);
            wfex.nSamplesPerSec = GST_READ_UINT32_LE (wfex_data + 8 + 4);
            wfex.nAvgBytesPerSec = GST_READ_UINT32_LE (wfex_data + 8 + 8);
            wfex.nBlockAlign = GST_READ_UINT16_LE (wfex_data + 8 + 12);
            wfex.wBitsPerSample = GST_READ_UINT16_LE (wfex_data + 8 + 14);
            wfex.cbSize = GST_READ_UINT16_LE (wfex_data + 8 + 16);

            GST_LOG_OBJECT (qtdemux, "Found wfex box in stsd:");
            GST_LOG_OBJECT (qtdemux, "FormatTag = 0x%04x, Channels = %u, "
                "SamplesPerSec = %u, AvgBytesPerSec = %u, BlockAlign = %u, "
                "BitsPerSample = %u, Size = %u", wfex.wFormatTag,
                wfex.nChannels, wfex.nSamplesPerSec, wfex.nAvgBytesPerSec,
                wfex.nBlockAlign, wfex.wBitsPerSample, wfex.cbSize);

            if (wfex.wFormatTag == 0x0161) {
              codec_name = "Windows Media Audio";
              version = 2;
            } else if (wfex.wFormatTag == 0x0162) {
              codec_name = "Windows Media Audio 9 Pro";
              version = 3;
            } else if (wfex.wFormatTag == 0x0163) {
              codec_name = "Windows Media Audio 9 Lossless";
              /* is that correct? gstffmpegcodecmap.c is missing it, but
               * fluendo codec seems to support it */
              version = 4;
            }

            gst_caps_set_simple (entry->caps,
                "wmaversion", G_TYPE_INT, version,
                "block_align", G_TYPE_INT, wfex.nBlockAlign,
                "bitrate", G_TYPE_INT, wfex.nAvgBytesPerSec,
                "width", G_TYPE_INT, wfex.wBitsPerSample,
                "depth", G_TYPE_INT, wfex.wBitsPerSample, NULL);

            if (wfex_size > 8 + wfex.cbSize) {
              GstBuffer *buf;

              buf = gst_buffer_new_and_alloc (wfex_size - 8 - wfex.cbSize);
              gst_buffer_fill (buf, 0, wfex_data + 8 + wfex.cbSize,
                  wfex_size - 8 - wfex.cbSize);
              gst_caps_set_simple (entry->caps,
                  "codec_data", GST_TYPE_BUFFER, buf, NULL);
              gst_buffer_unref (buf);
            } else {
              GST_WARNING_OBJECT (qtdemux, "no codec data");
            }

            if (codec_name) {
              g_free (codec);
              codec = g_strdup (codec_name);
            }
            break;
          }
          break;
        }
        case FOURCC_opus:
        {
          guint8 *channel_mapping = NULL;
          guint32 dops_len, rate;
          guint8 n_channels;
          guint8 channel_mapping_family;
          guint8 stream_count;
          guint8 coupled_count;
          guint8 i;

          GNode *dops;

          dops = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_dops);
          if (dops == NULL) {
            GST_WARNING_OBJECT (qtdemux, "Opus Specific Box not found");
            goto corrupt_file;
          }

          /* Opus Specific Box content:
           * 4 bytes: length
           * 4 bytes: "dOps"
           * 1 byte: Version;
           * 1 byte: OutputChannelCount;
           * 2 bytes: PreSkip (big-endians);
           * 4 bytes: InputSampleRate (big-endians);
           * 2 bytes: OutputGain (big-endians);
           * 1 byte: ChannelMappingFamily;
           * if (ChannelMappingFamily != 0) {
           *   1 byte: StreamCount;
           *   1 byte: CoupledCount;
           *   for (OutputChannel in 0..OutputChannelCount) {
           *     1 byte: ChannelMapping;
           *   }
           * }
           */

          dops_len = QT_UINT32 ((guint8 *) dops->data);
          if (len < offset + dops_len) {
            GST_WARNING_OBJECT (qtdemux,
                "Opus Sample Entry has bogus size %" G_GUINT32_FORMAT, len);
            goto corrupt_file;
          }
          if (dops_len < 19) {
            GST_WARNING_OBJECT (qtdemux,
                "Opus Specific Box has bogus size %" G_GUINT32_FORMAT,
                dops_len);
            goto corrupt_file;
          }

          n_channels = GST_READ_UINT8 ((guint8 *) dops->data + 9);
          rate = GST_READ_UINT32_BE ((guint8 *) dops->data + 12);
          channel_mapping_family = GST_READ_UINT8 ((guint8 *) dops->data + 18);

          if (channel_mapping_family != 0) {
            if (dops_len < 21 + n_channels) {
              GST_WARNING_OBJECT (qtdemux,
                  "Opus Specific Box has bogus size %" G_GUINT32_FORMAT,
                  dops_len);
              goto corrupt_file;
            }

            stream_count = GST_READ_UINT8 ((guint8 *) dops->data + 19);
            coupled_count = GST_READ_UINT8 ((guint8 *) dops->data + 20);

            if (n_channels > 0) {
              channel_mapping = g_malloc (n_channels * sizeof (guint8));
              for (i = 0; i < n_channels; i++)
                channel_mapping[i] =
                    GST_READ_UINT8 ((guint8 *) dops->data + i + 21);
            }
          } else if (n_channels == 1) {
            stream_count = 1;
            coupled_count = 0;
          } else if (n_channels == 2) {
            stream_count = 1;
            coupled_count = 1;
          } else {
            GST_WARNING_OBJECT (qtdemux,
                "Opus unexpected nb of channels %d without channel mapping",
                n_channels);
            goto corrupt_file;
          }

          gst_caps_unref (entry->caps);
          entry->caps = gst_codec_utils_opus_create_caps (rate, n_channels,
              channel_mapping_family, stream_count, coupled_count,
              channel_mapping);
          g_free (channel_mapping);

          entry->sampled = TRUE;

          break;
        }
        case FOURCC_ipcm:
        case FOURCC_fpcm:
        {
          GNode *pcmC;

          pcmC = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_pcmC);
          if (pcmC) {
            const guint8 *data = pcmC->data;
            gsize len = QT_UINT32 (data);
            if (len >= 8 + 6) {
              GstByteReader br = GST_BYTE_READER_INIT (data, len);

              gst_byte_reader_skip_unchecked (&br, 4 + 4);

              guint32 version_flags =
                  gst_byte_reader_get_uint32_be_unchecked (&br);
              // Version 0, no flags
              if (version_flags == 0) {
                guint8 format_flags = gst_byte_reader_get_uint8_unchecked (&br);
                guint8 pcm_sample_size =
                    gst_byte_reader_get_uint8_unchecked (&br);
                GstAudioFormat audio_format = GST_AUDIO_FORMAT_UNKNOWN;

                if (fourcc == FOURCC_ipcm) {
                  audio_format =
                      gst_audio_format_build_integer (TRUE,
                      (format_flags & 0x01) ? G_LITTLE_ENDIAN : G_BIG_ENDIAN,
                      pcm_sample_size, pcm_sample_size);
                } else {
                  switch (pcm_sample_size) {
                    case 32:
                      audio_format =
                          (format_flags & 0x01) ? GST_AUDIO_FORMAT_F32LE :
                          GST_AUDIO_FORMAT_F32BE;
                      break;
                    case 64:
                      audio_format =
                          (format_flags & 0x01) ? GST_AUDIO_FORMAT_F64LE :
                          GST_AUDIO_FORMAT_F64BE;
                      break;
                    default:
                      GST_WARNING_OBJECT (qtdemux,
                          "Unsupported floating point PCM sample size %u",
                          pcm_sample_size);
                      break;
                  }
                }
                gst_caps_set_simple (entry->caps,
                    "format", G_TYPE_STRING,
                    audio_format !=
                    GST_AUDIO_FORMAT_UNKNOWN ?
                    gst_audio_format_to_string (audio_format) : "UNKNOWN",
                    NULL);

                entry->bytes_per_sample = pcm_sample_size / 8;
                entry->samples_per_frame = entry->n_channels;
                entry->bytes_per_frame =
                    entry->n_channels * entry->bytes_per_sample;
                entry->samples_per_packet = entry->samples_per_frame;
                entry->bytes_per_packet = entry->bytes_per_sample;

                stream->min_buffer_size = 1024 * entry->bytes_per_frame;
                stream->max_buffer_size = entry->rate * entry->bytes_per_frame;
                GST_DEBUG ("setting min/max buffer sizes to %d/%d",
                    stream->min_buffer_size, stream->max_buffer_size);

                stream->alignment = pcm_sample_size / 8;
              } else {
                GST_WARNING_OBJECT (qtdemux,
                    "Unsupported pcmC version/flags %08x", version_flags);
              }
            }
          } else {
            GST_WARNING_OBJECT (qtdemux,
                "%" GST_FOURCC_FORMAT " without pcmC box",
                GST_FOURCC_ARGS (fourcc));
          }

          break;
        }
        default:
          break;
      }

      if (codec) {
        GstStructure *s;
        gint bitrate = 0;

        gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
            GST_TAG_AUDIO_CODEC, codec, NULL);
        g_free (codec);
        codec = NULL;

        /* some bitrate info may have ended up in caps */
        s = gst_caps_get_structure (entry->caps, 0);
        gst_structure_get_int (s, "bitrate", &bitrate);
        if (bitrate > 0)
          gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
              GST_TAG_BITRATE, bitrate, NULL);
      }

      esds = NULL;
      wave = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_wave);
      if (wave)
        esds = qtdemux_tree_get_child_by_type (wave, FOURCC_esds);
      if (!esds)
        esds = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_esds);

      btrt = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_btrt);

      if (btrt) {
        const guint8 *data;
        guint32 size;

        data = btrt->data;
        size = QT_UINT32 (data);

        /* bufferSizeDB, maxBitrate and avgBitrate - 4 bytes each */
        if (size >= 8 + 12) {

          guint32 max_bitrate = QT_UINT32 (data + 8 + 4);
          guint32 avg_bitrate = QT_UINT32 (data + 8 + 8);

          /* Some muxers seem to swap the average and maximum bitrates
           * (I'm looking at you, YouTube), so we swap for sanity. */
          if (max_bitrate > 0 && max_bitrate < avg_bitrate) {
            guint temp = avg_bitrate;

            avg_bitrate = max_bitrate;
            max_bitrate = temp;
          }
          if (max_bitrate > 0 && max_bitrate < G_MAXUINT32) {
            gst_tag_list_add (stream->stream_tags,
                GST_TAG_MERGE_REPLACE, GST_TAG_MAXIMUM_BITRATE,
                max_bitrate, NULL);
          }
          if (avg_bitrate > 0 && avg_bitrate < G_MAXUINT32) {
            gst_tag_list_add (stream->stream_tags,
                GST_TAG_MERGE_REPLACE, GST_TAG_BITRATE, avg_bitrate, NULL);
          }
        }
      }

      /* If the fourcc's bottom 16 bits gives 'sm', then the top
         16 bits is a byte-swapped wave-style codec identifier,
         and we can find a WAVE header internally to a 'wave' atom here.
         This can more clearly be thought of as 'ms' as the top 16 bits, and a
         codec id as the bottom 16 bits - but byte-swapped to store in QT (which
         is big-endian).
       */
      if ((fourcc & 0xffff) == (('s' << 8) | 'm')) {
        if (len < offset + 20) {
          GST_WARNING_OBJECT (qtdemux, "No wave atom in MS-style audio");
        } else {
          guint32 datalen = QT_UINT32 (stsd_entry_data + offset + 16);
          const guint8 *data = stsd_entry_data + offset + 16;

          if (len < datalen || len - datalen < offset + 16) {
            GST_WARNING_OBJECT (qtdemux, "Not enough data for waveheadernode");
          } else {
            GNode *wavenode;
            GNode *waveheadernode;

            wavenode = g_node_new ((guint8 *) data);
            if (qtdemux_parse_node (qtdemux, wavenode, data, datalen)) {
              const guint8 *waveheader;
              guint32 headerlen;

              waveheadernode =
                  qtdemux_tree_get_child_by_type (wavenode, fourcc);
              if (waveheadernode) {
                waveheader = (const guint8 *) waveheadernode->data;
                headerlen = QT_UINT32 (waveheader);

                if (headerlen > 8) {
                  gst_riff_strf_auds *header = NULL;
                  GstBuffer *headerbuf;
                  GstBuffer *extra;

                  waveheader += 8;
                  headerlen -= 8;

                  headerbuf = gst_buffer_new_and_alloc (headerlen);
                  gst_buffer_fill (headerbuf, 0, waveheader, headerlen);

                  if (gst_riff_parse_strf_auds (GST_ELEMENT_CAST (qtdemux),
                          headerbuf, &header, &extra)) {
                    gst_caps_unref (entry->caps);
                    /* FIXME: Need to do something with the channel reorder map */
                    entry->caps =
                        gst_riff_create_audio_caps (header->format, NULL,
                        header, extra, NULL, NULL, NULL);

                    if (extra)
                      gst_buffer_unref (extra);
                    g_free (header);
                  }
                }
              } else
                GST_DEBUG ("Didn't find waveheadernode for this codec");
            }
            g_node_destroy (wavenode);
          }
        }
      } else if (esds) {
        gst_qtdemux_handle_esds (qtdemux, stream, entry, esds,
            stream->stream_tags);
      } else {
        switch (fourcc) {
          case FOURCC_QDM2:
          case FOURCC_QDMC:
          {
            if (wave) {
              guint32 len = QT_UINT32 (wave->data);

              if (len > 8) {
                GstBuffer *buf = gst_buffer_new_and_alloc (len - 8);

                gst_buffer_fill (buf, 0, (const guint8 *) wave->data + 8,
                    len - 8);
                gst_caps_set_simple (entry->caps, "codec_data", GST_TYPE_BUFFER,
                    buf, NULL);
                gst_buffer_unref (buf);
              }
            }

            gst_caps_set_simple (entry->caps,
                "samplesize", G_TYPE_INT, samplesize, NULL);
            break;
          }
          case FOURCC_alac:
          {
            GNode *alac;

            /* apparently, m4a has this atom appended directly in the stsd entry,
             * while mov has it in a wave atom */
            /* alac now refers to stsd entry atom */
            if (wave)
              alac = qtdemux_tree_get_child_by_type (wave, FOURCC_alac);
            else
              alac = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_alac);
            if (alac) {
              const guint8 *alac_data = alac->data;
              gint len = QT_UINT32 (alac->data);
              GstBuffer *buf;

              if (len < 36) {
                GST_DEBUG_OBJECT (qtdemux,
                    "discarding alac atom with unexpected len %d", len);
              } else {
                /* codec-data contains alac atom size and prefix,
                 * ffmpeg likes it that way, not quite gst-ish though ...*/
                buf = gst_buffer_new_and_alloc (len);
                gst_buffer_fill (buf, 0, alac->data, len);
                gst_caps_set_simple (entry->caps,
                    "codec_data", GST_TYPE_BUFFER, buf, NULL);
                gst_buffer_unref (buf);

                entry->bytes_per_frame = QT_UINT32 (alac_data + 12);
                entry->n_channels = QT_UINT8 (alac_data + 21);
                entry->rate = QT_UINT32 (alac_data + 32);
                samplesize = QT_UINT8 (alac_data + 16 + 1);
              }
            }
            gst_caps_set_simple (entry->caps,
                "samplesize", G_TYPE_INT, samplesize, NULL);
            break;
          }
          case FOURCC_fLaC:
          {
            /* The codingname of the sample entry is 'fLaC' */
            /* The 'dfLa' box is added to the sample entry to convey
               initializing information for the decoder. */
            const GNode *dfla =
                qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_dfLa);

            if (dfla) {
              const guint32 len = QT_UINT32 (dfla->data);

              /* Must contain at least dfLa box header (12),
               * METADATA_BLOCK_HEADER (4), METADATA_BLOCK_STREAMINFO (34) */
              if (len < 50) {
                GST_DEBUG_OBJECT (qtdemux,
                    "discarding dfla atom with unexpected len %d", len);
              } else {
                /* skip dfLa header to get the METADATA_BLOCKs */
                const guint8 *metadata_blocks = (guint8 *) dfla->data + 12;
                const guint32 metadata_blocks_len = len - 12;

                gchar *stream_marker = g_strdup ("fLaC");
                GstBuffer *block = gst_buffer_new_wrapped (stream_marker,
                    strlen (stream_marker));

                guint32 index = 0;
                guint32 remainder = 0;
                guint32 block_size = 0;
                gboolean is_last = FALSE;

                GValue array = G_VALUE_INIT;
                GValue value = G_VALUE_INIT;

                g_value_init (&array, GST_TYPE_ARRAY);
                g_value_init (&value, GST_TYPE_BUFFER);

                gst_value_set_buffer (&value, block);
                gst_value_array_append_value (&array, &value);
                g_value_reset (&value);

                gst_buffer_unref (block);

                /* check there's at least one METADATA_BLOCK_HEADER's worth
                 * of data, and we haven't already finished parsing */
                while (!is_last && ((index + 3) < metadata_blocks_len)) {
                  remainder = metadata_blocks_len - index;

                  /* add the METADATA_BLOCK_HEADER size to the signalled size */
                  block_size = 4 +
                      (metadata_blocks[index + 1] << 16) +
                      (metadata_blocks[index + 2] << 8) +
                      metadata_blocks[index + 3];

                  /* be careful not to read off end of box */
                  if (block_size > remainder) {
                    break;
                  }

                  is_last = metadata_blocks[index] >> 7;

                  block = gst_buffer_new_and_alloc (block_size);

                  gst_buffer_fill (block, 0, &metadata_blocks[index],
                      block_size);

                  gst_value_set_buffer (&value, block);
                  gst_value_array_append_value (&array, &value);
                  g_value_reset (&value);

                  gst_buffer_unref (block);

                  index += block_size;
                }

                /* only append the metadata if we successfully read all of it */
                if (is_last) {
                  gst_structure_set_value (gst_caps_get_structure (CUR_STREAM
                          (stream)->caps, 0), "streamheader", &array);
                } else {
                  GST_WARNING_OBJECT (qtdemux,
                      "discarding all METADATA_BLOCKs due to invalid "
                      "block_size %d at idx %d, rem %d", block_size, index,
                      remainder);
                }

                g_value_unset (&value);
                g_value_unset (&array);

                /* The sample rate obtained from the stsd may not be accurate
                 * since it cannot represent rates greater than 65535Hz, so
                 * override that value with the sample rate from the
                 * METADATA_BLOCK_STREAMINFO block */
                CUR_STREAM (stream)->rate =
                    (QT_UINT32 (metadata_blocks + 14) >> 12) & 0xFFFFF;
              }
            }
            break;
          }
          case FOURCC_sawb:
            amrwb = TRUE;
            /* FALLTHROUGH */
          case FOURCC_samr:
          {
            const GNode *damr =
                qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_damr);
            if (damr) {
              guint32 len = QT_UINT32 (damr->data);
              GstBuffer *buf = gst_buffer_new_and_alloc (len);
              guint bitrate;

              gst_buffer_fill (buf, 0, damr->data, len);

              gst_caps_set_simple (entry->caps,
                  "codec_data", GST_TYPE_BUFFER, buf, NULL);
              gst_buffer_unref (buf);

              /* If we have enough data, let's try to get the 'damr' atom. See
               * the 3GPP container spec (26.244) for more details. */
              if (len > 8 &&
                  (bitrate =
                      qtdemux_parse_amr_bitrate (damr->data, len, amrwb))) {
                gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
                    GST_TAG_MAXIMUM_BITRATE, bitrate, NULL);
              }
            }
            break;
          }
          case FOURCC_mp4a:
          {
            /* mp4a atom withtout ESDS; Attempt to build codec data from atom */
            /* FIXME: Can this be determined somehow? There doesn't seem to be
             * anything in mp4a atom that specifies compression */
            gint profile = 2;
            guint16 channels = entry->n_channels;
            guint32 sample_rate = (guint32) entry->rate;
            gint sample_rate_index = -1;

            sample_rate_index =
                gst_codec_utils_aac_get_index_from_sample_rate (sample_rate);
            if (sample_rate_index >= 0 && channels > 0) {
              guint8 codec_data[2];
              GstBuffer *buf;

              /* build AAC codec data */
              codec_data[0] = profile << 3;
              codec_data[0] |= ((sample_rate_index >> 1) & 0x7);
              codec_data[1] = (sample_rate_index & 0x01) << 7;
              codec_data[1] |= (channels & 0xF) << 3;

              buf = gst_buffer_new_and_alloc (2);
              gst_buffer_fill (buf, 0, codec_data, 2);
              gst_caps_set_simple (entry->caps,
                  "codec_data", GST_TYPE_BUFFER, buf, NULL);
              gst_buffer_unref (buf);
            }
            break;
          }
          case FOURCC_opus:{
            /* Fully handled elsewhere */
            break;
          }
          case FOURCC_raw_:
          case FOURCC_sowt:
          case FOURCC_twos:
          case FOURCC_lpcm:
          case FOURCC_ipcm:
          case FOURCC_fpcm:
          case FOURCC_in24:
          case FOURCC_in32:
          case FOURCC_fl32:
          case FOURCC_fl64:
          case FOURCC_s16l:{
            GNode *chnl, *chan;

            // Parse channel layout information for raw PCM
            chnl = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_chnl);
            chan = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_chan);

            if (chnl) {
              const guint8 *data = chnl->data;
              gsize len = QT_UINT32 (data);
              if (len >= 8 + 4) {
                GstByteReader br = GST_BYTE_READER_INIT (data, len);
                // Skip over fourcc and length
                gst_byte_reader_skip_unchecked (&br, 4 + 4);
                qtdemux_parse_chnl (qtdemux, &br, stream, entry);
              }
            } else if (chan) {
              const guint8 *data = chan->data;
              gsize len = QT_UINT32 (data);
              if (len >= 8 + 4) {
                GstByteReader br = GST_BYTE_READER_INIT (data, len);
                // Skip over fourcc and length
                gst_byte_reader_skip_unchecked (&br, 4 + 4);
                qtdemux_parse_chan (qtdemux, &br, stream, entry);
              }
            } else {
              GST_DEBUG_OBJECT (qtdemux,
                  "Configuring default channel mask for %u channels",
                  entry->n_channels);

              if (entry->n_channels > 1) {
                // Set a default channel mask if all is unknown
                guint64 default_mask =
                    gst_audio_channel_get_fallback_mask (entry->n_channels);

                gst_caps_set_simple (entry->caps, "channel-mask",
                    GST_TYPE_BITMASK, default_mask, NULL);
              }
            }
            break;
          }
          default:
            GST_INFO_OBJECT (qtdemux,
                "unhandled type %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));
            break;
        }
      }
      GST_INFO_OBJECT (qtdemux,
          "type %" GST_FOURCC_FORMAT " caps %" GST_PTR_FORMAT,
          GST_FOURCC_ARGS (fourcc), entry->caps);

    } else if (stream->subtype == FOURCC_strm) {
      if (fourcc == FOURCC_rtsp) {
        stream->redirect_uri = qtdemux_get_rtsp_uri_from_hndl (qtdemux, minf);
      } else {
        GST_INFO_OBJECT (qtdemux, "unhandled stream type %" GST_FOURCC_FORMAT,
            GST_FOURCC_ARGS (fourcc));
        goto unknown_stream;
      }
      entry->sampled = TRUE;
    } else if (stream->subtype == FOURCC_subp || stream->subtype == FOURCC_text
        || stream->subtype == FOURCC_sbtl || stream->subtype == FOURCC_subt
        || stream->subtype == FOURCC_clcp || stream->subtype == FOURCC_wvtt) {

      entry->sampled = TRUE;
      entry->sparse = TRUE;

      entry->caps =
          qtdemux_sub_caps (qtdemux, stream, entry, fourcc, stsd_entry, &codec);
      if (codec) {
        gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
            GST_TAG_SUBTITLE_CODEC, codec, NULL);
        g_free (codec);
        codec = NULL;
      }

      /* hunt for sort-of codec data */
      switch (fourcc) {
        case FOURCC_mp4s:
        {
          GNode *esds = NULL;

          /* look for palette in a stsd->mp4s->esds sub-atom */
          esds = qtdemux_tree_get_child_by_type (stsd_entry, FOURCC_esds);
          if (esds == NULL) {
            /* Invalid STSD */
            GST_LOG_OBJECT (qtdemux, "Skipping invalid stsd: no esds child");
            break;
          }

          gst_qtdemux_handle_esds (qtdemux, stream, entry, esds,
              stream->stream_tags);
          break;
        }
        default:
          GST_INFO_OBJECT (qtdemux,
              "unhandled type %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));
          break;
      }
      GST_INFO_OBJECT (qtdemux,
          "type %" GST_FOURCC_FORMAT " caps %" GST_PTR_FORMAT,
          GST_FOURCC_ARGS (fourcc), entry->caps);
    } else if (stream->subtype == FOURCC_meta) {
      entry->sampled = TRUE;
      entry->sparse = TRUE;

      entry->caps =
          qtdemux_meta_caps (qtdemux, stream, entry, fourcc, stsd_entry,
          &codec);
      if (codec) {
        gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
            GST_TAG_CODEC, codec, NULL);
        g_free (codec);
        codec = NULL;
      }

      GST_INFO_OBJECT (qtdemux,
          "type %" GST_FOURCC_FORMAT " caps %" GST_PTR_FORMAT,
          GST_FOURCC_ARGS (fourcc), entry->caps);
    } else {
      /* everything in 1 sample */
      entry->sampled = TRUE;

      entry->caps =
          qtdemux_generic_caps (qtdemux, stream, entry, fourcc, stsd_entry,
          &codec);

      if (entry->caps == NULL)
        goto unknown_stream;

      if (codec) {
        gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
            GST_TAG_SUBTITLE_CODEC, codec, NULL);
        g_free (codec);
        codec = NULL;
      }
    }

    /* promote to sampled format */
    if (entry->fourcc == FOURCC_samr) {
      /* force mono 8000 Hz for AMR */
      entry->sampled = TRUE;
      entry->n_channels = 1;
      entry->rate = 8000;
    } else if (entry->fourcc == FOURCC_sawb) {
      /* force mono 16000 Hz for AMR-WB */
      entry->sampled = TRUE;
      entry->n_channels = 1;
      entry->rate = 16000;
    } else if (entry->fourcc == FOURCC_mp4a) {
      entry->sampled = TRUE;
    }
  }

  /* Sample grouping support */
  if (stream->protected && (stream->protection_scheme_type == FOURCC_cenc
          || stream->protection_scheme_type == FOURCC_cbcs)) {
    QtDemuxCencSampleSetInfo *info = stream->protection_scheme_info;
    GNode *sgpd_node;
    GstByteReader sgpd_data;

    if (!info)
      goto corrupt_file;

    if (info->track_group_properties) {
      g_ptr_array_free (info->fragment_group_properties, TRUE);
      info->fragment_group_properties = NULL;
    }

    sgpd_node = qtdemux_tree_get_child_by_type_full (stbl, FOURCC_sgpd,
        &sgpd_data);
    while (sgpd_node) {
      if (qtdemux_parse_sgpd (qtdemux, stream, &sgpd_data, FOURCC_seig,
              &info->track_group_properties)) {
        break;
      }
      sgpd_node = qtdemux_tree_get_sibling_by_type_full (sgpd_node,
          FOURCC_sgpd, &sgpd_data);
    }
  }

  /* collect sample information */
  if (!qtdemux_stbl_init (qtdemux, stream, stbl))
    goto samples_failed;

  if (qtdemux->fragmented) {
    guint64 offset;

    /* need all moov samples as basis; probably not many if any at all */
    /* prevent moof parsing taking of at this time */
    offset = qtdemux->moof_offset;
    qtdemux->moof_offset = 0;
    if (stream->n_samples &&
        !qtdemux_parse_samples (qtdemux, stream, stream->n_samples - 1)) {
      qtdemux->moof_offset = offset;
      goto samples_failed;
    }
    qtdemux->moof_offset = offset;
    /* movie duration more reliable in this case (e.g. mehd) */
    if (qtdemux->segment.duration &&
        GST_CLOCK_TIME_IS_VALID (qtdemux->segment.duration))
      stream->duration =
          GSTTIME_TO_QTSTREAMTIME (stream, qtdemux->segment.duration);
  }

  /* configure segments */
  if (!qtdemux_parse_segments (qtdemux, stream, trak))
    goto segments_failed;

  /* add some language tag, if useful */
  if (stream->lang_id[0] != '\0' && strcmp (stream->lang_id, "unk") &&
      strcmp (stream->lang_id, "und")) {
    const gchar *lang_code;

    /* convert ISO 639-2 code to ISO 639-1 */
    lang_code = gst_tag_get_language_code (stream->lang_id);
    gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
        GST_TAG_LANGUAGE_CODE, (lang_code) ? lang_code : stream->lang_id, NULL);
  }

  /* https://dev.w3.org/html5/html-sourcing-inband-tracks/#mpeg4
   * FIXME: For CEA 608 and CEA 708 we should use the channel_number and
   * service_number respectively.
   */
  if (stream->track_id) {
    gchar *track_id_str =
        g_strdup_printf ("%" G_GUINT32_FORMAT, stream->track_id);
    gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
        GST_TAG_CONTAINER_SPECIFIC_TRACK_ID, track_id_str, NULL);
    g_free (track_id_str);
  }

  /* Check for UDTA tags */
  if ((udta = qtdemux_tree_get_child_by_type (trak, FOURCC_udta))) {
    qtdemux_parse_udta (qtdemux, stream->stream_tags, udta);
  }

  /* Insert and sort new stream in track-id order.
   * This will help in comparing old/new streams during stream update check */
  g_ptr_array_add (qtdemux->active_streams, stream);
  g_ptr_array_sort (qtdemux->active_streams,
      (GCompareFunc) qtdemux_track_id_compare_func);
  GST_DEBUG_OBJECT (qtdemux, "n_streams is now %d",
      QTDEMUX_N_STREAMS (qtdemux));

  return TRUE;

/* ERRORS */
corrupt_file:
  {
    GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
        (_("This file is corrupt and cannot be played.")), (NULL));
    if (stream)
      gst_qtdemux_stream_unref (stream);
    return FALSE;
  }
error_encrypted:
  {
    GST_ELEMENT_ERROR (qtdemux, STREAM, DECRYPT, (NULL), (NULL));
    gst_qtdemux_stream_unref (stream);
    return FALSE;
  }
samples_failed:
segments_failed:
  {
    /* we posted an error already */
    /* free stbl sub-atoms */
    gst_qtdemux_stbl_free (stream);
    gst_qtdemux_stream_unref (stream);
    return FALSE;
  }
existing_stream:
  {
    GST_INFO_OBJECT (qtdemux, "stream with track id %i already exists",
        track_id);
    return TRUE;
  }
unknown_stream:
  {
    GST_INFO_OBJECT (qtdemux, "unknown subtype %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (stream->subtype));
    gst_qtdemux_stream_unref (stream);
    return TRUE;
  }
}

/* If we can estimate the overall bitrate, and don't have information about the
 * stream bitrate for exactly one stream, this guesses the stream bitrate as
 * the overall bitrate minus the sum of the bitrates of all other streams. This
 * should be useful for the common case where we have one audio and one video
 * stream and can estimate the bitrate of one, but not the other. */
static void
gst_qtdemux_guess_bitrate (GstQTDemux * qtdemux)
{
  QtDemuxStream *stream = NULL;
  gint64 size, sys_bitrate, sum_bitrate = 0;
  GstClockTime duration;
  guint bitrate;
  gint i;

  if (qtdemux->fragmented)
    return;

  GST_DEBUG_OBJECT (qtdemux, "Looking for streams with unknown bitrate");

  if (!gst_pad_peer_query_duration (qtdemux->sinkpad, GST_FORMAT_BYTES, &size)
      || size <= 0) {
    GST_DEBUG_OBJECT (qtdemux,
        "Size in bytes of the stream not known - bailing");
    return;
  }

  /* Subtract the header size */
  GST_DEBUG_OBJECT (qtdemux, "Total size %" G_GINT64_FORMAT ", header size %u",
      size, qtdemux->header_size);

  if (size < qtdemux->header_size)
    return;

  size = size - qtdemux->header_size;

  if (!gst_qtdemux_get_duration (qtdemux, &duration)) {
    GST_DEBUG_OBJECT (qtdemux, "Stream duration not known - bailing");
    return;
  }

  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *str = QTDEMUX_NTH_STREAM (qtdemux, i);
    switch (str->subtype) {
      case FOURCC_soun:
      case FOURCC_vide:
        GST_DEBUG_OBJECT (qtdemux, "checking bitrate for %" GST_PTR_FORMAT,
            CUR_STREAM (str)->caps);
        /* retrieve bitrate, prefer avg then max */
        bitrate = 0;
        if (str->stream_tags) {
          if (gst_tag_list_get_uint (str->stream_tags,
                  GST_TAG_MAXIMUM_BITRATE, &bitrate))
            GST_DEBUG_OBJECT (qtdemux, "max-bitrate: %u", bitrate);
          if (gst_tag_list_get_uint (str->stream_tags,
                  GST_TAG_NOMINAL_BITRATE, &bitrate))
            GST_DEBUG_OBJECT (qtdemux, "nominal-bitrate: %u", bitrate);
          if (gst_tag_list_get_uint (str->stream_tags,
                  GST_TAG_BITRATE, &bitrate))
            GST_DEBUG_OBJECT (qtdemux, "bitrate: %u", bitrate);
        }
        if (bitrate)
          sum_bitrate += bitrate;
        else {
          if (stream) {
            GST_DEBUG_OBJECT (qtdemux,
                ">1 stream with unknown bitrate - bailing");
            return;
          } else
            stream = str;
        }

      default:
        /* For other subtypes, we assume no significant impact on bitrate */
        break;
    }
  }

  if (!stream) {
    GST_DEBUG_OBJECT (qtdemux, "All stream bitrates are known");
    return;
  }

  sys_bitrate = gst_util_uint64_scale (size, GST_SECOND * 8, duration);

  if (sys_bitrate < sum_bitrate) {
    /* This can happen, since sum_bitrate might be derived from maximum
     * bitrates and not average bitrates */
    GST_DEBUG_OBJECT (qtdemux,
        "System bitrate less than sum bitrate - bailing");
    return;
  }

  bitrate = sys_bitrate - sum_bitrate;
  GST_DEBUG_OBJECT (qtdemux, "System bitrate = %" G_GINT64_FORMAT
      ", Stream bitrate = %u", sys_bitrate, bitrate);

  if (!stream->stream_tags)
    stream->stream_tags = gst_tag_list_new_empty ();
  else
    stream->stream_tags = gst_tag_list_make_writable (stream->stream_tags);

  gst_tag_list_add (stream->stream_tags, GST_TAG_MERGE_REPLACE,
      GST_TAG_BITRATE, bitrate, NULL);
}

static GstFlowReturn
qtdemux_prepare_streams (GstQTDemux * qtdemux)
{
  GstFlowReturn ret = GST_FLOW_OK;
  gint i;

  GST_DEBUG_OBJECT (qtdemux, "prepare %u streams", QTDEMUX_N_STREAMS (qtdemux));

  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    guint32 sample_num = 0;

    GST_DEBUG_OBJECT (qtdemux, "track-id %u, fourcc %" GST_FOURCC_FORMAT,
        stream->track_id, GST_FOURCC_ARGS (CUR_STREAM (stream)->fourcc));

    if (qtdemux->fragmented && qtdemux->pullbased) {
      /* need all moov samples first */
      GST_OBJECT_LOCK (qtdemux);
      while (stream->n_samples == 0)
        if ((ret = qtdemux_add_fragmented_samples (qtdemux)) != GST_FLOW_OK)
          break;
      GST_OBJECT_UNLOCK (qtdemux);
    } else {
      /* discard any stray moof */
      qtdemux->moof_offset = 0;
    }

    /* prepare braking */
    if (ret != GST_FLOW_ERROR)
      ret = GST_FLOW_OK;

    /* in pull mode, we should have parsed some sample info by now;
     * and quite some code will not handle no samples.
     * in push mode, we'll just have to deal with it */
    if (G_UNLIKELY (qtdemux->pullbased && !stream->n_samples)) {
      GST_DEBUG_OBJECT (qtdemux, "no samples for stream; discarding");
      g_ptr_array_remove_index (qtdemux->active_streams, i);
      i--;
      continue;
    } else if (stream->track_id == qtdemux->chapters_track_id &&
        (stream->subtype == FOURCC_text || stream->subtype == FOURCC_sbtl)) {
      /* TODO - parse chapters track and expose it as GstToc; For now just ignore it
         so that it doesn't look like a subtitle track */
      g_ptr_array_remove_index (qtdemux->active_streams, i);
      i--;
      continue;
    }

    /* parse the initial sample for use in setting the frame rate cap */
    while (sample_num == 0 && sample_num < stream->n_samples) {
      if (!qtdemux_parse_samples (qtdemux, stream, sample_num)) {
        ret = GST_FLOW_ERROR;
        break;
      }
      ++sample_num;
    }
  }

  qtdemux_check_if_is_gapless_audio (qtdemux);

  return ret;
}

static gboolean
_stream_equal_func (const QtDemuxStream * stream, const gchar * stream_id)
{
  return g_strcmp0 (stream->stream_id, stream_id) == 0;
}

static gboolean
qtdemux_is_streams_update (GstQTDemux * qtdemux)
{
  gint i;

  /* Different length, updated */
  if (QTDEMUX_N_STREAMS (qtdemux) != qtdemux->old_streams->len)
    return TRUE;

  /* streams in list are sorted in track-id order */
  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    /* Different stream-id, updated */
    if (g_strcmp0 (QTDEMUX_NTH_STREAM (qtdemux, i)->stream_id,
            QTDEMUX_NTH_OLD_STREAM (qtdemux, i)->stream_id))
      return TRUE;
  }

  return FALSE;
}

static gboolean
qtdemux_reuse_and_configure_stream (GstQTDemux * qtdemux,
    QtDemuxStream * oldstream, QtDemuxStream * newstream)
{
  /* Connect old stream's srcpad to new stream */
  newstream->pad = oldstream->pad;
  oldstream->pad = NULL;

  /* unset new_stream to prevent stream-start event, unless we are EOS in which
   * case we need to force one through */
  newstream->new_stream = newstream->pad != NULL
      && GST_PAD_IS_EOS (newstream->pad);

  return gst_qtdemux_configure_stream (qtdemux, newstream);
}

static gboolean
qtdemux_update_streams (GstQTDemux * qtdemux)
{
  gint i;
  g_assert (qtdemux->streams_aware);

  /* At below, figure out which stream in active_streams has identical stream-id
   * with that of in old_streams. If there is matching stream-id,
   * corresponding newstream will not be exposed again,
   * but demux will reuse srcpad of matched old stream
   *
   * active_streams : newly created streams from the latest moov
   * old_streams : existing streams (belong to previous moov)
   */

  for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
    QtDemuxStream *oldstream = NULL;
    guint target;

    GST_DEBUG_OBJECT (qtdemux, "track-id %u, fourcc %" GST_FOURCC_FORMAT,
        stream->track_id, GST_FOURCC_ARGS (CUR_STREAM (stream)->fourcc));

    if (g_ptr_array_find_with_equal_func (qtdemux->old_streams,
            stream->stream_id, (GEqualFunc) _stream_equal_func, &target)) {
      oldstream = QTDEMUX_NTH_OLD_STREAM (qtdemux, target);

      /* null pad stream cannot be reused */
      if (oldstream->pad == NULL)
        oldstream = NULL;
    }

    if (oldstream) {
      GST_DEBUG_OBJECT (qtdemux, "Reuse track-id %d", oldstream->track_id);

      if (!qtdemux_reuse_and_configure_stream (qtdemux, oldstream, stream))
        return FALSE;

      /* we don't need to preserve order of old streams */
      g_ptr_array_remove_fast (qtdemux->old_streams, oldstream);
    } else {
      GstTagList *list;

      /* now we have all info and can expose */
      list = stream->stream_tags;
      stream->stream_tags = NULL;
      if (!gst_qtdemux_add_stream (qtdemux, stream, list))
        return FALSE;
    }
  }

  return TRUE;
}

/* Must be called with expose lock */
static GstFlowReturn
qtdemux_expose_streams (GstQTDemux * qtdemux)
{
  gint i;

  GST_DEBUG_OBJECT (qtdemux, "exposing streams");

  if (!qtdemux_is_streams_update (qtdemux)) {
    GST_DEBUG_OBJECT (qtdemux, "Reuse all streams");
    for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
      QtDemuxStream *new_stream = QTDEMUX_NTH_STREAM (qtdemux, i);
      QtDemuxStream *old_stream = QTDEMUX_NTH_OLD_STREAM (qtdemux, i);
      if (!qtdemux_reuse_and_configure_stream (qtdemux, old_stream, new_stream))
        return GST_FLOW_ERROR;
    }

    g_ptr_array_set_size (qtdemux->old_streams, 0);
    qtdemux->need_segment = TRUE;

    return GST_FLOW_OK;
  }

  if (qtdemux->streams_aware) {
    if (!qtdemux_update_streams (qtdemux))
      return GST_FLOW_ERROR;
  } else {
    for (i = 0; i < QTDEMUX_N_STREAMS (qtdemux); i++) {
      QtDemuxStream *stream = QTDEMUX_NTH_STREAM (qtdemux, i);
      GstTagList *list;

      /* now we have all info and can expose */
      list = stream->stream_tags;
      stream->stream_tags = NULL;
      if (!gst_qtdemux_add_stream (qtdemux, stream, list))
        return GST_FLOW_ERROR;

    }
  }

  gst_qtdemux_guess_bitrate (qtdemux);

  /* If we have still old_streams, it's no more used stream */
  for (i = 0; i < qtdemux->old_streams->len; i++) {
    QtDemuxStream *stream = QTDEMUX_NTH_OLD_STREAM (qtdemux, i);

    if (stream->pad) {
      GstEvent *event;

      event = gst_event_new_eos ();
      if (qtdemux->segment_seqnum)
        gst_event_set_seqnum (event, qtdemux->segment_seqnum);

      gst_pad_push_event (stream->pad, event);
    }
  }

  g_ptr_array_set_size (qtdemux->old_streams, 0);

  gst_element_no_more_pads (GST_ELEMENT_CAST (qtdemux));

  /* check if we should post a redirect in case there is a single trak
   * and it is a redirecting trak */
  if (QTDEMUX_N_STREAMS (qtdemux) == 1 &&
      QTDEMUX_NTH_STREAM (qtdemux, 0)->redirect_uri != NULL) {
    GstMessage *m;

    GST_INFO_OBJECT (qtdemux, "Issuing a redirect due to a single track with "
        "an external content");
    m = gst_message_new_element (GST_OBJECT_CAST (qtdemux),
        gst_structure_new ("redirect",
            "new-location", G_TYPE_STRING,
            QTDEMUX_NTH_STREAM (qtdemux, 0)->redirect_uri, NULL));
    gst_element_post_message (GST_ELEMENT_CAST (qtdemux), m);
    g_free (qtdemux->redirect_location);
    qtdemux->redirect_location =
        g_strdup (QTDEMUX_NTH_STREAM (qtdemux, 0)->redirect_uri);
  }

  g_ptr_array_foreach (qtdemux->active_streams,
      (GFunc) qtdemux_do_allocation, qtdemux);

  qtdemux->need_segment = TRUE;

  qtdemux->exposed = TRUE;
  return GST_FLOW_OK;
}

typedef struct
{
  GstStructure *structure;      /* helper for sort function */
  gchar *location;
  guint min_req_bitrate;
  guint min_req_qt_version;
} GstQtReference;

static gint
qtdemux_redirects_sort_func (gconstpointer a, gconstpointer b)
{
  GstQtReference *ref_a = (GstQtReference *) a;
  GstQtReference *ref_b = (GstQtReference *) b;

  if (ref_b->min_req_qt_version != ref_a->min_req_qt_version)
    return ref_b->min_req_qt_version - ref_a->min_req_qt_version;

  /* known bitrates go before unknown; higher bitrates go first */
  return ref_b->min_req_bitrate - ref_a->min_req_bitrate;
}

/* sort the redirects and post a message for the application.
 */
static void
qtdemux_process_redirects (GstQTDemux * qtdemux, GList * references)
{
  GstQtReference *best;
  GstStructure *s;
  GstMessage *msg;
  GValue list_val = { 0, };
  GList *l;

  g_assert (references != NULL);

  references = g_list_sort (references, qtdemux_redirects_sort_func);

  best = (GstQtReference *) references->data;

  g_value_init (&list_val, GST_TYPE_LIST);

  for (l = references; l != NULL; l = l->next) {
    GstQtReference *ref = (GstQtReference *) l->data;
    GValue struct_val = { 0, };

    ref->structure = gst_structure_new ("redirect",
        "new-location", G_TYPE_STRING, ref->location, NULL);

    if (ref->min_req_bitrate > 0) {
      gst_structure_set (ref->structure, "minimum-bitrate", G_TYPE_INT,
          ref->min_req_bitrate, NULL);
    }

    g_value_init (&struct_val, GST_TYPE_STRUCTURE);
    g_value_set_boxed (&struct_val, ref->structure);
    gst_value_list_append_value (&list_val, &struct_val);
    g_value_unset (&struct_val);
    /* don't free anything here yet, since we need best->structure below */
  }

  g_assert (best != NULL);
  s = gst_structure_copy (best->structure);

  if (g_list_length (references) > 1) {
    gst_structure_set_value (s, "locations", &list_val);
  }

  g_value_unset (&list_val);

  for (l = references; l != NULL; l = l->next) {
    GstQtReference *ref = (GstQtReference *) l->data;

    gst_structure_free (ref->structure);
    g_free (ref->location);
    g_free (ref);
  }
  g_list_free (references);

  GST_INFO_OBJECT (qtdemux, "posting redirect message: %" GST_PTR_FORMAT, s);
  g_free (qtdemux->redirect_location);
  qtdemux->redirect_location =
      g_strdup (gst_structure_get_string (s, "new-location"));
  msg = gst_message_new_element (GST_OBJECT_CAST (qtdemux), s);
  gst_element_post_message (GST_ELEMENT_CAST (qtdemux), msg);
}

/* look for redirect nodes, collect all redirect information and
 * process it.
 */
static gboolean
qtdemux_parse_redirects (GstQTDemux * qtdemux)
{
  GNode *rmra, *rmda, *rdrf;

  rmra = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_rmra);
  if (rmra) {
    GList *redirects = NULL;

    rmda = qtdemux_tree_get_child_by_type (rmra, FOURCC_rmda);
    while (rmda) {
      GstQtReference ref = { NULL, NULL, 0, 0 };
      GNode *rmdr, *rmvc;

      if ((rmdr = qtdemux_tree_get_child_by_type (rmda, FOURCC_rmdr))) {
        ref.min_req_bitrate = QT_UINT32 ((guint8 *) rmdr->data + 12);
        GST_LOG_OBJECT (qtdemux, "data rate atom, required bitrate = %u",
            ref.min_req_bitrate);
      }

      if ((rmvc = qtdemux_tree_get_child_by_type (rmda, FOURCC_rmvc))) {
        guint32 package = QT_FOURCC ((guint8 *) rmvc->data + 12);
        guint version = QT_UINT32 ((guint8 *) rmvc->data + 16);

#ifndef GST_DISABLE_GST_DEBUG
        guint bitmask = QT_UINT32 ((guint8 *) rmvc->data + 20);
#endif
        guint check_type = QT_UINT16 ((guint8 *) rmvc->data + 24);

        GST_LOG_OBJECT (qtdemux,
            "version check atom [%" GST_FOURCC_FORMAT "], version=0x%08x"
            ", mask=%08x, check_type=%u", GST_FOURCC_ARGS (package), version,
            bitmask, check_type);
        if (package == FOURCC_qtim && check_type == 0) {
          ref.min_req_qt_version = version;
        }
      }

      rdrf = qtdemux_tree_get_child_by_type (rmda, FOURCC_rdrf);
      if (rdrf) {
        guint32 ref_type;
        guint8 *ref_data;
        guint ref_len;

        ref_len = QT_UINT32 ((guint8 *) rdrf->data);
        if (ref_len > 20) {
          ref_type = QT_FOURCC ((guint8 *) rdrf->data + 12);
          ref_data = (guint8 *) rdrf->data + 20;
          if (ref_type == FOURCC_alis) {
            guint record_len, record_version, fn_len;

            if (ref_len > 70) {
              /* MacOSX alias record, google for alias-layout.txt */
              record_len = QT_UINT16 (ref_data + 4);
              record_version = QT_UINT16 (ref_data + 4 + 2);
              fn_len = QT_UINT8 (ref_data + 50);
              if (record_len > 50 && record_version == 2 && fn_len > 0) {
                ref.location = g_strndup ((gchar *) ref_data + 51, fn_len);
              }
            } else {
              GST_WARNING_OBJECT (qtdemux, "Invalid rdrf/alis size (%u < 70)",
                  ref_len);
            }
          } else if (ref_type == FOURCC_url_) {
            ref.location = g_strndup ((gchar *) ref_data, ref_len - 8);
          } else {
            GST_DEBUG_OBJECT (qtdemux,
                "unknown rdrf reference type %" GST_FOURCC_FORMAT,
                GST_FOURCC_ARGS (ref_type));
          }
          if (ref.location != NULL) {
            GST_INFO_OBJECT (qtdemux, "New location: %s", ref.location);
            redirects =
                g_list_prepend (redirects, g_memdup2 (&ref, sizeof (ref)));
          } else {
            GST_WARNING_OBJECT (qtdemux,
                "Failed to extract redirect location from rdrf atom");
          }
        } else {
          GST_WARNING_OBJECT (qtdemux, "Invalid rdrf size (%u < 20)", ref_len);
        }
      }

      /* look for others */
      rmda = qtdemux_tree_get_sibling_by_type (rmda, FOURCC_rmda);
    }

    if (redirects != NULL) {
      qtdemux_process_redirects (qtdemux, redirects);
    }
  }
  return TRUE;
}

static GstTagList *
qtdemux_add_container_format (GstQTDemux * qtdemux, GstTagList * tags)
{
  const gchar *fmt;

  if (tags == NULL) {
    tags = gst_tag_list_new_empty ();
    gst_tag_list_set_scope (tags, GST_TAG_SCOPE_GLOBAL);
  }

  if (qtdemux->major_brand == FOURCC_mjp2)
    fmt = "Motion JPEG 2000";
  else if ((qtdemux->major_brand & 0xffff) == FOURCC_3g__)
    fmt = "3GP";
  else if (qtdemux->major_brand == FOURCC_qt__)
    fmt = "Quicktime";
  else if (qtdemux->fragmented)
    fmt = "ISO fMP4";
  else
    fmt = "ISO MP4/M4A";

  GST_LOG_OBJECT (qtdemux, "mapped %" GST_FOURCC_FORMAT " to '%s'",
      GST_FOURCC_ARGS (qtdemux->major_brand), fmt);

  gst_tag_list_add (tags, GST_TAG_MERGE_REPLACE, GST_TAG_CONTAINER_FORMAT,
      fmt, NULL);

  return tags;
}

/* we have read the complete moov node now.
 * This function parses all of the relevant info, creates the traks and
 * prepares all data structures for playback
 */
static gboolean
qtdemux_parse_tree (GstQTDemux * qtdemux)
{
  GNode *mvhd;
  GNode *trak;
  GNode *udta;
  GNode *mvex;
  GNode *pssh;
  guint64 creation_time;
  GstDateTime *datetime = NULL;
  guint8 version;
  GstByteReader mvhd_reader;
  guint32 matrix[9];

  /* make sure we have a usable taglist */
  qtdemux->tag_list = gst_tag_list_make_writable (qtdemux->tag_list);

  mvhd = qtdemux_tree_get_child_by_type_full (qtdemux->moov_node,
      FOURCC_mvhd, &mvhd_reader);
  if (mvhd == NULL) {
    GST_LOG_OBJECT (qtdemux, "No mvhd node found, looking for redirects.");
    return qtdemux_parse_redirects (qtdemux);
  }

  if (!gst_byte_reader_get_uint8 (&mvhd_reader, &version))
    return FALSE;
  /* flags */
  if (!gst_byte_reader_skip (&mvhd_reader, 3))
    return FALSE;
  if (version == 1) {
    if (!gst_byte_reader_get_uint64_be (&mvhd_reader, &creation_time))
      return FALSE;
    /* modification time */
    if (!gst_byte_reader_skip (&mvhd_reader, 8))
      return FALSE;
    if (!gst_byte_reader_get_uint32_be (&mvhd_reader, &qtdemux->timescale))
      return FALSE;
    if (!gst_byte_reader_get_uint64_be (&mvhd_reader, &qtdemux->duration))
      return FALSE;
  } else if (version == 0) {
    guint32 tmp;

    if (!gst_byte_reader_get_uint32_be (&mvhd_reader, &tmp))
      return FALSE;
    creation_time = tmp;
    /* modification time */
    if (!gst_byte_reader_skip (&mvhd_reader, 4))
      return FALSE;
    if (!gst_byte_reader_get_uint32_be (&mvhd_reader, &qtdemux->timescale))
      return FALSE;
    if (!gst_byte_reader_get_uint32_be (&mvhd_reader, &tmp))
      return FALSE;
    qtdemux->duration = tmp;
  } else {
    GST_WARNING_OBJECT (qtdemux, "Unhandled mvhd version %d", version);
    return FALSE;
  }

  if (!gst_byte_reader_skip (&mvhd_reader, 4 + 2 + 2 + 2 * 4))
    return FALSE;

  if (!qtdemux_parse_transformation_matrix (qtdemux, &mvhd_reader, matrix,
          "mvhd"))
    return FALSE;

  /* Moving qt creation time (secs since 1904) to unix time */
  if (creation_time != 0) {
    /* Try to use epoch first as it should be faster and more commonly found */
    if (creation_time >= QTDEMUX_SECONDS_FROM_1904_TO_1970) {
      gint64 now_s;

      creation_time -= QTDEMUX_SECONDS_FROM_1904_TO_1970;
      /* some data cleansing sanity */
      now_s = g_get_real_time () / G_USEC_PER_SEC;
      if (now_s + 24 * 3600 < creation_time) {
        GST_DEBUG_OBJECT (qtdemux, "discarding bogus future creation time");
      } else {
        datetime = gst_date_time_new_from_unix_epoch_utc (creation_time);
      }
    } else {
      GDateTime *base_dt = g_date_time_new_utc (1904, 1, 1, 0, 0, 0);
      GDateTime *dt, *dt_local;

      dt = g_date_time_add_seconds (base_dt, creation_time);
      dt_local = g_date_time_to_local (dt);
      datetime = gst_date_time_new_from_g_date_time (dt_local);

      g_date_time_unref (base_dt);
      g_date_time_unref (dt);
    }
  }
  if (datetime) {
    /* Use KEEP as explicit tags should have a higher priority than mvhd tag */
    gst_tag_list_add (qtdemux->tag_list, GST_TAG_MERGE_KEEP, GST_TAG_DATE_TIME,
        datetime, NULL);
    gst_date_time_unref (datetime);
  }

  GST_INFO_OBJECT (qtdemux, "timescale: %u", qtdemux->timescale);
  GST_INFO_OBJECT (qtdemux, "duration: %" G_GUINT64_FORMAT, qtdemux->duration);

  /* check for fragmented file and get some (default) data */
  mvex = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_mvex);
  if (mvex) {
    GNode *mehd;
    GstByteReader mehd_data;

    /* let track parsing or anyone know weird stuff might happen ... */
    qtdemux->fragmented = TRUE;

    /* compensate for total duration */
    mehd = qtdemux_tree_get_child_by_type_full (mvex, FOURCC_mehd, &mehd_data);
    if (mehd)
      qtdemux_parse_mehd (qtdemux, &mehd_data);
  }

  /* Update the movie segment duration, unless it was directly given to us
   * by upstream. Otherwise let it as is, as we don't want to mangle the
   * duration provided by upstream that may come e.g. from a MPD file. */
  if (!qtdemux->upstream_format_is_time) {
    GstClockTime duration;
    /* set duration in the segment info */
    gst_qtdemux_get_duration (qtdemux, &duration);
    qtdemux->segment.duration = duration;
    /* also do not exceed duration; stop is set that way post seek anyway,
     * and segment activation falls back to duration,
     * whereas loop only checks stop, so let's align this here as well */
    qtdemux->segment.stop = duration;
  }

  /* parse all traks */
  trak = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_trak);
  while (trak) {
    qtdemux_parse_trak (qtdemux, trak, matrix);
    /* iterate all siblings */
    trak = qtdemux_tree_get_sibling_by_type (trak, FOURCC_trak);
  }

  qtdemux->tag_list = gst_tag_list_make_writable (qtdemux->tag_list);

  /* find tags */
  udta = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_udta);
  if (udta) {
    qtdemux_parse_udta (qtdemux, qtdemux->tag_list, udta);
  } else {
    GST_LOG_OBJECT (qtdemux, "No udta node found.");
  }

  /* maybe also some tags in meta box */
  udta = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_meta);
  if (udta) {
    GST_DEBUG_OBJECT (qtdemux, "Parsing meta box for tags.");
    qtdemux_parse_udta (qtdemux, qtdemux->tag_list, udta);
  } else {
    GST_LOG_OBJECT (qtdemux, "No meta node found.");
  }

  /* parse any protection system info */
  pssh = qtdemux_tree_get_child_by_type (qtdemux->moov_node, FOURCC_pssh);
  if (pssh) {
    /* Unref old protection events if we are going to receive new ones. */
    qtdemux_clear_protection_events_on_all_streams (qtdemux);
  }
  while (pssh) {
    GST_LOG_OBJECT (qtdemux, "Parsing pssh box.");
    qtdemux_parse_pssh (qtdemux, pssh);
    pssh = qtdemux_tree_get_sibling_by_type (pssh, FOURCC_pssh);
  }

  qtdemux->tag_list = qtdemux_add_container_format (qtdemux, qtdemux->tag_list);

  return TRUE;
}

/* taken from ffmpeg */
static int
read_descr_size (guint8 * ptr, guint8 * end, guint8 ** end_out)
{
  int count = 4;
  int len = 0;

  while (count--) {
    int c;

    if (ptr >= end)
      return -1;

    c = *ptr++;
    len = (len << 7) | (c & 0x7f);
    if (!(c & 0x80))
      break;
  }
  *end_out = ptr;
  return len;
}

static GList *
parse_xiph_stream_headers (GstQTDemux * qtdemux, gpointer codec_data,
    gsize codec_data_size)
{
  GList *list = NULL;
  guint8 *p = codec_data;
  gint i, offset, num_packets;
  guint *length, last;

  GST_MEMDUMP_OBJECT (qtdemux, "xiph codec data", codec_data, codec_data_size);

  if (codec_data == NULL || codec_data_size == 0)
    goto error;

  /* start of the stream and vorbis audio or theora video, need to
   * send the codec_priv data as first three packets */
  num_packets = p[0] + 1;
  GST_DEBUG_OBJECT (qtdemux,
      "%u stream headers, total length=%" G_GSIZE_FORMAT " bytes",
      (guint) num_packets, codec_data_size);

  /* Let's put some limits, Don't think there even is a xiph codec
   * with more than 3-4 headers */
  if (G_UNLIKELY (num_packets > 16)) {
    GST_WARNING_OBJECT (qtdemux,
        "Unlikely number of xiph headers, most likely not valid");
    goto error;
  }

  length = g_alloca (num_packets * sizeof (guint));
  last = 0;
  offset = 1;

  /* first packets, read length values */
  for (i = 0; i < num_packets - 1; i++) {
    length[i] = 0;
    while (offset < codec_data_size) {
      length[i] += p[offset];
      if (p[offset++] != 0xff)
        break;
    }
    last += length[i];
  }
  if (offset + last > codec_data_size)
    goto error;

  /* last packet is the remaining size */
  length[i] = codec_data_size - offset - last;

  for (i = 0; i < num_packets; i++) {
    GstBuffer *hdr;

    GST_DEBUG_OBJECT (qtdemux, "buffer %d: %u bytes", i, (guint) length[i]);

    if (offset + length[i] > codec_data_size)
      goto error;

    hdr = gst_buffer_new_memdup (p + offset, length[i]);
    list = g_list_append (list, hdr);

    offset += length[i];
  }

  return list;

  /* ERRORS */
error:
  {
    if (list != NULL)
      g_list_free_full (list, (GDestroyNotify) gst_buffer_unref);
    return NULL;
  }

}

/* this can change the codec originally present in @list */
static void
gst_qtdemux_handle_esds (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, GNode * esds, GstTagList * list)
{
  int len = QT_UINT32 (esds->data);
  guint8 *ptr = esds->data;
  guint8 *end = ptr + len;
  int tag;
  guint8 *data_ptr = NULL;
  int data_len = 0;
  guint8 object_type_id = 0;
  guint8 stream_type = 0;
  const char *codec_name = NULL;
  GstCaps *caps = NULL;

  GST_MEMDUMP_OBJECT (qtdemux, "esds", ptr, len);
  ptr += 8;
  GST_DEBUG_OBJECT (qtdemux, "version/flags = %08x", QT_UINT32 (ptr));
  ptr += 4;
  while (ptr + 1 < end) {
    tag = QT_UINT8 (ptr);
    GST_DEBUG_OBJECT (qtdemux, "tag = %02x", tag);
    ptr++;
    len = read_descr_size (ptr, end, &ptr);
    GST_DEBUG_OBJECT (qtdemux, "len = %d", len);

    /* Check the stated amount of data is available for reading */
    if (len < 0 || ptr + len > end)
      break;

    switch (tag) {
      case ES_DESCRIPTOR_TAG:
        GST_DEBUG_OBJECT (qtdemux, "ID 0x%04x", QT_UINT16 (ptr));
        GST_DEBUG_OBJECT (qtdemux, "priority 0x%04x", QT_UINT8 (ptr + 2));
        ptr += 3;
        break;
      case DECODER_CONFIG_DESC_TAG:{
        guint max_bitrate, avg_bitrate;

        object_type_id = QT_UINT8 (ptr);
        stream_type = QT_UINT8 (ptr + 1) >> 2;
        max_bitrate = QT_UINT32 (ptr + 5);
        avg_bitrate = QT_UINT32 (ptr + 9);
        GST_DEBUG_OBJECT (qtdemux, "object_type_id %02x", object_type_id);
        GST_DEBUG_OBJECT (qtdemux, "stream_type %02x", stream_type);
        GST_DEBUG_OBJECT (qtdemux, "buffer_size_db %02x", QT_UINT24 (ptr + 2));
        GST_DEBUG_OBJECT (qtdemux, "max bitrate %u", max_bitrate);
        GST_DEBUG_OBJECT (qtdemux, "avg bitrate %u", avg_bitrate);
        if (max_bitrate > 0 && max_bitrate < G_MAXUINT32) {
          gst_tag_list_add (list, GST_TAG_MERGE_REPLACE,
              GST_TAG_MAXIMUM_BITRATE, max_bitrate, NULL);
        }
        if (avg_bitrate > 0 && avg_bitrate < G_MAXUINT32) {
          gst_tag_list_add (list, GST_TAG_MERGE_REPLACE, GST_TAG_BITRATE,
              avg_bitrate, NULL);
        }
        ptr += 13;
        break;
      }
      case DECODER_SPECIFIC_INFO_TAG:
        GST_MEMDUMP_OBJECT (qtdemux, "data", ptr, len);
        if (object_type_id == 0xe0 && len == 0x40) {
          guint8 *data;
          GstStructure *s;
          guint32 clut[16];
          gint i;

          GST_DEBUG_OBJECT (qtdemux,
              "Have VOBSUB palette. Creating palette event");
          /* move to decConfigDescr data and read palette */
          data = ptr;
          for (i = 0; i < 16; i++) {
            clut[i] = QT_UINT32 (data);
            data += 4;
          }

          s = gst_structure_new ("application/x-gst-dvd", "event",
              G_TYPE_STRING, "dvd-spu-clut-change",
              "clut00", G_TYPE_INT, clut[0], "clut01", G_TYPE_INT, clut[1],
              "clut02", G_TYPE_INT, clut[2], "clut03", G_TYPE_INT, clut[3],
              "clut04", G_TYPE_INT, clut[4], "clut05", G_TYPE_INT, clut[5],
              "clut06", G_TYPE_INT, clut[6], "clut07", G_TYPE_INT, clut[7],
              "clut08", G_TYPE_INT, clut[8], "clut09", G_TYPE_INT, clut[9],
              "clut10", G_TYPE_INT, clut[10], "clut11", G_TYPE_INT, clut[11],
              "clut12", G_TYPE_INT, clut[12], "clut13", G_TYPE_INT, clut[13],
              "clut14", G_TYPE_INT, clut[14], "clut15", G_TYPE_INT, clut[15],
              NULL);

          /* store event and trigger custom processing */
          stream->pending_event =
              gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, s);
        } else {
          /* Generic codec_data handler puts it on the caps */
          data_ptr = ptr;
          data_len = len;
        }

        ptr += len;
        break;
      case SL_CONFIG_DESC_TAG:
        GST_DEBUG_OBJECT (qtdemux, "data %02x", QT_UINT8 (ptr));
        ptr += 1;
        break;
      default:
        GST_DEBUG_OBJECT (qtdemux, "Unknown/unhandled descriptor tag %02x",
            tag);
        GST_MEMDUMP_OBJECT (qtdemux, "descriptor data", ptr, len);
        ptr += len;
        break;
    }
  }

  /* object_type_id in the esds atom in mp4a and mp4v tells us which codec is
   * in use, and should also be used to override some other parameters for some
   * codecs. */
  switch (object_type_id) {
    case 0x20:                 /* MPEG-4 */
      /* 4 bytes for the visual_object_sequence_start_code and 1 byte for the
       * profile_and_level_indication */
      if (data_ptr != NULL && data_len >= 5 &&
          GST_READ_UINT32_BE (data_ptr) == 0x000001b0) {
        gst_codec_utils_mpeg4video_caps_set_level_and_profile (entry->caps,
            data_ptr + 4, data_len - 4);
      }
      break;                    /* Nothing special needed here */
    case 0x21:                 /* H.264 */
      codec_name = "H.264 / AVC";
      caps = gst_caps_new_simple ("video/x-h264",
          "stream-format", G_TYPE_STRING, "avc",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case 0x40:                 /* AAC (any) */
    case 0x66:                 /* AAC Main */
    case 0x67:                 /* AAC LC */
    case 0x68:                 /* AAC SSR */
      /* Override channels and rate based on the codec_data, as it's often
       * wrong. */
      /* Only do so for basic setup without HE-AAC extension */
      if (data_ptr && data_len == 2) {
        guint channels, rate;

        channels = gst_codec_utils_aac_get_channels (data_ptr, data_len);
        if (channels > 0)
          entry->n_channels = channels;

        rate = gst_codec_utils_aac_get_sample_rate (data_ptr, data_len);
        if (rate > 0)
          entry->rate = rate;
      }

      /* Set level and profile if possible */
      if (data_ptr != NULL && data_len >= 2) {
        gst_codec_utils_aac_caps_set_level_and_profile (entry->caps,
            data_ptr, data_len);
      } else {
        const gchar *profile_str = NULL;
        GstBuffer *buffer;
        GstMapInfo map;
        guint8 *codec_data;
        gint rate_idx, profile;

        /* No codec_data, let's invent something.
         * FIXME: This is wrong for SBR! */

        GST_WARNING_OBJECT (qtdemux, "No codec_data for AAC available");

        buffer = gst_buffer_new_and_alloc (2);
        gst_buffer_map (buffer, &map, GST_MAP_WRITE);
        codec_data = map.data;

        rate_idx =
            gst_codec_utils_aac_get_index_from_sample_rate (CUR_STREAM
            (stream)->rate);

        switch (object_type_id) {
          case 0x66:
            profile_str = "main";
            profile = 0;
            break;
          case 0x67:
            profile_str = "lc";
            profile = 1;
            break;
          case 0x68:
            profile_str = "ssr";
            profile = 2;
            break;
          default:
            profile = 3;
            break;
        }

        codec_data[0] = ((profile + 1) << 3) | ((rate_idx & 0xE) >> 1);
        codec_data[1] =
            ((rate_idx & 0x1) << 7) | (CUR_STREAM (stream)->n_channels << 3);

        gst_buffer_unmap (buffer, &map);
        gst_caps_set_simple (CUR_STREAM (stream)->caps, "codec_data",
            GST_TYPE_BUFFER, buffer, NULL);
        gst_buffer_unref (buffer);

        if (profile_str) {
          gst_caps_set_simple (CUR_STREAM (stream)->caps, "profile",
              G_TYPE_STRING, profile_str, NULL);
        }
      }
      break;
    case 0x60:                 /* MPEG-2, various profiles */
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
      codec_name = "MPEG-2 video";
      caps = gst_caps_new_simple ("video/mpeg",
          "mpegversion", G_TYPE_INT, 2,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case 0x69:                 /* MPEG-2 BC audio */
    case 0x6B:                 /* MPEG-1 audio */
      caps = gst_caps_new_simple ("audio/mpeg",
          "mpegversion", G_TYPE_INT, 1, NULL);
      codec_name = "MPEG-1 audio";
      break;
    case 0x6A:                 /* MPEG-1 */
      codec_name = "MPEG-1 video";
      caps = gst_caps_new_simple ("video/mpeg",
          "mpegversion", G_TYPE_INT, 1,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case 0x6C:                 /* MJPEG */
      caps =
          gst_caps_new_simple ("image/jpeg", "parsed", G_TYPE_BOOLEAN, TRUE,
          NULL);
      codec_name = "Motion-JPEG";
      break;
    case 0x6D:                 /* PNG */
      caps =
          gst_caps_new_simple ("image/png", "parsed", G_TYPE_BOOLEAN, TRUE,
          NULL);
      codec_name = "PNG still images";
      break;
    case 0x6E:                 /* JPEG2000 */
      codec_name = "JPEG-2000";
      caps = gst_caps_new_simple ("image/x-j2c", "fields", G_TYPE_INT, 1, NULL);
      break;
    case 0xA4:                 /* Dirac */
      codec_name = "Dirac";
      caps = gst_caps_new_empty_simple ("video/x-dirac");
      break;
    case 0xA5:                 /* AC3 */
      codec_name = "AC-3 audio";
      caps = gst_caps_new_simple ("audio/x-ac3",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      break;
    case 0xA9:                 /* AC3 */
      codec_name = "DTS audio";
      caps = gst_caps_new_simple ("audio/x-dts",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      break;
    case 0xDD:
      if (stream_type == 0x05 && data_ptr) {
        GList *headers =
            parse_xiph_stream_headers (qtdemux, data_ptr, data_len);
        if (headers) {
          GList *tmp;
          GValue arr_val = G_VALUE_INIT;
          GValue buf_val = G_VALUE_INIT;
          GstStructure *s;

          /* Let's assume it's vorbis if it's an audio stream of type 0xdd and we have codec data that extracts properly */
          codec_name = "Vorbis";
          caps = gst_caps_new_empty_simple ("audio/x-vorbis");
          g_value_init (&arr_val, GST_TYPE_ARRAY);
          g_value_init (&buf_val, GST_TYPE_BUFFER);
          for (tmp = headers; tmp; tmp = tmp->next) {
            g_value_set_boxed (&buf_val, (GstBuffer *) tmp->data);
            gst_value_array_append_value (&arr_val, &buf_val);
          }
          s = gst_caps_get_structure (caps, 0);
          gst_structure_take_value (s, "streamheader", &arr_val);
          g_value_unset (&buf_val);
          g_list_free (headers);

          data_ptr = NULL;
          data_len = 0;
        }
      }
      break;
    case 0xE1:                 /* QCELP */
      /* QCELP, the codec_data is a riff tag (little endian) with
       * more info (http://ftp.3gpp2.org/TSGC/Working/2003/2003-05-SanDiego/TSG-C-2003-05-San%20Diego/WG1/SWG12/C12-20030512-006%20=%20C12-20030217-015_Draft_Baseline%20Text%20of%20FFMS_R2.doc). */
      caps = gst_caps_new_empty_simple ("audio/qcelp");
      codec_name = "QCELP";
      break;
    default:
      break;
  }

  /* If we have a replacement caps, then change our caps for this stream */
  if (caps) {
    gst_caps_unref (entry->caps);
    entry->caps = caps;
  }

  if (codec_name && list)
    gst_tag_list_add (list, GST_TAG_MERGE_REPLACE,
        GST_TAG_AUDIO_CODEC, codec_name, NULL);

  /* Add the codec_data attribute to caps, if we have it */
  if (data_ptr) {
    GstBuffer *buffer;

    buffer = gst_buffer_new_and_alloc (data_len);
    gst_buffer_fill (buffer, 0, data_ptr, data_len);

    GST_DEBUG_OBJECT (qtdemux, "setting codec_data from esds");
    GST_MEMDUMP_OBJECT (qtdemux, "codec_data from esds", data_ptr, data_len);

    gst_caps_set_simple (entry->caps, "codec_data", GST_TYPE_BUFFER,
        buffer, NULL);
    gst_buffer_unref (buffer);
  }

}

static inline GstCaps *
_get_unknown_codec_name (const gchar * type, guint32 fourcc)
{
  GstCaps *caps;
  guint i;
  char *s, fourstr[5];

  g_snprintf (fourstr, 5, "%" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));
  for (i = 0; i < 4; i++) {
    if (!g_ascii_isalnum (fourstr[i]))
      fourstr[i] = '_';
  }
  s = g_strdup_printf ("%s/x-gst-fourcc-%s", type, g_strstrip (fourstr));
  caps = gst_caps_new_empty_simple (s);
  g_free (s);
  return caps;
}

#define _codec(name) \
  do { \
    if (codec_name) { \
      *codec_name = g_strdup (name); \
    } \
  } while (0)

static GstCaps *
qtdemux_video_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    GNode * stsd_entry, gchar ** codec_name)
{
  GstCaps *caps = NULL;
  GstVideoFormat format = GST_VIDEO_FORMAT_UNKNOWN;

  switch (fourcc) {
    case FOURCC_png:
      _codec ("PNG still images");
      caps = gst_caps_new_empty_simple ("image/png");
      break;
    case FOURCC_jpeg:
      _codec ("JPEG still images");
      caps =
          gst_caps_new_simple ("image/jpeg", "parsed", G_TYPE_BOOLEAN, TRUE,
          NULL);
      break;
    case GST_MAKE_FOURCC ('m', 'j', 'p', 'a'):
    case GST_MAKE_FOURCC ('A', 'V', 'D', 'J'):
    case GST_MAKE_FOURCC ('M', 'J', 'P', 'G'):
    case GST_MAKE_FOURCC ('d', 'm', 'b', '1'):
      _codec ("Motion-JPEG");
      caps =
          gst_caps_new_simple ("image/jpeg", "parsed", G_TYPE_BOOLEAN, TRUE,
          NULL);
      break;
    case GST_MAKE_FOURCC ('m', 'j', 'p', 'b'):
      _codec ("Motion-JPEG format B");
      caps = gst_caps_new_empty_simple ("video/x-mjpeg-b");
      break;
    case FOURCC_mjp2:
      _codec ("JPEG-2000");
      /* override to what it should be according to spec, avoid palette_data */
      entry->bits_per_sample = 24;
      caps = gst_caps_new_simple ("image/x-j2c", "fields", G_TYPE_INT, 1, NULL);
      break;
    case FOURCC_SVQ3:
      _codec ("Sorensen video v.3");
      caps = gst_caps_new_simple ("video/x-svq",
          "svqversion", G_TYPE_INT, 3, NULL);
      break;
    case GST_MAKE_FOURCC ('s', 'v', 'q', 'i'):
    case GST_MAKE_FOURCC ('S', 'V', 'Q', '1'):
      _codec ("Sorensen video v.1");
      caps = gst_caps_new_simple ("video/x-svq",
          "svqversion", G_TYPE_INT, 1, NULL);
      break;
    case GST_MAKE_FOURCC ('W', 'R', 'A', 'W'):
      caps = gst_caps_new_empty_simple ("video/x-raw");
      gst_caps_set_simple (caps, "format", G_TYPE_STRING, "RGB8P", NULL);
      _codec ("Windows Raw RGB");
      stream->alignment = 32;
      break;
    case FOURCC_raw_:
    {
      guint16 bps;

      // Read VisualSampleEntry depth. Size is checked by the caller already.
      bps = QT_UINT16 ((const guint8 *) stsd_entry->data + 82);
      switch (bps) {
        case 15:
          format = GST_VIDEO_FORMAT_RGB15;
          break;
        case 16:
          format = GST_VIDEO_FORMAT_RGB16;
          break;
        case 24:
          format = GST_VIDEO_FORMAT_RGB;
          break;
        case 32:
          format = GST_VIDEO_FORMAT_ARGB;
          break;
        default:
          /* unknown */
          break;
      }
      break;
    }
    case GST_MAKE_FOURCC ('y', 'v', '1', '2'):
      format = GST_VIDEO_FORMAT_I420;
      break;
    case GST_MAKE_FOURCC ('y', 'u', 'v', '2'):
    case GST_MAKE_FOURCC ('Y', 'u', 'v', '2'):
      format = GST_VIDEO_FORMAT_I420;
      break;
    case FOURCC_2vuy:
    case GST_MAKE_FOURCC ('2', 'V', 'u', 'y'):
      format = GST_VIDEO_FORMAT_UYVY;
      break;
    case GST_MAKE_FOURCC ('v', '3', '0', '8'):
      format = GST_VIDEO_FORMAT_v308;
      break;
    case GST_MAKE_FOURCC ('v', '2', '1', '6'):
      format = GST_VIDEO_FORMAT_v216;
      break;
    case FOURCC_v210:
      format = GST_VIDEO_FORMAT_v210;
      break;
    case GST_MAKE_FOURCC ('r', '2', '1', '0'):
      format = GST_VIDEO_FORMAT_r210;
      break;
      /* Packed YUV 4:4:4 10 bit in 32 bits, complex
         case GST_MAKE_FOURCC ('v', '4', '1', '0'):
         format = GST_VIDEO_FORMAT_v410;
         break;
       */
      /* Packed YUV 4:4:4:4 8 bit in 32 bits
       * but different order than AYUV
       case GST_MAKE_FOURCC ('v', '4', '0', '8'):
       format = GST_VIDEO_FORMAT_v408;
       break;
       */
    case GST_MAKE_FOURCC ('m', 'p', 'e', 'g'):
    case GST_MAKE_FOURCC ('m', 'p', 'g', '1'):
    case GST_MAKE_FOURCC ('m', '1', 'v', ' '):
      _codec ("MPEG-1 video");
      caps = gst_caps_new_simple ("video/mpeg", "mpegversion", G_TYPE_INT, 1,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case GST_MAKE_FOURCC ('h', 'd', 'v', '1'): /* HDV 720p30 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '2'): /* HDV 1080i60 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '3'): /* HDV 1080i50 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '4'): /* HDV 720p24 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '5'): /* HDV 720p25 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '6'): /* HDV 1080p24 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '7'): /* HDV 1080p25 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '8'): /* HDV 1080p30 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', '9'): /* HDV 720p60 */
    case GST_MAKE_FOURCC ('h', 'd', 'v', 'a'): /* HDV 720p50 */
    case GST_MAKE_FOURCC ('m', 'x', '5', 'n'): /* MPEG2 IMX NTSC 525/60 50mb/s produced by FCP */
    case GST_MAKE_FOURCC ('m', 'x', '5', 'p'): /* MPEG2 IMX PAL 625/60 50mb/s produced by FCP */
    case GST_MAKE_FOURCC ('m', 'x', '4', 'n'): /* MPEG2 IMX NTSC 525/60 40mb/s produced by FCP */
    case GST_MAKE_FOURCC ('m', 'x', '4', 'p'): /* MPEG2 IMX PAL 625/60 40mb/s produced by FCP */
    case GST_MAKE_FOURCC ('m', 'x', '3', 'n'): /* MPEG2 IMX NTSC 525/60 30mb/s produced by FCP */
    case GST_MAKE_FOURCC ('m', 'x', '3', 'p'): /* MPEG2 IMX PAL 625/50 30mb/s produced by FCP */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '1'): /* XDCAM HD 720p30 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '2'): /* XDCAM HD 1080i60 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '3'): /* XDCAM HD 1080i50 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '4'): /* XDCAM HD 720p24 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '5'): /* XDCAM HD 720p25 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '6'): /* XDCAM HD 1080p24 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '7'): /* XDCAM HD 1080p25 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '8'): /* XDCAM HD 1080p30 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', '9'): /* XDCAM HD 720p60 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', 'a'): /* XDCAM HD 720p50 35Mb/s */
    case GST_MAKE_FOURCC ('x', 'd', 'v', 'b'): /* XDCAM EX 1080i60 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', 'v', 'c'): /* XDCAM EX 1080i50 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', 'v', 'd'): /* XDCAM HD 1080p24 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', 'v', 'e'): /* XDCAM HD 1080p25 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', 'v', 'f'): /* XDCAM HD 1080p30 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', '1'): /* XDCAM HD422 720p30 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', '4'): /* XDCAM HD422 720p24 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', '5'): /* XDCAM HD422 720p25 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', '9'): /* XDCAM HD422 720p60 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', 'a'): /* XDCAM HD422 720p50 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', 'b'): /* XDCAM HD422 1080i50 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', 'c'): /* XDCAM HD422 1080i50 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', 'd'): /* XDCAM HD422 1080p24 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', 'e'): /* XDCAM HD422 1080p25 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', '5', 'f'): /* XDCAM HD422 1080p30 50Mb/s CBR */
    case GST_MAKE_FOURCC ('x', 'd', 'h', 'd'): /* XDCAM HD 540p */
    case GST_MAKE_FOURCC ('x', 'd', 'h', '2'): /* XDCAM HD422 540p */
    case GST_MAKE_FOURCC ('A', 'V', 'm', 'p'): /* AVID IMX PAL */
    case GST_MAKE_FOURCC ('m', 'p', 'g', '2'): /* AVID IMX PAL */
    case GST_MAKE_FOURCC ('m', 'p', '2', 'v'): /* AVID IMX PAL */
    case GST_MAKE_FOURCC ('m', '2', 'v', '1'):
      _codec ("MPEG-2 video");
      caps = gst_caps_new_simple ("video/mpeg", "mpegversion", G_TYPE_INT, 2,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case GST_MAKE_FOURCC ('g', 'i', 'f', ' '):
      _codec ("GIF still images");
      caps = gst_caps_new_empty_simple ("image/gif");
      break;
    case FOURCC_h263:
    case GST_MAKE_FOURCC ('H', '2', '6', '3'):
    case FOURCC_s263:
    case GST_MAKE_FOURCC ('U', '2', '6', '3'):
      _codec ("H.263");
      /* ffmpeg uses the height/width props, don't know why */
      caps = gst_caps_new_simple ("video/x-h263",
          "variant", G_TYPE_STRING, "itu", NULL);
      break;
    case FOURCC_mp4v:
    case FOURCC_MP4V:
      _codec ("MPEG-4 video");
      caps = gst_caps_new_simple ("video/mpeg", "mpegversion", G_TYPE_INT, 4,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case GST_MAKE_FOURCC ('3', 'i', 'v', 'd'):
    case GST_MAKE_FOURCC ('3', 'I', 'V', 'D'):
      _codec ("Microsoft MPEG-4 4.3");  /* FIXME? */
      caps = gst_caps_new_simple ("video/x-msmpeg",
          "msmpegversion", G_TYPE_INT, 43, NULL);
      break;
    case GST_MAKE_FOURCC ('D', 'I', 'V', '3'):
      _codec ("DivX 3");
      caps = gst_caps_new_simple ("video/x-divx",
          "divxversion", G_TYPE_INT, 3, NULL);
      break;
    case GST_MAKE_FOURCC ('D', 'I', 'V', 'X'):
    case GST_MAKE_FOURCC ('d', 'i', 'v', 'x'):
      _codec ("DivX 4");
      caps = gst_caps_new_simple ("video/x-divx",
          "divxversion", G_TYPE_INT, 4, NULL);
      break;
    case GST_MAKE_FOURCC ('D', 'X', '5', '0'):
      _codec ("DivX 5");
      caps = gst_caps_new_simple ("video/x-divx",
          "divxversion", G_TYPE_INT, 5, NULL);
      break;

    case GST_MAKE_FOURCC ('F', 'F', 'V', '1'):
      _codec ("FFV1");
      caps = gst_caps_new_simple ("video/x-ffv",
          "ffvversion", G_TYPE_INT, 1, NULL);
      break;

    case GST_MAKE_FOURCC ('3', 'I', 'V', '1'):
    case GST_MAKE_FOURCC ('3', 'I', 'V', '2'):
    case FOURCC_XVID:
    case FOURCC_xvid:
    case FOURCC_FMP4:
    case FOURCC_fmp4:
    case GST_MAKE_FOURCC ('U', 'M', 'P', '4'):
      caps = gst_caps_new_simple ("video/mpeg", "mpegversion", G_TYPE_INT, 4,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      _codec ("MPEG-4");
      break;

    case GST_MAKE_FOURCC ('c', 'v', 'i', 'd'):
      _codec ("Cinepak");
      caps = gst_caps_new_empty_simple ("video/x-cinepak");
      break;
    case GST_MAKE_FOURCC ('q', 'd', 'r', 'w'):
      _codec ("Apple QuickDraw");
      caps = gst_caps_new_empty_simple ("video/x-qdrw");
      break;
    case GST_MAKE_FOURCC ('r', 'p', 'z', 'a'):
      _codec ("Apple video");
      caps = gst_caps_new_empty_simple ("video/x-apple-video");
      break;
    case FOURCC_H264:
    case FOURCC_avc1:
    case FOURCC_dva1:
      _codec ("H.264 / AVC");
      caps = gst_caps_new_simple ("video/x-h264",
          "stream-format", G_TYPE_STRING, "avc",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_avc3:
    case FOURCC_dvav:
      _codec ("H.264 / AVC");
      caps = gst_caps_new_simple ("video/x-h264",
          "stream-format", G_TYPE_STRING, "avc3",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_ai12:
    case FOURCC_ai13:
    case FOURCC_ai15:
    case FOURCC_ai16:
    case FOURCC_ai1p:
    case FOURCC_ai1q:
    case FOURCC_ai52:
    case FOURCC_ai53:
    case FOURCC_ai55:
    case FOURCC_ai56:
    case FOURCC_ai5p:
    case FOURCC_ai5q:
      _codec ("H.264 / AVC");
      caps = gst_caps_new_simple ("video/x-h264",
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_H265:
    case FOURCC_hvc1:
    case FOURCC_dvh1:
      _codec ("H.265 / HEVC");
      caps = gst_caps_new_simple ("video/x-h265",
          "stream-format", G_TYPE_STRING, "hvc1",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_hev1:
    case FOURCC_dvhe:
      _codec ("H.265 / HEVC");
      caps = gst_caps_new_simple ("video/x-h265",
          "stream-format", G_TYPE_STRING, "hev1",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_H266:
    case FOURCC_vvc1:
      _codec ("H.266 / VVC");
      caps = gst_caps_new_simple ("video/x-h266",
          "stream-format", G_TYPE_STRING, "vvc1",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_vvi1:
      _codec ("H.266 / VVC");
      caps = gst_caps_new_simple ("video/x-h266",
          "stream-format", G_TYPE_STRING, "vvi1",
          "alignment", G_TYPE_STRING, "au", NULL);
      break;
    case FOURCC_rle_:
      _codec ("Run-length encoding");
      caps = gst_caps_new_simple ("video/x-rle",
          "layout", G_TYPE_STRING, "quicktime", NULL);
      break;
    case FOURCC_WRLE:
      _codec ("Run-length encoding");
      caps = gst_caps_new_simple ("video/x-rle",
          "layout", G_TYPE_STRING, "microsoft", NULL);
      break;
    case GST_MAKE_FOURCC ('I', 'V', '3', '2'):
    case GST_MAKE_FOURCC ('i', 'v', '3', '2'):
      _codec ("Indeo Video 3");
      caps = gst_caps_new_simple ("video/x-indeo",
          "indeoversion", G_TYPE_INT, 3, NULL);
      break;
    case GST_MAKE_FOURCC ('I', 'V', '4', '1'):
    case GST_MAKE_FOURCC ('i', 'v', '4', '1'):
      _codec ("Intel Video 4");
      caps = gst_caps_new_simple ("video/x-indeo",
          "indeoversion", G_TYPE_INT, 4, NULL);
      break;
    case FOURCC_dvcp:
    case FOURCC_dvc_:
    case GST_MAKE_FOURCC ('d', 'v', 's', 'd'):
    case GST_MAKE_FOURCC ('D', 'V', 'S', 'D'):
    case GST_MAKE_FOURCC ('d', 'v', 'c', 's'):
    case GST_MAKE_FOURCC ('D', 'V', 'C', 'S'):
    case GST_MAKE_FOURCC ('d', 'v', '2', '5'):
    case GST_MAKE_FOURCC ('d', 'v', 'p', 'p'):
      _codec ("DV Video");
      caps = gst_caps_new_simple ("video/x-dv", "dvversion", G_TYPE_INT, 25,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case FOURCC_dv5n:          /* DVCPRO50 NTSC */
    case FOURCC_dv5p:          /* DVCPRO50 PAL */
      _codec ("DVCPro50 Video");
      caps = gst_caps_new_simple ("video/x-dv", "dvversion", G_TYPE_INT, 50,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case GST_MAKE_FOURCC ('d', 'v', 'h', '5'): /* DVCPRO HD 50i produced by FCP */
    case GST_MAKE_FOURCC ('d', 'v', 'h', '6'): /* DVCPRO HD 60i produced by FCP */
      _codec ("DVCProHD Video");
      caps = gst_caps_new_simple ("video/x-dv", "dvversion", G_TYPE_INT, 100,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case GST_MAKE_FOURCC ('s', 'm', 'c', ' '):
      _codec ("Apple Graphics (SMC)");
      caps = gst_caps_new_empty_simple ("video/x-smc");
      break;
    case GST_MAKE_FOURCC ('V', 'P', '3', '1'):
      _codec ("VP3");
      caps = gst_caps_new_empty_simple ("video/x-vp3");
      break;
    case GST_MAKE_FOURCC ('V', 'P', '6', 'F'):
      _codec ("VP6 Flash");
      caps = gst_caps_new_empty_simple ("video/x-vp6-flash");
      break;
    case FOURCC_XiTh:
      _codec ("Theora");
      caps = gst_caps_new_empty_simple ("video/x-theora");
      /* theora uses one byte of padding in the data stream because it does not
       * allow 0 sized packets while theora does */
      entry->padding = 1;
      break;
    case FOURCC_drac:
      _codec ("Dirac");
      caps = gst_caps_new_empty_simple ("video/x-dirac");
      break;
    case GST_MAKE_FOURCC ('t', 'i', 'f', 'f'):
      _codec ("TIFF still images");
      caps = gst_caps_new_empty_simple ("image/tiff");
      break;
    case GST_MAKE_FOURCC ('i', 'c', 'o', 'd'):
      _codec ("Apple Intermediate Codec");
      caps = gst_caps_from_string ("video/x-apple-intermediate-codec");
      break;
    case GST_MAKE_FOURCC ('A', 'V', 'd', 'n'):
      _codec ("AVID DNxHD");
      caps =
          gst_caps_new_simple ("video/x-dnxhd", "profile", G_TYPE_STRING,
          "dnxhd", NULL);
      break;
    case GST_MAKE_FOURCC ('A', 'V', 'd', 'h'):
      _codec ("AVID DNxHR");
      caps =
          gst_caps_new_simple ("video/x-dnxhd", "profile", G_TYPE_STRING,
          "dnxhr", NULL);
      break;
    case FOURCC_VP80:
    case FOURCC_vp08:
      _codec ("On2 VP8");
      caps = gst_caps_from_string ("video/x-vp8");
      break;
    case FOURCC_vp09:
      _codec ("Google VP9");
      caps = gst_caps_from_string ("video/x-vp9");
      break;
    case FOURCC_apcs:
      _codec ("Apple ProRes LT");
      caps =
          gst_caps_new_simple ("video/x-prores", "variant", G_TYPE_STRING, "lt",
          NULL);
      break;
    case FOURCC_apch:
      _codec ("Apple ProRes HQ");
      caps =
          gst_caps_new_simple ("video/x-prores", "variant", G_TYPE_STRING, "hq",
          NULL);
      break;
    case FOURCC_apcn:
      _codec ("Apple ProRes");
      caps =
          gst_caps_new_simple ("video/x-prores", "variant", G_TYPE_STRING,
          "standard", NULL);
      break;
    case FOURCC_apco:
      _codec ("Apple ProRes Proxy");
      caps =
          gst_caps_new_simple ("video/x-prores", "variant", G_TYPE_STRING,
          "proxy", NULL);
      break;
    case FOURCC_ap4h:
      _codec ("Apple ProRes 4444");
      caps =
          gst_caps_new_simple ("video/x-prores", "variant", G_TYPE_STRING,
          "4444", NULL);

      /* 24 bits per sample = an alpha channel is coded but image is always opaque */
      if (entry->bits_per_sample > 0) {
        gst_caps_set_simple (caps, "depth", G_TYPE_INT, entry->bits_per_sample,
            NULL);
      }
      break;
    case FOURCC_ap4x:
      _codec ("Apple ProRes 4444 XQ");
      caps =
          gst_caps_new_simple ("video/x-prores", "variant", G_TYPE_STRING,
          "4444xq", NULL);

      /* 24 bits per sample = an alpha channel is coded but image is always opaque */
      if (entry->bits_per_sample > 0) {
        gst_caps_set_simple (caps, "depth", G_TYPE_INT, entry->bits_per_sample,
            NULL);
      }
      break;
    case FOURCC_cfhd:
      _codec ("GoPro CineForm");
      caps = gst_caps_from_string ("video/x-cineform");
      break;
    case FOURCC_vc_1:
    case FOURCC_ovc1:
      _codec ("VC-1");
      caps = gst_caps_new_simple ("video/x-wmv",
          "wmvversion", G_TYPE_INT, 3, "format", G_TYPE_STRING, "WVC1", NULL);
      break;
    case FOURCC_av01:
      _codec ("AV1");
      caps = gst_caps_new_simple ("video/x-av1",
          "stream-format", G_TYPE_STRING, "obu-stream",
          "alignment", G_TYPE_STRING, "tu", NULL);
      break;
    case FOURCC_SHQ0:
    case FOURCC_SHQ1:
    case FOURCC_SHQ2:
    case FOURCC_SHQ3:
    case FOURCC_SHQ4:
    case FOURCC_SHQ5:
    case FOURCC_SHQ6:
    case FOURCC_SHQ7:
    case FOURCC_SHQ8:
    case FOURCC_SHQ9:{
      gchar *format =
          g_strdup_printf ("%" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));
      _codec ("SpeedHQ");
      caps =
          gst_caps_new_simple ("video/x-speedhq", "variant", G_TYPE_STRING,
          format, NULL);
      g_free (format);
      break;
    }
    case FOURCC_LAGS:
      _codec ("Lagarith lossless video codec");
      caps = gst_caps_new_empty_simple ("video/x-lagarith");
      break;
    case FOURCC_Hap1:
    case FOURCC_Hap5:
    case FOURCC_HapY:
    case FOURCC_HapM:
    case FOURCC_HapA:
    case FOURCC_Hap7:
    case FOURCC_HapH:{
      gchar *variant =
          g_strdup_printf ("%" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (fourcc));
      caps = gst_caps_new_simple ("video/x-hap",
          "variant", G_TYPE_STRING, variant, NULL);
      g_free (variant);

      // https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
      switch (fourcc) {
        case FOURCC_Hap5:
          _codec ("Hap Alpha");
          break;
        case FOURCC_HapY:
          _codec ("Hap Q");
          break;
        case FOURCC_HapM:
          _codec ("Hap Q Alpha");
          break;
        case FOURCC_HapA:
          _codec ("Hap Alpha-Only");
          break;
        case FOURCC_Hap7:
          _codec ("Hap R");
          break;
        case FOURCC_HapH:
          _codec ("Hap HDR");
          break;
        case FOURCC_Hap1:
        default:
          _codec ("Hap");
          break;
      }
      break;
    }
    case FOURCC_uncv:
    {
      GNode *uncC_node, *cmpd_node;

      GstByteReader reader;
      UncompressedFrameConfigBox uncC = { 0 };
      ComponentDefinitionBox cmpd = { 0 };

      uncC_node =
          qtdemux_tree_get_child_by_type_full (stsd_entry, FOURCC_uncC,
          &reader);
      if (!uncC_node) {
        GST_WARNING_OBJECT (qtdemux,
            "Expected to find uncC box when parsing uncv");
        break;
      }

      if (!qtdemux_parse_uncC (qtdemux, &reader, &uncC)) {
        GST_WARNING_OBJECT (qtdemux, "Failed parsing uncC box");
        break;
      }

      cmpd_node =
          qtdemux_tree_get_child_by_type_full (stsd_entry, FOURCC_cmpd,
          &reader);
      if (uncC.version == 0 && !cmpd_node) {
        GST_WARNING_OBJECT (qtdemux,
            "Expected to find cmpd box when parsing uncv");
        break;
      }

      if (cmpd_node && !qtdemux_parse_cmpd (qtdemux, &reader, &cmpd)) {
        GST_WARNING_OBJECT (qtdemux, "Failed parsing cmpd box");
        break;
      }

      format = qtdemux_get_format_from_uncv (qtdemux, &uncC, &cmpd);
      gst_video_info_set_format (&stream->pre_info, format, entry->width,
          entry->height);
      qtdemux_set_info_from_uncv (qtdemux, entry, &uncC, &stream->pre_info);
      stream->alignment = 32;

      /* Free Memory */
      qtdemux_clear_uncC (&uncC);
      qtdemux_clear_cmpd (&cmpd);
      break;
    }
    case GST_MAKE_FOURCC ('k', 'p', 'c', 'd'):
    default:
    {
      caps = _get_unknown_codec_name ("video", fourcc);
      break;
    }
  }

  if (format != GST_VIDEO_FORMAT_UNKNOWN) {
    gst_video_info_set_format (&stream->info, format, entry->width,
        entry->height);

    caps = gst_video_info_to_caps (&stream->info);
    *codec_name = gst_pb_utils_get_codec_description (caps);

    /* If pre_info is initialized, then row_alignment may be neccessary */
    if (stream->pre_info.size) {
      stream->needs_row_alignment =
          !gst_video_info_is_equal (&stream->info, &stream->pre_info);
    }

    /* enable clipping for raw video streams */
    stream->need_clip = TRUE;
    stream->alignment = 32;
  }

  return caps;
}

static guint
round_up_pow2 (guint n)
{
  n = n - 1;
  n = n | (n >> 1);
  n = n | (n >> 2);
  n = n | (n >> 4);
  n = n | (n >> 8);
  n = n | (n >> 16);
  return n + 1;
}

static GstCaps *
qtdemux_audio_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    guint8 stsd_version, guint32 version,
    GNode * stsd_entry, gchar ** codec_name)
{
  GstCaps *caps;
  const GstStructure *s;
  const gchar *name;
  gint endian = 0;
  GstAudioFormat format = 0;
  gint depth;

  GST_DEBUG_OBJECT (qtdemux, "resolve fourcc 0x%08x", GUINT32_TO_BE (fourcc));

  depth = entry->bytes_per_packet * 8;

  switch (fourcc) {
    case GST_MAKE_FOURCC ('N', 'O', 'N', 'E'):
    case FOURCC_raw_:
      /* 8-bit audio is unsigned */
      if (depth == 8)
        format = GST_AUDIO_FORMAT_U8;
      /* otherwise it's signed and big-endian just like 'twos' */
      /* FALLTHROUGH */
    case FOURCC_twos:
      endian = G_BIG_ENDIAN;
      /* FALLTHROUGH */
    case FOURCC_sowt:
    {
      gchar *str;

      if (!endian)
        endian = G_LITTLE_ENDIAN;

      if (!format)
        format = gst_audio_format_build_integer (TRUE, endian, depth, depth);

      str = g_strdup_printf ("Raw %d-bit PCM audio", depth);
      _codec (str);
      g_free (str);

      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, gst_audio_format_to_string (format),
          "layout", G_TYPE_STRING, "interleaved", NULL);
      stream->alignment = GST_ROUND_UP_8 (depth);
      stream->alignment = round_up_pow2 (stream->alignment);
      break;
    }
    case FOURCC_fl64:
      _codec ("Raw 64-bit floating-point audio");
      /* we assume BIG ENDIAN, an enda box will tell us to change this to little
       * endian later */
      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, "F64BE",
          "layout", G_TYPE_STRING, "interleaved", NULL);
      stream->alignment = 8;
      break;
    case FOURCC_fl32:
      _codec ("Raw 32-bit floating-point audio");
      /* we assume BIG ENDIAN, an enda box will tell us to change this to little
       * endian later */
      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, "F32BE",
          "layout", G_TYPE_STRING, "interleaved", NULL);
      stream->alignment = 4;
      break;
    case FOURCC_in24:
      _codec ("Raw 24-bit PCM audio");
      /* we assume BIG ENDIAN, an enda box will tell us to change this to little
       * endian later */
      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, "S24BE",
          "layout", G_TYPE_STRING, "interleaved", NULL);
      stream->alignment = 4;
      break;
    case FOURCC_in32:
      _codec ("Raw 32-bit PCM audio");
      /* we assume BIG ENDIAN, an enda box will tell us to change this to little
       * endian later */
      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, "S32BE",
          "layout", G_TYPE_STRING, "interleaved", NULL);
      stream->alignment = 4;
      break;
    case FOURCC_s16l:
      _codec ("Raw 16-bit PCM audio");
      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, "S16LE",
          "layout", G_TYPE_STRING, "interleaved", NULL);
      stream->alignment = 2;
      break;
    case FOURCC_ulaw:
      _codec ("Mu-law audio");
      caps = gst_caps_new_empty_simple ("audio/x-mulaw");
      break;
    case FOURCC_alaw:
      _codec ("A-law audio");
      caps = gst_caps_new_empty_simple ("audio/x-alaw");
      break;
    case 0x0200736d:
    case 0x6d730002:
      _codec ("Microsoft ADPCM");
      /* Microsoft ADPCM-ACM code 2 */
      caps = gst_caps_new_simple ("audio/x-adpcm",
          "layout", G_TYPE_STRING, "microsoft", NULL);
      break;
    case 0x1100736d:
    case 0x6d730011:
      _codec ("DVI/IMA ADPCM");
      caps = gst_caps_new_simple ("audio/x-adpcm",
          "layout", G_TYPE_STRING, "dvi", NULL);
      break;
    case 0x1700736d:
    case 0x6d730017:
      _codec ("DVI/Intel IMA ADPCM");
      /* FIXME DVI/Intel IMA ADPCM/ACM code 17 */
      caps = gst_caps_new_simple ("audio/x-adpcm",
          "layout", G_TYPE_STRING, "quicktime", NULL);
      break;
    case 0x5500736d:
    case 0x6d730055:
      /* MPEG layer 3, CBR only (pre QT4.1) */
    case FOURCC__mp3:
    case FOURCC_mp3_:
      _codec ("MPEG-1 layer 3");
      /* MPEG layer 3, CBR & VBR (QT4.1 and later) */
      caps = gst_caps_new_simple ("audio/mpeg", "layer", G_TYPE_INT, 3,
          "mpegversion", G_TYPE_INT, 1, NULL);
      break;
    case GST_MAKE_FOURCC ('.', 'm', 'p', '2'):
      _codec ("MPEG-1 layer 2");
      /* MPEG layer 2 */
      caps = gst_caps_new_simple ("audio/mpeg", "layer", G_TYPE_INT, 2,
          "mpegversion", G_TYPE_INT, 1, NULL);
      break;
    case 0x20736d:
    case GST_MAKE_FOURCC ('e', 'c', '-', '3'):
      _codec ("EAC-3 audio");
      caps = gst_caps_new_simple ("audio/x-eac3",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      entry->sampled = TRUE;
      break;
    case GST_MAKE_FOURCC ('s', 'a', 'c', '3'): // Nero Recode
    case FOURCC_ac_3:
      _codec ("AC-3 audio");
      caps = gst_caps_new_simple ("audio/x-ac3",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      entry->sampled = TRUE;
      break;
    case GST_MAKE_FOURCC ('d', 't', 's', 'c'):
    case GST_MAKE_FOURCC ('D', 'T', 'S', ' '):
      _codec ("DTS audio");
      caps = gst_caps_new_simple ("audio/x-dts",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      entry->sampled = TRUE;
      break;
    case GST_MAKE_FOURCC ('d', 't', 's', 'h'): // DTS-HD
    case GST_MAKE_FOURCC ('d', 't', 's', 'l'): // DTS-HD Lossless
      _codec ("DTS-HD audio");
      caps = gst_caps_new_simple ("audio/x-dts",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      entry->sampled = TRUE;
      break;
    case FOURCC_MAC3:
      _codec ("MACE-3");
      caps = gst_caps_new_simple ("audio/x-mace",
          "maceversion", G_TYPE_INT, 3, NULL);
      break;
    case FOURCC_MAC6:
      _codec ("MACE-6");
      caps = gst_caps_new_simple ("audio/x-mace",
          "maceversion", G_TYPE_INT, 6, NULL);
      break;
    case GST_MAKE_FOURCC ('O', 'g', 'g', 'V'):
      /* ogg/vorbis */
      caps = gst_caps_new_empty_simple ("application/ogg");
      break;
    case GST_MAKE_FOURCC ('d', 'v', 'c', 'a'):
      _codec ("DV audio");
      caps = gst_caps_new_empty_simple ("audio/x-dv");
      break;
    case FOURCC_mp4a:
      _codec ("MPEG-4 AAC audio");
      caps = gst_caps_new_simple ("audio/mpeg",
          "mpegversion", G_TYPE_INT, 4, "framed", G_TYPE_BOOLEAN, TRUE,
          "stream-format", G_TYPE_STRING, "raw", NULL);
      break;
    case FOURCC_QDMC:
      _codec ("QDesign Music");
      caps = gst_caps_new_empty_simple ("audio/x-qdm");
      break;
    case FOURCC_QDM2:
      _codec ("QDesign Music v.2");
      /* FIXME: QDesign music version 2 (no constant) */
      // if (FALSE && data) {
      //   caps = gst_caps_new_simple ("audio/x-qdm2",
      //       "framesize", G_TYPE_INT, QT_UINT32 (data + 52),
      //       "bitrate", G_TYPE_INT, QT_UINT32 (data + 40),
      //       "blocksize", G_TYPE_INT, QT_UINT32 (data + 44), NULL);
      // } else {
      caps = gst_caps_new_empty_simple ("audio/x-qdm2");
      //}
      break;
    case FOURCC_agsm:
      _codec ("GSM audio");
      caps = gst_caps_new_empty_simple ("audio/x-gsm");
      break;
    case FOURCC_samr:
      _codec ("AMR audio");
      caps = gst_caps_new_empty_simple ("audio/AMR");
      break;
    case FOURCC_sawb:
      _codec ("AMR-WB audio");
      caps = gst_caps_new_empty_simple ("audio/AMR-WB");
      break;
    case FOURCC_ima4:
      _codec ("Quicktime IMA ADPCM");
      caps = gst_caps_new_simple ("audio/x-adpcm",
          "layout", G_TYPE_STRING, "quicktime", NULL);
      break;
    case FOURCC_alac:
      _codec ("Apple lossless audio");
      caps = gst_caps_new_empty_simple ("audio/x-alac");
      break;
    case FOURCC_fLaC:
      _codec ("Free Lossless Audio Codec");
      caps = gst_caps_new_simple ("audio/x-flac",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      break;
    case GST_MAKE_FOURCC ('Q', 'c', 'l', 'p'):
      _codec ("QualComm PureVoice");
      caps = gst_caps_from_string ("audio/qcelp");
      break;
    case FOURCC_wma_:
    case FOURCC_owma:
      _codec ("WMA");
      caps = gst_caps_new_empty_simple ("audio/x-wma");
      break;
    case FOURCC_opus:
      _codec ("Opus");
      caps = gst_caps_new_empty_simple ("audio/x-opus");
      break;
    case FOURCC_lpcm:
    {
      const guint8 *data;
      guint32 len;
      guint32 flags = 0;
      guint32 depth = 0;
      guint32 width = 0;
      GstAudioFormat format;
      enum
      {
        FLAG_IS_FLOAT = 0x1,
        FLAG_IS_BIG_ENDIAN = 0x2,
        FLAG_IS_SIGNED = 0x4,
        FLAG_IS_PACKED = 0x8,
        FLAG_IS_ALIGNED_HIGH = 0x10,
        FLAG_IS_NON_INTERLEAVED = 0x20
      };
      _codec ("Raw LPCM audio");

      data = stsd_entry->data;
      len = QT_UINT32 (data);

      if (stsd_version == 0 && version == 0x00020000 && len >= 16 + 56) {
        /* sample description entry (16) + sound sample description v0 (20) */
        depth = QT_UINT32 (data + 36 + 20);
        flags = QT_UINT32 (data + 36 + 24);
        width = QT_UINT32 (data + 36 + 28) * 8 / entry->n_channels;
      }

      if ((flags & FLAG_IS_FLOAT) == 0) {
        if (depth == 0)
          depth = 16;
        if (width == 0)
          width = 16;
        if ((flags & FLAG_IS_ALIGNED_HIGH))
          depth = width;

        format = gst_audio_format_build_integer ((flags & FLAG_IS_SIGNED) ?
            TRUE : FALSE, (flags & FLAG_IS_BIG_ENDIAN) ?
            G_BIG_ENDIAN : G_LITTLE_ENDIAN, width, depth);
        caps = gst_caps_new_simple ("audio/x-raw",
            "format", G_TYPE_STRING,
            format !=
            GST_AUDIO_FORMAT_UNKNOWN ? gst_audio_format_to_string (format) :
            "UNKNOWN", "layout", G_TYPE_STRING,
            (flags & FLAG_IS_NON_INTERLEAVED) ? "non-interleaved" :
            "interleaved", NULL);
        stream->alignment = GST_ROUND_UP_8 (depth);
        stream->alignment = round_up_pow2 (stream->alignment);
      } else {
        if (width == 0)
          width = 32;
        if (width == 64) {
          if (flags & FLAG_IS_BIG_ENDIAN)
            format = GST_AUDIO_FORMAT_F64BE;
          else
            format = GST_AUDIO_FORMAT_F64LE;
        } else {
          if (flags & FLAG_IS_BIG_ENDIAN)
            format = GST_AUDIO_FORMAT_F32BE;
          else
            format = GST_AUDIO_FORMAT_F32LE;
        }
        caps = gst_caps_new_simple ("audio/x-raw",
            "format", G_TYPE_STRING, gst_audio_format_to_string (format),
            "layout", G_TYPE_STRING, (flags & FLAG_IS_NON_INTERLEAVED) ?
            "non-interleaved" : "interleaved", NULL);
        stream->alignment = width / 8;
      }
      break;
    }
    case FOURCC_ipcm:
    case FOURCC_fpcm:
    {
      _codec ("RAW PCM audio");
      caps =
          gst_caps_new_simple ("audio/x-raw", "layout", G_TYPE_STRING,
          "interleaved", NULL);
      break;
    }
    case GST_MAKE_FOURCC ('a', 'c', '-', '4'):
    {
      _codec ("AC4");
      caps = gst_caps_new_empty_simple ("audio/x-ac4");
      break;
    }
    case GST_MAKE_FOURCC ('q', 't', 'v', 'r'):
      /* ? */
    default:
    {
      caps = _get_unknown_codec_name ("audio", fourcc);
      break;
    }
  }

  if (caps) {
    GstCaps *templ_caps =
        gst_static_pad_template_get_caps (&gst_qtdemux_audiosrc_template);
    GstCaps *intersection = gst_caps_intersect (caps, templ_caps);
    gst_caps_unref (caps);
    gst_caps_unref (templ_caps);
    caps = intersection;
  }

  /* enable clipping for raw audio streams */
  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);
  if (g_str_has_prefix (name, "audio/x-raw")) {
    stream->need_clip = TRUE;
    stream->min_buffer_size = 1024 * entry->bytes_per_frame;
    stream->max_buffer_size = entry->rate * entry->bytes_per_frame;
    GST_DEBUG ("setting min/max buffer sizes to %d/%d", stream->min_buffer_size,
        stream->max_buffer_size);
  }
  return caps;
}

static GstCaps *
qtdemux_sub_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    GNode * stsd_entry, gchar ** codec_name)
{
  GstCaps *caps;

  GST_DEBUG_OBJECT (qtdemux, "resolve fourcc 0x%08x", GUINT32_TO_BE (fourcc));

  switch (fourcc) {
    case FOURCC_mp4s:
      _codec ("DVD subtitle");
      caps = gst_caps_new_empty_simple ("subpicture/x-dvd");
      stream->process_func = gst_qtdemux_process_buffer_dvd;
      break;
    case FOURCC_text:
      _codec ("Quicktime timed text");
      goto text;
    case FOURCC_tx3g:
      _codec ("3GPP timed text");
    text:
      caps = gst_caps_new_simple ("text/x-raw", "format", G_TYPE_STRING,
          "utf8", NULL);
      /* actual text piece needs to be extracted */
      stream->process_func = gst_qtdemux_process_buffer_text;
      break;
    case FOURCC_stpp:
      _codec ("XML subtitles");
      caps = gst_caps_new_empty_simple ("application/ttml+xml");
      break;
    case FOURCC_wvtt:
    {
      GstBuffer *buffer;
      const gchar *buf = "WEBVTT\n\n";

      _codec ("WebVTT subtitles");
      caps = gst_caps_new_empty_simple ("application/x-subtitle-vtt");
      stream->process_func = gst_qtdemux_process_buffer_wvtt;

      /* FIXME: Parse the vttC atom and get the entire WEBVTT header */
      buffer = gst_buffer_new_and_alloc (8);
      gst_buffer_fill (buffer, 0, buf, 8);
      stream->buffers = g_slist_append (stream->buffers, buffer);

      break;
    }
    case FOURCC_c608:
      _codec ("CEA 608 Closed Caption");
      caps =
          gst_caps_new_simple ("closedcaption/x-cea-608", "format",
          G_TYPE_STRING, "s334-1a", NULL);
      stream->process_func = gst_qtdemux_process_buffer_clcp;
      stream->need_split = TRUE;
      break;
    case FOURCC_c708:
      _codec ("CEA 708 Closed Caption");
      caps =
          gst_caps_new_simple ("closedcaption/x-cea-708", "format",
          G_TYPE_STRING, "cdp", NULL);
      stream->process_func = gst_qtdemux_process_buffer_clcp;
      break;

    default:
    {
      caps = _get_unknown_codec_name ("text", fourcc);
      break;
    }
  }
  return caps;
}

static GstCaps *
qtdemux_meta_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    GNode * stsd_entry, gchar ** codec_name)
{
  GstCaps *caps = NULL;

  GST_DEBUG_OBJECT (qtdemux, "resolve fourcc 0x%08x", GUINT32_TO_BE (fourcc));

  switch (fourcc) {
    case FOURCC_metx:{
      const guint8 *stsd_entry_data = stsd_entry->data;
      gsize size = QT_UINT32 (stsd_entry_data);
      GstByteReader reader = GST_BYTE_READER_INIT (stsd_entry_data, size);
      const gchar *content_encoding;
      const gchar *namespaces;
      const gchar *schema_locations;

      if (!gst_byte_reader_skip (&reader, 8 + 6 + 2)) {
        GST_WARNING_OBJECT (qtdemux, "Too short metx sample entry");
        break;
      }

      if (!gst_byte_reader_get_string (&reader, &content_encoding) ||
          !gst_byte_reader_get_string (&reader, &namespaces) ||
          !gst_byte_reader_get_string (&reader, &schema_locations)) {
        GST_WARNING_OBJECT (qtdemux, "Too short metx sample entry");
        break;
      }

      if (strstr (namespaces, "http://www.onvif.org/ver10/schema") != 0) {
        if (content_encoding == NULL || *content_encoding == '\0'
            || g_ascii_strcasecmp (content_encoding, "xml") == 0) {
          _codec ("ONVIF Timed XML MetaData");
          caps =
              gst_caps_new_simple ("application/x-onvif-metadata", "parsed",
              G_TYPE_BOOLEAN, TRUE, NULL);
        } else {
          GST_DEBUG_OBJECT (qtdemux, "Unknown content encoding: %s",
              content_encoding);
        }
      } else {
        GST_DEBUG_OBJECT (qtdemux, "Unknown metadata namespaces: %s",
            namespaces);
      }

      break;
    }
    default:
      break;
  }

  if (!caps)
    caps = _get_unknown_codec_name ("meta", fourcc);

  return caps;
}

static GstCaps *
qtdemux_generic_caps (GstQTDemux * qtdemux, QtDemuxStream * stream,
    QtDemuxStreamStsdEntry * entry, guint32 fourcc,
    GNode * stsd_entry, gchar ** codec_name)
{
  GstCaps *caps;

  switch (fourcc) {
    case FOURCC_m1v:
      _codec ("MPEG 1 video");
      caps = gst_caps_new_simple ("video/mpeg", "mpegversion", G_TYPE_INT, 1,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    default:
      caps = NULL;
      break;
  }
  return caps;
}

static void
gst_qtdemux_append_protection_system_id (GstQTDemux * qtdemux,
    const gchar * system_id)
{
  gint i;

  if (!qtdemux->protection_system_ids)
    qtdemux->protection_system_ids =
        g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);
  /* Check whether we already have an entry for this system ID. */
  for (i = 0; i < qtdemux->protection_system_ids->len; ++i) {
    const gchar *id = g_ptr_array_index (qtdemux->protection_system_ids, i);
    if (g_ascii_strcasecmp (system_id, id) == 0) {
      return;
    }
  }
  GST_DEBUG_OBJECT (qtdemux, "Adding cenc protection system ID %s", system_id);
  g_ptr_array_add (qtdemux->protection_system_ids, g_ascii_strdown (system_id,
          -1));
}
