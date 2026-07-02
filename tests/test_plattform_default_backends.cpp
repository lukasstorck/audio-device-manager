#include <doctest/doctest.h>

#include <ADM>
#include <string>

namespace {

audio_device_manager::AudioDeviceManager& make_manager() {
  static audio_device_manager::AudioDeviceManager manager;
  static bool initialized = [] {
    audio_device_manager::register_audio_backends(manager);
    manager.refresh();
    return true;
  }();
  (void)initialized;
  return manager;
}

audio_device_manager::Device* get_default_output_device(audio_device_manager::AudioDeviceManager& manager) {
  for (auto& device : manager.get_devices())
    if (device.get().type() == audio_device_manager::DeviceType::Output && device.get().is_default()) return &device.get();
  return nullptr;
}

}  // namespace

TEST_CASE("default backends: backend count") {
  auto& manager = make_manager();
#if defined(__linux__)
  // CHECK(manager.backend_count() == 2);
  CHECK(manager.backend_count() == 1);
#elif defined(_WIN32)
  CHECK(manager.backend_count() == 1);
#endif
}

TEST_CASE("default backends: device list non-empty with valid fields") {
  auto devices = make_manager().get_devices();
  REQUIRE(devices.size() > 0);

  std::string device_list;
  for (auto& device : devices) {
    CHECK(!device.get().name().empty());
    CHECK(!device.get().id().backend_device_id.empty());
    CHECK(device.get().connected());
    auto type = device.get().type() == audio_device_manager::DeviceType::Input ? "input" : "output";
    device_list += std::string(type) + "  " + device.get().id().backend_device_id + "  " + device.get().name() + "\n";
  }
  MESSAGE("devices:\n", device_list);
}

TEST_CASE("default backends: backend name on devices") {
  auto devices = make_manager().get_devices();
  REQUIRE(devices.size() > 0);
  for (auto& device : devices) {
#if defined(__linux__)
    CHECK(device.get().backend_name() == "PulseAudio");
    // CHECK(d.get().backend_name() == "PulseAudio" || d.get().backend_name() == "ALSA");
#elif defined(_WIN32)
    CHECK(d.get().backend_name() == "WASAPI");
#endif
  }
}

TEST_CASE("default backends: exactly one default per type") {
  auto devices = make_manager().get_devices();
  REQUIRE(devices.size() > 0);

  int default_outputs = 0;
  int default_inputs  = 0;
  for (auto& device : devices) {
    if (!device.get().is_default()) continue;
    if (device.get().type() == audio_device_manager::DeviceType::Output) ++default_outputs;
    if (device.get().type() == audio_device_manager::DeviceType::Input) ++default_inputs;
  }

  CHECK(default_outputs == 1);
  CHECK(default_inputs == 1);
}

TEST_CASE("default backends: volume within min/max range") {
  auto devices = make_manager().get_devices();
  REQUIRE(devices.size() > 0);
  for (auto& device : devices) {
    CHECK(device.get().volume() >= device.get().min_level());
    CHECK(device.get().volume() <= device.get().max_level());
  }
}

TEST_CASE("default backends: set_volume changes value and restores") {
  auto& manager = make_manager();
  auto* target  = get_default_output_device(manager);
  REQUIRE(target != nullptr);

  float original  = target->volume();
  float alternate = original > 0.5f ? original - 0.1f : original + 0.1f;

  CHECK(target->set_volume(alternate).status == audio_device_manager::CommandStatus::Ok);
  manager.refresh();

  CHECK(target->volume() == doctest::Approx(alternate).epsilon(0.01f));

  CHECK(target->set_volume(original).status == audio_device_manager::CommandStatus::Ok);
  manager.refresh();

  CHECK(target->volume() == doctest::Approx(original).epsilon(0.01f));
}

TEST_CASE("default backends: set_mute toggles and restores") {
  auto& manager = make_manager();
  auto* target  = get_default_output_device(manager);
  REQUIRE(target != nullptr);

  bool original = target->muted();

  CHECK(target->set_mute(!original).status == audio_device_manager::CommandStatus::Ok);
  manager.refresh();

  CHECK(target->muted() == !original);

  CHECK(target->set_mute(original).status == audio_device_manager::CommandStatus::Ok);
  manager.refresh();

  CHECK(target->muted() == original);
}

TEST_CASE("default backends: refresh preserves device ids") {
  auto& manager = make_manager();
  auto before   = manager.get_devices();
  REQUIRE(before.size() > 0);

  std::vector<std::string> ids_before;
  for (auto& d : before) ids_before.push_back(d.get().id().backend_device_id);

  manager.refresh();
  manager.refresh();

  auto after = manager.get_devices();
  CHECK(before.size() == after.size());

  std::vector<std::string> ids_after;
  for (auto& d : after) ids_after.push_back(d.get().id().backend_device_id);

  std::sort(ids_before.begin(), ids_before.end());
  std::sort(ids_after.begin(), ids_after.end());
  CHECK(ids_before == ids_after);
}

TEST_CASE("default backends: concurrent refresh_async calls coalesce") {
  auto& manager = make_manager();

  auto f1 = manager.refresh_async();
  auto f2 = manager.refresh_async();
  auto f3 = manager.refresh_async();

  auto r1 = f1.get();
  auto r2 = f2.get();
  auto r3 = f3.get();

  CHECK(r1.status == audio_device_manager::CommandStatus::Ok);
  CHECK(r2.status == audio_device_manager::CommandStatus::Ok);
  CHECK(r3.status == audio_device_manager::CommandStatus::Ok);
}
