#include <doctest/doctest.h>

#include <ADM>
#include <audio_device_manager/backends/mock_backend.hpp>

using namespace audio_device_manager;

namespace {

DeviceSnapshot make_snapshot(std::string id = "dev1") {
  return {std::move(id), "Speakers", DeviceType::Output, /*muted=*/false, /*is_default=*/false, /*volume=*/0.5f, 0.f, 1.f};
}

}  // namespace

TEST_CASE("set_volume updates device state via backend round-trip") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  Device& device = devices[0];
  REQUIRE(device.volume() == doctest::Approx(0.5f));

  CommandResult result = device.set_volume(0.9f);

  CHECK(result.status == CommandStatus::Ok);
  CHECK(static_cast<bool>(result) == true);
  CHECK(device.volume() == doctest::Approx(0.9f));
}

TEST_CASE("set_mute updates device state via backend round-trip") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  Device& device = devices[0];
  REQUIRE(device.muted() == false);

  CommandResult result = device.set_mute(true);

  CHECK(result.status == CommandStatus::Ok);
  CHECK(device.muted() == true);
}

TEST_CASE("set_default updates device state via backend round-trip") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  Device& device = devices[0];
  REQUIRE(device.is_default() == false);

  CommandResult result = device.set_default();

  CHECK(result.status == CommandStatus::Ok);
  CHECK(device.is_default() == true);
}

TEST_CASE("set_volume_async fires on_done callback with success result") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  Device& device = devices[0];

  std::promise<CommandResult> callback_promise;
  auto callback_future = callback_promise.get_future();

  auto future = device.set_volume_async(0.3f, [&callback_promise](CommandResult r) { callback_promise.set_value(r); });

  CommandResult future_result   = future.get();
  CommandResult callback_result = callback_future.get();

  CHECK(future_result.status == CommandStatus::Ok);
  CHECK(callback_result.status == CommandStatus::Ok);
  CHECK(device.volume() == doctest::Approx(0.3f));
}

TEST_CASE("set_volume on a vanished device returns DeviceNotFound") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  Device& device = devices[0];

  mock->push_snapshot({});  // device vanishes from backend, mock's internal snapshot cleared too
  REQUIRE(device.connected() == false);

  CommandResult result = device.set_volume(0.7f);

  CHECK(result.status == CommandStatus::DeviceNotFound);
  CHECK(static_cast<bool>(result) == false);
  CHECK(device.volume() == doctest::Approx(0.5f));  // failed command, no state mutated
}

TEST_CASE("set_default clears is_default on other devices") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  auto snap_a       = make_snapshot("dev1");
  snap_a.is_default = true;
  auto snap_b       = make_snapshot("dev2");
  mock->push_snapshot({snap_a, snap_b});

  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 2);

  Device* dev_a = nullptr;
  Device* dev_b = nullptr;
  for (Device& d : devices) {
    if (d.id().backend_device_id == "dev1") dev_a = &d;
    if (d.id().backend_device_id == "dev2") dev_b = &d;
  }
  REQUIRE(dev_a != nullptr);
  REQUIRE(dev_b != nullptr);
  REQUIRE(dev_a->is_default() == true);

  dev_b->set_default();

  CHECK(dev_b->is_default() == true);
  CHECK(dev_a->is_default() == false);
}

TEST_CASE("device reports correct backend name") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);

  CHECK(devices[0].get().backend_name() == "Mock");
}
