#pragma once
#include <functional>
#include <string>
#include <vector>

#include "types.hpp"

namespace audio_device_manager {

using BackendUpdateEventCallback = std::function<void(std::vector<DeviceSnapshot>)>;

class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  const std::string& name() const { return name_; }
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

  void push_update_event(std::vector<DeviceSnapshot> snapshots) {
    if (this->on_change_) this->on_change_(std::move(snapshots));
  }
  void subscribe(BackendUpdateEventCallback on_change) { this->on_change_ = std::move(on_change); }
  void unsubscribe() { this->on_change_ = nullptr; }

 private:
  const std::string name_;

 protected:
  AudioBackend(char const* name) : name_(name) {}
  BackendUpdateEventCallback on_change_;
};

}  // namespace audio_device_manager
