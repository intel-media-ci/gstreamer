/* GStreamer
 * Copyright (C) 2023 Seungha Yang <seungha@centricular.com>
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
#include "config.h"
#endif

#include "gstd3d11ipc.h"
#include <gst/d3d11/gstd3d11-private.h>
#include <string.h>
#include <cctype>
#include <locale>
#include <codecvt>
#include <algorithm>

#define GST_D3D11_IPC_MAGIC_NUMBER 0xD3D1110C

bool
gst_d3d11_ipc_pkt_identify (std::vector < guint8 > &buf,
    GstD3D11IpcPacketHeader & header)
{
  g_return_val_if_fail (buf.size () >= GST_D3D11_IPC_PKT_HEADER_SIZE, false);

  memcpy (&header, &buf[0], GST_D3D11_IPC_PKT_HEADER_SIZE);

  if (header.magic != GST_D3D11_IPC_MAGIC_NUMBER)
    return false;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE + header.payload_size);

  return true;
}

bool
gst_d3d11_ipc_pkt_build_config (std::vector < guint8 > &buf,
    gint64 adapter_luid, GstCaps * caps)
{
  GstD3D11IpcPacketHeader header;
  guint8 *ptr;
  gchar *caps_str = nullptr;
  guint caps_size = 0;

  g_return_val_if_fail (GST_IS_CAPS (caps), false);

  caps_str = gst_caps_serialize (caps, GST_SERIALIZE_FLAG_NONE);
  if (!caps_str)
    return false;

  caps_size = strlen (caps_str) + 1;

  header.type = GstD3D11IpcPktType::CONFIG;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = sizeof (gint64) + caps_size;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE + header.payload_size);

  ptr = &buf[0];

  memcpy (ptr, &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
  ptr += GST_D3D11_IPC_PKT_HEADER_SIZE;

  memcpy (ptr, &adapter_luid, sizeof (gint64));
  ptr += sizeof (gint64);

  strcpy ((char *) ptr, caps_str);
  g_free (caps_str);

  return true;
}

bool
gst_d3d11_ipc_pkt_parse_config (std::vector < guint8 > &buf,
    gint64 & adapter_luid, GstCaps ** caps)
{
  GstD3D11IpcPacketHeader header;
  const guint8 *ptr;
  std::string str;

  g_return_val_if_fail (buf.size () >
      GST_D3D11_IPC_PKT_HEADER_SIZE + sizeof (gint64), false);
  g_return_val_if_fail (caps, false);

  ptr = &buf[0];
  memcpy (&header, ptr, GST_D3D11_IPC_PKT_HEADER_SIZE);

  if (header.type != GstD3D11IpcPktType::CONFIG ||
      header.magic != GST_D3D11_IPC_MAGIC_NUMBER ||
      header.payload_size <= sizeof (gint64)) {
    return false;
  }

  ptr += GST_D3D11_IPC_PKT_HEADER_SIZE;

  memcpy (&adapter_luid, ptr, sizeof (gint64));
  ptr += sizeof (gint64);

  *caps = gst_caps_from_string ((const gchar *) ptr);
  if (*caps == nullptr)
    return false;

  return true;
}

void
gst_d3d11_ipc_pkt_build_need_data (std::vector < guint8 > &buf)
{
  GstD3D11IpcPacketHeader header;

  header.type = GstD3D11IpcPktType::NEED_DATA;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = 0;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE);

  memcpy (&buf[0], &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
}

/* *INDENT-OFF* */
bool
gst_d3d11_ipc_pkt_build_have_data (std::vector < guint8 > &buf,
    GstClockTime pts, const GstD3D11IpcMemLayout & layout,
    const std::wstring & name, GstCaps * caps)
{
  GstD3D11IpcPacketHeader header;
  guint8 *ptr;
  guint name_size;
  gchar *caps_str = nullptr;
  guint caps_size = 1;

  name_size = (name.length () + 1) * sizeof (wchar_t);

  if (caps) {
    caps_str = gst_caps_serialize (caps, GST_SERIALIZE_FLAG_NONE);
    if (!caps_str)
      return false;

    caps_size = strlen (caps_str) + 1;
  }

  header.type = GstD3D11IpcPktType::HAVE_DATA;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = sizeof (GstClockTime) + sizeof (GstD3D11IpcMemLayout) +
      name_size + caps_size;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE + header.payload_size);

  ptr = &buf[0];
  memcpy (ptr, &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
  ptr += GST_D3D11_IPC_PKT_HEADER_SIZE;

  memcpy (ptr, &pts, sizeof (GstClockTime));
  ptr += sizeof (GstClockTime);

  memcpy (ptr, &layout, sizeof (GstD3D11IpcMemLayout));
  ptr += sizeof (GstD3D11IpcMemLayout);

  wcscpy ((wchar_t *) ptr, name.c_str ());
  ptr += name_size;

  if (caps) {
    *ptr = 1;
    ptr++;

    strcpy ((char *) ptr, caps_str);
  } else {
    *ptr = 0;
  }

  g_free (caps_str);

  return true;
}
/* *INDENT-ON* */

bool
gst_d3d11_ipc_pkt_parse_have_data (const std::vector < guint8 > &buf,
    GstClockTime & pts, GstD3D11IpcMemLayout & layout,
    std::wstring & name, GstCaps ** caps)
{
  GstD3D11IpcPacketHeader header;
  const guint8 *ptr;
  std::string str;

  g_return_val_if_fail (buf.size () >
      GST_D3D11_IPC_PKT_HEADER_SIZE + sizeof (GstClockTime) +
      sizeof (GstD3D11IpcMemLayout), false);
  g_return_val_if_fail (caps, false);

  ptr = &buf[0];
  memcpy (&header, ptr, GST_D3D11_IPC_PKT_HEADER_SIZE);

  if (header.type != GstD3D11IpcPktType::HAVE_DATA ||
      header.magic != GST_D3D11_IPC_MAGIC_NUMBER ||
      header.payload_size <= sizeof (GstClockTime) +
      sizeof (GstD3D11IpcMemLayout)) {
    return false;
  }
  ptr += GST_D3D11_IPC_PKT_HEADER_SIZE;

  memcpy (&pts, ptr, sizeof (GstClockTime));
  ptr += sizeof (GstClockTime);

  memcpy (&layout, ptr, sizeof (GstD3D11IpcMemLayout));
  ptr += sizeof (GstD3D11IpcMemLayout);

  name = (wchar_t *) ptr;

  ptr += (name.size () + 1) * sizeof (wchar_t);

  if (*ptr) {
    ptr++;

    *caps = gst_caps_from_string ((const gchar *) ptr);
    if (*caps == nullptr)
      return false;
  }

  return true;
}

void
gst_d3d11_ipc_pkt_build_read_done (std::vector < guint8 > &buf)
{
  GstD3D11IpcPacketHeader header;

  header.type = GstD3D11IpcPktType::READ_DONE;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = 0;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE);

  memcpy (&buf[0], &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
}

void
gst_d3d11_ipc_pkt_build_release_data (std::vector < guint8 > &buf,
    const std::wstring & name)
{
  GstD3D11IpcPacketHeader header;
  guint8 *ptr;

  header.type = GstD3D11IpcPktType::RELEASE_DATA;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = (name.size () + 1) * sizeof (wchar_t);

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE + header.payload_size);

  ptr = &buf[0];
  memcpy (ptr, &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
  ptr += GST_D3D11_IPC_PKT_HEADER_SIZE;

  wcscpy ((wchar_t *) ptr, name.c_str ());
}

bool
gst_d3d11_ipc_pkt_parse_release_data (std::vector < guint8 > &buf,
    std::wstring & name)
{
  g_return_val_if_fail (buf.size () > GST_D3D11_IPC_PKT_HEADER_SIZE, false);

  name = (wchar_t *) (&buf[0] + GST_D3D11_IPC_PKT_HEADER_SIZE);

  return true;
}

void
gst_d3d11_ipc_pkt_build_eos (std::vector < guint8 > &buf)
{
  GstD3D11IpcPacketHeader header;

  header.type = GstD3D11IpcPktType::EOS;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = 0;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE);

  memcpy (&buf[0], &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
}

void
gst_d3d11_ipc_pkt_build_fin (std::vector < guint8 > &buf)
{
  GstD3D11IpcPacketHeader header;

  header.type = GstD3D11IpcPktType::FIN;
  header.magic = GST_D3D11_IPC_MAGIC_NUMBER;
  header.payload_size = 0;

  buf.resize (GST_D3D11_IPC_PKT_HEADER_SIZE);

  memcpy (&buf[0], &header, GST_D3D11_IPC_PKT_HEADER_SIZE);
}

bool
gst_d3d11_ipc_clock_is_system (GstClock * clock)
{
  GstClockType clock_type = GST_CLOCK_TYPE_MONOTONIC;
  GstClock *mclock;

  if (G_OBJECT_TYPE (clock) != GST_TYPE_SYSTEM_CLOCK)
    return false;

  g_object_get (clock, "clock-type", &clock_type, nullptr);
  if (clock_type != GST_CLOCK_TYPE_MONOTONIC)
    return false;

  mclock = gst_clock_get_master (clock);
  if (!mclock)
    return true;

  gst_object_unref (mclock);
  return false;
}

GstClockTime
gst_d3d11_ipc_clock_now (void)
{
  static LARGE_INTEGER freq;
  LARGE_INTEGER cur_time;

  GST_D3D11_CALL_ONCE_BEGIN {
    QueryPerformanceFrequency (&freq);
  } GST_D3D11_CALL_ONCE_END;

  QueryPerformanceCounter (&cur_time);
  return gst_util_uint64_scale (cur_time.QuadPart, GST_SECOND, freq.QuadPart);
}

std::string
gst_d3d11_ipc_wstring_to_string (const std::wstring & str)
{
  std::wstring_convert < std::codecvt_utf8 < wchar_t >>conv;
  return conv.to_bytes (str);
}

std::wstring
gst_d3d11_ipc_string_to_wstring (const std::string & str)
{
  std::wstring_convert < std::codecvt_utf8 < wchar_t >>conv;
  return conv.from_bytes (str);
}

/* *INDENT-OFF* */
static inline void rtrim(std::string &s) {
  s.erase (std::find_if (s.rbegin(), s.rend(),
      [](unsigned char ch) {
        return !std::isspace (ch);
      }).base (), s.end ());
}
/* *INDENT-ON* */

std::string
gst_d3d11_ipc_win32_error_to_string (guint err)
{
  wchar_t buffer[1024];

  if (!FormatMessageW (FORMAT_MESSAGE_IGNORE_INSERTS |
          FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, buffer, 1024, nullptr)) {
    return std::string ("");
  }

  std::string ret = gst_d3d11_ipc_wstring_to_string (buffer);
  rtrim (ret);

  return ret;
}

gint64
gst_d3d11_ipc_get_shared_resource_token (void)
{
  static gint64 token = 0;

  GST_D3D11_CALL_ONCE_BEGIN {
    token = gst_d3d11_create_user_token ();
  } GST_D3D11_CALL_ONCE_END;

  return token;
}

static DWORD
gst_d3d11_ipc_get_pid (void)
{
  static DWORD pid = 0;

  GST_D3D11_CALL_ONCE_BEGIN {
    pid = GetCurrentProcessId ();
  } GST_D3D11_CALL_ONCE_END;

  return pid;
}

std::wstring
gst_d3d11_ipc_get_resource_prefix (void)
{
  static ULONG global_index = 0;

  std::wstring prefix = std::wstring (L"Local\\gst.d3d11.ipc.") +
      std::to_wstring (gst_d3d11_ipc_get_pid ()) + std::wstring (L".") +
      std::to_wstring (InterlockedIncrement (&global_index)) +
      std::wstring (L".");

  return prefix;
}
