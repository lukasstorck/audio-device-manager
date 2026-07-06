#pragma once
#if defined(__linux__)

#include <alsa/asoundlib.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../async.hpp"
#include "../audio_backend.hpp"
#include "dynamic_library.hpp"

namespace audio_device_manager {

constexpr BackendFeature ALSA_SUPPORTED_FEATURES = BackendFeature::ListDevices | BackendFeature::ReadDeviceVolume | BackendFeature::ReadDeviceMute |
                                                   BackendFeature::ReadDefaultDevice | BackendFeature::SetDeviceVolume | BackendFeature::SetDeviceMute |
                                                   BackendFeature::SetDefaultDevice;  // | BackendFeature::DeviceChangeNotifications // TODO: needs polling

// ALSA has no daemon and no push-notification mechanism for device/mixer
// changes usable here without a persistent poll thread, so this backend is
// purely pull-based: request_refresh() re-enumerates cards + mixer elements
// on the AsyncWorker thread and pushes a fresh snapshot. Commands look up
// their target in the cache built by the last refresh and reopen a mixer
// handle just for the duration of the call.
class AlsaBackend : public AudioBackend {
 public:
  AlsaBackend() : AudioBackend("ALSA", ALSA_SUPPORTED_FEATURES) { this->try_init(); }

  AudioBackendType type() const override { return AudioBackendType::Alsa; }
  bool available() const override { return this->available_; }

  void request_refresh() override {
    this->worker_.post([this] { this->refresh_devices(); });
  }

  CommandResultFuture set_volume_async(const std::string& device_id, float volume, std::function<void(CommandResult)> on_done = nullptr) override {
    auto promise = std::make_shared<std::promise<CommandResult>>();
    auto future  = promise->get_future();
    this->worker_.post([this, device_id, volume, on_done = std::move(on_done), promise] {
      auto result = this->run_set_volume(device_id, volume);
      if (on_done) on_done(result);
      promise->set_value(result);
    });
    return future;
  }
  CommandResultFuture set_mute_async(const std::string& device_id, bool muted, std::function<void(CommandResult)> on_done = nullptr) override {
    auto promise = std::make_shared<std::promise<CommandResult>>();
    auto future  = promise->get_future();
    this->worker_.post([this, device_id, muted, on_done = std::move(on_done), promise] {
      auto result = this->run_set_mute(device_id, muted);
      if (on_done) on_done(result);
      promise->set_value(result);
    });
    return future;
  }
  CommandResultFuture set_default_async(const std::string& device_id, std::function<void(CommandResult)> on_done = nullptr) override {
    auto promise = std::make_shared<std::promise<CommandResult>>();
    auto future  = promise->get_future();
    this->worker_.post([this, device_id, on_done = std::move(on_done), promise] {
      // ALSA has no OS-level "default device" concept to switch, unlike
      // PulseAudio's default sink/source or WASAPI's default endpoint.
      CommandResult result{CommandStatus::Unsupported, "ALSA has no default-device API"};
      if (!this->lookup(device_id)) result = {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};
      if (on_done) on_done(result);
      promise->set_value(result);
    });
    return future;
  }

 private:
  struct DeviceCacheEntry {
    int card = -1;
    std::string elem_name;
    bool is_playback = true;
    long min_raw     = 0;
    long max_raw     = 0;
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

    // try to open the logical device "default" to test ALSA backend availability
    snd_pcm_t* probe = nullptr;
    int err          = this->pcm_open_(&probe, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
      this->available_ = false;
      return;
    }
    this->pcm_close_(probe);
    this->available_ = true;
  }

  bool resolve_symbols() {
    this->pcm_open_                  = this->lib_.resolve<decltype(&::snd_pcm_open)>("snd_pcm_open");
    this->pcm_close_                 = this->lib_.resolve<decltype(&::snd_pcm_close)>("snd_pcm_close");
    this->strerror_                  = this->lib_.resolve<decltype(&::snd_strerror)>("snd_strerror");
    this->card_next_                 = this->lib_.resolve<decltype(&::snd_card_next)>("snd_card_next");
    this->ctl_open_                  = this->lib_.resolve<decltype(&::snd_ctl_open)>("snd_ctl_open");
    this->ctl_close_                 = this->lib_.resolve<decltype(&::snd_ctl_close)>("snd_ctl_close");
    this->ctl_pcm_next_device_       = this->lib_.resolve<decltype(&::snd_ctl_pcm_next_device)>("snd_ctl_pcm_next_device");
    this->ctl_pcm_info_              = this->lib_.resolve<decltype(&::snd_ctl_pcm_info)>("snd_ctl_pcm_info");
    this->pcm_info_malloc_           = this->lib_.resolve<decltype(&::snd_pcm_info_malloc)>("snd_pcm_info_malloc");
    this->pcm_info_free_             = this->lib_.resolve<decltype(&::snd_pcm_info_free)>("snd_pcm_info_free");
    this->pcm_info_set_device_       = this->lib_.resolve<decltype(&::snd_pcm_info_set_device)>("snd_pcm_info_set_device");
    this->pcm_info_set_subdevice_    = this->lib_.resolve<decltype(&::snd_pcm_info_set_subdevice)>("snd_pcm_info_set_subdevice");
    this->pcm_info_set_stream_       = this->lib_.resolve<decltype(&::snd_pcm_info_set_stream)>("snd_pcm_info_set_stream");
    this->pcm_info_get_name_         = this->lib_.resolve<decltype(&::snd_pcm_info_get_name)>("snd_pcm_info_get_name");
    this->ctl_card_info_malloc_      = this->lib_.resolve<decltype(&::snd_ctl_card_info_malloc)>("snd_ctl_card_info_malloc");
    this->ctl_card_info_free_        = this->lib_.resolve<decltype(&::snd_ctl_card_info_free)>("snd_ctl_card_info_free");
    this->ctl_card_info_             = this->lib_.resolve<decltype(&::snd_ctl_card_info)>("snd_ctl_card_info");
    this->ctl_card_info_get_name_    = this->lib_.resolve<decltype(&::snd_ctl_card_info_get_name)>("snd_ctl_card_info_get_name");
    this->mixer_open_                = this->lib_.resolve<decltype(&::snd_mixer_open)>("snd_mixer_open");
    this->mixer_close_               = this->lib_.resolve<decltype(&::snd_mixer_close)>("snd_mixer_close");
    this->mixer_attach_              = this->lib_.resolve<decltype(&::snd_mixer_attach)>("snd_mixer_attach");
    this->mixer_selem_register_      = this->lib_.resolve<decltype(&::snd_mixer_selem_register)>("snd_mixer_selem_register");
    this->mixer_load_                = this->lib_.resolve<decltype(&::snd_mixer_load)>("snd_mixer_load");
    this->mixer_first_elem_          = this->lib_.resolve<decltype(&::snd_mixer_first_elem)>("snd_mixer_first_elem");
    this->mixer_elem_next_           = this->lib_.resolve<decltype(&::snd_mixer_elem_next)>("snd_mixer_elem_next");
    this->selem_get_name_            = this->lib_.resolve<decltype(&::snd_mixer_selem_get_name)>("snd_mixer_selem_get_name");
    this->selem_has_playback_volume_ = this->lib_.resolve<decltype(&::snd_mixer_selem_has_playback_volume)>("snd_mixer_selem_has_playback_volume");
    this->selem_has_capture_volume_  = this->lib_.resolve<decltype(&::snd_mixer_selem_has_capture_volume)>("snd_mixer_selem_has_capture_volume");
    this->selem_has_playback_switch_ = this->lib_.resolve<decltype(&::snd_mixer_selem_has_playback_switch)>("snd_mixer_selem_has_playback_switch");
    this->selem_has_capture_switch_  = this->lib_.resolve<decltype(&::snd_mixer_selem_has_capture_switch)>("snd_mixer_selem_has_capture_switch");
    this->selem_get_playback_volume_range_ =
        this->lib_.resolve<decltype(&::snd_mixer_selem_get_playback_volume_range)>("snd_mixer_selem_get_playback_volume_range");
    this->selem_get_capture_volume_range_ =
        this->lib_.resolve<decltype(&::snd_mixer_selem_get_capture_volume_range)>("snd_mixer_selem_get_capture_volume_range");
    this->selem_get_playback_volume_     = this->lib_.resolve<decltype(&::snd_mixer_selem_get_playback_volume)>("snd_mixer_selem_get_playback_volume");
    this->selem_get_capture_volume_      = this->lib_.resolve<decltype(&::snd_mixer_selem_get_capture_volume)>("snd_mixer_selem_get_capture_volume");
    this->selem_set_playback_volume_all_ = this->lib_.resolve<decltype(&::snd_mixer_selem_set_playback_volume_all)>("snd_mixer_selem_set_playback_volume_all");
    this->selem_set_capture_volume_all_  = this->lib_.resolve<decltype(&::snd_mixer_selem_set_capture_volume_all)>("snd_mixer_selem_set_capture_volume_all");
    this->selem_get_playback_switch_     = this->lib_.resolve<decltype(&::snd_mixer_selem_get_playback_switch)>("snd_mixer_selem_get_playback_switch");
    this->selem_get_capture_switch_      = this->lib_.resolve<decltype(&::snd_mixer_selem_get_capture_switch)>("snd_mixer_selem_get_capture_switch");
    this->selem_set_playback_switch_all_ = this->lib_.resolve<decltype(&::snd_mixer_selem_set_playback_switch_all)>("snd_mixer_selem_set_playback_switch_all");
    this->selem_set_capture_switch_all_  = this->lib_.resolve<decltype(&::snd_mixer_selem_set_capture_switch_all)>("snd_mixer_selem_set_capture_switch_all");

    return this->pcm_open_ && this->pcm_close_ && this->strerror_ && this->card_next_ && this->ctl_open_ && this->ctl_close_ && this->ctl_pcm_next_device_ &&
           this->ctl_pcm_info_ && this->pcm_info_malloc_ && this->pcm_info_free_ && this->pcm_info_set_device_ && this->pcm_info_set_subdevice_ &&
           this->pcm_info_set_stream_ && this->pcm_info_get_name_ && this->ctl_card_info_malloc_ && this->ctl_card_info_free_ && this->ctl_card_info_ &&
           this->ctl_card_info_get_name_ && this->mixer_open_ && this->mixer_close_ && this->mixer_attach_ && this->mixer_selem_register_ &&
           this->mixer_load_ && this->mixer_first_elem_ && this->mixer_elem_next_ && this->selem_get_name_ && this->selem_has_playback_volume_ &&
           this->selem_has_capture_volume_ && this->selem_has_playback_switch_ && this->selem_has_capture_switch_ && this->selem_get_playback_volume_range_ &&
           this->selem_get_capture_volume_range_ && this->selem_get_playback_volume_ && this->selem_get_capture_volume_ &&
           this->selem_set_playback_volume_all_ && this->selem_set_capture_volume_all_ && this->selem_get_playback_switch_ && this->selem_get_capture_switch_ &&
           this->selem_set_playback_switch_all_ && this->selem_set_capture_switch_all_;
  }

  // Run function `fn(elem)` on every element of the mixer attached to "hw:<card>". Returns false on failure to open the mixer.
  template <typename Fn>
  bool for_each_elem(int card, Fn&& fn) {
    snd_mixer_t* mixer = nullptr;
    if (this->mixer_open_(&mixer, 0) < 0) return false;

    std::string ctl_name = "hw:" + std::to_string(card);
    if (this->mixer_attach_(mixer, ctl_name.c_str()) < 0 || this->mixer_selem_register_(mixer, nullptr, nullptr) < 0 || this->mixer_load_(mixer) < 0) {
      this->mixer_close_(mixer);
      return false;
    }

    for (snd_mixer_elem_t* elem = this->mixer_first_elem_(mixer); elem != nullptr; elem = this->mixer_elem_next_(elem)) fn(elem);

    this->mixer_close_(mixer);
    return true;
  }

  void refresh_devices() {
    std::vector<DeviceSnapshot> snapshots;
    std::unordered_map<std::string, DeviceCacheEntry> new_cache;

    int card = -1;
    while (this->card_next_(&card) >= 0 && card >= 0) {
      std::string ctl_name = "hw:" + std::to_string(card);
      snd_ctl_t* ctl       = nullptr;
      if (this->ctl_open_(&ctl, ctl_name.c_str(), 0) < 0) continue;  // card vanished between enumeration and open

      std::string card_name = this->read_card_name(ctl, ctl_name);
      bool found_playback   = false;
      bool found_capture    = false;

      this->for_each_elem(card, [&](snd_mixer_elem_t* elem) {
        const char* elem_name = this->selem_get_name_(elem);
        if (!elem_name) return;

        if (this->selem_has_playback_volume_(elem)) {
          this->emit_snapshot(card, card_name, elem, elem_name, /*is_playback=*/true, snapshots, new_cache);
          found_playback = true;
        }
        if (this->selem_has_capture_volume_(elem)) {
          this->emit_snapshot(card, card_name, elem, elem_name, /*is_playback=*/false, snapshots, new_cache);
          found_capture = true;
        }
      });

      // fallback to pcm devices when no mixer element is found (i.e. most likely another process has exclusive access)
      if (!found_playback) this->emit_pcm_devices(ctl, card, card_name, SND_PCM_STREAM_PLAYBACK, snapshots, new_cache);
      if (!found_capture) this->emit_pcm_devices(ctl, card, card_name, SND_PCM_STREAM_CAPTURE, snapshots, new_cache);

      this->ctl_close_(ctl);
    }

    {
      std::lock_guard lock(this->cache_mutex_);
      this->device_cache_ = std::move(new_cache);
    }
    this->push_update_event(std::move(snapshots));
  }

  void emit_snapshot(int card, const std::string& card_name, snd_mixer_elem_t* elem, const char* elem_name, bool is_playback,
                     std::vector<DeviceSnapshot>& snapshots, std::unordered_map<std::string, DeviceCacheEntry>& new_cache) {
    long min_raw = 0, max_raw = 0;
    if (is_playback) {
      this->selem_get_playback_volume_range_(elem, &min_raw, &max_raw);
    } else {
      this->selem_get_capture_volume_range_(elem, &min_raw, &max_raw);
    }

    long raw = min_raw;
    if (is_playback) {
      this->selem_get_playback_volume_(elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
    } else {
      this->selem_get_capture_volume_(elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
    }

    bool muted = false;
    int sw     = 1;
    if (is_playback && this->selem_has_playback_switch_(elem)) {
      this->selem_get_playback_switch_(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
      muted = sw == 0;
    } else if (!is_playback && this->selem_has_capture_switch_(elem)) {
      this->selem_get_capture_switch_(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
      muted = sw == 0;
    }

    DeviceSnapshot snap;
    snap.backend_device_id = std::to_string(card) + ":" + elem_name + ":" + (is_playback ? "playback" : "capture");
    snap.name              = card_name + " - " + elem_name;
    snap.type              = is_playback ? DeviceType::Output : DeviceType::Input;
    snap.muted             = muted;
    snap.is_default        = false;  // NOTE: ALSA has no real default concept
    snap.volume            = max_raw > min_raw ? static_cast<float>(raw - min_raw) / static_cast<float>(max_raw - min_raw) : 0.f;
    snap.min_level         = 0.f;
    snap.max_level         = 1.f;
    snapshots.push_back(snap);

    new_cache[snap.backend_device_id] = DeviceCacheEntry{card, elem_name, is_playback, min_raw, max_raw};
  }

  // Fallback: read pcm devices
  void emit_pcm_devices(snd_ctl_t* ctl, int card, const std::string& card_name, snd_pcm_stream_t stream, std::vector<DeviceSnapshot>& snapshots,
                        std::unordered_map<std::string, DeviceCacheEntry>& new_cache) {
    snd_pcm_info_t* info = nullptr;
    if (this->pcm_info_malloc_(&info) < 0) return;

    bool is_playback = stream == SND_PCM_STREAM_PLAYBACK;
    int device       = -1;
    while (this->ctl_pcm_next_device_(ctl, &device) >= 0 && device >= 0) {
      this->pcm_info_set_device_(info, device);
      this->pcm_info_set_subdevice_(info, 0);
      this->pcm_info_set_stream_(info, stream);
      if (this->ctl_pcm_info_(ctl, info) < 0) continue;  // this device index doesn't support this direction

      const char* pcm_name = this->pcm_info_get_name_(info);

      DeviceSnapshot snap;
      snap.backend_device_id = std::to_string(card) + ":" + std::to_string(device) + ":" + (is_playback ? "playback" : "capture");
      snap.name              = pcm_name ? card_name + " - " + pcm_name : card_name;
      snap.type              = is_playback ? DeviceType::Output : DeviceType::Input;
      snap.muted             = false;
      snap.is_default        = false;  // NOTE: ALSA has no real default concept
      snap.volume            = 1.f;
      snap.min_level         = 0.f;
      snap.max_level         = 1.f;
      snapshots.push_back(snap);

      new_cache[snap.backend_device_id] = DeviceCacheEntry{card, /*elem_name=*/"", is_playback, 0, 0};
    }

    this->pcm_info_free_(info);
  }

  std::string read_card_name(snd_ctl_t* ctl, const std::string& fallback) {
    std::string name          = fallback;
    snd_ctl_card_info_t* info = nullptr;
    if (this->ctl_card_info_malloc_(&info) >= 0) {
      if (this->ctl_card_info_(ctl, info) >= 0) {
        const char* n = this->ctl_card_info_get_name_(info);
        if (n) name = n;
      }
      this->ctl_card_info_free_(info);
    }
    return name;
  }

  bool lookup(const std::string& device_id) {
    std::lock_guard lock(this->cache_mutex_);
    return this->device_cache_.count(device_id) > 0;
  }

  CommandResult run_set_volume(const std::string& device_id, float volume) {
    DeviceCacheEntry entry;
    {
      std::lock_guard lock(this->cache_mutex_);
      auto it = this->device_cache_.find(device_id);
      if (it == this->device_cache_.end()) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};
      entry = it->second;
    }

    if (entry.elem_name.empty()) return {CommandStatus::Unsupported, "device has no mixer volume control"};

    long raw = entry.min_raw + static_cast<long>(volume * static_cast<float>(entry.max_raw - entry.min_raw));
    CommandResult result{};
    bool applied = this->for_each_elem(entry.card, [&](snd_mixer_elem_t* elem) {
      const char* name = this->selem_get_name_(elem);
      bool has_dir     = entry.is_playback ? this->selem_has_playback_volume_(elem) : this->selem_has_capture_volume_(elem);
      if (!name || entry.elem_name != name || !has_dir) return;
      int rc = entry.is_playback ? this->selem_set_playback_volume_all_(elem, raw) : this->selem_set_capture_volume_all_(elem, raw);
      if (rc < 0) result = {CommandStatus::BackendError, this->strerror_(rc)};
    });
    if (!applied) return {CommandStatus::BackendError, "failed to open mixer for card " + std::to_string(entry.card)};
    return result;
  }

  CommandResult run_set_mute(const std::string& device_id, bool muted) {
    DeviceCacheEntry entry;
    {
      std::lock_guard lock(this->cache_mutex_);
      auto it = this->device_cache_.find(device_id);
      if (it == this->device_cache_.end()) return {CommandStatus::DeviceNotFound, "device " + device_id + " not found"};
      entry = it->second;
    }

    if (entry.elem_name.empty()) return {CommandStatus::Unsupported, "device has no mixer control"};

    CommandResult result{CommandStatus::Unsupported, "mixer element has no mute switch"};
    bool applied = this->for_each_elem(entry.card, [&](snd_mixer_elem_t* elem) {
      const char* name = this->selem_get_name_(elem);
      bool has_dir     = entry.is_playback ? this->selem_has_playback_volume_(elem) : this->selem_has_capture_volume_(elem);
      if (!name || entry.elem_name != name || !has_dir) return;

      bool has_switch = entry.is_playback ? this->selem_has_playback_switch_(elem) : this->selem_has_capture_switch_(elem);
      if (!has_switch) return;

      int rc = entry.is_playback ? this->selem_set_playback_switch_all_(elem, muted ? 0 : 1) : this->selem_set_capture_switch_all_(elem, muted ? 0 : 1);
      result = rc < 0 ? CommandResult{CommandStatus::BackendError, this->strerror_(rc)} : CommandResult{};
    });
    if (!applied) return {CommandStatus::BackendError, "failed to open mixer for card " + std::to_string(entry.card)};
    return result;
  }

  DynamicLibrary lib_{"libasound.so.2"};
  bool available_ = false;

  std::mutex cache_mutex_;
  std::unordered_map<std::string, DeviceCacheEntry> device_cache_;

  decltype(&::snd_pcm_open) pcm_open_                                                     = nullptr;
  decltype(&::snd_pcm_close) pcm_close_                                                   = nullptr;
  decltype(&::snd_strerror) strerror_                                                     = nullptr;
  decltype(&::snd_card_next) card_next_                                                   = nullptr;
  decltype(&::snd_ctl_open) ctl_open_                                                     = nullptr;
  decltype(&::snd_ctl_close) ctl_close_                                                   = nullptr;
  decltype(&::snd_ctl_pcm_next_device) ctl_pcm_next_device_                               = nullptr;
  decltype(&::snd_ctl_pcm_info) ctl_pcm_info_                                             = nullptr;
  decltype(&::snd_pcm_info_malloc) pcm_info_malloc_                                       = nullptr;
  decltype(&::snd_pcm_info_free) pcm_info_free_                                           = nullptr;
  decltype(&::snd_pcm_info_set_device) pcm_info_set_device_                               = nullptr;
  decltype(&::snd_pcm_info_set_subdevice) pcm_info_set_subdevice_                         = nullptr;
  decltype(&::snd_pcm_info_set_stream) pcm_info_set_stream_                               = nullptr;
  decltype(&::snd_pcm_info_get_name) pcm_info_get_name_                                   = nullptr;
  decltype(&::snd_ctl_card_info_malloc) ctl_card_info_malloc_                             = nullptr;
  decltype(&::snd_ctl_card_info_free) ctl_card_info_free_                                 = nullptr;
  decltype(&::snd_ctl_card_info) ctl_card_info_                                           = nullptr;
  decltype(&::snd_ctl_card_info_get_name) ctl_card_info_get_name_                         = nullptr;
  decltype(&::snd_mixer_open) mixer_open_                                                 = nullptr;
  decltype(&::snd_mixer_close) mixer_close_                                               = nullptr;
  decltype(&::snd_mixer_attach) mixer_attach_                                             = nullptr;
  decltype(&::snd_mixer_selem_register) mixer_selem_register_                             = nullptr;
  decltype(&::snd_mixer_load) mixer_load_                                                 = nullptr;
  decltype(&::snd_mixer_first_elem) mixer_first_elem_                                     = nullptr;
  decltype(&::snd_mixer_elem_next) mixer_elem_next_                                       = nullptr;
  decltype(&::snd_mixer_selem_get_name) selem_get_name_                                   = nullptr;
  decltype(&::snd_mixer_selem_has_playback_volume) selem_has_playback_volume_             = nullptr;
  decltype(&::snd_mixer_selem_has_capture_volume) selem_has_capture_volume_               = nullptr;
  decltype(&::snd_mixer_selem_has_playback_switch) selem_has_playback_switch_             = nullptr;
  decltype(&::snd_mixer_selem_has_capture_switch) selem_has_capture_switch_               = nullptr;
  decltype(&::snd_mixer_selem_get_playback_volume_range) selem_get_playback_volume_range_ = nullptr;
  decltype(&::snd_mixer_selem_get_capture_volume_range) selem_get_capture_volume_range_   = nullptr;
  decltype(&::snd_mixer_selem_get_playback_volume) selem_get_playback_volume_             = nullptr;
  decltype(&::snd_mixer_selem_get_capture_volume) selem_get_capture_volume_               = nullptr;
  decltype(&::snd_mixer_selem_set_playback_volume_all) selem_set_playback_volume_all_     = nullptr;
  decltype(&::snd_mixer_selem_set_capture_volume_all) selem_set_capture_volume_all_       = nullptr;
  decltype(&::snd_mixer_selem_get_playback_switch) selem_get_playback_switch_             = nullptr;
  decltype(&::snd_mixer_selem_get_capture_switch) selem_get_capture_switch_               = nullptr;
  decltype(&::snd_mixer_selem_set_playback_switch_all) selem_set_playback_switch_all_     = nullptr;
  decltype(&::snd_mixer_selem_set_capture_switch_all) selem_set_capture_switch_all_       = nullptr;

  AsyncWorker worker_;  // NOTE: declared last, so worker will be destructed before device cache and its mutex, which might be used by worker
};

}  // namespace audio_device_manager
#endif
