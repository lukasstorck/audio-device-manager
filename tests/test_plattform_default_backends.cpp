#include <doctest/doctest.h>

#include <ADM>

TEST_CASE("default backends: exactly one backend registered") {
  audio_device_manager::AudioDeviceManager manager;
  audio_device_manager::register_audio_backends(manager);
  CHECK(manager.backend_count() == 1);
}
