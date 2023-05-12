/* GStreamer
 * Copyright (C) 2020 Igalia, S.L.
 *     Author: Víctor Jáquez <vjaquez@igalia.com>
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
 * SECTION:gstvadisplay
 * @title: GstVaDisplay
 * @short_description: Generic VADisplay wrapper.
 * @sources:
 * - gstva.h
 * - gstvadisplay.h
 *
 * It is a generic wrapper for VADisplay. To create new instances
 * subclasses are required, depending on the display type to use
 * (v.gr. DRM, X11, Wayland, etc.).
 *
 * The purpose of this class is to be shared among pipelines via
 * #GstContext so all the VA processing elements will use the same
 * display entry. Application developers can create their own
 * subclass, based on their display, and shared it via the synced bus
 * message for the application.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvadisplay.h"

#include <va/va.h>

GST_DEBUG_CATEGORY (gst_va_display_debug);
#define GST_CAT_DEFAULT gst_va_display_debug

typedef struct _GstVaDisplayPrivate GstVaDisplayPrivate;
struct _GstVaDisplayPrivate
{
  VADisplay display;

  gboolean foreign;
  gboolean init;
  GstVaImplementation impl;
  gchar *vendor_desc;
};

#define gst_va_display_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVaDisplay, gst_va_display, GST_TYPE_OBJECT,
    G_ADD_PRIVATE (GstVaDisplay);
    GST_DEBUG_CATEGORY_INIT (gst_va_display_debug, "vadisplay", 0,
        "VA Display"));
enum
{
  PROP_VA_DISPLAY = 1,
  PROP_DESC,
  N_PROPERTIES
};

static GParamSpec *g_properties[N_PROPERTIES];

#define GET_PRIV(obj) gst_va_display_get_instance_private (GST_VA_DISPLAY (obj))

static GstVaImplementation
_get_implementation (const char *vendor)
{
  if (g_str_has_prefix (vendor, "Mesa Gallium driver"))
    return GST_VA_IMPLEMENTATION_MESA_GALLIUM;
  else if (g_str_has_prefix (vendor, "Intel i965 driver"))
    return GST_VA_IMPLEMENTATION_INTEL_I965;
  else if (g_str_has_prefix (vendor, "Intel iHD driver"))
    return GST_VA_IMPLEMENTATION_INTEL_IHD;

  return GST_VA_IMPLEMENTATION_OTHER;
}

static char *
_get_desc (const char *vendor, GstVaImplementation impl)
{
  char *end, *start;
  char desc[1024];
  size_t size;

  if (impl == GST_VA_IMPLEMENTATION_OTHER)
    return g_strdup (vendor);

  start = strstr (vendor, "for ");
  if (!start)
    return g_strdup (vendor);
  start += 4;

  switch (impl) {
    case GST_VA_IMPLEMENTATION_MESA_GALLIUM:
      end = strchr (start, '(');
      break;
    default:
      end = strstr (start, "- ");
      break;
  }

  if (!end)
    return g_strdup (vendor);
  end -= 1;

  size = MIN (1024, end - start);
  memcpy (desc, start, size);
  desc[size] = '\0';
  return g_strdup (desc);
}

static gboolean
_gst_va_display_filter_driver (GstVaDisplay * self, gpointer foreign_display)
{
  GstVaDisplayPrivate *priv = GET_PRIV (self);
  VADisplay dpy;
  const char *vendor;

  g_assert ((foreign_display != NULL) ^ (priv->display != NULL));
  dpy = foreign_display ? foreign_display : priv->display;

  vendor = vaQueryVendorString (dpy);
  GST_INFO ("VA-API driver vendor: %s", vendor);

  /* XXX(victor): driver allow list */

  if (foreign_display) {
    priv->display = foreign_display;
    priv->foreign = TRUE;
  }
  priv->impl = _get_implementation (vendor);
  priv->vendor_desc = _get_desc (vendor, priv->impl);

  return TRUE;
}

static void
gst_va_display_set_display (GstVaDisplay * self, gpointer display)
{
  GstVaDisplayPrivate *priv = GET_PRIV (self);

  if (!display)
    return;

  if (vaDisplayIsValid (display) == 0) {
    GST_WARNING_OBJECT (self,
        "User's VA display is invalid. An internal one will be tried.");
    return;
  }

  /* assume driver is already initialized */
  priv->init = TRUE;

  _gst_va_display_filter_driver (self, display);
}

static void
gst_va_display_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVaDisplay *self = GST_VA_DISPLAY (object);

  switch (prop_id) {
    case PROP_VA_DISPLAY:{
      gpointer display = g_value_get_pointer (value);
      gst_va_display_set_display (self, display);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_va_display_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVaDisplayPrivate *priv = GET_PRIV (object);

  switch (prop_id) {
    case PROP_VA_DISPLAY:
      g_value_set_pointer (value, priv->display);
      break;
    case PROP_DESC:
      g_value_set_string (value, priv->vendor_desc);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_va_display_constructed (GObject * object)
{
  GstVaDisplay *self = GST_VA_DISPLAY (object);
  GstVaDisplayPrivate *priv = GET_PRIV (object);
  GstVaDisplayClass *klass = GST_VA_DISPLAY_GET_CLASS (object);

  if (!priv->display && klass->create_va_display)
    priv->display = klass->create_va_display (self);

  G_OBJECT_CLASS (parent_class)->constructed (object);
}

static void
gst_va_display_dispose (GObject * object)
{
  GstVaDisplayPrivate *priv = GET_PRIV (object);

  if (priv->display && !priv->foreign)
    vaTerminate (priv->display);
  priv->display = NULL;

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_va_display_finalize (GObject * object)
{
  GstVaDisplayPrivate *priv = GET_PRIV (object);

  g_free (priv->vendor_desc);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_va_display_class_init (GstVaDisplayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = gst_va_display_set_property;
  gobject_class->get_property = gst_va_display_get_property;
  gobject_class->constructed = gst_va_display_constructed;
  gobject_class->dispose = gst_va_display_dispose;
  gobject_class->finalize = gst_va_display_finalize;

  g_properties[PROP_VA_DISPLAY] =
      g_param_spec_pointer ("va-display", "VADisplay", "VA Display handler",
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_properties[PROP_DESC] =
      g_param_spec_string ("description", "Description",
      "Vendor specific VA implementation description", NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, g_properties);
}

static void
gst_va_display_init (GstVaDisplay * self)
{
  GstVaDisplayPrivate *priv = GET_PRIV (self);

  priv->impl = GST_VA_IMPLEMENTATION_INVALID;
}

#ifndef GST_DISABLE_GST_DEBUG
static gchar *
_strip_msg (const char *message)
{
  gchar *msg = g_strdup (message);
  if (!msg)
    return NULL;
  return g_strstrip (msg);
}

static void
_va_warning (gpointer object, const char *message)
{
  GstVaDisplay *self = GST_VA_DISPLAY (object);
  gchar *msg;

  if ((msg = _strip_msg (message))) {
    GST_WARNING_OBJECT (self, "VA error: %s", msg);
    g_free (msg);
  }
}

static void
_va_info (gpointer object, const char *message)
{
  GstVaDisplay *self = GST_VA_DISPLAY (object);
  gchar *msg;

  if ((msg = _strip_msg (message))) {
    GST_INFO_OBJECT (self, "VA info: %s", msg);
    g_free (msg);
  }
}
#endif

/**
 * gst_va_display_initialize:
 * @self: a #GstVaDisplay
 *
 * If the display is set by the user (foreign) it is assumed that the
 * driver is already initialized, thus this function is noop.
 *
 * If the display is opened internally, this function will initialize
 * the driver and it will set driver's message callbacks.
 *
 * NOTE: this function is supposed to be private, only used by
 * GstVaDisplay descendants.
 *
 * Returns: %TRUE if the VA driver can be initialized; %FALSE
 *     otherwise
 *
 * Since: 1.20
 **/
gboolean
gst_va_display_initialize (GstVaDisplay * self)
{
  GstVaDisplayPrivate *priv;
  VAStatus status;
  int major_version = -1, minor_version = -1;

  g_return_val_if_fail (GST_IS_VA_DISPLAY (self), FALSE);

  priv = GET_PRIV (self);

  if (priv->init)
    return TRUE;

  if (!priv->display)
    return FALSE;

#ifndef GST_DISABLE_GST_DEBUG
  vaSetErrorCallback (priv->display, _va_warning, self);
  vaSetInfoCallback (priv->display, _va_info, self);
#endif

  status = vaInitialize (priv->display, &major_version, &minor_version);
  if (status != VA_STATUS_SUCCESS) {
    GST_WARNING_OBJECT (self, "vaInitialize: %s", vaErrorStr (status));
    return FALSE;
  }

  GST_INFO_OBJECT (self, "VA-API version %d.%d", major_version, minor_version);

  priv->init = TRUE;

  if (!_gst_va_display_filter_driver (self, NULL))
    return FALSE;

  return TRUE;
}

/**
 * gst_va_display_get_va_dpy:
 * @self: a #GstVaDisplay type display.
 *
 * Get the VA display handle of the @self.
 *
 * Returns: the VA display handle.
 *
 * Since: 1.20
 */
gpointer
gst_va_display_get_va_dpy (GstVaDisplay * self)
{
  VADisplay dpy;

  g_return_val_if_fail (GST_IS_VA_DISPLAY (self), NULL);

  g_object_get (self, "va-display", &dpy, NULL);
  return dpy;
}

/**
 * gst_va_display_get_implementation:
 * @self: a #GstVaDisplay type display.
 *
 * Get the the #GstVaImplementation type of @self.
 *
 * Returns: #GstVaImplementation.
 *
 * Since: 1.20
 */
GstVaImplementation
gst_va_display_get_implementation (GstVaDisplay * self)
{
  GstVaDisplayPrivate *priv;

  g_return_val_if_fail (GST_IS_VA_DISPLAY (self),
      GST_VA_IMPLEMENTATION_INVALID);

  priv = GET_PRIV (self);
  return priv->impl;
}
