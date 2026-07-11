#pragma once

#include "types.hpp"

namespace audio_device_manager {

class AudioBackend;  // fwd decl

class Device {
 public:
  Device(AudioBackend& backend, DeviceId id, std::string name, DeviceType type, float min_level, float max_level)
      : backend_(backend), id_(std::move(id)), name_(std::move(name)), type_(type), min_level_(min_level), max_level_(max_level) {}

  // Device is non-copyable and non-movable, only create references
  Device(const Device&)            = delete;
  Device& operator=(const Device&) = delete;

  /// Returns the device's unique identifier
  const DeviceId& id() const { return this->id_; }
  /// Returns the device's name
  const std::string& name() const { return this->name_; }
  /// Returns the device's type
  DeviceType type() const { return this->type_; }
  /// Returns the device's backend name as a human-readable string
  const std::string& backend_name() const { return this->backend_.name(); }

  /// Returns whether the device is actively detected by the backend
  bool connected() const { return this->connected_.load(std::memory_order_acquire); }
  /// Returns whether the device is muted
  bool muted() const { return this->muted_.load(std::memory_order_acquire); }
  /// Returns whether the device is set as the default device
  bool is_default() const { return this->is_default_.load(std::memory_order_acquire); }
  /// Returns the device's current volume level
  float volume() const { return this->volume_.load(std::memory_order_acquire); }
  /// Returns the device's minimum volume level
  float min_level() const { return this->min_level_.load(std::memory_order_acquire); }
  /// Returns the device's maximum volume level
  float max_level() const { return this->max_level_.load(std::memory_order_acquire); }

  /// Asynchronously set this device's output volume.
  ///
  /// Command dispatched to responsible backend. Execution occurs on the backend's
  /// worker thread.
  ///
  /// @param volume Target volume. Valid range depends on the backend.
  /// @param on_done Optional completion callback. Invoked on the backend's worker
  ///        thread after the command finishes.
  /// @return Future representing the command result. Call .get() to wait for
  ///         completion or discard the future for fire-and-forget execution.
  CommandResultFuture set_volume_async(float volume, std::function<void(CommandResult)> on_done = nullptr) {
    return backend_.request_set_volume(id_.backend_device_id, volume, std::move(on_done));
  }

  /// Asynchronously change this device's mute state.
  ///
  /// Command dispatched to responsible backend. Execution occurs on the backend's
  /// worker thread.
  ///
  /// @param muted True to mute the device, false to unmute.
  /// @param on_done Optional completion callback. Invoked on the backend's worker
  ///        thread after the command finishes.
  /// @return Future representing the command result. Call .get() to wait for
  ///         completion or discard the future for fire-and-forget execution.
  CommandResultFuture set_mute_async(bool muted, std::function<void(CommandResult)> on_done = nullptr) {
    return backend_.request_set_mute(id_.backend_device_id, muted, std::move(on_done));
  }

  /// Asynchronously make this device the system default.
  ///
  /// Command dispatched to responsible backend. Execution occurs on the backend's
  /// worker thread.
  ///
  /// @param on_done Optional completion callback. Invoked on the backend's worker
  ///        thread after the command finishes.
  /// @return Future representing the command result. Call .get() to wait for
  ///         completion or discard the future for fire-and-forget execution.
  CommandResultFuture set_default_async(std::function<void(CommandResult)> on_done = nullptr) {
    return backend_.request_set_default(id_.backend_device_id, std::move(on_done));
  }

  /// Set this device's output volume.
  ///
  /// Blocks until the internal async backend command completes.
  ///
  /// @param volume Target volume. Valid range depends on the backend.
  /// @return Result of the completed command.
  CommandResult set_volume(float volume) { return this->set_volume_async(volume).get(); }

  /// Change this device's mute state.
  ///
  /// Blocks until the internal async backend command completes.
  ///
  /// @param muted True to mute the device, false to unmute.
  /// @return Result of the completed command.
  CommandResult set_mute(bool muted) { return this->set_mute_async(muted).get(); }

  /// Make this device the system default.
  ///
  /// Blocks until the internal async backend command completes.
  ///
  /// @return Result of the completed command.
  CommandResult set_default() { return this->set_default_async().get(); }

 private:
  friend class AudioDeviceManager;  // allow AudioDeviceManager to merge snapshots
  /// Apply a snapshot to this device. Returns whether a change was detected
  bool apply_snapshot(const DeviceSnapshot& snap) {
    bool change_detected = false;

    bool now_connected = true;
    change_detected |= connected_.exchange(now_connected, std::memory_order_acq_rel) != now_connected;
    change_detected |= muted_.exchange(snap.muted, std::memory_order_acq_rel) != snap.muted;
    change_detected |= is_default_.exchange(snap.is_default, std::memory_order_acq_rel) != snap.is_default;
    change_detected |= volume_.exchange(snap.volume, std::memory_order_acq_rel) != snap.volume;
    change_detected |= min_level_.exchange(snap.min_level, std::memory_order_acq_rel) != snap.min_level;
    change_detected |= max_level_.exchange(snap.max_level, std::memory_order_acq_rel) != snap.max_level;

    return change_detected;
  }
  /// Mark this device as disconnected. Returns whether a change was detected
  bool mark_disconnected() { return connected_.exchange(false, std::memory_order_acquire) == true; }

  const DeviceId id_;
  const std::string name_;
  const DeviceType type_;
  AudioBackend& backend_;

  std::atomic<bool> connected_{true};
  std::atomic<bool> muted_{false};
  std::atomic<bool> is_default_{false};
  std::atomic<float> volume_{0.f};
  std::atomic<float> min_level_{0.f};
  std::atomic<float> max_level_{1.f};
};

}  // namespace audio_device_manager
