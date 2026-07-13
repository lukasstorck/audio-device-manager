#include <doctest/doctest.h>

#include <ADM>
#include <audio_device_manager/backends/mock_backend.hpp>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <audio_device_manager/backends/alsa_backend.hpp>
#include <audio_device_manager/backends/pulseaudio_backend.hpp>
#elif defined(_WIN32)
#include <audio_device_manager/backends/wasapi_backend.hpp>
#endif

namespace {

using namespace audio_device_manager;

// mock backend starts empty; seed it with one input + one output so the
// common tests have something to operate on, same as a real backend would
void seed_if_mock(AudioBackend& backend) {
  if (auto* mock = dynamic_cast<MockBackend*>(&backend)) {
    mock->push_snapshot({
        {"out1", "Test Output", DeviceType::Output, /*muted=*/false, /*is_default=*/true, 0.5f, 0.f, 1.f},
        {"in1", "Test Input", DeviceType::Input, /*muted=*/false, /*is_default=*/true, 0.5f, 0.f, 1.f},
    });
  }
}

Device* find_device(AudioDeviceManager& manager, DeviceType type) {
  for (auto& device : manager.get_devices())
    if (device.get().type() == type) return &device.get();
  return nullptr;
}

// common setup shared by every test body: construct BackendT, bail out (test
// passes trivially) if this machine doesn't actually have that backend,
// register it, seed it if it's the mock, and do an initial refresh
template <typename BackendT>
bool setup(AudioDeviceManager& manager, AudioBackend*& backend_out) {
  auto probe = std::make_unique<BackendT>();
  if (!probe->available()) {
    MESSAGE("backend not available on this machine, skipping");
    return false;
  }
  backend_out = probe.get();
  manager.register_backend(std::move(probe));
  seed_if_mock(*backend_out);
  manager.refresh();
  return true;
}

template <typename BackendT>
void common_device_list_valid() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  auto devices = manager.get_devices();
  REQUIRE(devices.size() > 0);
  for (auto& device : devices) {
    CHECK(!device.get().name().empty());
    CHECK(!device.get().id().backend_device_id.empty());
    CHECK(device.get().connected());
    CHECK(device.get().backend_name() == backend->name());
  }
}

template <typename BackendT>
void common_volume_in_range() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  for (auto& device : manager.get_devices()) {
    CHECK(device.get().volume() >= device.get().min_level());
    CHECK(device.get().volume() <= device.get().max_level());
  }
}

template <typename BackendT>
void common_exactly_one_default_per_type() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  if (!backend->is_supported(BackendFeature::ReadDefaultDevice)) {
    MESSAGE("backend doesn't report default devices, skipping");
    return;
  }

  std::unordered_map<DeviceType, int> device_count;
  std::unordered_map<DeviceType, int> default_count;
  for (auto& device : manager.get_devices()) {
    ++device_count[device.get().type()];
    if (device.get().is_default()) ++default_count[device.get().type()];
  }

  // every type that actually has devices must have exactly one default
  for (auto& [type, count] : device_count) CHECK(default_count[type] == 1);
}

template <typename BackendT>
void common_set_volume() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  auto* device = find_device(manager, DeviceType::Output);
  REQUIRE(device != nullptr);

  if (!backend->is_supported(BackendFeature::SetDeviceVolume)) {
    CHECK(device->set_volume(0.5f).status == CommandStatus::Unsupported);
    return;
  }

  float original  = device->volume();
  float alternate = original > 0.5f ? original - 0.1f : original + 0.1f;

  CHECK(device->set_volume(alternate).status == CommandStatus::Ok);
  manager.refresh();
  CHECK(device->volume() == doctest::Approx(alternate).epsilon(0.02f));

  CHECK(device->set_volume(original).status == CommandStatus::Ok);
  manager.refresh();
  CHECK(device->volume() == doctest::Approx(original).epsilon(0.02f));
}

template <typename BackendT>
void common_set_mute() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  auto* device = find_device(manager, DeviceType::Output);
  REQUIRE(device != nullptr);

  if (!backend->is_supported(BackendFeature::SetDeviceMute)) {
    CHECK(device->set_mute(true).status == CommandStatus::Unsupported);
    return;
  }

  bool original = device->muted();

  CHECK(device->set_mute(!original).status == CommandStatus::Ok);
  manager.refresh();
  CHECK(device->muted() == !original);

  CHECK(device->set_mute(original).status == CommandStatus::Ok);
  manager.refresh();
  CHECK(device->muted() == original);
}

void test_set_default_respecting_device_types(AudioDeviceManager& manager, DeviceType type, DeviceType other_type) {
  auto get_devices_of_type = [&](DeviceType t) {
    std::vector<Device*> result;
    for (auto& device : manager.get_devices())
      if (device.get().type() == t) result.push_back(&device.get());
    return result;
  };

  auto all_devices_with_target_type = get_devices_of_type(type);
  if (all_devices_with_target_type.empty()) return;  // no devices of this type, nothing to test

  Device* previous_default_device = nullptr;
  for (auto* device : all_devices_with_target_type)
    if (device->is_default()) previous_default_device = device;

  // try to find a different device than the current default, but of same device type
  Device* target = previous_default_device;
  for (auto* device : all_devices_with_target_type)
    if (device != previous_default_device) {
      target = device;
      break;
    }

  // find default device of other type, if it exists, and remember its id for comparison after set_default test
  auto other_type_devices        = get_devices_of_type(other_type);
  Device* other_previous_default = nullptr;
  for (auto* device : other_type_devices)
    if (device->is_default()) other_previous_default = device;
  std::string other_previous_default_id = other_previous_default ? other_previous_default->id().backend_device_id : "";

  // run set_default test on target device
  CHECK(target->set_default().status == CommandStatus::Ok);
  manager.refresh();

  CHECK(target->is_default());  // target device is now default

  if (previous_default_device != nullptr && previous_default_device != target) {
    CHECK(!previous_default_device->is_default());  // previous default device is no longer default
  }

  for (auto* device : all_devices_with_target_type) {
    if (device != target) CHECK(!device->is_default());  // all other devices of same type are not default
  }

  if (other_previous_default != nullptr) {  // if no device of other type exists, nothing to check
    Device* other_after = nullptr;
    for (auto* device : get_devices_of_type(other_type))
      if (device->id().backend_device_id == other_previous_default_id) other_after = device;
    REQUIRE(other_after != nullptr);   // device must not have vanished from a mere default change
    CHECK(other_after->is_default());  // same device of other type is still the default
  }

  // restore whatever was default before this test touched it
  if (previous_default_device != nullptr && previous_default_device != target) {
    previous_default_device->set_default();
    manager.refresh();
  }
}

template <typename BackendT>
void common_set_default() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  if (!backend->is_supported(BackendFeature::SetDefaultDevice)) {
    auto devices = manager.get_devices();
    REQUIRE(!devices.empty());
    CHECK(devices.front().get().set_default().status == CommandStatus::Unsupported);
    return;
  }

  SUBCASE("SUBCASE: set default output device") { test_set_default_respecting_device_types(manager, DeviceType::Output, DeviceType::Input); }
  SUBCASE("SUBCASE: set default input device") { test_set_default_respecting_device_types(manager, DeviceType::Input, DeviceType::Output); }
}

template <typename BackendT>
void common_refresh_preserves_ids() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  auto before = manager.get_devices();
  REQUIRE(before.size() > 0);
  std::vector<std::string> ids_before;
  for (auto& device : before) ids_before.push_back(device.get().id().backend_device_id);

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

template <typename BackendT>
void common_refresh_async_coalesces() {
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  auto f1 = manager.refresh_async();
  auto f2 = manager.refresh_async();
  auto f3 = manager.refresh_async();

  CHECK(f1.get().status == CommandStatus::Ok);
  CHECK(f2.get().status == CommandStatus::Ok);
  CHECK(f3.get().status == CommandStatus::Ok);
}

template <typename BackendT>
void common_set_volume_async_callback() {
  // TODO: could get stuck if the callback never gets called
  AudioDeviceManager manager;
  AudioBackend* backend = nullptr;
  if (!setup<BackendT>(manager, backend)) return;

  auto* device = find_device(manager, DeviceType::Output);
  REQUIRE(device != nullptr);
  const std::string& device_id = device->id().backend_device_id;

  if (!backend->is_supported(BackendFeature::SetDeviceVolume)) {
    CHECK(backend->request_set_volume(device_id, 0.5f).get().status == CommandStatus::Unsupported);
    return;
  }

  float original = device->volume();
  float target   = original > 0.5f ? original - 0.1f : original + 0.1f;

  std::promise<CommandResult> callback_promise;
  auto callback_future = callback_promise.get_future();

  auto future = backend->request_set_volume(device_id, target, [&callback_promise](CommandResult r) { callback_promise.set_value(r); });

  CommandResult future_result   = future.get();
  CommandResult callback_result = callback_future.get();  // fails via future_error if on_done was never called

  CHECK(future_result.status == CommandStatus::Ok);
  CHECK(callback_result.status == CommandStatus::Ok);

  manager.refresh();
  CHECK(device->volume() == doctest::Approx(target).epsilon(0.02f));

  backend->request_set_volume(device_id, original).get();  // restore
}

}  // namespace

// X-macro listing every backend to run the common suite against. BackendLabel
// is a bare token (stringified for the test name), BackendType the concrete
// AudioBackend subclass to instantiate.
#if defined(__linux__)
#define ADM_FOR_EACH_BACKEND(M, ...)                      \
  M(Mock, audio_device_manager::MockBackend, __VA_ARGS__) \
  M(ALSA, audio_device_manager::AlsaBackend, __VA_ARGS__) \
  M(PulseAudio, audio_device_manager::PulseAudioBackend, __VA_ARGS__)
#elif defined(_WIN32)
#define ADM_FOR_EACH_BACKEND(M, ...)                      \
  M(Mock, audio_device_manager::MockBackend, __VA_ARGS__) \
  M(WASAPI, audio_device_manager::WasapiBackend, __VA_ARGS__)
#endif

// expands to one TEST_CASE calling FuncName<BackendType>(), named
// "common: <TestNameStr> [<BackendLabel>]"
#define ADM_TEST_CASE_ONE(BackendLabel, BackendType, TestNameStr, FuncName) \
  TEST_CASE("common: " TestNameStr " [" #BackendLabel "]") { FuncName<BackendType>(); }

// generates one TEST_CASE per backend for a single common test
#define ADM_COMMON_TEST(TestNameStr, FuncName) ADM_FOR_EACH_BACKEND(ADM_TEST_CASE_ONE, TestNameStr, FuncName)

ADM_COMMON_TEST("device list non-empty with valid fields", common_device_list_valid)
ADM_COMMON_TEST("volume within min/max range", common_volume_in_range)
ADM_COMMON_TEST("exactly one default device per type", common_exactly_one_default_per_type)
ADM_COMMON_TEST("set_volume changes value and restores", common_set_volume)
ADM_COMMON_TEST("set_mute toggles and restores", common_set_mute)
ADM_COMMON_TEST("set_default per device type", common_set_default)
ADM_COMMON_TEST("set_volume_async on_done callback fires", common_set_volume_async_callback)
ADM_COMMON_TEST("refresh preserves device ids", common_refresh_preserves_ids)
ADM_COMMON_TEST("concurrent refresh_async calls coalesce", common_refresh_async_coalesces)

#undef ADM_COMMON_TEST
#undef ADM_TEST_CASE_ONE
#undef ADM_FOR_EACH_BACKEND
