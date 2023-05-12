/* GStreamer Editing Services
 *
 * Copyright (C) <2015> Mathieu Duponchelle <mathieu.duponchelle@opencreed.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ges-structure-parser.h"

#include <ges/ges.h>

G_DEFINE_TYPE (GESStructureParser, ges_structure_parser, G_TYPE_OBJECT);

static void
ges_structure_parser_init (GESStructureParser * self)
{
}

static void
_finalize (GObject * self)
{
  GESStructureParser *parser = GES_STRUCTURE_PARSER (self);

  g_list_free_full (parser->structures, (GDestroyNotify) gst_structure_free);
  g_list_free_full (parser->wrong_strings, (GDestroyNotify) g_free);

  G_OBJECT_CLASS (ges_structure_parser_parent_class)->finalize (self);
}

static void
ges_structure_parser_class_init (GESStructureParserClass * klass)
{
  G_OBJECT_CLASS (klass)->finalize = _finalize;
}

void
ges_structure_parser_parse_string (GESStructureParser * self,
    const gchar * text, gboolean is_symbol)
{
  gchar *new_string = NULL;

  if (self->current_string) {
    new_string = g_strconcat (self->current_string, text, NULL);
  } else if (is_symbol) {
    new_string = g_strdup (text);
  }

  g_free (self->current_string);
  self->current_string = new_string;
}

void
ges_structure_parser_parse_value (GESStructureParser * self, const gchar * text)
{
  /* text starts with '=' */
  gchar *val_string = g_strconcat ("=(string)", text + 1, NULL);
  ges_structure_parser_parse_string (self, val_string, FALSE);
  g_free (val_string);
}

void
ges_structure_parser_parse_default (GESStructureParser * self,
    const gchar * text)
{
  gchar *new_string = NULL;

  if (self->add_comma && self->current_string) {
    new_string = g_strconcat (self->current_string, ",", text, NULL);
    g_free (self->current_string);
    self->current_string = new_string;
    self->add_comma = FALSE;
  } else {
    ges_structure_parser_parse_string (self, text, FALSE);
  }
}

void
ges_structure_parser_parse_whitespace (GESStructureParser * self)
{
  self->add_comma = TRUE;
}

static void
_finish_structure (GESStructureParser * self)
{
  GstStructure *structure;

  if (!self->current_string)
    return;

  structure = gst_structure_new_from_string (self->current_string);

  if (structure == NULL) {
    GST_ERROR ("Could not parse %s", self->current_string);

    self->wrong_strings = g_list_append (self->wrong_strings,
        self->current_string);
    self->current_string = NULL;
    return;
  }

  self->structures = g_list_append (self->structures, structure);
  g_free (self->current_string);
  self->current_string = NULL;
}


void
ges_structure_parser_end_of_file (GESStructureParser * self)
{
  _finish_structure (self);
}

void
ges_structure_parser_parse_symbol (GESStructureParser * self,
    const gchar * symbol)
{
  _finish_structure (self);

  while (*symbol == ' ' || *symbol == '+')
    symbol++;

  self->add_comma = FALSE;
  if (!g_ascii_strncasecmp (symbol, "clip", 4))
    ges_structure_parser_parse_string (self, "clip, uri=(string)", TRUE);
  else if (!g_ascii_strncasecmp (symbol, "test-clip", 9))
    ges_structure_parser_parse_string (self, "test-clip, pattern=(string)",
        TRUE);
  else if (!g_ascii_strncasecmp (symbol, "effect", 6))
    ges_structure_parser_parse_string (self, "effect, bin-description=(string)",
        TRUE);
  else if (!g_ascii_strncasecmp (symbol, "transition", 10))
    ges_structure_parser_parse_string (self, "transition, type=(string)", TRUE);
  else if (!g_ascii_strncasecmp (symbol, "title", 5))
    ges_structure_parser_parse_string (self, "title, text=(string)", TRUE);
  else if (!g_ascii_strncasecmp (symbol, "track", 5))
    ges_structure_parser_parse_string (self, "track, type=(string)", TRUE);
  else if (!g_ascii_strncasecmp (symbol, "keyframes", 8)) {
    ges_structure_parser_parse_string (self,
        "keyframes, property-name=(string)", TRUE);
  }
}

void
ges_structure_parser_parse_setter (GESStructureParser * self,
    const gchar * setter)
{
  gchar *parsed_setter;

  _finish_structure (self);

  while (*setter == '-' || *setter == ' ')
    setter++;

  while (*setter != '-')
    setter++;

  setter++;

  parsed_setter = g_strdup_printf ("set-property, property=(string)%s, "
      "value=(string)", setter);
  self->add_comma = FALSE;
  ges_structure_parser_parse_string (self, parsed_setter, TRUE);
  g_free (parsed_setter);
}

GESStructureParser *
ges_structure_parser_new (void)
{
  return (g_object_new (GES_TYPE_STRUCTURE_PARSER, NULL));
}

GError *
ges_structure_parser_get_error (GESStructureParser * self)
{
  GList *tmp;
  GError *error = NULL;
  GString *msg = NULL;

  if (self->wrong_strings == NULL)
    return NULL;

  msg = g_string_new ("Could not parse: ");


  for (tmp = self->wrong_strings; tmp; tmp = tmp->next) {
    g_string_append_printf (msg, " %s", (gchar *) tmp->data);
  }

  error = g_error_new_literal (GES_ERROR, 0, msg->str);
  g_string_free (msg, TRUE);

  return error;
}
