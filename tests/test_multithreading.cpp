#include <doctest/doctest.h>

#include <ADM>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>

namespace {

audio_device_manager::Device* get_default_device(audio_device_manager::AudioDeviceManager& manager, audio_device_manager::DeviceType type) {
  for (auto& device : manager.get_devices_sorted())
    if (device.get().type() == type && device.get().is_default()) return &device.get();
  return nullptr;
}

}  // namespace

TEST_CASE("multi-threading: OS-level device change on one manager is observed by another") {
  audio_device_manager::AudioDeviceManager writer_manager{std::chrono::milliseconds(0)};
  audio_device_manager::AudioDeviceManager listener_manager{std::chrono::milliseconds(0)};

  audio_device_manager::register_audio_backends(writer_manager);
  audio_device_manager::register_audio_backends(listener_manager);
  writer_manager.refresh();
  listener_manager.refresh();

  auto run_for = [&](audio_device_manager::DeviceType type) {
    auto* writer_device   = get_default_device(writer_manager, type);
    auto* listener_device = get_default_device(listener_manager, type);
    REQUIRE(writer_device != nullptr);
    REQUIRE(listener_device != nullptr);
    // sanity check: both managers really are looking at the same physical endpoint
    REQUIRE(writer_device->id().backend_device_id == listener_device->id().backend_device_id);

    std::mutex mtx;
    std::condition_variable cv;

    // listener_manager's own subscription; only used to wake the waiter below,
    // the actual values are read straight off listener_device once woken.
    auto subscription = listener_manager.subscribe([&] {
      std::lock_guard lock(mtx);
      cv.notify_all();
    });

    // Bounded wait instead of a busy-loop: a push notification that never
    // arrives is exactly the failure mode this test exists to catch, so it
    // must be able to time out and fail rather than hang forever.
    auto wait_until = [&](auto predicate) {
      std::unique_lock lock(mtx);
      return cv.wait_for(lock, std::chrono::seconds(1), predicate);
    };

    // --- volume ---
    float original_volume = writer_device->volume();
    float target_volume   = original_volume > 0.5f ? original_volume - 0.1f : original_volume + 0.1f;

    REQUIRE(writer_device->set_volume(target_volume).status == audio_device_manager::CommandStatus::Ok);
    bool volume_change_observed = wait_until([&] { return std::fabs(listener_device->volume() - target_volume) < 0.02f; });
    CHECK(volume_change_observed);
    if (!volume_change_observed) MESSAGE("listener_manager never observed the volume change made by writer_manager");

    REQUIRE(writer_device->set_volume(original_volume).status == audio_device_manager::CommandStatus::Ok);
    CHECK(wait_until([&] { return std::fabs(listener_device->volume() - original_volume) < 0.02f; }));

    // --- mute ---
    bool original_muted = writer_device->muted();

    REQUIRE(writer_device->set_mute(!original_muted).status == audio_device_manager::CommandStatus::Ok);
    bool mute_change_observed = wait_until([&] { return listener_device->muted() == !original_muted; });
    CHECK(mute_change_observed);
    if (!mute_change_observed) MESSAGE("listener_manager never observed the mute change made by writer_manager");

    REQUIRE(writer_device->set_mute(original_muted).status == audio_device_manager::CommandStatus::Ok);
    CHECK(wait_until([&] { return listener_device->muted() == original_muted; }));
  };

  SUBCASE("default output device") { run_for(audio_device_manager::DeviceType::Output); }
  SUBCASE("default input device") { run_for(audio_device_manager::DeviceType::Input); }
}
