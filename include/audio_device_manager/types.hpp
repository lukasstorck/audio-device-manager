#pragma once

#include <functional>
#include <future>
#include <string>

namespace audio_device_manager {

enum class DeviceType { Input, Output };
enum class AudioBackendType { Mock, PulseAudio, Alsa, Wasapi };

enum class BackendFeature : unsigned {
  ListDevices               = 1 << 0,  /// backend supports enumerating all available devices
  ReadDeviceVolume          = 1 << 1,  /// backend supports reading the volume of a device
  ReadDeviceMute            = 1 << 2,  /// backend supports reading the mute state of a device
  ReadDefaultDevice         = 1 << 3,  /// backend supports reading the default device
  SetDeviceVolume           = 1 << 4,  /// backend supports setting the volume of a device
  SetDeviceMute             = 1 << 5,  /// backend supports setting the mute state of a device
  SetDefaultDevice          = 1 << 6,  /// backend supports setting the default device
  DeviceChangeNotifications = 1 << 7,  /// backend supports sending notifications of device changes

  All = ListDevices | ReadDeviceVolume | ReadDeviceMute | ReadDefaultDevice | SetDeviceVolume | SetDeviceMute | SetDefaultDevice | DeviceChangeNotifications,
};

constexpr BackendFeature operator|(BackendFeature lhs, BackendFeature rhs) {
  return static_cast<BackendFeature>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

constexpr BackendFeature operator&(BackendFeature lhs, BackendFeature rhs) {
  return static_cast<BackendFeature>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

constexpr BackendFeature& operator|=(BackendFeature& lhs, BackendFeature rhs) {
  lhs = lhs | rhs;
  return lhs;
}

struct DeviceId {
  AudioBackendType backend_type;
  std::string backend_device_id;  // stable id as reported by the backend

  bool operator==(const DeviceId&) const = default;
};

struct DeviceIdHash {
  size_t operator()(const DeviceId& id) const noexcept {
    size_t h1 = std::hash<std::string>{}(id.backend_device_id);
    size_t h2 = std::hash<int>{}(static_cast<int>(id.backend_type));
    return h1 ^ (h2 << 1);
  }
};

struct DeviceSnapshot {
  std::string backend_device_id;
  std::string name;
  DeviceType type = DeviceType::Output;
  bool muted      = false;
  bool is_default = false;
  float volume    = 0.f;
  float min_level = 0.f;
  float max_level = 1.f;
};

// TODO: adjust to what is really used
enum class CommandStatus { Ok, DeviceNotFound, BackendError, Unsupported, Timeout };

struct CommandResult {
  CommandStatus status = CommandStatus::Ok;
  std::string detail;  // backend-specific message; empty on success
  explicit operator bool() const { return status == CommandStatus::Ok; }
};

using CommandResultFuture = std::future<CommandResult>;

}  // namespace audio_device_manager
