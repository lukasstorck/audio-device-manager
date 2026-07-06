#pragma once

#include "../async.hpp"
#include "../audio_backend.hpp"

namespace audio_device_manager {

class MockBackend : public AudioBackend {
 public:
  MockBackend() : AudioBackend("Mock", BackendFeature::All) {}
  AudioBackendType type() const override { return AudioBackendType::Mock; }
  bool available() const override { return true; }

  CommandResultFuture set_volume_async(const std::string& device_id, float volume, std::function<void(CommandResult)> on_done = nullptr) override {
    return this->run_command(device_id, std::move(on_done),
                             [volume](std::vector<DeviceSnapshot>& snaps, std::vector<DeviceSnapshot>::iterator it) { it->volume = volume; });
  }
  CommandResultFuture set_mute_async(const std::string& device_id, bool muted, std::function<void(CommandResult)> on_done = nullptr) override {
    return this->run_command(device_id, std::move(on_done),
                             [muted](std::vector<DeviceSnapshot>& snaps, std::vector<DeviceSnapshot>::iterator it) { it->muted = muted; });
  }
  CommandResultFuture set_default_async(const std::string& device_id, std::function<void(CommandResult)> on_done = nullptr) override {
    return this->run_command(device_id, std::move(on_done), [](std::vector<DeviceSnapshot>& snaps, std::vector<DeviceSnapshot>::iterator it) {
      for (auto& s : snaps) s.is_default = false;  // enforce exclusivity, mirrors real backends
      it->is_default = true;
    });
  }

  void request_refresh() override {
    this->worker_.post([this] {
      std::vector<DeviceSnapshot> copy;
      {
        std::lock_guard lock(this->snapshots_mutex_);
        copy = this->snapshots_;
      }
      this->push_update_event(std::move(copy));
    });
  }

  // update backend state and send mock audio backend device update event
  void push_snapshot(std::vector<DeviceSnapshot> snapshots, bool suppress_update_event = false) {
    {
      std::lock_guard lock(this->snapshots_mutex_);
      this->snapshots_ = snapshots;
    }
    if (!suppress_update_event) this->push_update_event(std::move(snapshots));
  }

 private:
  using Mutator = std::function<void(std::vector<DeviceSnapshot>&, std::vector<DeviceSnapshot>::iterator)>;

  // run snapshot mutation on worker thread, then send update event
  CommandResultFuture run_command(std::string device_id, std::function<void(CommandResult)> on_done, Mutator mutate) {
    auto promise = std::make_shared<std::promise<CommandResult>>();
    auto future  = promise->get_future();

    this->worker_.post([this, device_id = std::move(device_id), on_done = std::move(on_done), mutate = std::move(mutate), promise] {
      CommandResult result{};
      try {
        std::vector<DeviceSnapshot> snapshot_copy;
        {
          std::lock_guard lock(this->snapshots_mutex_);
          auto it = std::find_if(this->snapshots_.begin(), this->snapshots_.end(), [&](const DeviceSnapshot& s) { return s.backend_device_id == device_id; });
          if (it == this->snapshots_.end()) {
            result.status = CommandStatus::DeviceNotFound;
            result.detail = "device " + device_id + " not found";
          } else {
            mutate(this->snapshots_, it);
            snapshot_copy = this->snapshots_;
          }
        }

        if (result) this->push_update_event(std::move(snapshot_copy));

        if (on_done) on_done(result);
        promise->set_value(result);
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
    });

    return future;
  }

  std::mutex snapshots_mutex_;
  std::vector<DeviceSnapshot> snapshots_;  // last known mock state
  AsyncWorker worker_;
};

}  // namespace audio_device_manager
