#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>


#include <ADM>
#include <audio_device_manager/backends/mock_backend.hpp>

TEST_CASE("mock backend registers and is counted as available") {
  audio_device_manager::AudioDeviceManager manager;
  CHECK(manager.backend_count() == 0);

  manager.register_backend(std::make_unique<audio_device_manager::MockBackend>());

  CHECK(manager.backend_count() == 1);

  // check names of registered backends
  auto backend = audio_device_manager::MockBackend();
  CHECK(backend.name() == "Mock");
}
