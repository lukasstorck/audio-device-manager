#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ADM>
#include <audio_device_manager/backends/mock_backend.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

audio_device_manager::DeviceSnapshot make_snapshot(std::string id = "dev1") {
  return {std::move(id), "Speakers", audio_device_manager::DeviceType::Output, /*muted=*/false, /*is_default=*/false, /*volume=*/0.5f, 0.f, 1.f};
}

}  // namespace

TEST_CASE("mock backend: register_backend(...), backend_count(), name()") {
  audio_device_manager::AudioDeviceManager manager;
  CHECK(manager.backend_count() == 0);

  manager.register_backend(std::make_unique<audio_device_manager::MockBackend>());
  CHECK(manager.backend_count() == 1);

  CHECK(audio_device_manager::MockBackend().name() == "Mock");
}

TEST_CASE("mock backend: push_snapshot(...)") {
  audio_device_manager::AudioDeviceManager manager;
  auto* mock = new audio_device_manager::MockBackend();
  manager.register_backend(std::unique_ptr<audio_device_manager::AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  CHECK(devices.size() == 1);
}

TEST_CASE("mock backend: backend name reported on device") {
  audio_device_manager::AudioDeviceManager manager;
  auto* mock = new audio_device_manager::MockBackend();
  manager.register_backend(std::unique_ptr<audio_device_manager::AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);

  CHECK(devices[0].get().backend_name() == "Mock");
}

TEST_CASE("mock backend: set_default(...) clears other devices of same type only") {
  audio_device_manager::AudioDeviceManager manager;
  auto* mock = new audio_device_manager::MockBackend();
  manager.register_backend(std::unique_ptr<audio_device_manager::AudioBackend>(mock));

  auto out_a       = make_snapshot("out1");
  out_a.is_default = true;
  auto out_b       = make_snapshot("out2");
  auto in_a        = make_snapshot("in1");
  in_a.type        = audio_device_manager::DeviceType::Input;
  in_a.is_default  = true;
  mock->push_snapshot({out_a, out_b, in_a});

  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 3);

  audio_device_manager::Device* dev_out_b = nullptr;
  audio_device_manager::Device* dev_in_a  = nullptr;
  for (audio_device_manager::Device& d : devices) {
    if (d.id().backend_device_id == "out2") dev_out_b = &d;
    if (d.id().backend_device_id == "in1") dev_in_a = &d;
  }
  REQUIRE(dev_out_b != nullptr);
  REQUIRE(dev_in_a != nullptr);
  REQUIRE(dev_in_a->is_default() == true);

  dev_out_b->set_default();

  CHECK(dev_out_b->is_default() == true);
  CHECK(dev_in_a->is_default() == true);  // different type, must stay untouched
}

TEST_CASE("mock backend: set_volume(...) on vanished device returns DeviceNotFound") {
  audio_device_manager::AudioDeviceManager manager;
  auto* mock = new audio_device_manager::MockBackend();
  manager.register_backend(std::unique_ptr<audio_device_manager::AudioBackend>(mock));

  mock->push_snapshot({make_snapshot()});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  audio_device_manager::Device& device = devices[0];

  auto previous_volume = device.volume();

  mock->push_snapshot({});  // device vanishes from backend, mock's internal snapshot cleared too
  REQUIRE(device.connected() == false);

  audio_device_manager::CommandResult result = device.set_volume(0.7f);

  CHECK(result.status == audio_device_manager::CommandStatus::DeviceNotFound);
  CHECK(static_cast<bool>(result) == false);
  CHECK(device.volume() == doctest::Approx(previous_volume));  // command failed, volume unchanged
}

TEST_CASE("mock backend: subscription callback fires on device state change") {
  audio_device_manager::AudioDeviceManager manager{/*notify_rate_limit=*/std::chrono::milliseconds(0)};
  auto* mock = new audio_device_manager::MockBackend();
  manager.register_backend(std::unique_ptr<audio_device_manager::AudioBackend>(mock));

  std::mutex mtx;
  std::condition_variable cv;
  int fired = 0;

  auto sub = manager.subscribe([&] {
    std::lock_guard lock(mtx);
    ++fired;
    cv.notify_all();
  });

  auto wait_for_notification = [&] {
    std::unique_lock lock(mtx);
    int expected = fired + 1;
    return cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return fired >= expected; });
  };

  CHECK(!wait_for_notification());  // after 50 ms timeout the callback should not have been fired yet -> notification not expected

  mock->push_snapshot({{{"dev1"}, "Speakers", audio_device_manager::DeviceType::Output}}, /*suppress_update_event=*/true);
  CHECK(!wait_for_notification());  // update supressed -> notification not expected
  manager.refresh_async();
  CHECK(wait_for_notification());  // notification expected

  mock->push_snapshot({{{"dev1"}, "Speakers", audio_device_manager::DeviceType::Output, /*muted=*/true}}, /*suppress_update_event=*/true);
  CHECK(!wait_for_notification());  // update supressed -> notification not expected
  manager.refresh_async();
  CHECK(wait_for_notification());  // notification expected
}
