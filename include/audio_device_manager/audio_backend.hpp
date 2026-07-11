#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "async.hpp"
#include "types.hpp"

namespace audio_device_manager {

using Request = std::function<CommandResult()>;

class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  /// Get a human-readable name for this backend
  const std::string& name() const { return this->name_; }
  /// Test if specified feature is supported
  bool is_supported(BackendFeature feature) const { return std::to_underlying(this->supported_features_ & feature) != 0; }
  virtual AudioBackendType type() const = 0;
  virtual bool available() const        = 0;

  // request_* functions are async commands that only set up the request on the worker thread
  // handle_* functions are blocking commands on the worker thread, that need to be implemented for each backend
  // all requests follow the same structure in handle_request(...):
  // - prepare promise/future, set up job and return future
  // - job on worker thread:
  //   - execute request
  //   - retrieve command result
  //   - call on_done callback if provided
  //   - set future value

  CommandResultFuture request_set_volume(const std::string& device_id, float volume, std::function<void(CommandResult)> on_done = nullptr) {
    return this->handle_request([this, device_id, volume] { return this->handle_set_volume(device_id, volume); }, std::move(on_done));
  }
  CommandResultFuture request_set_mute(const std::string& device_id, bool muted, std::function<void(CommandResult)> on_done = nullptr) {
    return this->handle_request([this, device_id, muted] { return this->handle_set_mute(device_id, muted); }, std::move(on_done));
  }
  CommandResultFuture request_set_default(const std::string& device_id, std::function<void(CommandResult)> on_done = nullptr) {
    return this->handle_request([this, device_id] { return this->handle_set_default(device_id); }, std::move(on_done));
  }
  CommandResultFuture request_refresh(std::function<void(CommandResult)> on_done = nullptr) {
    return this->handle_request([this] { return this->handle_refresh(); }, std::move(on_done));
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

  virtual CommandResult handle_set_volume(const std::string& device_id, float volume) = 0;
  virtual CommandResult handle_set_mute(const std::string& device_id, bool muted)     = 0;
  virtual CommandResult handle_set_default(const std::string& device_id)              = 0;
  virtual CommandResult handle_refresh()                                              = 0;

 protected:
  // on_thread_start/on_thread_end run once on the worker thread, before/after
  // the request loop. Backends needing per-thread setup (e.g. WASAPI's
  // CoInitializeEx) hook in here instead of managing their own worker.
  AudioBackend(char const* name, BackendFeature supported_features, std::function<void()> on_thread_start = nullptr,
               std::function<void()> on_thread_end = nullptr)
      : name_(name), supported_features_(supported_features), worker_(std::move(on_thread_start), std::move(on_thread_end)) {}
  std::mutex on_change_mutex_;
  BackendUpdateEventCallback on_change_;

  CommandResultFuture handle_request(Request request, std::function<void(CommandResult)> on_done) {
    auto promise = std::make_shared<std::promise<CommandResult>>();
    auto future  = promise->get_future();

    this->worker_.post([request = std::move(request), on_done = std::move(on_done), promise] {
      CommandResult result = request();

      if (on_done) on_done(result);
      promise->set_value(result);
    });

    return future;
  }

  void push_update_event(std::vector<DeviceSnapshot> snapshots) {
    std::lock_guard lock(this->on_change_mutex_);
    if (this->on_change_) this->on_change_(std::move(snapshots));
  }

  // std::thread polling_thread_;  // TODO
  AsyncWorker worker_;
};

}  // namespace audio_device_manager
