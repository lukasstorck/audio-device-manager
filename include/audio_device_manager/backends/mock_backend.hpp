#pragma once

#include "../audio_backend.hpp"

namespace audio_device_manager {

class MockBackend : public AudioBackend {
 public:
  MockBackend() : AudioBackend("Mock", BackendFeature::All) {}
  AudioBackendType type() const override { return AudioBackendType::Mock; }
  bool available() const override { return true; }

  // update backend state and send mock audio backend device update event
  void push_snapshot(std::vector<DeviceSnapshot> snapshots, bool suppress_update_event = false) {
    {
      std::lock_guard lock(this->snapshots_mutex_);
      this->snapshots_ = snapshots;
    }
    if (!suppress_update_event) this->push_update_event(std::move(snapshots));
  }

 private:
  CommandResult handle_set_volume(const std::string& device_id, float volume) override {
    return this->mutate_device(device_id, [volume](DeviceSnapshot& snap) { snap.volume = volume; });
  }
  CommandResult handle_set_mute(const std::string& device_id, bool muted) override {
    return this->mutate_device(device_id, [muted](DeviceSnapshot& snap) { snap.muted = muted; });
  }
  CommandResult handle_set_default(const std::string& device_id) override {
    return this->mutate_device(
        device_id,
        /*mutate_target=*/[](DeviceSnapshot& snap) { snap.is_default = true; },
        /*mutate_others=*/[](DeviceSnapshot& snap) { snap.is_default = false; });  // enforce exclusivity, mirrors real backends
  }

  CommandResult handle_refresh() override {
    std::vector<DeviceSnapshot> copy;
    {
      std::lock_guard lock(this->snapshots_mutex_);
      copy = this->snapshots_;
    }
    this->push_update_event(std::move(copy));
    return {};
  }

  using Mutator = std::function<void(DeviceSnapshot&)>;

  /// @brief Locate a device by id and apply a mutation to it, optionally mutating every
  /// other device first, then push the resulting state exactly once.
  ///
  /// @param device_id id of the device to mutate
  /// @param mutate_target mutation applied to the target device
  /// @param mutate_others optional mutation applied to every device other than the target;
  ///                      skipped entirely if the target device is not found
  /// @return `DeviceNotFound` if no device with `device_id` exists, `Ok` otherwise
  CommandResult mutate_device(const std::string& device_id, Mutator mutate_target, Mutator mutate_others = nullptr) {
    CommandResult result{};
    std::vector<DeviceSnapshot> snapshot_copy;
    {
      std::lock_guard lock(this->snapshots_mutex_);
      auto it = std::find_if(this->snapshots_.begin(), this->snapshots_.end(), [&](const DeviceSnapshot& s) { return s.backend_device_id == device_id; });
      if (it == this->snapshots_.end()) {
        result.status = CommandStatus::DeviceNotFound;
        result.detail = "device " + device_id + " not found";
        return result;
      }

      if (mutate_others) {
        for (auto& snap : this->snapshots_)
          if (snap.backend_device_id != device_id) mutate_others(snap);
      }
      mutate_target(*it);

      snapshot_copy = this->snapshots_;
    }

    this->push_update_event(std::move(snapshot_copy));
    return result;
  }

  std::mutex snapshots_mutex_;
  std::vector<DeviceSnapshot> snapshots_;  // last known mock state
};

}  // namespace audio_device_manager
