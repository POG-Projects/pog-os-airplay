#include "pogwake_engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"
#include <frontend.h>
#include <frontend_util.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <new>
#include <cstring>

extern "C" const unsigned char pogwake_jarvis_model[], pogwake_nabu_model[],
    pogwake_alexa_model[];
extern "C" const unsigned char pogwake_jarvis_model_end[], pogwake_nabu_model_end[],
    pogwake_alexa_model_end[];

struct pogwake_engine {
  FrontendState frontend{};
  tflite::MicroMutableOpResolver<13> ops;
  tflite::MicroInterpreter *interpreter = nullptr;
  uint8_t *arena = nullptr, *variables = nullptr, *model_data = nullptr;
  unsigned stride_step = 0, warmup = 100, probability_index = 0;
  uint8_t probabilities[5]{};
  unsigned cutoff = 247;
  bool frontend_ready = false;
};

const char *pogwake_engine_phrase(unsigned model) {
  const char *names[] = {"Hey Jarvis", "Okay Nabu", "Alexa"};
  return model < 3 ? names[model] : nullptr;
}

pogwake_engine_t *pogwake_engine_create(unsigned model) {
  if (!pogwake_engine_phrase(model)) return nullptr;
  void *memory = heap_caps_calloc(1, sizeof(pogwake_engine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) return nullptr;
  auto *e = new (memory) pogwake_engine;
  e->cutoff = model == 2 ? 229 : 247;
  const unsigned char *models[] = {pogwake_jarvis_model, pogwake_nabu_model, pogwake_alexa_model};
  const unsigned char *ends[] = {pogwake_jarvis_model_end, pogwake_nabu_model_end, pogwake_alexa_model_end};
  const tflite::Model *graph = nullptr;
  {
    uint32_t size = 0;
    memcpy(&size, models[model], sizeof(size));
    if (size < 8 || size > 65536) goto fail;
    e->model_data = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *inflate = static_cast<tinfl_decompressor *>(heap_caps_calloc(1, sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!inflate) goto fail;
    if (!e->model_data) { heap_caps_free(inflate); goto fail; }
    size_t input = ends[model] - models[model] - 4, output = size;
    tinfl_init(inflate);
    tinfl_status result = tinfl_decompress(inflate, models[model] + 4, &input,
        e->model_data, e->model_data, &output,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(inflate);
    if (result != TINFL_STATUS_DONE || output != size ||
        input != static_cast<size_t>(ends[model] - models[model] - 4) ||
        memcmp(e->model_data + 4, "TFL3", 4)) goto fail;
  }
  graph = tflite::GetModel(e->model_data);
  if (graph->version() != TFLITE_SCHEMA_VERSION) goto fail;
  e->arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, 65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  e->variables = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!e->arena || !e->variables) goto fail;
  if (e->ops.AddCallOnce() != kTfLiteOk || e->ops.AddVarHandle() != kTfLiteOk ||
      e->ops.AddReshape() != kTfLiteOk || e->ops.AddReadVariable() != kTfLiteOk ||
      e->ops.AddConcatenation() != kTfLiteOk || e->ops.AddStridedSlice() != kTfLiteOk ||
      e->ops.AddAssignVariable() != kTfLiteOk || e->ops.AddConv2D() != kTfLiteOk ||
      e->ops.AddDepthwiseConv2D() != kTfLiteOk || e->ops.AddFullyConnected() != kTfLiteOk ||
      e->ops.AddLogistic() != kTfLiteOk || e->ops.AddQuantize() != kTfLiteOk ||
      e->ops.AddSplitV() != kTfLiteOk) goto fail;
  {
    auto *allocator = tflite::MicroAllocator::Create(e->variables, 4096);
    if (!allocator) goto fail;
    auto *vars = tflite::MicroResourceVariables::Create(allocator, 20);
    if (!vars) goto fail;
    e->interpreter = new (std::nothrow) tflite::MicroInterpreter(graph, e->ops, e->arena, 65536, vars);
    if (!e->interpreter || e->interpreter->AllocateTensors() != kTfLiteOk) goto fail;
    auto *in = e->interpreter->input(0), *out = e->interpreter->output(0);
    if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[0] != 1 ||
        in->dims->data[1] < 1 || in->dims->data[1] > 16 || in->dims->data[2] != 40 ||
        out->type != kTfLiteUInt8 || out->bytes != 1) goto fail;
    FrontendConfig cfg{};
    /* microWakeWord's training frontend: 40 log-mel bands, 30 ms window,
     * 10 ms step. These values are part of the bundled models' input format. */
    cfg.window.size_ms = 30; cfg.window.step_size_ms = 10;
    cfg.filterbank.num_channels = 40;
    cfg.filterbank.lower_band_limit = 125; cfg.filterbank.upper_band_limit = 7500;
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = .025f;
    cfg.noise_reduction.odd_smoothing = .06f;
    cfg.noise_reduction.min_signal_remaining = .05f;
    cfg.pcan_gain_control.enable_pcan = 1;
    cfg.pcan_gain_control.strength = .95f; cfg.pcan_gain_control.offset = 80;
    cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = 1; cfg.log_scale.scale_shift = 6;
    e->frontend_ready = true;
    if (!FrontendPopulateState(&cfg, &e->frontend, 16000)) goto fail;
  }
  return e;
fail:
  ESP_LOGE("pogwake", "Wake model initialization failed");
  pogwake_engine_destroy(e);
  return nullptr;
}

int pogwake_engine_feed(pogwake_engine_t *e, const int16_t *pcm, size_t count) {
  if (!e || !pcm) return -1;
  while (count) {
    size_t used = 0;
    auto features = FrontendProcessSamples(&e->frontend, pcm, count, &used);
    if (used > count || (!used && !features.size)) return -1;
    pcm += used; count -= used;
    if (!features.size) continue;
    if (features.size != 40) return -1;
    auto *input = e->interpreter->input(0);
    for (size_t i = 0; i < 40; i++) {
      int value = (features.values[i] * 256 + 333) / 666 - 128;
      input->data.int8[e->stride_step * 40 + i] = value < -128 ? -128 : value > 127 ? 127 : value;
    }
    if (++e->stride_step < static_cast<unsigned>(input->dims->data[1])) continue;
    e->stride_step = 0;
    if (e->interpreter->Invoke() != kTfLiteOk) return -1;
    unsigned probability = e->interpreter->output(0)->data.uint8[0];
    e->probabilities[e->probability_index++ % 5] = probability;
    if (e->warmup) {
      if (probability < e->cutoff) --e->warmup;
      continue;
    }
    unsigned sum = 0;
    for (unsigned p : e->probabilities) sum += p;
    if (sum > e->cutoff * 5) return 1;
  }
  return 0;
}

void pogwake_engine_destroy(pogwake_engine_t *e) {
  if (!e) return;
  delete e->interpreter;
  if (e->frontend_ready) FrontendFreeStateContents(&e->frontend);
  heap_caps_free(e->arena); heap_caps_free(e->variables);
  heap_caps_free(e->model_data);
  e->~pogwake_engine();
  heap_caps_free(e);
}
