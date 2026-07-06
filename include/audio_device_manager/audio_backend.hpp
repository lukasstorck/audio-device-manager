#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "types.hpp"

namespace audio_device_manager {

using BackendUpdateEventCallback = std::function<void(std::vector<DeviceSnapshot>)>;

class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  const std::string& name() const { return this->name_; }
  bool is_supported(BackendFeature feature) const { return std::to_underlying(this->supported_features_ & feature) != 0; }
  virtual AudioBackendType type() const = 0;
  virtual bool available() const        = 0;

  // Async command API. Implementations must serialize access to the
  // underlying SDK themselves (own thread / AsyncWorker) so these are
  // safe to call concurrently from any caller thread. `on_done`, if
  // given, fires on that serializing thread once the result is known,
  // independent of whether the caller kept the returned future.
  virtual CommandResultFuture set_volume_async(const std::string& device_id, float volume, std::function<void(CommandResult)> on_done = nullptr) = 0;
  virtual CommandResultFuture set_mute_async(const std::string& device_id, bool muted, std::function<void(CommandResult)> on_done = nullptr)     = 0;
  virtual CommandResultFuture set_default_async(const std::string& device_id, std::function<void(CommandResult)> on_done = nullptr)              = 0;

  virtual void request_refresh() = 0;

  void push_update_event(std::vector<DeviceSnapshot> snapshots) {
    std::lock_guard lock(this->on_change_mutex_);
    if (this->on_change_) this->on_change_(std::move(snapshots));
  }
  virtual void subscribe(BackendUpdateEventCallback on_change) {
    std::lock_guard lock(this->on_change_mutex_);
    this->on_change_ = std::move(on_change);
    this->request_refresh();
  }
  virtual void unsubscribe() {
    std::lock_guard lock(this->on_change_mutex_);
    this->on_change_ = nullptr;
  }

 private:
  const std::string name_;
  const BackendFeature supported_features_;

 protected:
  AudioBackend(char const* name, BackendFeature supported_features) : name_(name), supported_features_(supported_features) {}
  std::mutex on_change_mutex_;
  BackendUpdateEventCallback on_change_;
};

}  // namespace audio_device_manager
