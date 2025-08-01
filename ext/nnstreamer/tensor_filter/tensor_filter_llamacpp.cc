/**
 * GStreamer Tensor_Filter, llamacpp Module
 * Copyright (C) 2024 Yelin Jeong <yelini.jeong@samsung.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * @file    tensor_filter_llamacpp.cc
 * @date    5 Dec 2024
 * @brief   llamacpp module for tensor_filter gstreamer plugin
 * @see     http://github.com/nnstreamer/nnstreamer
 *          https://github.com/ggerganov/llama.cpp
 * @author  Yelin Jeong <yelini.jeong@samsung.com>
 * @bug     No known bugs except for NYI items
 * @details Pipeline example
 *          gst-launch-1.0 filesrc location=input.txt ! application/octet-stream !
 *          tensor_converter ! other/tensors,format=flexible !
 *          tensor_filter framework=llamacpp model=llama-2-7b-chat.Q2_K.gguf invoke-dynamic=TRUE
 *          custom=num_predict:32 ! other/tensors,format=flexible ! tensor_decoder mode=octet_stream !
 *          application/octet-stream ! filesink location=output.txt
 */

#include <assert.h>
#include <atomic>
#include <condition_variable>
#include <errno.h>
#include <functional>
#include <memory>
#include <nnstreamer_cppplugin_api_filter.hh>
#include <nnstreamer_log.h>
#include <nnstreamer_plugin_api_util.h>
#include <nnstreamer_util.h>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "ggml.h"
#include "llama.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
void init_filter_llamacpp (void) __attribute__ ((constructor));
void fini_filter_llamacpp (void) __attribute__ ((destructor));
#ifdef __cplusplus
}
#endif /* __cplusplus */

namespace nnstreamer
{
namespace tensorfilter_llamacpp
{
/**
 * @brief Class for TensorFilterLlamaCpp subplugin
 */
class TensorFilterLlamaCpp : public tensor_filter_subplugin
{
  public:
  TensorFilterLlamaCpp ();
  ~TensorFilterLlamaCpp ();

  /* mandatory methods */
  tensor_filter_subplugin &getEmptyInstance ();
  void configure_instance (const GstTensorFilterProperties *prop);
  void invoke (const GstTensorMemory *input, GstTensorMemory *output);
  void invoke_dynamic (GstTensorFilterProperties *prop,
      const GstTensorMemory *input, GstTensorMemory *output);
  void getFrameworkInfo (GstTensorFilterFrameworkInfo &info);
  int getModelInfo (model_info_ops ops, GstTensorsInfo &in_info, GstTensorsInfo &out_info);
  int eventHandler (event_ops ops, GstTensorFilterFrameworkEventData &data);

  /* static methods */
  static void init_filter_llama ();
  static void fini_filter_llama ();

  private:
  static TensorFilterLlamaCpp *registered;
  static const char *name;

  llama_model *model;
  llama_sampler *sampler;
  llama_model_params model_params;
  llama_sampler_chain_params sampler_params;
  llama_context_params ctx_params;

  int n_gpu_layers = 99;
  int n_predict = 32;

  std::thread output_thread; /* thread for async output */
  std::queue<std::pair<GstTensorFilterProperties *, std::string>> input_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::atomic<bool> stop_thread{ false }; /* flag to stop the thread */
  void parseCustomProperties (const GstTensorFilterProperties *prop);
  void generateTokens (GstTensorFilterProperties *prop, std::string &&prompt,
      GstTensorMemory *output);
  bool createOutputTensor (const std::string &buf, GstTensorMemory *output,
      GstTensorFilterProperties *prop);
  GstTensorMemory *prepareOutputAsync (
      const std::string &tokenStr, GstTensorFilterProperties *prop);
  void outputThreadLoop ();
};

TensorFilterLlamaCpp *TensorFilterLlamaCpp::registered = nullptr;
const char *TensorFilterLlamaCpp::name = "llamacpp";

/**
 * @brief Construct a new llamacpp subplugin instance
 */
TensorFilterLlamaCpp::TensorFilterLlamaCpp ()
    : tensor_filter_subplugin (), model (nullptr), sampler (nullptr)
{
}

/**
 * @brief Destructor of TensorFilterllamacpp
 */
TensorFilterLlamaCpp::~TensorFilterLlamaCpp ()
{
  stop_thread = true;
  queue_cv.notify_one ();
  if (output_thread.joinable ()) {
    output_thread.join ();
  }

  {
    std::lock_guard<std::mutex> lock (queue_mutex);
    while (!input_queue.empty ())
      input_queue.pop ();
  }

  if (sampler) {
    llama_sampler_free (sampler);
    sampler = nullptr;
  }

  if (model) {
    llama_model_free (model);
    model = nullptr;
  }
}

/**
 * @brief Method to get an empty object
 */
tensor_filter_subplugin &
TensorFilterLlamaCpp::getEmptyInstance ()
{
  return *(new TensorFilterLlamaCpp ());
}

/**
 * @brief Parse custom prop and set instance options accordingly.
 */
void
TensorFilterLlamaCpp::parseCustomProperties (const GstTensorFilterProperties *prop)
{
  using uniq_g_strv = std::unique_ptr<gchar *, std::function<void (gchar **)>>;
  guint len, i;

  if (!prop->custom_properties)
    return;

  uniq_g_strv options (g_strsplit (prop->custom_properties, ",", -1), g_strfreev);
  len = g_strv_length (options.get ());

  for (i = 0; i < len; ++i) {
    uniq_g_strv option (g_strsplit (options.get ()[i], ":", -1), g_strfreev);

    if (g_strv_length (option.get ()) > 1) {
      g_strstrip (option.get ()[0]);
      g_strstrip (option.get ()[1]);

      if (g_ascii_strcasecmp (option.get ()[0], "num_gpu_layers") == 0) {
        n_gpu_layers = (int) g_ascii_strtoull (option.get ()[1], NULL, 10);
      } else if (g_ascii_strcasecmp (option.get ()[0], "num_predict") == 0) {
        n_predict = (int) g_ascii_strtoull (option.get ()[1], NULL, 10);
      } else {
        throw std::invalid_argument (
            "Unknown custom property " + std::string (option.get ()[0]));
      }
    }
  }

  return;
}

/**
 * @brief Configure llamacpp instance
 */
void
TensorFilterLlamaCpp::configure_instance (const GstTensorFilterProperties *prop)
{
  if (!prop->invoke_dynamic) {
    throw std::invalid_argument (
        "llamacpp only supports invoke-dynamic mode. Set `invoke-dynamic=true`");
  }

  try {
    parseCustomProperties (prop);
  } catch (const std::invalid_argument &e) {
    throw std::invalid_argument ("Failed to parse \"custom\" prop:"
                                 + std::string (e.what ()) + "\n\tReference: " + __FILE__);
  }

  /* load dynamic backends */
  ggml_backend_load_all ();

  /* initialize model */
  model_params = llama_model_default_params ();
  model_params.n_gpu_layers = n_gpu_layers;

  model = llama_model_load_from_file ((char *) prop->model_files[0], model_params);
  if (model == nullptr) {
    throw std::invalid_argument ("Failed to load model");
  }

  /* context will be initialized in invoke callback */
  ctx_params = llama_context_default_params ();
  /* enable performance counters */
  ctx_params.no_perf = false;

  /* initialize sampler */
  sampler_params = llama_sampler_chain_default_params ();
  sampler_params.no_perf = false;

  sampler = llama_sampler_chain_init (sampler_params);
  llama_sampler_chain_add (sampler, llama_sampler_init_greedy ());

  stop_thread = false;
  output_thread = std::thread (&TensorFilterLlamaCpp::outputThreadLoop, this);
}

/**
 * @brief Invoke llamacpp using input tensors
 */
void
TensorFilterLlamaCpp::invoke (const GstTensorMemory *input, GstTensorMemory *output)
{
  UNUSED (input);
  UNUSED (output);

  throw std::runtime_error (
      "llamacpp only supports invoke-dynamic mode. Set `invoke-dynamic=true`");
}

/**
 * @brief Create output tensor
 *
 * The memory allocated for the output tensor data using strndup
 * must be freed by the caller of this function to avoid memory leaks.
 */
bool
TensorFilterLlamaCpp::createOutputTensor (const std::string &buf,
    GstTensorMemory *output, GstTensorFilterProperties *prop)
{
  if (buf.empty () || output == nullptr || prop == nullptr) {
    ml_loge ("Invalid arguments passed to createOutputTensor");
    return false;
  }

  output[0].size = buf.length (); /* Stores string length, excluding NULL appended to the end of the string */
  output[0].data = strndup (buf.c_str (), buf.length ());

  if (output[0].data == nullptr) {
    ml_loge ("createOutputTensor: strndup failed");
    return false;
  }

  gst_tensors_info_init (&prop->output_meta);
  prop->output_meta.num_tensors = 1;
  prop->output_meta.format = _NNS_TENSOR_FORMAT_FLEXIBLE;
  prop->output_meta.info[0].type = _NNS_UINT8;
  prop->output_meta.info[0].dimension[0] = output[0].size;

  return true;
}

/**
 * @brief Prepare output tensor for invoke_async
 */
GstTensorMemory *
TensorFilterLlamaCpp::prepareOutputAsync (
    const std::string &tokenStr, GstTensorFilterProperties *prop)
{
  GstTensorMemory *output = g_try_new0 (GstTensorMemory, 1);

  if (!output) {
    ml_loge ("Memory allocation failed");
    return nullptr;
  }

  if (!createOutputTensor (tokenStr, output, prop)) {
    g_free (output);
    ml_loge ("Failed to create output tensor");
    return nullptr;
  }
  return output;
}

/**
 * @brief Generates output tokens from the given input prompt using llamacpp.
 *
 * If invoke_async is true, each token is dispatched asynchronously using
 * nnstreamer_filter_dispatch_output_async(); otherwise, the result is stored in
 * output. GstTensorMemory is freed by nnstreamer; do not free it manually.
 */
void
TensorFilterLlamaCpp::generateTokens (GstTensorFilterProperties *prop,
    std::string &&prompt, GstTensorMemory *output)
{
  const llama_vocab *vocab = llama_model_get_vocab (model);
  int n_prompt, n_pos, n;
  llama_context *ctx = nullptr;
  llama_token new_token_id;
  llama_batch batch;
  std::string output_accumulated;

  n_prompt = -llama_tokenize (vocab, prompt.c_str (), prompt.size (), NULL, 0, true, true);
  std::vector<llama_token> prompt_tokens (n_prompt);
  n = llama_tokenize (vocab, prompt.c_str (), prompt.size (),
      prompt_tokens.data (), prompt_tokens.size (), true, true);
  if (n < 0) {
    throw std::invalid_argument ("Failed to tokenize the prompt");
  }

  ctx_params.n_ctx = n_prompt + n_predict - 1;
  /* n_batch is the maximum number of tokens that can be processed in a single call to llama_decode */
  ctx_params.n_batch = n_prompt;

  ctx = llama_init_from_model (model, ctx_params);
  if (ctx == nullptr) {
    throw std::invalid_argument ("Failed to create llama context");
  }

  for (auto id : prompt_tokens) {
    char buf[128] = { 0 };
    GstTensorMemory *output_async = nullptr;

    n = llama_token_to_piece (vocab, id, buf, sizeof (buf), 0, true);
    if (n < 0) {
      throw std::invalid_argument ("Failed to convert token to piece");
    }

    if (prop->invoke_async) {
      output_async = prepareOutputAsync (std::string (buf, n), prop);
      if (!output_async)
        throw std::runtime_error (
            "generateTokens: Failed to allocate GstTensorMemory for output_async");
      nnstreamer_filter_dispatch_output_async (prop, output_async);
    } else {
      output_accumulated.append (buf, n);
    }
  }

  batch = llama_batch_get_one (prompt_tokens.data (), prompt_tokens.size ());

  for (n_pos = 0; n_pos + batch.n_tokens < n_prompt + n_predict;) {
    /* evaluate the current batch with the transformer model */
    if (llama_decode (ctx, batch)) {
      throw std::invalid_argument ("Failed to eval");
    }

    n_pos += batch.n_tokens;

    /* sample the next token */
    {
      char buf[128] = { 0 };
      GstTensorMemory *output_async = nullptr;

      new_token_id = llama_sampler_sample (sampler, ctx, -1);

      /* is it an end of generation? */
      if (llama_vocab_is_eog (vocab, new_token_id)) {
        break;
      }

      n = llama_token_to_piece (vocab, new_token_id, buf, sizeof (buf), 0, true);
      if (n < 0) {
        throw std::runtime_error ("generateTokens: Failed to convert token to piece");
      }

      if (prop->invoke_async) {
        output_async = prepareOutputAsync (std::string (buf, n), prop);
        if (!output_async)
          throw std::runtime_error (
              "generateTokens: Failed to allocate GstTensorMemory for output_async");
        nnstreamer_filter_dispatch_output_async (prop, output_async);
      } else {
        output_accumulated.append (buf, n);
      }
      batch = llama_batch_get_one (&new_token_id, 1);
    }
  }

  if (!prop->invoke_async) {
    /* Final output for synchronous mode is created here. */
    if (!createOutputTensor (output_accumulated, output, prop)) {
      throw std::runtime_error ("generateTokens: Failed to create output tensor");
    }
  }

  if (ctx) {
    llama_free (ctx);
    ctx = nullptr;
  }
}

/**
 * @brief Asynchronous output thread loop. This thread generates tokens
 */
void
TensorFilterLlamaCpp::outputThreadLoop ()
{
  while (!stop_thread) {
    std::unique_lock<std::mutex> lock (queue_mutex);

    queue_cv.wait (lock, [this] () {
      return !input_queue.empty () || stop_thread;
    }); /*wait when condition is false*/

    if (stop_thread)
      break;

    auto [prop, prompt] = std::move (input_queue.front ());
    input_queue.pop ();
    lock.unlock ();
    try {
      this->generateTokens (prop, std::move (prompt), NULL);
    } catch (const std::exception &e) {
      /* thread exception */
      ml_loge ("Exception occurred during token generation: %s", e.what ());
    }
  }
}

/**
 * @brief Invoke llamacpp using input tensors
 */
void
TensorFilterLlamaCpp::invoke_dynamic (GstTensorFilterProperties *prop,
    const GstTensorMemory *input, GstTensorMemory *output)
{
  std::string prompt;

  if (!input || !input[0].data || input[0].size == 0) {
    throw std::invalid_argument ("Invalid input tensor data");
  }

  /* The part that fills in the prompt value was moved out of the thread, because input is freed when the invoke_dynamic function returns.*/
  prompt.assign ((char *) input[0].data, input[0].size);
  prompt.erase (prompt.find_last_not_of (" \t\n\r\f\v") + 1);

  if (prop->invoke_async) {

    if (prop->invoke_async_callback == nullptr) {
      throw std::invalid_argument ("invoke_async_callback is null");
    }
    {
      std::lock_guard<std::mutex> lock (queue_mutex);
      input_queue.push (std::make_pair (prop, std::move (prompt)));
    }
    queue_cv.notify_one (); /* notify waiting thread */
  } else {
    try {
      generateTokens (prop, std::move (prompt), output);
    } catch (const std::exception &e) {
      /* thread exception */
      ml_loge ("Exception occurred during token generation: %s", e.what ());
    }
  }
}

/**
 * @brief Get llama framework info.
 */
void
TensorFilterLlamaCpp::getFrameworkInfo (GstTensorFilterFrameworkInfo &info)
{
  info.name = name;
  info.allow_in_place = 0;
  info.allocate_in_invoke = 1;
  info.run_without_model = 0;
  info.verify_model_path = 1;
}

/**
 * @brief Get llama model info.
 */
int
TensorFilterLlamaCpp::getModelInfo (
    model_info_ops ops, GstTensorsInfo &in_info, GstTensorsInfo &out_info)
{
  switch (ops) {
    case GET_IN_OUT_INFO:
      in_info.num_tensors = 1;
      in_info.format = _NNS_TENSOR_FORMAT_FLEXIBLE;

      out_info.num_tensors = 1;
      out_info.format = _NNS_TENSOR_FORMAT_FLEXIBLE;
      break;
    default:
      return -ENOENT;
  }

  return 0;
}

/**
 * @brief Method to handle the event
 */
int
TensorFilterLlamaCpp::eventHandler (event_ops ops, GstTensorFilterFrameworkEventData &data)
{
  UNUSED (ops);
  UNUSED (data);
  return -ENOENT;
}

/** @brief Initialize this object for tensor_filter subplugin runtime register */
void
TensorFilterLlamaCpp::init_filter_llama ()
{
  registered = tensor_filter_subplugin::register_subplugin<TensorFilterLlamaCpp> ();
  nnstreamer_filter_set_custom_property_desc (name, "num_predict", "Number of tokens to predict",
      "num_gpu_layers", "Number of layers to offload to the GPU", NULL);
}

/** @brief Destruct the subplugin */
void
TensorFilterLlamaCpp::fini_filter_llama ()
{
  /* internal logic error */
  assert (registered != nullptr);
  tensor_filter_subplugin::unregister_subplugin (registered);
}

} /* namespace tensorfilter_llamacpp */
} /* namespace nnstreamer */

/**
 * @brief Subplugin initializer
 */
void
init_filter_llamacpp ()
{
  nnstreamer::tensorfilter_llamacpp::TensorFilterLlamaCpp::init_filter_llama ();
}

/**
 * @brief Subplugin finalizer
 */
void
fini_filter_llamacpp ()
{
  nnstreamer::tensorfilter_llamacpp::TensorFilterLlamaCpp::fini_filter_llama ();
}
