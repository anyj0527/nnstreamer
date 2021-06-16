/* SPDX-License-Identifier: LGPL-2.1-only */
/**
 * Copyright (C) 2021 Samsung Electronics Co., Ltd.
 *
 * @file	tensor_sparse_dec.c
 * @date	02 Jul 2021
 * @brief	GStreamer element to decode sparse tensors into dense tensors
 * @see		https://github.com/nnstreamer/nnstreamer
 * @author	Yongjoo Ahn <yongjoo1.ahn@samsung.com>
 * @bug		No known bugs except for NYI items
 */

/**
 * SECTION:element-tensor_sparse_dec
 *
 * @todo: FILL HERE
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include "tensor_sparse_dec.h"
#include "tensor_data.h"

/**
 * @brief Macro for debug message.
 */
#define silent_debug_caps(caps,msg) do { \
  if (1) { \
    if (caps) { \
      GstStructure *caps_s; \
      gchar *caps_s_string; \
      guint caps_size, caps_idx; \
      caps_size = gst_caps_get_size (caps);\
      for (caps_idx = 0; caps_idx < caps_size; caps_idx++) { \
        caps_s = gst_caps_get_structure (caps, caps_idx); \
        caps_s_string = gst_structure_to_string (caps_s); \
        nns_logw (msg " = %s", caps_s_string); \
        g_free (caps_s_string); \
      } \
    } \
  } \
} while (0)

GST_DEBUG_CATEGORY_STATIC (gst_tensor_sparse_dec_debug);
#define GST_CAT_DEFAULT gst_tensor_sparse_dec_debug

/**
 * @brief tensor_sparse_dec properties
 */
enum
{
  PROP_0,
  PROP_SILENT
};

/**
 * @brief Flag to print minimized log.
 */
#define DEFAULT_SILENT TRUE

/**
 * @brief Template for sink pad.
 */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_TENSORS_FLEX_CAP_DEFAULT));

/**
 * @brief Template for src pad.
 */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_TENSORS_CAP_DEFAULT));

#define gst_tensor_sparse_dec_parent_class parent_class
G_DEFINE_TYPE (GstTensorSparseDec, gst_tensor_sparse_dec, GST_TYPE_ELEMENT);

static void gst_tensor_sparse_dec_finalize (GObject * object);
static void gst_tensor_sparse_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_tensor_sparse_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstFlowReturn
gst_tensor_sparse_dec_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean
gst_tensor_sparse_dec_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean
gst_tensor_sparse_dec_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean
gst_tensor_sparse_dec_src_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean
gst_tensor_sparse_dec_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

/**
 * @brief Initialize the tensor_sparse's class.
 */
static void
gst_tensor_sparse_dec_class_init (GstTensorSparseDecClass * klass)
{
  GObjectClass *object_class;
  GstElementClass *element_class;

  GST_DEBUG_CATEGORY_INIT (gst_tensor_sparse_dec_debug, "tensor_sparse_dec", 0,
      "Element to decode sparse tensors");

  object_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  object_class->set_property = gst_tensor_sparse_dec_set_property;
  object_class->get_property = gst_tensor_sparse_dec_get_property;
  object_class->finalize = gst_tensor_sparse_dec_finalize;

  /**
   * GstTensorSparseDec::silent:
   *
   * The flag to enable/disable debugging messages.
   */
  g_object_class_install_property (object_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output",
          DEFAULT_SILENT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));

  gst_element_class_set_static_metadata (element_class,
      "TensorSparseDec",
      "Filter/Tensor",
      "Element to decode dense tensors into sparse tensors",
      "Samsung Electronics Co., Ltd.");
}

/**
 * @brief Initialize tensor_sparse_dec element.
 */
static void
gst_tensor_sparse_dec_init (GstTensorSparseDec * self)
{
  /* setup sink pad */
  self->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  /* setup src pad */
  self->srcpad = gst_pad_new_from_static_template (&src_template, "src");
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  /* setup chain function */
  gst_pad_set_chain_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_tensor_sparse_dec_chain));

  /* setup event function */
  gst_pad_set_event_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_tensor_sparse_dec_sink_event));

  /* setup event function */
  gst_pad_set_event_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_tensor_sparse_dec_src_event));


  gst_pad_set_query_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_tensor_sparse_dec_sink_query));
  gst_pad_set_query_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_tensor_sparse_dec_src_query));



  /* init properties */
  self->silent = DEFAULT_SILENT;
}

/**
 * @brief Function to finalize instance.
 */
static void
gst_tensor_sparse_dec_finalize (GObject * object)
{
  // GstTensorSparseDec *self;
  // self = GST_TENSOR_SPARSE_DEC (object);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**
 * @brief Setter for tensor_sparse_dec properties.
 */
static void
gst_tensor_sparse_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTensorSparseDec *self;

  self = GST_TENSOR_SPARSE_DEC (object);

  switch (prop_id) {
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 * @brief Getter for tensor_sparse_dec properties.
 */
static void
gst_tensor_sparse_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstTensorSparseDec *self;

  self = GST_TENSOR_SPARSE_DEC (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 * @brief This function handles sink pad query.
 */
static gboolean
gst_tensor_sparse_dec_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstTensorSparseDec *self;

  self = GST_TENSOR_SPARSE_DEC (parent);

  GST_WARNING_OBJECT (self, "Received %s query: %" GST_PTR_FORMAT,
      GST_QUERY_TYPE_NAME (query), query);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      // GstCaps *caps;
      GstCaps *filter;

      gst_query_parse_caps (query, &filter);
      // caps = gst_tensor_sparse_enc_query_caps (self, pad, filter);
      silent_debug_caps (filter, "[sparse_dec][sink_query] Got GST_QUERY_CAPS");
      // gst_query_set_caps_result (query, caps);
      // gst_caps_unref (caps);
      // return TRUE;
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;
      // GstCaps *template_caps;
      // gboolean res = FALSE;

      gst_query_parse_accept_caps (query, &caps);
      silent_debug_caps (caps, "[sparse_dec][sink_query] Got GST_QUERY_ACCEPT_CAPS");

      // if (gst_caps_is_fixed (caps)) {
      //   template_caps = gst_pad_get_pad_template_caps (pad);

      //   res = gst_caps_can_intersect (template_caps, caps);
      //   gst_caps_unref (template_caps);
      // }

      // gst_query_set_accept_caps_result (query, res);
      // return TRUE;
      break;
    }
    default:
      nns_logw ("[sparse_dec][sink_query] Got some query");
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_tensor_sparse_dec_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
g_return_val_if_fail (event != NULL, FALSE);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      /* disable seeking */
      gst_event_unref (event);
      return FALSE;
    case GST_EVENT_EOS:
      nns_logw ("[sparse_dec] Got EOS event");
      break;
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      // GstStructure *structure;

      gst_event_parse_caps (event, &caps);
      // structure = gst_caps_get_structure (caps, 0);
      silent_debug_caps (caps, "[sparse_dec][sink_event] Got GST_EVENT_CAPS");
      // gst_event_unref (event);
      // return gst_tensors_config_validate (&cpad->config);
      break;
    }
    default:
      nns_logw ("[sparse_dec][sink_event] Got some event");
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}


static gboolean
gst_tensor_sparse_dec_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
g_return_val_if_fail (event != NULL, FALSE);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      /* disable seeking */
      gst_event_unref (event);
      return FALSE;
    case GST_EVENT_EOS:
      nns_logw ("[sparse_dec] Got EOS event");
      break;
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      // GstStructure *structure;

      gst_event_parse_caps (event, &caps);
      // structure = gst_caps_get_structure (caps, 0);
      silent_debug_caps (caps, "[sparse_dec][src_event] Got GST_EVENT_CAPS");
      // gst_event_unref (event);
      // return gst_tensors_config_validate (&cpad->config);
      break;
    }
    default:
      nns_logw ("[sparse_dec][src_event] Got some event");
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

/**
 * @brief This function handles src pad query.
 */
static gboolean
gst_tensor_sparse_dec_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstTensorSparseDec *self;

  self = GST_TENSOR_SPARSE_DEC (parent);

  GST_WARNING_OBJECT (self, "Received %s query: %" GST_PTR_FORMAT,
      GST_QUERY_TYPE_NAME (query), query);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      // GstCaps *caps;
      GstCaps *filter;

      gst_query_parse_caps (query, &filter);
      silent_debug_caps (filter, "[sparse_dec][src_query] Got GST_QUERY_CAPS");
      // caps = gst_tensor_sparse_enc_query_caps (self, pad, filter);

      // gst_query_set_caps_result (query, caps);
      // gst_caps_unref (caps);
      break;
      // return TRUE;
    }
    default:
      nns_logw ("[sparse_dec][src_query] Got some query");
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}


/**
 * @brief Internal function to transform the input buffer.
 */
static GstFlowReturn
gst_tensor_sparse_dec_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstFlowReturn ret;
  GstTensorSparseDec *tensor_sparse_dec = GST_TENSOR_SPARSE_DEC (parent);
  GstTensorMetaInfo meta;
  GstMemory *mem, *out_mem;
  GstMapInfo map;
  GstBuffer *outbuf;


  nns_logw ("[gst_tensor_sparse_dec_chain] got buf of size %" G_GSIZE_FORMAT" bytes!", gst_buffer_get_size (buf));

  outbuf = gst_buffer_new ();

  mem = gst_buffer_peek_memory (buf, 0);
  if (!gst_tensor_meta_info_parse_memory (&meta, mem)) {
    nns_loge ("failed to parse meta info from given buffer");
    return GST_FLOW_ERROR;
  }

  if (!gst_memory_map (mem, &map, GST_MAP_READ)) {
    nns_loge ("failed to map given buffer");
    return GST_FLOW_ERROR;
  }

  out_mem = gst_tensor_sparse_to_dense (&meta, map.data);
  gst_memory_unmap (mem, &map);

  gst_buffer_append_memory (outbuf, out_mem);

  ret = gst_pad_push (tensor_sparse_dec->srcpad, outbuf);

  return ret;
}
