#pragma once
#if defined(__linux__)

#include <pulse/pulseaudio.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../audio_backend.hpp"
#include "dynamic_library.hpp"

namespace audio_device_manager {

class PulseAudioBackend : public AudioBackend {
 public:
  PulseAudioBackend() : AudioBackend("PulseAudio", BackendFeature::All) { this->try_init(); }

  ~PulseAudioBackend() override {
    if (!this->mainloop_) return;
    {
      MainloopLockGuard lock(this);
      if (this->context_) {
        this->context_set_state_callback_(this->context_, nullptr, nullptr);
        this->context_disconnect_(this->context_);
        this->context_unref_(this->context_);
      }
    }
    this->mainloop_stop_(this->mainloop_);
    this->mainloop_free_(this->mainloop_);
  }

  AudioBackendType type() const override { return AudioBackendType::PulseAudio; }
  bool available() const override { return this->available_; }

 private:
  CommandResult handle_set_volume(const std::string& device_id, float volume) override {
    DeviceCacheEntry entry;
    if (!this->lookup_device(device_id, entry)) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};

    pa_cvolume cvol;
    pa_volume_t pa_vol = static_cast<pa_volume_t>(volume * static_cast<float>(PA_VOLUME_NORM));
    this->cvolume_set_(&cvol, entry.channels > 0 ? entry.channels : 1, pa_vol);

    SuccessCallbackData cb_data{this};
    MainloopLockGuard lock(this);
    pa_operation* op = entry.type == DeviceType::Output
                           ? this->context_set_sink_volume_by_name_(this->context_, device_id.c_str(), &cvol, &PulseAudioBackend::success_callback, &cb_data)
                           : this->context_set_source_volume_by_name_(this->context_, device_id.c_str(), &cvol, &PulseAudioBackend::success_callback, &cb_data);
    return this->wait_operation(op, &cb_data.backend_success);
  }

  CommandResult handle_set_mute(const std::string& device_id, bool muted) override {
    DeviceCacheEntry entry;
    if (!this->lookup_device(device_id, entry)) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};

    SuccessCallbackData cb_data{this};
    MainloopLockGuard lock(this);
    pa_operation* op =
        entry.type == DeviceType::Output
            ? this->context_set_sink_mute_by_name_(this->context_, device_id.c_str(), muted ? 1 : 0, &PulseAudioBackend::success_callback, &cb_data)
            : this->context_set_source_mute_by_name_(this->context_, device_id.c_str(), muted ? 1 : 0, &PulseAudioBackend::success_callback, &cb_data);
    return this->wait_operation(op, &cb_data.backend_success);
  }

  CommandResult handle_set_default(const std::string& device_id) override {
    DeviceCacheEntry entry;
    if (!this->lookup_device(device_id, entry)) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};

    SuccessCallbackData cb_data{this};
    MainloopLockGuard lock(this);
    pa_operation* op = entry.type == DeviceType::Output
                           ? this->context_set_default_sink_(this->context_, device_id.c_str(), &PulseAudioBackend::success_callback, &cb_data)
                           : this->context_set_default_source_(this->context_, device_id.c_str(), &PulseAudioBackend::success_callback, &cb_data);
    return this->wait_operation(op, &cb_data.backend_success);
  }

  CommandResult handle_refresh() override {
    if (!this->available_) return {};
    MainloopLockGuard lock(this);
    this->refresh_devices();
    return {};
  }

  struct DeviceCacheEntry {
    DeviceType type;
    uint8_t channels;
  };

  void try_init() {
    if (!this->lib_.loaded()) {
      this->available_ = false;
      return;
    }
    if (!this->resolve_symbols()) {
      this->available_ = false;
      return;
    }

    this->mainloop_ = this->mainloop_new_();
    if (!this->mainloop_) {
      this->available_ = false;
      return;
    }

    pa_mainloop_api* api = this->mainloop_get_api_(this->mainloop_);
    this->context_       = this->context_new_(api, "audio_device_manager");
    if (!this->context_) {
      this->mainloop_free_(this->mainloop_);
      this->mainloop_  = nullptr;
      this->available_ = false;
      return;
    }

    this->context_set_state_callback_(this->context_, &PulseAudioBackend::context_state_callback, this);
    this->mainloop_start_(this->mainloop_);

    bool ready = false;
    {
      MainloopLockGuard lock(this);
      this->context_connect_(this->context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
      while (true) {
        pa_context_state_t state = this->context_get_state_(this->context_);
        if (state == PA_CONTEXT_READY || state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) break;
        this->mainloop_wait_(this->mainloop_);
      }
      ready = this->context_get_state_(this->context_) == PA_CONTEXT_READY;
    }

    if (!ready) {
      this->available_ = false;
      return;
    }

    this->context_set_subscribe_callback_(this->context_, &PulseAudioBackend::subscribe_callback, this);
    {
      MainloopLockGuard lock(this);
      this->context_subscribe_(this->context_,
                               static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER),
                               nullptr, nullptr);
    }

    this->available_ = true;
  }

  bool resolve_symbols() {
    this->mainloop_new_                      = this->lib_.resolve<decltype(&::pa_threaded_mainloop_new)>("pa_threaded_mainloop_new");
    this->mainloop_free_                     = this->lib_.resolve<decltype(&::pa_threaded_mainloop_free)>("pa_threaded_mainloop_free");
    this->mainloop_start_                    = this->lib_.resolve<decltype(&::pa_threaded_mainloop_start)>("pa_threaded_mainloop_start");
    this->mainloop_stop_                     = this->lib_.resolve<decltype(&::pa_threaded_mainloop_stop)>("pa_threaded_mainloop_stop");
    this->mainloop_lock_                     = this->lib_.resolve<decltype(&::pa_threaded_mainloop_lock)>("pa_threaded_mainloop_lock");
    this->mainloop_unlock_                   = this->lib_.resolve<decltype(&::pa_threaded_mainloop_unlock)>("pa_threaded_mainloop_unlock");
    this->mainloop_wait_                     = this->lib_.resolve<decltype(&::pa_threaded_mainloop_wait)>("pa_threaded_mainloop_wait");
    this->mainloop_signal_                   = this->lib_.resolve<decltype(&::pa_threaded_mainloop_signal)>("pa_threaded_mainloop_signal");
    this->mainloop_get_api_                  = this->lib_.resolve<decltype(&::pa_threaded_mainloop_get_api)>("pa_threaded_mainloop_get_api");
    this->context_new_                       = this->lib_.resolve<decltype(&::pa_context_new)>("pa_context_new");
    this->context_unref_                     = this->lib_.resolve<decltype(&::pa_context_unref)>("pa_context_unref");
    this->context_set_state_callback_        = this->lib_.resolve<decltype(&::pa_context_set_state_callback)>("pa_context_set_state_callback");
    this->context_connect_                   = this->lib_.resolve<decltype(&::pa_context_connect)>("pa_context_connect");
    this->context_disconnect_                = this->lib_.resolve<decltype(&::pa_context_disconnect)>("pa_context_disconnect");
    this->context_get_state_                 = this->lib_.resolve<decltype(&::pa_context_get_state)>("pa_context_get_state");
    this->context_get_server_info_           = this->lib_.resolve<decltype(&::pa_context_get_server_info)>("pa_context_get_server_info");
    this->context_get_sink_info_list_        = this->lib_.resolve<decltype(&::pa_context_get_sink_info_list)>("pa_context_get_sink_info_list");
    this->context_get_source_info_list_      = this->lib_.resolve<decltype(&::pa_context_get_source_info_list)>("pa_context_get_source_info_list");
    this->context_set_subscribe_callback_    = this->lib_.resolve<decltype(&::pa_context_set_subscribe_callback)>("pa_context_set_subscribe_callback");
    this->context_subscribe_                 = this->lib_.resolve<decltype(&::pa_context_subscribe)>("pa_context_subscribe");
    this->context_set_sink_volume_by_name_   = this->lib_.resolve<decltype(&::pa_context_set_sink_volume_by_name)>("pa_context_set_sink_volume_by_name");
    this->context_set_sink_mute_by_name_     = this->lib_.resolve<decltype(&::pa_context_set_sink_mute_by_name)>("pa_context_set_sink_mute_by_name");
    this->context_set_default_sink_          = this->lib_.resolve<decltype(&::pa_context_set_default_sink)>("pa_context_set_default_sink");
    this->context_set_source_volume_by_name_ = this->lib_.resolve<decltype(&::pa_context_set_source_volume_by_name)>("pa_context_set_source_volume_by_name");
    this->context_set_source_mute_by_name_   = this->lib_.resolve<decltype(&::pa_context_set_source_mute_by_name)>("pa_context_set_source_mute_by_name");
    this->context_set_default_source_        = this->lib_.resolve<decltype(&::pa_context_set_default_source)>("pa_context_set_default_source");
    this->context_errno_                     = this->lib_.resolve<decltype(&::pa_context_errno)>("pa_context_errno");
    this->strerror_                          = this->lib_.resolve<decltype(&::pa_strerror)>("pa_strerror");
    this->operation_unref_                   = this->lib_.resolve<decltype(&::pa_operation_unref)>("pa_operation_unref");
    this->operation_get_state_               = this->lib_.resolve<decltype(&::pa_operation_get_state)>("pa_operation_get_state");
    this->cvolume_set_                       = this->lib_.resolve<decltype(&::pa_cvolume_set)>("pa_cvolume_set");
    this->cvolume_avg_                       = this->lib_.resolve<decltype(&::pa_cvolume_avg)>("pa_cvolume_avg");

    return this->mainloop_new_ && this->mainloop_free_ && this->mainloop_start_ && this->mainloop_stop_ && this->mainloop_lock_ && this->mainloop_unlock_ &&
           this->mainloop_wait_ && this->mainloop_signal_ && this->mainloop_get_api_ && this->context_new_ && this->context_unref_ &&
           this->context_set_state_callback_ && this->context_connect_ && this->context_disconnect_ && this->context_get_state_ &&
           this->context_get_server_info_ && this->context_get_sink_info_list_ && this->context_get_source_info_list_ &&
           this->context_set_subscribe_callback_ && this->context_subscribe_ && this->context_set_sink_volume_by_name_ &&
           this->context_set_sink_mute_by_name_ && this->context_set_default_sink_ && this->context_set_source_volume_by_name_ &&
           this->context_set_source_mute_by_name_ && this->context_set_default_source_ && this->context_errno_ && this->strerror_ && this->operation_unref_ &&
           this->operation_get_state_ && this->cvolume_set_ && this->cvolume_avg_;
  }

  struct MainloopLockGuard {
    PulseAudioBackend* self;
    explicit MainloopLockGuard(PulseAudioBackend* s) : self(s) { self->mainloop_lock_(self->mainloop_); }
    ~MainloopLockGuard() { self->mainloop_unlock_(self->mainloop_); }
  };

  static void context_state_callback(pa_context*, void* userdata) {
    auto* self = static_cast<PulseAudioBackend*>(userdata);
    self->mainloop_signal_(self->mainloop_, 0);
  }

  static void subscribe_callback(pa_context*, pa_subscription_event_type_t, uint32_t, void* userdata) {
    auto* self = static_cast<PulseAudioBackend*>(userdata);
    self->refresh_devices();  // already running on the mainloop thread here
  }

  struct RefreshState {
    PulseAudioBackend* self;
    std::string default_sink_name;
    std::string default_source_name;
    std::vector<DeviceSnapshot> snapshots;
    int pending = 2;  // sink list + source list

    void complete_one() {
      if (--this->pending > 0) return;
      this->self->finish_refresh(std::move(this->snapshots));
    }
  };

  static void server_info_callback(pa_context*, const pa_server_info* info, void* userdata) {
    auto* holder = static_cast<std::shared_ptr<RefreshState>*>(userdata);
    auto state   = *holder;
    delete holder;
    if (info) {
      if (info->default_sink_name) state->default_sink_name = info->default_sink_name;
      if (info->default_source_name) state->default_source_name = info->default_source_name;
    }
    state->self->list_sinks(state);
    state->self->list_sources(state);
  }

  void refresh_devices() {
    auto state   = std::make_shared<RefreshState>();
    state->self  = this;
    auto* holder = new std::shared_ptr<RefreshState>(state);  // kept alive until server_info_callback consumes it

    auto* op = this->context_get_server_info_(this->context_, &PulseAudioBackend::server_info_callback, holder);
    if (op) this->operation_unref_(op);
  }

  void list_sinks(std::shared_ptr<RefreshState> state) {
    auto* holder = new std::shared_ptr<RefreshState>(state);
    auto* op     = this->context_get_sink_info_list_(this->context_, &PulseAudioBackend::sink_info_callback, holder);
    if (op) this->operation_unref_(op);
  }

  void list_sources(std::shared_ptr<RefreshState> state) {
    auto* holder = new std::shared_ptr<RefreshState>(state);
    auto* op     = this->context_get_source_info_list_(this->context_, &PulseAudioBackend::source_info_callback, holder);
    if (op) this->operation_unref_(op);
  }

  static void sink_info_callback(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
    auto* holder = static_cast<std::shared_ptr<RefreshState>*>(userdata);
    auto state   = *holder;
    if (eol) {
      delete holder;
      state->complete_one();
      return;
    }
    if (!info) return;

    DeviceSnapshot snap;
    snap.backend_device_id = info->name;
    snap.name              = info->description ? info->description : info->name;
    snap.type              = DeviceType::Output;
    snap.muted             = info->mute != 0;
    snap.is_default        = state->default_sink_name == info->name;
    snap.volume            = static_cast<float>(state->self->cvolume_avg_(&info->volume)) / static_cast<float>(PA_VOLUME_NORM);
    snap.min_level         = 0.f;
    snap.max_level         = static_cast<float>(PA_VOLUME_MAX) / static_cast<float>(PA_VOLUME_NORM);
    state->snapshots.push_back(std::move(snap));

    std::lock_guard lock(state->self->cache_mutex_);
    state->self->device_cache_[info->name] = DeviceCacheEntry{DeviceType::Output, info->channel_map.channels};
  }

  static void source_info_callback(pa_context*, const pa_source_info* info, int eol, void* userdata) {
    auto* holder = static_cast<std::shared_ptr<RefreshState>*>(userdata);
    auto state   = *holder;
    if (eol) {
      delete holder;
      state->complete_one();
      return;
    }
    if (!info) return;
    // skip monitor sources of sinks, not real input devices
    // if (info->monitor_of_sink != PA_INVALID_INDEX) return;

    DeviceSnapshot snap;
    snap.backend_device_id = info->name;
    snap.name              = info->description ? info->description : info->name;
    snap.type              = DeviceType::Input;
    snap.muted             = info->mute != 0;
    snap.is_default        = state->default_source_name == info->name;
    snap.volume            = static_cast<float>(state->self->cvolume_avg_(&info->volume)) / static_cast<float>(PA_VOLUME_NORM);
    snap.min_level         = 0.f;
    snap.max_level         = static_cast<float>(PA_VOLUME_MAX) / static_cast<float>(PA_VOLUME_NORM);
    state->snapshots.push_back(std::move(snap));

    std::lock_guard lock(state->self->cache_mutex_);
    state->self->device_cache_[info->name] = DeviceCacheEntry{DeviceType::Input, info->channel_map.channels};
  }

  void finish_refresh(std::vector<DeviceSnapshot> snapshots) { this->push_update_event(std::move(snapshots)); }

  bool lookup_device(const std::string& device_id, DeviceCacheEntry& out) {
    std::lock_guard lock(this->cache_mutex_);
    auto it = this->device_cache_.find(device_id);
    if (it == this->device_cache_.end()) return false;
    out = it->second;
    return true;
  }

  static void success_callback(pa_context* context, int success, void* userdata) {
    auto* self            = static_cast<SuccessCallbackData*>(userdata);
    self->backend_success = success != 0;
    self->backend->mainloop_signal_(self->backend->mainloop_, 0);
  }

  struct SuccessCallbackData {
    PulseAudioBackend* backend;
    bool backend_success = false;
  };

  // Blocks (releasing the mainloop lock internally via mainloop_wait_) until
  // `op` reaches a terminal state. Must be called while holding MainloopLockGuard.
  // `success` must point at the flag written by success_callback for this op.
  CommandResult wait_operation(pa_operation* op, const bool* success) {
    if (!op) return {CommandStatus::BackendError, "failed to create pulseaudio operation"};
    while (this->operation_get_state_(op) == PA_OPERATION_RUNNING) this->mainloop_wait_(this->mainloop_);
    bool completed = this->operation_get_state_(op) == PA_OPERATION_DONE;
    this->operation_unref_(op);
    if (!completed) return {CommandStatus::BackendError, "pulseaudio operation was cancelled"};
    if (!*success) return {CommandStatus::BackendError, "pulseaudio command failed"};
    return {};
  }

  DynamicLibrary lib_{"libpulse.so.0"};
  bool available_ = false;

  pa_threaded_mainloop* mainloop_ = nullptr;
  pa_context* context_            = nullptr;

  std::mutex cache_mutex_;
  std::unordered_map<std::string, DeviceCacheEntry> device_cache_;

  decltype(&::pa_threaded_mainloop_new) mainloop_new_                                  = nullptr;
  decltype(&::pa_threaded_mainloop_free) mainloop_free_                                = nullptr;
  decltype(&::pa_threaded_mainloop_start) mainloop_start_                              = nullptr;
  decltype(&::pa_threaded_mainloop_stop) mainloop_stop_                                = nullptr;
  decltype(&::pa_threaded_mainloop_lock) mainloop_lock_                                = nullptr;
  decltype(&::pa_threaded_mainloop_unlock) mainloop_unlock_                            = nullptr;
  decltype(&::pa_threaded_mainloop_wait) mainloop_wait_                                = nullptr;
  decltype(&::pa_threaded_mainloop_signal) mainloop_signal_                            = nullptr;
  decltype(&::pa_threaded_mainloop_get_api) mainloop_get_api_                          = nullptr;
  decltype(&::pa_context_new) context_new_                                             = nullptr;
  decltype(&::pa_context_unref) context_unref_                                         = nullptr;
  decltype(&::pa_context_set_state_callback) context_set_state_callback_               = nullptr;
  decltype(&::pa_context_connect) context_connect_                                     = nullptr;
  decltype(&::pa_context_disconnect) context_disconnect_                               = nullptr;
  decltype(&::pa_context_get_state) context_get_state_                                 = nullptr;
  decltype(&::pa_context_get_server_info) context_get_server_info_                     = nullptr;
  decltype(&::pa_context_get_sink_info_list) context_get_sink_info_list_               = nullptr;
  decltype(&::pa_context_get_source_info_list) context_get_source_info_list_           = nullptr;
  decltype(&::pa_context_set_subscribe_callback) context_set_subscribe_callback_       = nullptr;
  decltype(&::pa_context_subscribe) context_subscribe_                                 = nullptr;
  decltype(&::pa_context_set_sink_volume_by_name) context_set_sink_volume_by_name_     = nullptr;
  decltype(&::pa_context_set_sink_mute_by_name) context_set_sink_mute_by_name_         = nullptr;
  decltype(&::pa_context_set_default_sink) context_set_default_sink_                   = nullptr;
  decltype(&::pa_context_set_source_volume_by_name) context_set_source_volume_by_name_ = nullptr;
  decltype(&::pa_context_set_source_mute_by_name) context_set_source_mute_by_name_     = nullptr;
  decltype(&::pa_context_set_default_source) context_set_default_source_               = nullptr;
  decltype(&::pa_context_errno) context_errno_                                         = nullptr;
  decltype(&::pa_strerror) strerror_                                                   = nullptr;
  decltype(&::pa_operation_unref) operation_unref_                                     = nullptr;
  decltype(&::pa_operation_get_state) operation_get_state_                             = nullptr;
  decltype(&::pa_cvolume_set) cvolume_set_                                             = nullptr;
  decltype(&::pa_cvolume_avg) cvolume_avg_                                             = nullptr;
};

}  // namespace audio_device_manager
#endif
