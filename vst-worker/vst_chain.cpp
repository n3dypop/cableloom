#include "vst_chain.hpp"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/uid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <xmmintrin.h>

namespace cableloom {
namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;
using VST3::Hosting::ClassInfo;
using VST3::Hosting::Module;
using VST3::Hosting::PluginFactory;

std::string json(const std::string &text) {
  std::string result = "\"";
  for (const unsigned char c : text) {
    switch (c) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (c < 0x20) {
        static constexpr char hex[] = "0123456789abcdef";
        result += "\\u00";
        result += hex[c >> 4];
        result += hex[c & 15];
      } else {
        result += static_cast<char>(c);
      }
    }
  }
  result += '"';
  return result;
}

std::pair<std::string, std::string> split_spec(const std::string &spec) {
  const auto marker = spec.rfind('#');
  if (marker == std::string::npos)
    return {spec, {}};
  const auto id = spec.substr(marker + 1);
  if (id.size() != 32)
    return {spec, {}};
  return {spec.substr(0, marker), id};
}

uint64_t now_ns() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class ComponentHandler final
    : public U::Implements<U::Directly<IComponentHandler>> {
public:
  tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
  tresult PLUGIN_API performEdit(ParamID, ParamValue) override {
    return kResultOk;
  }
  tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
  tresult PLUGIN_API restartComponent(int32 flags) override {
    flags_.fetch_or(static_cast<uint32_t>(flags), std::memory_order_relaxed);
    return kResultOk;
  }
  uint32_t take_flags() noexcept {
    return flags_.exchange(0, std::memory_order_relaxed);
  }

private:
  std::atomic<uint32_t> flags_{};
};

class Plugin {
public:
  Plugin(const std::string &spec, FUnknown *host, uint32_t sample_rate,
         uint32_t max_block)
      : sample_rate_(sample_rate), max_block_(max_block),
        output_left_(max_block), output_right_(max_block) {
    try {
      const auto [path, requested_id] = split_spec(spec);
      std::string error;
      module_ = Module::create(path, error);
      if (!module_)
        throw std::runtime_error("Cannot load VST3 module '" + path +
                                 "': " + error);
      path_ = path;
      const auto factory = module_->getFactory();
      factory.setHostContext(host);
      bool found = false;
      for (const auto &candidate : factory.classInfos()) {
        if (candidate.category() != kVstAudioEffectClass)
          continue;
        if (!requested_id.empty() && candidate.ID().toString() != requested_id)
          continue;
        info_ = candidate;
        found = true;
        break;
      }
      if (!found) {
        throw std::runtime_error(
            requested_id.empty() ? "Module contains no VST3 audio effect."
                                 : "Requested VST3 class ID was not found.");
      }

      component_ = factory.createInstance<IComponent>(info_.ID());
      if (!component_)
        throw std::runtime_error("Cannot create VST3 component: " +
                                 info_.name());
      component_base_ = U::cast<IPluginBase>(component_);
      if (!component_base_ || component_base_->initialize(host) != kResultOk)
        throw std::runtime_error("Cannot initialize VST3 component: " +
                                 info_.name());
      component_initialized_ = true;

      controller_ = U::cast<IEditController>(component_);
      if (!controller_) {
        TUID controller_id{};
        if (component_->getControllerClassId(controller_id) == kResultTrue) {
          controller_ =
              factory.createInstance<IEditController>(VST3::UID(controller_id));
          if (controller_) {
            controller_base_ = U::cast<IPluginBase>(controller_);
            if (!controller_base_ ||
                controller_base_->initialize(host) != kResultOk)
              throw std::runtime_error("Cannot initialize VST3 controller: " +
                                       info_.name());
            controller_initialized_ = true;
          }
        }
      }
      handler_ = owned(new ComponentHandler());
      if (controller_ &&
          controller_->setComponentHandler(handler_) != kResultOk)
        throw std::runtime_error("Cannot set VST3 component handler: " +
                                 info_.name());

      component_connection_ = U::cast<IConnectionPoint>(component_);
      controller_connection_ = U::cast<IConnectionPoint>(controller_);
      if (component_connection_ && controller_connection_) {
        if (component_connection_->connect(controller_connection_) != kResultOk)
          throw std::runtime_error(
              "Cannot connect VST3 component and controller: " + info_.name());
        component_connected_ = true;
        if (controller_connection_->connect(component_connection_) != kResultOk)
          throw std::runtime_error(
              "Cannot connect VST3 controller and component: " + info_.name());
        controller_connected_ = true;
      }

      processor_ = U::cast<IAudioProcessor>(component_);
      if (!processor_)
        throw std::runtime_error("VST3 class has no audio processor: " +
                                 info_.name());
      if (processor_->canProcessSampleSize(kSample32) != kResultTrue)
        throw std::runtime_error(
            "VST3 effect does not support float32 processing: " + info_.name());

      SpeakerArrangement input = SpeakerArr::kStereo;
      SpeakerArrangement output = SpeakerArr::kStereo;
      if (processor_->setBusArrangements(&input, 1, &output, 1) != kResultOk)
        throw std::runtime_error("VST3 effect rejected stereo input/output: " +
                                 info_.name());
      BusInfo input_info{};
      BusInfo output_info{};
      if (component_->getBusInfo(kAudio, kInput, 0, input_info) != kResultOk ||
          component_->getBusInfo(kAudio, kOutput, 0, output_info) !=
              kResultOk ||
          input_info.channelCount != 2 || output_info.channelCount != 2)
        throw std::runtime_error(
            "VST3 effect does not expose a stereo main bus: " + info_.name());
      if (component_->activateBus(kAudio, kInput, 0, true) != kResultOk ||
          component_->activateBus(kAudio, kOutput, 0, true) != kResultOk)
        throw std::runtime_error("Cannot activate VST3 stereo buses: " +
                                 info_.name());

      ProcessSetup setup{};
      setup.processMode = kRealtime;
      setup.symbolicSampleSize = kSample32;
      setup.maxSamplesPerBlock = static_cast<int32>(max_block_);
      setup.sampleRate = static_cast<double>(sample_rate_);
      if (processor_->setupProcessing(setup) != kResultOk)
        throw std::runtime_error("VST3 setupProcessing failed: " +
                                 info_.name());
      if (!process_data_.prepare(*component_, 0, kSample32))
        throw std::runtime_error("Cannot prepare VST3 process buffers: " +
                                 info_.name());
      process_data_.processMode = kRealtime;
      process_data_.symbolicSampleSize = kSample32;
      process_data_.inputEvents = nullptr;
      process_data_.outputEvents = nullptr;
      process_data_.inputParameterChanges = nullptr;
      process_data_.outputParameterChanges = nullptr;
      context_.sampleRate = static_cast<double>(sample_rate_);
      process_data_.processContext = &context_;
      if (component_->setActive(true) != kResultOk)
        throw std::runtime_error("VST3 setActive failed: " + info_.name());
      active_ = true;
      if (processor_->setProcessing(true) != kResultOk)
        throw std::runtime_error("VST3 setProcessing failed: " + info_.name());
      processing_ = true;
      latency_samples_.store(processor_->getLatencySamples(),
                             std::memory_order_relaxed);
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~Plugin() { shutdown(); }

  bool process(const float *input_left, const float *input_right,
               uint32_t frames) noexcept {
    if (!processing_ || frames > max_block_)
      return false;
    process_data_.numSamples = static_cast<int32>(frames);
    process_data_.inputs[0].silenceFlags = 0;
    process_data_.outputs[0].silenceFlags = 0;
    if (!process_data_.setChannelBuffer(kInput, 0, 0,
                                        const_cast<float *>(input_left)) ||
        !process_data_.setChannelBuffer(kInput, 0, 1,
                                        const_cast<float *>(input_right)) ||
        !process_data_.setChannelBuffer(kOutput, 0, 0, output_left_.data()) ||
        !process_data_.setChannelBuffer(kOutput, 0, 1, output_right_.data()))
      return false;
    const auto begin = now_ns();
    const auto result = processor_->process(process_data_);
    const auto elapsed = now_ns() - begin;
    const uint64_t deadline =
        static_cast<uint64_t>(frames) * 1000000000ull / sample_rate_;
    metric_.record(elapsed, deadline);
    const auto flags = handler_->take_flags();
    observed_restart_flags_.fetch_or(flags, std::memory_order_relaxed);
    if (flags & (kLatencyChanged | kIoChanged | kReloadComponent))
      reconfigure_requested_.store(true, std::memory_order_relaxed);
    return result == kResultOk &&
           !reconfigure_requested_.load(std::memory_order_relaxed);
  }

  const float *left() const noexcept { return output_left_.data(); }
  const float *right() const noexcept { return output_right_.data(); }
  PluginSnapshot snapshot() const {
    PluginSnapshot value;
    value.path = path_;
    value.class_id = info_.ID().toString();
    value.name = info_.name();
    value.vendor = info_.vendor();
    value.version = info_.version();
    value.declared_latency_samples =
        latency_samples_.load(std::memory_order_relaxed);
    value.restart_flags =
        observed_restart_flags_.load(std::memory_order_relaxed);
    value.reconfigure_requested =
        reconfigure_requested_.load(std::memory_order_relaxed);
    value.process_time = metric_.snapshot();
    return value;
  }
  void reset_metric() noexcept { metric_.reset(); }

private:
  void shutdown() noexcept {
    if (processing_ && processor_)
      processor_->setProcessing(false);
    processing_ = false;
    if (active_ && component_)
      component_->setActive(false);
    active_ = false;
    process_data_.unprepare();
    if (controller_)
      controller_->setComponentHandler(nullptr);
    if (controller_connected_ && controller_connection_)
      controller_connection_->disconnect(component_connection_);
    controller_connected_ = false;
    if (component_connected_ && component_connection_)
      component_connection_->disconnect(controller_connection_);
    component_connected_ = false;
    controller_connection_.reset();
    component_connection_.reset();
    processor_.reset();
    handler_.reset();
    controller_.reset();
    if (controller_initialized_ && controller_base_)
      controller_base_->terminate();
    controller_initialized_ = false;
    controller_base_.reset();
    component_.reset();
    if (component_initialized_ && component_base_)
      component_base_->terminate();
    component_initialized_ = false;
    component_base_.reset();
  }
  uint32_t sample_rate_{};
  uint32_t max_block_{};
  std::string path_;
  ClassInfo info_;
  Module::Ptr module_;
  IPtr<IComponent> component_;
  IPtr<IPluginBase> component_base_;
  IPtr<IEditController> controller_;
  IPtr<IPluginBase> controller_base_;
  IPtr<IConnectionPoint> component_connection_;
  IPtr<IConnectionPoint> controller_connection_;
  IPtr<IAudioProcessor> processor_;
  IPtr<ComponentHandler> handler_;
  HostProcessData process_data_;
  ProcessContext context_{};
  std::vector<float> output_left_;
  std::vector<float> output_right_;
  RealtimeMetric metric_;
  std::atomic<uint32_t> latency_samples_{};
  std::atomic<uint32_t> observed_restart_flags_{};
  std::atomic<bool> reconfigure_requested_{};
  bool component_initialized_{};
  bool controller_initialized_{};
  bool component_connected_{};
  bool controller_connected_{};
  bool active_{};
  bool processing_{};
};

std::string metric_json(const MetricSnapshot &metric) {
  std::ostringstream out;
  out << "{\"calls\":" << metric.calls << ",\"total_ns\":" << metric.total_ns
      << ",\"average_ns\":"
      << (metric.calls ? metric.total_ns / metric.calls : 0)
      << ",\"p50_upper_ns\":" << metric.p50_ns
      << ",\"p95_upper_ns\":" << metric.p95_ns
      << ",\"p99_upper_ns\":" << metric.p99_ns
      << ",\"max_ns\":" << metric.max_ns
      << ",\"deadline_misses\":" << metric.deadline_misses << '}';
  return out.str();
}

} // namespace

class VstChain::Impl {
public:
  Impl(const std::vector<std::string> &specs, uint32_t sample_rate,
       uint32_t max_block)
      : sample_rate(sample_rate), max_block(max_block),
        host(owned(new HostApplication())) {
    if (!sample_rate || !max_block)
      throw std::invalid_argument("Invalid VST processing setup.");
    plugins.reserve(specs.size());
    for (const auto &spec : specs)
      plugins.push_back(
          std::make_unique<Plugin>(spec, host, sample_rate, max_block));
  }

  uint32_t sample_rate;
  uint32_t max_block;
  IPtr<HostApplication> host;
  std::vector<std::unique_ptr<Plugin>> plugins;
  std::atomic<uint64_t> processed_frames{};
  std::atomic<uint64_t> failed_blocks{};
  RealtimeMetric metric;
};

VstChain::VstChain(std::vector<std::string> specs, uint32_t sample_rate,
                   uint32_t max_block)
    : impl_(std::make_unique<Impl>(specs, sample_rate, max_block)) {}
VstChain::~VstChain() = default;
VstChain::VstChain(VstChain &&) noexcept = default;
VstChain &VstChain::operator=(VstChain &&) noexcept = default;

bool VstChain::process(const float *input_left, const float *input_right,
                       float *output_left, float *output_right,
                       uint32_t frames) noexcept {
  if (!frames || frames > impl_->max_block)
    return false;
  constexpr unsigned int denormal_mode =
      0x8040u; // flush-to-zero and denormals-are-zero
  const auto control = _mm_getcsr();
  if ((control & denormal_mode) != denormal_mode)
    _mm_setcsr(control | denormal_mode);
  const auto begin = now_ns();
  const float *left = input_left;
  const float *right = input_right;
  for (auto &plugin : impl_->plugins) {
    if (!plugin->process(left, right, frames)) {
      impl_->failed_blocks.fetch_add(1, std::memory_order_relaxed);
      std::fill_n(output_left, frames, 0.0f);
      std::fill_n(output_right, frames, 0.0f);
      return false;
    }
    left = plugin->left();
    right = plugin->right();
  }
  std::copy_n(left, frames, output_left);
  std::copy_n(right, frames, output_right);
  const auto elapsed = now_ns() - begin;
  impl_->metric.record(elapsed, static_cast<uint64_t>(frames) * 1000000000ull /
                                    impl_->sample_rate);
  impl_->processed_frames.fetch_add(frames, std::memory_order_relaxed);
  return true;
}

ChainSnapshot VstChain::snapshot() const {
  ChainSnapshot value;
  value.sample_rate = impl_->sample_rate;
  value.max_block_frames = impl_->max_block;
  value.processed_frames =
      impl_->processed_frames.load(std::memory_order_relaxed);
  value.failed_blocks = impl_->failed_blocks.load(std::memory_order_relaxed);
  value.process_time = impl_->metric.snapshot();
  for (const auto &plugin : impl_->plugins) {
    auto current = plugin->snapshot();
    value.declared_latency_samples += current.declared_latency_samples;
    value.plugins.push_back(std::move(current));
  }
  return value;
}

bool VstChain::empty() const noexcept { return impl_->plugins.empty(); }

void VstChain::reset_metrics() noexcept {
  impl_->processed_frames.store(0, std::memory_order_relaxed);
  impl_->failed_blocks.store(0, std::memory_order_relaxed);
  impl_->metric.reset();
  for (auto &plugin : impl_->plugins)
    plugin->reset_metric();
}

std::string scan_module_json(const std::string &path) {
  std::string error;
  const auto module = Module::create(path, error);
  if (!module)
    throw std::runtime_error("Cannot load VST3 module: " + error);
  const auto factory = module->getFactory();
  const auto vendor = factory.info().vendor();
  std::ostringstream out;
  out << "{\"schema\":1,\"path\":" << json(path)
      << ",\"factory_vendor\":" << json(vendor) << ",\"classes\":[";
  bool first = true;
  for (const auto &info : factory.classInfos()) {
    if (info.category() != kVstAudioEffectClass)
      continue;
    if (!first)
      out << ',';
    first = false;
    out << "{\"class_id\":" << json(info.ID().toString())
        << ",\"name\":" << json(info.name())
        << ",\"vendor\":" << json(info.vendor())
        << ",\"version\":" << json(info.version())
        << ",\"sdk_version\":" << json(info.sdkVersion())
        << ",\"subcategories\":" << json(info.subCategoriesString()) << '}';
  }
  out << "]}";
  return out.str();
}

std::string chain_snapshot_json(const ChainSnapshot &snapshot,
                                const char *mode) {
  std::ostringstream out;
  out << "{\"schema\":1,\"mode\":" << json(mode)
      << ",\"sample_rate\":" << snapshot.sample_rate
      << ",\"max_block_frames\":" << snapshot.max_block_frames
      << ",\"processed_frames\":" << snapshot.processed_frames
      << ",\"failed_blocks\":" << snapshot.failed_blocks
      << ",\"declared_latency_samples\":" << snapshot.declared_latency_samples
      << ",\"declared_latency_ms\":"
      << (snapshot.sample_rate ? snapshot.declared_latency_samples * 1000.0 /
                                     snapshot.sample_rate
                               : 0.0)
      << ",\"process_time\":" << metric_json(snapshot.process_time)
      << ",\"plugins\":[";
  for (size_t i = 0; i < snapshot.plugins.size(); ++i) {
    if (i)
      out << ',';
    const auto &plugin = snapshot.plugins[i];
    out << "{\"index\":" << i << ",\"path\":" << json(plugin.path)
        << ",\"class_id\":" << json(plugin.class_id)
        << ",\"name\":" << json(plugin.name)
        << ",\"vendor\":" << json(plugin.vendor)
        << ",\"version\":" << json(plugin.version)
        << ",\"declared_latency_samples\":" << plugin.declared_latency_samples
        << ",\"restart_flags\":" << plugin.restart_flags
        << ",\"reconfigure_requested\":"
        << (plugin.reconfigure_requested ? "true" : "false")
        << ",\"process_time\":" << metric_json(plugin.process_time) << '}';
  }
  out << "]}";
  return out.str();
}

} // namespace cableloom
