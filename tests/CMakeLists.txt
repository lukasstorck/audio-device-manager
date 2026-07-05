# tests/CMakeLists.txt
include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.12
)
FetchContent_MakeAvailable(doctest)

add_executable(adm_tests
  test_mock_backend.cpp   # must be first due to doctest main initialization
  test_plattform_default_backends.cpp
  test_multithreading.cpp
)
target_link_libraries(adm_tests PRIVATE ADM::ADM doctest::doctest)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(adm_tests)
