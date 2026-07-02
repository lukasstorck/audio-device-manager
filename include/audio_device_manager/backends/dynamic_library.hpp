#pragma once

#if defined(__linux__) || defined(__unix__)
#include <dlfcn.h>

namespace audio_device_manager {

class DynamicLibrary {
 public:
  explicit DynamicLibrary(const char* soname) : handle_(dlopen(soname, RTLD_LAZY | RTLD_GLOBAL)) {}
  ~DynamicLibrary() {
    if (this->handle_) dlclose(this->handle_);
  }

  DynamicLibrary(const DynamicLibrary&)            = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&&)                 = delete;
  DynamicLibrary& operator=(DynamicLibrary&&)      = delete;

  bool loaded() const { return this->handle_ != nullptr; }

  template <typename FnPtr>
  FnPtr resolve(const char* symbol) const {
    return this->handle_ ? reinterpret_cast<FnPtr>(dlsym(this->handle_, symbol)) : nullptr;
  }

 private:
  void* handle_ = nullptr;
};

}  // namespace audio_device_manager
#endif
