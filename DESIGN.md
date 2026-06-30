# AudioDeviceManager (ADM) Design

## 1. Architecture overview

```
                  ┌────────────────────────────────────────────┐
                  │             AudioDeviceManager             │
                  │                                            │
   app thread --> │  get_devices() --> [Device&, Device&, ...] │
   (any thread)   │  subscribe(cb) --> Subscription (RAII)     │
                  │                                            │
                  │  devices_: unordered_map<DeviceId, Device> │
                  │            (node-stable, never erased)     │
                  │  subscribers_: list of callbacks           │
                  └───────────────┬────────────────────────────┘
                                  │ on_backend_event(snapshots)
                                  │ (diff + merge, under lock)
            ┌─────────────────────┼─────────────────────┐
            │                     │                     │
    ┌───────▼──────┐      ┌───────▼──────┐      ┌───────▼──────┐
    │ PulseAudio   │      │ ALSA         │      │ WASAPI       │
    │ Backend      │      │ Backend      │      │ Backend      │
    │ (own thread: │      │ (AsyncWorker │      │ (AsyncWorker │
    │ pa mainloop) │      │  thread)     │      │  + COM apt.) │
    └──────────────┘      └──────────────┘      └──────────────┘
```

Key idea:
**`Device` objects forward commands back to the backend that created them.**
`AudioDeviceManager` only routes snapshot updates *in* and owns the storage;
it does not sit in the call path for `set_volume()` etc.

```cpp
device.set_volume(0.7f);
//   └─► Device::set_volume_async() ─► backend_.set_volume_async(id_, 0.7f)
//        (backend serializes the call on its own worker thread/mainloop)
```

## 2. Module layout

```
AudioDeviceManager/
├── CMakeLists.txt
├── DESIGN.md
├── examples/
│   ├── alsa_test.cpp
│   ├── pulse_list_devices.cpp
│   └── pulse_test.cpp
├── include/
│   ├── ADM                            # short master include
│   └── audio_device_manager/
│       ├── types.hpp                  # DeviceType, AudioBackendType, DeviceId,
│       │                              # DeviceIdHash, DeviceSnapshot,
│       │                              # CommandStatus, CommandResult
│       ├── async.hpp                  # AsyncWorker
│       ├── device.hpp                 # persistent Device model
│       ├── audio_device_manager.hpp   # AudioDeviceManager class, bootstrap
|       |                              # backends, get() instance
│       ├── audio_backend.hpp          # abstract backend interface
│       └── backends/
│           ├── dynamic_library.hpp    # helper for dynamic library loading
│           ├── pulseaudio_backend.hpp # PulseAudio backend implementation
│           ├── alsa_backend.hpp       # ALSA backend implementation
│           ├── wasapi_backend.hpp     # WASAPI backend implementation
│           └── mock_backend.hpp       # mock backend for testing
└── tests/
    ├── test_device_diffing.cpp
    └── test_subscriptions.cpp
```

### `<ADM>`

`include/ADM` is a plain extensionless file so that
`#include <ADM>`
is equivalent to
`#include "audio_device_manager/audio_device_manager.hpp"`.

```cpp
// include/ADM
#pragma once
#include "audio_device_manager/audio_device_manager.hpp"
```

## 3. Core types

### 3.1 `DeviceId`

The same physical device can be reported by two backends (PulseAudio + ALSA both seeing the same sink).
They are **not** the same `Device` instance, each backend gets its own entry, identified by
`(backend_type, backend_local_id)`:

```cpp
// include/audio_device_manager/types.hpp
enum class DeviceType      { Input, Output };
enum class AudioBackendType { PulseAudio, Alsa, Wasapi };

struct DeviceId {
  AudioBackendType backend_type;
  std::string backend_device_id;   // stable id as reported by that backend

  bool operator==(const DeviceId&) const = default;
};

struct DeviceIdHash {
  size_t operator()(const DeviceId& id) const noexcept {
    size_t h1 = std::hash<std::string>{}(id.backend_device_id);
    size_t h2 = std::hash<int>{}(static_cast<int>(id.backend_type));
    return h1 ^ (h2 << 1);
  }
};
```

### 3.2 `DeviceSnapshot`

This is the transient payload a backend hands to `AudioDeviceManager` on every refresh;
`AudioDeviceManager` diffs it against the persistent `Device` it already owns.

```cpp
// include/audio_device_manager/types.hpp
struct DeviceSnapshot {
  std::string backend_device_id;
  std::string name;
  DeviceType  type;
  bool        muted;
  bool        is_default;
  float       volume;
  float       min_level;
  float       max_level;
};
```

### 3.3 `CommandResult`

The result of a command, whether it succeeded or failed.
Can be checked with `if (!result) { /* error handling */ }`.

```cpp
// include/audio_device_manager/types.hpp
enum class CommandStatus {
  Ok,
  DeviceNotFound,
  BackendError,
  Unsupported,
  Timeout,
};

struct CommandResult {
  CommandStatus status = CommandStatus::Ok;
  std::string detail;   // backend-specific message; empty on success

  explicit operator bool() const { return status == CommandStatus::Ok; }
};
```

Every backend is responsible for translating its native error reporting into this.
PulseAudio's `success_cb(int success)` -> `Ok`/`BackendError`;
ALSA's negative `errno`-style return codes -> `BackendError` with `detail = snd_strerror(rc)`;
WASAPI's `HRESULT` -> `BackendError` with a formatted hex code in `detail`, `Unsupported` for `E_NOTIMPL`-style results.
The point is callers never see PulseAudio-vs-ALSA-vs-WASAPI differences, just one small enum.

### 3.4 `Device` model

```cpp
// include/audio_device_manager/device.hpp
class AudioBackend; // fwd decl

class Device {
 public:
  Device(AudioBackend& backend, DeviceId id, std::string name, DeviceType type,
         float min_level, float max_level);

  // Device is non-copyable and non-movable, only create references
  Device(const Device&)            = delete;
  Device& operator=(const Device&) = delete;

  const DeviceId&    id()   const { return this->id_; }
  const std::string& name() const { return this->name_; }
  DeviceType         type() const { return this->type_; }
  const std::string& backend_name() const { return this->backend_.name(); }

  bool  connected()  const { return this->connected_.load(std::memory_order_acquire); }
  bool  muted()       const { return this->muted_.load(std::memory_order_acquire); }
  bool  is_default() const { return this->is_default_.load(std::memory_order_acquire); }
  float volume()      const { return this->volume_.load(std::memory_order_acquire); }
  float min_level()   const { return this->min_level_.load(std::memory_order_acquire); }
  float max_level()   const { return this->max_level_.load(std::memory_order_acquire); }

  // command API: forwards to the owning backend
  // Asynchronous execution by default handled by a promise-backed std::future
  // on the backend's worker thread. If a callback is provided, it is invoked
  // on the backend's worker thread once the command completes.
  // The return value is a promise-backed std::future, which can be discarded
  // or checked for completion with the blocking .get().
  CommandResultFuture set_volume_async(float volume, std::function<void(CommandResult)> on_done = nullptr);
  CommandResultFuture set_mute_async(bool muted, std::function<void(CommandResult)> on_done = nullptr);
  CommandResultFuture set_default_async(std::function<void(CommandResult)> on_done = nullptr);

  // synchronous API wrapper variants
  CommandResult set_volume(float volume) { return set_volume_async(volume).get(); }
  CommandResult set_mute(bool muted)     { return set_mute_async(muted).get(); }
  CommandResult set_default()            { return set_default_async().get(); }

 private:
  friend class AudioDeviceManager;                 // only AudioDeviceManager merges snapshots in
  bool apply_snapshot(const DeviceSnapshot& snap); // returns true if changed
  bool mark_disconnected();                        // returns true if changed

  DeviceId    id_;
  std::string name_;
  DeviceType  type_;
  AudioBackend& backend_;

  std::atomic<bool>  connected_  {true};
  std::atomic<bool>  muted_      {false};
  std::atomic<bool>  is_default_ {false};
  std::atomic<float> volume_     {0.f};
  std::atomic<float> min_level_  {0.f};
  std::atomic<float> max_level_  {1.f};
};
```

`set_volume_async` just forwards to the backend it was created from:

```cpp
CommandResultFuture Device::set_volume_async(float volume, std::function<void(CommandResult)> on_done) {
  return backend_.set_volume_async(id_.backend_device_id, volume, std::move(on_done));
}
```

## 4. `AudioBackend` interface

```cpp
// include/audio_device_manager/audio_backend.hpp
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  virtual std::string       name() const = 0;
  virtual AudioBackendType  type() const = 0;
  virtual bool              available() const = 0;

  // Async command API. Implementations must serialize access to the
  // underlying SDK themselves (own thread / AsyncWorker) so these are
  // safe to call concurrently from any caller thread. `on_done`, if
  // given, fires on that serializing thread once the result is known
  // independent of whether the caller kept the returned future.
  virtual CommandResultFuture set_volume_async(
    const std::string& device_id, float volume,
    std::function<void(CommandResult)> on_done = nullptr) = 0;
  virtual CommandResultFuture set_mute_async(
    const std::string& device_id, bool muted,
    std::function<void(CommandResult)> on_done = nullptr) = 0;
  virtual CommandResultFuture set_default_async(
    const std::string& device_id,
    std::function<void(CommandResult)> on_done = nullptr) = 0;

  using ChangeCallback = std::function<void(std::vector<DeviceSnapshot>)>;
  virtual void subscribe(ChangeCallback on_change) = 0;
  virtual void unsubscribe() = 0;
};
```

### 4.1 How `AudioBackend::available()` gets decided:

`AudioBackend::available()` uses a `DynamicLibrary` helper loader to check availability, load and resolve the different audio API libraries.
There are some build-time dependencies to load the types from the headers, but there are no runtime dependencies.
Before loading or using a library, it is checked if it's available on the user-machine, so if the library is present and reachable.

```cpp
// include/audio_device_manager/backends/dynamic_library.hpp
#pragma once
#if defined(__linux__) || defined(__unix__)
#include <dlfcn.h>

namespace audio_device_manager {

class DynamicLibrary {
 public:
  explicit DynamicLibrary(const char* soname) : handle_(dlopen(soname, RTLD_LAZY | RTLD_GLOBAL)) {}
  ~DynamicLibrary() { if (this->handle_) dlclose(this->handle_); }
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

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
```

Each Linux backend's constructor does the presence check, resolves every symbol it actually calls (typed via `decltype` off the real vendor header).
The original `pa_context_new(...)` functions are never called, only the resolved ones, e.g. `this->context_new_(...)`.
Then it probes whether the service is actually *running*:

```cpp
// include/audio_device_manager/backends/pulseaudio_backend.hpp
#if defined(__linux__)
#include <pulse/pulseaudio.h>   // types/decltype source only, not linked at build time
#include "dynamic_library.hpp"

namespace audio_device_manager {

class PulseAudioBackend : public AudioBackend {
 public:
  PulseAudioBackend() { this->try_init(); }
  bool available() const override { return this->available_; }
  // ...

 private:
  void try_init() {
    if (!this->lib_.loaded()) { this->available_ = false; return; }

    this->context_new_     = this->lib_.resolve<decltype(&::pa_context_new)>("pa_context_new");
    this->context_connect_ = this->lib_.resolve<decltype(&::pa_context_connect)>("pa_context_connect");
    // ... every pa_* symbol this backend actually calls ...
    if (!this->context_new_ || !this->context_connect_ /* || ... */) { this->available_ = false; return; }

    this->available_ = this->probe_server_running();  // connect + wait (bounded) for PA_CONTEXT_READY
  }

  DynamicLibrary lib_{"libpulse.so.0"};
  bool available_ = false;
  decltype(&::pa_context_new)     this->context_new_     = nullptr;
  decltype(&::pa_context_connect) this->context_connect_ = nullptr;
  // ... every pa_* symbol this backend actually calls ...
};

}  // namespace audio_device_manager
#endif
```

Any backend that is not available is not added to registered backends:

```cpp
void AudioDeviceManager::register_backend(std::unique_ptr<AudioBackend> backend) {
  if (!backend->available()) return;   // never added
  backend->subscribe(...);  // subscribe to change events
  backends_.push_back(std::move(backend));
}
```

| Backend    | Presence check                             | "Running" check                                                               |
| ---------- | ------------------------------------------ | ----------------------------------------------------------------------------- |
| ALSA       | `dlopen("libasound.so.2")`                 | `snd_pcm_open("default", ...)` succeeding *is* the check, no separate daemon  |
| PulseAudio | `dlopen("libpulse.so.0")`                  | connect a context, bounded wait for `PA_CONTEXT_READY` vs `PA_CONTEXT_FAILED` |
| WASAPI     | n/a, COM ships with Windows, link normally | `CoCreateInstance(MMDeviceEnumerator)` failing *is* the unavailable signal    |

## 5. `AudioDeviceManager`

### 5.1 Persistant device references

The `AudioDeviceManager` is the central component, that holds a stable map of `Device` objects and a list of available audio backends.
It owns all detected device objects and updates their state based on backend events.
It provides devices only as references, which are stay valid forever, even when disconnected (unless you call `prune_disconnected()`).

```cpp
// include/audio_device_manager/audio_device_manager.hpp
class AudioDeviceManager {
 public:
  AudioDeviceManager(std::chrono::milliseconds notify_rate_limit = 500)
    : notify_rate_limit_(notify_rate_limit), scheduler_thread_([this] { this->scheduler_run(); }) {}
  ~AudioDeviceManager() {
    {
      std::lock_guard lock(this->scheduler_mutex_);
      this->scheduler_stop_ = true;
    }
    this->scheduler_cv_.notify_all();
    this->scheduler_thread_.join();
  }

  void register_backend(std::unique_ptr<AudioBackend> backend);

  std::vector<std::reference_wrapper<Device>> get_devices();
  std::vector<std::reference_wrapper<Device>> get_devices(DeviceType type);
  std::reference_wrapper<Device> get_device(DeviceId id);

  // Erases every currently-disconnected device. Returns the count
  // removed. Should be used with caution, as it breaks reference validity
  // for exactly the devices it removes.
  std::size_t prune_disconnected();

  class Subscription {
    public:
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    ~Subscription();  // destructor auto-unsubscribes
   private:
    friend class AudioDeviceManager;
    Subscription(AudioDeviceManager& manager, std::size_t id) : manager_(&manager), id_(id) {}
    AudioDeviceManager* manager_;
    std::size_t         id_;
  };
  [[nodiscard]] Subscription subscribe(std::function<void()> on_change);

 private:
  void on_backend_event(AudioBackend& backend, std::vector<DeviceSnapshot> snapshots);
  void schedule_notify();
  void scheduler_run();
  void notify_subscribers();
  void unsubscribe(std::size_t id);

  std::vector<std::unique_ptr<AudioBackend>> backends_;

  std::shared_mutex devices_mutex_;
  std::unordered_map<DeviceId, Device, DeviceIdHash> devices_;

  std::mutex subscribers_mutex_;
  std::unordered_map<std::size_t, std::function<void()>> subscribers_;  // subscription_id -> callback
  std::atomic<std::size_t> next_subscription_id_{0};

  // --- callback scheduler ---
  std::mutex                            scheduler_mutex_;
  std::condition_variable               scheduler_cv_;
  bool                                  scheduler_pending_ = false;
  bool                                  scheduler_stop_    = false;
  std::chrono::steady_clock::time_point last_notify_{};
  std::chrono::milliseconds             notify_rate_limit_{500};
  std::thread                           scheduler_thread_;
};
```

### 5.2 Handle backend device update events

```cpp
void AudioDeviceManager::on_backend_event(AudioBackend& backend,
                                          std::vector<DeviceSnapshot> snapshots) {
  bool any_change = false;
  {
    std::unique_lock lock(this->devices_mutex_);

    std::unordered_set<DeviceId, DeviceIdHash> seen;
    for (auto& snap : snapshots) {
      DeviceId key{backend.type(), snap.backend_device_id};
      seen.insert(key);

      auto it = this->devices_.find(key);
      if (it == this->devices_.end()) {
        auto [new_it, inserted] = this->devices_.emplace(
            key, Device(backend, key, snap.name, snap.type,
                        snap.min_level, snap.max_level));
        it = new_it;
        any_change = true;
      }
      any_change |= it->second.apply_snapshot(snap);  // also sets device.connected_=true
    }

    // anything this backend previously reported but didn't this time must be disconnected
    for (auto& [key, device] : this->devices_) {
      if (key.backend_type != backend.type() || seen.count(key)) continue;
      any_change |= device.mark_disconnected();
    }
  }  // lock released before notifying

  if (any_change) this->schedule_notify();
}
```

### 5.3 `get_devices()`

Return list of all at that point detected devices by reference. This is a cached list, aggregated from all the individual backend lists. All mutable values of the devices are atomics and can be safely read concurrently.

```cpp
std::vector<std::reference_wrapper<Device>> AudioDeviceManager::get_devices() {
  std::shared_lock lock(devices_mutex_);
  std::vector<std::reference_wrapper<Device>> result;
  result.reserve(this->devices_.size());
  for (auto& [key, device] : this->devices_) result.push_back(device);
  return result;
}
```

### 5.4 Subscriptions and subscription callback scheduler

```cpp
AudioDeviceManager::Subscription AudioDeviceManager::subscribe(std::function<void()> on_change) {
  auto id = this->next_subscription_id_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(this->subscribers_mutex_);
  this->subscribers_.emplace(id, std::move(on_change));
  return this->Subscription(*this, id);
}

void AudioDeviceManager::schedule_notify() {
  std::lock_guard lock(this->scheduler_mutex_);
  if (this->scheduler_pending_) return;   // already pending, nothing to do
  this->scheduler_pending_ = true;
  this->scheduler_cv_.notify_one();
}

void AudioDeviceManager::scheduler_run() {
  std::unique_lock lock(this->scheduler_mutex_);
  while (true) {
    this->scheduler_cv_.wait(lock, [this] {
      return this->scheduler_stop_ || this->scheduler_pending_;
    });
    if (this->scheduler_stop_) return;

    auto now     = std::chrono::steady_clock::now();
    auto elapsed = now - this->last_notify_;

    if (elapsed >= this->notify_rate_limit_) {
      this->scheduler_pending_ = false;
      this->last_notify_       = now;
      lock.unlock();
      this->notify_subscribers();
      lock.lock();
    } else {
      // window not elapsed yet
      auto remaining = this->notify_rate_limit_ - elapsed;
      this->scheduler_cv_.wait_for(lock, remaining, [this] { return this->scheduler_stop_; });
    }
  }
}

void AudioDeviceManager::notify_subscribers() {
  std::vector<std::function<void()>> copy;
  {
    std::lock_guard lock(this->subscribers_mutex_);
    copy.reserve(this->subscribers_.size());
    for (auto& [id, callback] : this->subscribers_) copy.push_back(callback);
  }
  for (auto& callback : copy) callback();
}
```

Callbacks fire on scheduler's dedicated thread.
The backend thread calling the on_backend_event callback only ever flips a bool and returns immediately.
The callback scheduler coalesces bursts, e.g. 10 backend events in 500 ms result in 2 callback rounds, not 10.
The first callback round immediately, after the first event, then the scheduler waits for the 500 ms rate limit to elapse, then it fires the second callback round which provides the notification update after backend events 2-10.
A non-burst, single event does only trigger one single callback round.

### 5.5 `prune_disconnected()`

```cpp
std::size_t AudioDeviceManager::prune_disconnected() {
  std::unique_lock lock(this->devices_mutex_);
  std::size_t removed = 0;
  for (auto it = this->devices_.begin(); it != this->devices_.end(); ) {
    if (!it->second.connected()) { it = this->devices_.erase(it); ++removed; }
    else ++it;
  }
  return removed;
}
```
Erasing a node *does* invalidate any reference/pointer still pointing at that specific `Device`.
Up to this point nothing in the design ever erases, which is precisely what let `get_devices()` hand out forever-valid references.
Normally, there should only be a limited number of devices that can accumulate, so there is no need to clean up disconnected devices and worry about invalid references.
If this method is called, the caller has to make sure that any old references to now disconnected/pruned devices are no longer used / are discarded.

### 5.6 Loading Backends + global accessor

Generally, all potentially available backends are included and attempted to be registered.
Only backends that are unavailable on an OS are not included.
This can either be done manually by calling `register_audio_backends()` or `AudioDeviceManager::register_backend()` for custom backends (e.g. the `MockBackend`).
Or it will automatically done by requesting the singleton `AudioDeviceManager` instance with `audio_device_manager::get()`.

```cpp
// include/audio_device_manager/audio_device_manager.hpp

#if defined(__linux__)
  #include "backends/alsa_backend.hpp"
  #include "backends/pulseaudio_backend.hpp"
#elif defined(_WIN32)
  #include "backends/wasapi_backend.hpp"
#endif

namespace audio_device_manager {

inline void register_audio_backends(AudioDeviceManager& manager) {
#if defined(__linux__)
  manager.register_backend(std::make_unique<AlsaBackend>());
  manager.register_backend(std::make_unique<PulseAudioBackend>());
#elif defined(_WIN32)
  manager.register_backend(std::make_unique<WasapiBackend>());
#endif
}

inline AudioDeviceManager& get() {
  static AudioDeviceManager instance;
  // value `initialized` is irrelevant, but needed for the one-time
  // initialization
  static bool initialized = (register_audio_backends(instance), true);
  return instance;
}

}  // namespace audio_device_manager
```

Example usage to get a reference to the `AudioDeviceManager` singleton:

```cpp
#include <ADM>

AudioDeviceManager& manager = audio_device_manager::get();
manager.get_devices();
manager.subscribe([] { refresh_ui(); });

// or
audio_device_manager::get().get_devices();  // same instance
```

## 6. Async / sync API

### 6.2 API call patterns

There are four possible API call patterns which are the combinations of the answers to these two questions:
- Should the call be blocking? async or sync?
- Is the caller interested in the result of the call?

All variations are based on the same underlying async call that is capable of a callback, but also returns a `std::future<CommandResult>` to the caller.
The callback is optional, and so is checking the result of the call.
For the synchronous call, there are synchronous API wrapper variants, which call the async API and block until the result is available.

Examples:

```cpp
// 1. async, no result observed, non-blocking
device.set_volume_async(0.6f);

// 2.a. async + check with callback, non-blocking
device.set_volume_async(0.6f, [](CommandResult result) {
  if (!result) log_error(r.detail);
});

// 2.b. async dispatch, blocking check later
CommandResultFuture pending = device.set_volume_async(0.6f);
// ...do other work...
auto result = pending.get();  // blocking
if (!result) log_error(r.detail);

// 3. sync, blocking call, check returned result or (4.) ignore it
auto result = device.set_volume(0.6f);  // blocking
if (!result) log_error(r.detail);
```

### 6.3 `AsyncWorker`

```cpp
// include/audio_device_manager/async.hpp
class AsyncWorker {
 public:
  AsyncWorker() : thread_([this] { this->run(); }) {}
  ~AsyncWorker() {
    { std::lock_guard lock(this->mutex_); this->stop_ = true; }
    this->cv_.notify_all();
    this->thread_.join();
  }
  void post(std::function<void()> task) {
    { std::lock_guard lock(this->mutex_); this->queue_.push(std::move(task)); }
    this->cv_.notify_one();
  }
 private:
  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lock(this->mutex_);
        this->cv_.wait(lock, [this] { return this->stop_ || !this->queue_.empty(); });
        if (this->stop_ && this->queue_.empty()) return;
        task = std::move(this->queue_.front());
        this->queue_.pop();
      }
      task();
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> queue_;
  std::thread thread_;
  bool stop_ = false;
};
```

PulseAudio already serializes via `pa_threaded_mainloop_lock`/its own mainloop thread, so `PulseAudioBackend` may not need `AsyncWorker` at all.
Wrap the existing lock/unlock pattern in a `Promise<CommandResult>`, translating each `success_cb(int success)` into `CommandResult{Ok}` or `CommandResult{BackendError, "..."}`.
ALSA's API is blocking/synchronous by nature, so `AlsaBackend` almost certainly wants an `AsyncWorker` to avoid blocking caller threads.
WASAPI needs COM apartment considerations (`IMMDeviceEnumerator`/`IAudioEndpointVolume` calls should happen on a consistently-initialized COM thread).
`AsyncWorker` with COM init in `run()` is a natural fit there too.

## 7. Threading model summary

| What                               | Guarded by                                           | Notes                                                                                               |
| ---------------------------------- | ---------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| `AudioDeviceManager::devices_`     | `shared_mutex`                                       | writers = backend events (rare), readers = `get_devices()` (frequent)                               |
| Individual `Device` fields         | `std::atomic<...>`                                   | lock-free reads from any thread, no map lock needed to read a single device                         |
| `AudioDeviceManager::subscribers_` | `mutex`                                              | independent from device lock; callbacks copied out and invoked unlocked                             |
| Backend-internal SDK calls         | backend's own thread (mainloop or `AsyncWorker`)     | never called directly from arbitrary threads                                                        |
| Multiple backends                  | run fully independently, no shared lock between them | each owns its own thread(s); `AudioDeviceManager` is the only shared state, and it's lock-protected |

## 8. CMake (header-only interface library)

```cmake
cmake_minimum_required(VERSION 3.20)
project(AudioDeviceManager CXX)

add_library(AudioDeviceManager INTERFACE)
add_library(ADM::ADM ALIAS AudioDeviceManager)   # so consumers can link ADM::ADM
target_include_directories(AudioDeviceManager INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(AudioDeviceManager INTERFACE cxx_std_23)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # no linking against audio APIs, only optionally widening the include search path
  # for -dev headers in case they live in a non-standard prefix
  find_package(PkgConfig)
  if(PkgConfig_FOUND)
    pkg_check_modules(PULSEAUDIO libpulse)
    pkg_check_modules(ALSA alsa)
    if(PULSEAUDIO_FOUND)
      target_include_directories(AudioDeviceManager INTERFACE ${PULSEAUDIO_INCLUDE_DIRS})
    endif()
    if(ALSA_FOUND)
      target_include_directories(AudioDeviceManager INTERFACE ${ALSA_INCLUDE_DIRS})
    endif()
  endif()

  # dlopen/dlsym/dlclose, dynamically linking against audio APIs
  # pthread, async worker thread
  target_link_libraries(AudioDeviceManager INTERFACE ${CMAKE_DL_LIBS} pthread)

elseif(WIN32)
  target_link_libraries(AudioDeviceManager INTERFACE ole32 oleaut32)
endif()

option(ADM_BUILD_TESTS "Build AudioDeviceManager tests" ON)
if(ADM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

## 9. Testing strategy

Instead of using real backends, add a `MockBackend` (`include/audio_device_manager/backends/mock_backend.hpp`) implementing `AudioBackend` with an in-memory device list and a `push_snapshot(...)` test hook to manually fire change events.
This lets the tests exercise `AudioDeviceManager`'s actual logic (merge, disconnect-not-erase, dedup-no-notify-on-noop) without touching PulseAudio
at all.

```cpp
#include <doctest/doctest.h>

#include <ADM>

TEST_CASE("disconnected device keeps its reference valid") {
  AudioDeviceManager manager;
  auto* mock = new MockBackend();
  manager.register_backend(std::unique_ptr<AudioBackend>(mock));

  mock->push_snapshot({{"dev1", "Speakers", DeviceType::Output, false, true, 0.5f, 0.f, 1.f}});
  auto devices = manager.get_devices();
  REQUIRE(devices.size() == 1);
  Device& d = devices[0];

  mock->push_snapshot({}); // device vanished
  CHECK(d.connected() == false);   // same reference, still valid, just flagged
}
```

## 12. Suggested next two prompts

### Prompt 1 — Scaffold & compile

> Using the `design.md` design doc, scaffold the `AudioDeviceManager`
> project per its actual layout (§2): `CMakeLists.txt` (INTERFACE library
> + `ADM::ADM` alias + tests subdir, see §8), the
> top-level `<ADM>` master include, and all headers under
> `include/audio_device_manager/`: `types.hpp` (`DeviceType`, `AudioBackendType`,
> `DeviceId`, `DeviceIdHash`, `DeviceSnapshot`, `CommandStatus`,
> `CommandResult`), `async.hpp` (`AsyncWorker`),
> `device.hpp`, `audio_backend.hpp`, `audio_device_manager.hpp` (the
> `AudioDeviceManager` class incl. `prune_disconnected()`, plus the OS-guarded
> backend `#include`s, `register_audio_backends()`, and
> `audio_device_manager::get()` at the bottom — §5.6). Implement
> `backends/dynamic_library.hpp` for real (§4.1) — it has no SDK
> dependency, so it's fully implementable and testable now. For
> `pulseaudio_backend.hpp`/`alsa_backend.hpp`/`wasapi_backend.hpp`, stub
> just enough to compile and have `available()` correctly return `false`
> via the real `dlopen`-fails-gracefully path (not a hardcoded
> `return false`) — full command/snapshot logic stays for Prompt 2.
> Implement `backends/mock_backend.hpp` fully (tests need it, including
> a way to simulate `CommandResult` failures for command calls). Add
> doctest-based tests in `tests/` covering: device diffing (new device
> appears, disconnect-not-erase, no-op snapshot → no notification),
> `prune_disconnected()` erasing only disconnected entries, subscription
> add/remove/multi-subscriber, `DynamicLibrary` against a real always-present system library
> (e.g. `libc.so.6`) and against a nonexistent one, and all four
> `set_volume_async` call patterns from §6.2 against the mock backend.
> Get it compiling and all tests passing.

### Prompt 2 — PulseAudio implementation

> Implement `backends/pulseaudio_backend.hpp` for real: `dlopen`-based
> symbol resolution per §4.1 (every `pa_*` call site goes through a
> resolved function pointer, never the bare symbol — confirm with `ldd`
> that the resulting binary has no `libpulse.so` `NEEDED` entry), an
> `available()` that's `true` only once presence *and* a bounded
> connect-and-wait-for-`PA_CONTEXT_READY` probe both succeed, and the
> mainloop-thread + watcher-thread pattern from the original
> `PulseAudioBackend.cpp`/`.hpp` adapted to the new interface:
> `set_volume_async`/`set_mute_async`/`set_default_async` return
> `CommandResultFuture`, accept an optional completion callback, and
> translate PulseAudio's `success_cb(int success)` into `CommandResult`
> (failure → `CommandStatus::BackendError` with a useful `detail` string;
> a not-found device before the call even reaches PulseAudio →
> `CommandStatus::DeviceNotFound`). `subscribe()` delivers
> `vector<DeviceSnapshot>` instead of calling a bare `on_change()`. Keep
> `AlsaBackend`/`WasapiBackend` as availability-only stubs, but double
> check `AudioBackend`, `DynamicLibrary`, and `AsyncWorker` are generic
> enough that implementing them later won't require touching the
> interface again. Add a doctest suite that runs against a real
> PulseAudio server if reachable, and is skipped gracefully if not.
