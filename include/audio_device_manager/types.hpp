#pragma once

#include <functional>
#include <future>
#include <string>

namespace audio_device_manager {

enum class DeviceType { Input, Output };
enum class AudioBackendType { Mock, PulseAudio, Alsa, Wasapi };

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
