#ifndef __GST_PARSE_TYPES_H__
#define __GST_PARSE_TYPES_H__

#include <glib-object.h>
#include "../gstelement.h"
#include "../gstparse.h"

typedef struct {
  GstElement *element;
  gchar *name;
  GSList *pads;
} reference_t;

typedef struct {
  reference_t src;
  reference_t sink;
  GstCaps *caps;
  gboolean all_pads;
} link_t;

typedef struct {
  GSList *elements;
  reference_t first;
  reference_t last;
} chain_t;

typedef struct {
  gchar *factory_name;
  GSList *values;
  GSList *presets;
} element_t;

/* Filled from a bus sync handler if a error message is posted during the
 * construction of the chain.
 * The mutex is necessary as the bus could -- at least in principle -- have
 * messages posted from several threads simultaneously. */
typedef struct {
  GMutex mutex;
  gchar *reason; /* owned by the struct */
} reason_receiver_t;


typedef struct _graph_t graph_t;
struct _graph_t {
  chain_t *chain; /* links are supposed to be done now */
  GSList *links;
  GError **error;
  reason_receiver_t *error_probable_reason_receiver;
  GstParseContext *ctx; /* may be NULL */
  GstParseFlags flags;
};


/*
 * Memory checking. Should probably be done with gsttrace stuff, but that
 * doesn't really work.
 * This is not safe from reentrance issues, but that doesn't matter as long as
 * we lock a mutex before parsing anyway.
 *
 * FIXME: Disable this for now for the above reasons
 */
#if 0
#ifdef GST_DEBUG_ENABLED
#  define __GST_PARSE_TRACE
#endif
#endif

#ifdef __GST_PARSE_TRACE
G_GNUC_INTERNAL  gchar  *__gst_parse_strdup (gchar *org);
G_GNUC_INTERNAL  void	__gst_parse_strfree (gchar *str);
G_GNUC_INTERNAL  link_t *__gst_parse_link_new (void);
G_GNUC_INTERNAL  void	__gst_parse_link_free (link_t *data);
G_GNUC_INTERNAL  chain_t *__gst_parse_chain_new (void);
G_GNUC_INTERNAL  void	__gst_parse_chain_free (chain_t *data);
G_GNUC_INTERNAL  element_t *__gst_parse_element_new (void);
G_GNUC_INTERNAL  void	__gst_parse_element_free (element_t *data);
#  define gst_parse_strdup __gst_parse_strdup
#  define gst_parse_strfree __gst_parse_strfree
#  define gst_parse_link_new __gst_parse_link_new
#  define gst_parse_link_free __gst_parse_link_free
#  define gst_parse_chain_new __gst_parse_chain_new
#  define gst_parse_chain_free __gst_parse_chain_free
#  define gst_parse_element_new __gst_parse_element_new
#  define gst_parse_element_free __gst_parse_element_free
#else /* __GST_PARSE_TRACE */
#  define gst_parse_strdup g_strdup
#  define gst_parse_strfree g_free
#  define gst_parse_link_new() g_new0 (link_t, 1)
#  define gst_parse_link_free(l) g_free (l)
#  define gst_parse_chain_new() g_new0 (chain_t, 1)
#  define gst_parse_chain_free(c) g_free (c)
#  define gst_parse_element_new() g_new0 (element_t, 1)
#  define gst_parse_element_free(e) g_free (e)
#endif /* __GST_PARSE_TRACE */

static inline void
gst_parse_unescape (gchar *str)
{
  gchar *walk;
  gboolean in_quotes;

  g_return_if_fail (str != NULL);

  walk = str;
  in_quotes = FALSE;

  GST_DEBUG ("unescaping %s", str);

  while (*walk) {
    if (*walk == '\\' && !in_quotes) {
      walk++;
      /* make sure we don't read beyond the end of the string */
      if (*walk == '\0')
        break;
    } else if (*walk == '"' && (!in_quotes || *(walk - 1) != '\\')) {
      /* don't unescape inside quotes and don't switch
       * state with escaped quoted inside quotes */
      in_quotes = !in_quotes;
    }
    *str = *walk;
    str++;
    walk++;
  }
  *str = '\0';
}

G_GNUC_INTERNAL GstElement *priv_gst_parse_launch (const gchar      * str,
                                                   GError          ** err,
                                                   GstParseContext  * ctx,
                                                   GstParseFlags      flags);

#endif /* __GST_PARSE_TYPES_H__ */
