#pragma once

#include <shared_mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "audio_backend.hpp"
#include "device.hpp"

#if defined(__linux__)
#include "backends/alsa_backend.hpp"
#include "backends/pulseaudio_backend.hpp"
#elif defined(_WIN32)
#include "backends/wasapi_backend.hpp"
#endif

namespace audio_device_manager {

class AudioDeviceManager {
 public:
  AudioDeviceManager(std::chrono::milliseconds notify_rate_limit = std::chrono::milliseconds(500))
      : notify_rate_limit_(notify_rate_limit), scheduler_thread_([this] { this->scheduler_run(); }) {}
  ~AudioDeviceManager() {
    for (auto& backend : this->backends_) backend->unsubscribe();
    {
      std::lock_guard lock(this->scheduler_mutex_);
      this->scheduler_stop_ = true;
    }
    this->scheduler_cv_.notify_all();
    this->scheduler_thread_.join();
  }

  // Register a new backend, return success if the backend was successfully registered
  bool register_backend(std::unique_ptr<AudioBackend> backend) {
    if (!backend->available()) return false;

    // check for duplicate backend type
    for (const auto& existing : this->backends_) {
      if (existing->type() == backend->type()) {
        throw std::runtime_error("AudioDeviceManager::register_backend(): backend type already registered");
      }
    }

    backend->subscribe([this, ptr = backend.get()](std::vector<DeviceSnapshot> snapshots) { this->on_backend_event(*ptr, std::move(snapshots)); });
    this->backends_.push_back(std::move(backend));
    return true;
  }

  // Count of registered backends
  std::size_t backend_count() const { return this->backends_.size(); }

  // Returns a list of all currently or previously connected devices
  std::vector<std::reference_wrapper<Device>> get_devices() {
    std::shared_lock lock(this->devices_mutex_);
    std::vector<std::reference_wrapper<Device>> result;
    result.reserve(this->devices_.size());
    for (auto& [key, device] : this->devices_) result.push_back(device);
    return result;
  }

  // Returns sorted list of all currently or previously connected devices
  std::vector<std::reference_wrapper<Device>> get_devices_sorted() {
    auto devices = this->get_devices();

    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
      const auto& device_a = a.get();
      const auto& device_b = b.get();

      const auto& backend_name_a = device_a.backend_name();
      const auto& backend_name_b = device_b.backend_name();
      if (backend_name_a != backend_name_b) return backend_name_a < backend_name_b;

      const auto device_type_a = device_a.type() == audio_device_manager::DeviceType::Input ? 0 : 1;
      const auto device_type_b = device_b.type() == audio_device_manager::DeviceType::Input ? 0 : 1;
      if (device_type_a != device_type_b) return device_type_a < device_type_b;

      const auto device_id_a = device_a.id().backend_device_id;
      const auto device_id_b = device_b.id().backend_device_id;
      return device_id_a < device_id_b;
    });

    return devices;
  }

  // Erases every currently-disconnected device. Returns the count
  // removed. Should be used with caution, as it breaks reference validity
  // for exactly the devices it removes.
  std::size_t prune_disconnected() {
    std::unique_lock lock(this->devices_mutex_);
    std::size_t removed = 0;
    for (auto it = this->devices_.begin(); it != this->devices_.end();) {
      if (!it->second.connected()) {
        it = this->devices_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  /// A RAII handle representing an active subscription.
  ///
  /// Destroying the handle automatically unsubscribes the associated callback.
  /// The handle is movable but not copyable.
  class Subscription {
   public:
    Subscription(Subscription&& other) noexcept : manager_(std::exchange(other.manager_, nullptr)), id_(other.id_) {}

    Subscription& operator=(Subscription&& other) noexcept {
      if (this != &other) {
        if (manager_) manager_->unsubscribe(id_);

        manager_ = std::exchange(other.manager_, nullptr);
        id_      = other.id_;
      }
      return *this;
    }

    ~Subscription() {
      if (manager_) manager_->unsubscribe(id_);
    }

   private:
    friend class audio_device_manager::AudioDeviceManager;
    Subscription(AudioDeviceManager& manager, std::size_t id) : manager_(&manager), id_(id) {}
    AudioDeviceManager* manager_;
    std::size_t id_;
  };

  /// Subscribes a callback that is invoked whenever the audio device state changes.
  ///
  /// Returns a `Subscription` handle that keeps the callback registered.
  /// Destroying (or overwriting via move-assignment) the returned handle
  /// automatically unregisters the callback.
  ///
  /// The returned handle must be kept alive for as long as notifications are desired.
  [[nodiscard]] Subscription subscribe(std::function<void()> on_change) {
    auto id = this->next_subscription_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(this->subscribers_mutex_);
    this->subscribers_.emplace(id, std::move(on_change));
    return Subscription(*this, id);
  }

  /// Unregisters the subscription identified by `id`.
  ///
  /// This is typically called automatically when a `Subscription` is destroyed.
  void unsubscribe(std::size_t id) {
    std::lock_guard lock(this->subscribers_mutex_);
    this->subscribers_.erase(id);
  }

  /// Asynchronously requests all backends to report a freshed devices.
  ///
  /// Returns immediately. The returned future resolves once all backends have
  /// acknowledged completion of the refresh with `acknowledge_backend()` via
  /// `on_done`. Fires on the thread that delivers the last backend acknowledgement.
  CommandResultFuture refresh_async(std::function<void(CommandResult)> on_done = nullptr) {
    bool is_new_pending_refresh = false;
    std::future<CommandResult> future;
    {
      std::lock_guard lock(this->pending_refresh_mutex_);

      // no backends registered -> early exit
      if (this->backends_.empty()) {
        std::promise<CommandResult> promise;
        future = promise.get_future();
        CommandResult result{};
        if (on_done) on_done(result);
        promise.set_value(result);
        return future;
      }

      // prepare refresh (if not already pending)
      if (!this->pending_refresh) {
        this->pending_refresh = std::make_shared<PendingRefresh>();
        for (auto& backend : this->backends_) this->pending_refresh->pending_backends.insert(backend->type());
        is_new_pending_refresh = true;
      }

      // wire up result feature and callback (to new or existing promise)
      this->pending_refresh->promises.emplace_back();
      future = this->pending_refresh->promises.back().get_future();
      this->pending_refresh->callbacks.push_back(std::move(on_done));
    }

    // trigger a pending refresh, unless one is already pending
    if (is_new_pending_refresh) {
      for (auto& backend : this->backends_) {
        AudioBackendType type = backend->type();
        backend->request_refresh([this, type](CommandResult) { this->acknowledge_backend(type); });
      }
    }

    return future;
  }

  /// Synchronously refreshes all backends and blocks until all have reported back.
  CommandResult refresh() { return this->refresh_async().get(); }

 private:
  struct PendingRefresh {
    std::unordered_set<AudioBackendType> pending_backends;
    std::vector<std::promise<CommandResult>> promises;
    std::vector<std::function<void(CommandResult)>> callbacks;

    void complete() {
      CommandResult result{};
      for (auto& callback : this->callbacks) {
        if (callback) callback(result);
      }
      for (auto& promise : this->promises) promise.set_value(result);
    }
  };

  void acknowledge_backend(AudioBackendType backend_type) {
    std::shared_ptr<PendingRefresh> completed;
    {
      std::lock_guard lock(this->pending_refresh_mutex_);
      if (!this->pending_refresh) return;
      this->pending_refresh->pending_backends.erase(backend_type);
      if (!this->pending_refresh->pending_backends.empty()) return;
      completed = std::move(this->pending_refresh);  // clears active slot before calling out
    }
    completed->complete();  // called outside lock: callbacks may themselves call refresh_async
  }

  void on_backend_event(AudioBackend& backend, std::vector<DeviceSnapshot> snapshots) {
    bool any_change = false;
    {
      std::unique_lock lock(this->devices_mutex_);

      std::unordered_set<DeviceId, DeviceIdHash> seen;
      for (auto& snap : snapshots) {
        DeviceId key{backend.type(), snap.backend_device_id};
        seen.insert(key);

        auto it = this->devices_.find(key);
        if (it == this->devices_.end()) {
          auto [new_it, inserted] = this->devices_.emplace(std::piecewise_construct, std::forward_as_tuple(key),
                                                           std::forward_as_tuple(backend, key, snap.name, snap.type, snap.min_level, snap.max_level));
          it                      = new_it;
          any_change              = true;
        }
        any_change |= it->second.apply_snapshot(snap);
      }

      // any devices that this backend reported previously, but didn't this time, must be disconnected
      for (auto& [key, device] : this->devices_) {
        if (key.backend_type != backend.type() || seen.count(key)) continue;
        any_change |= device.mark_disconnected();
      }
    }  // lock released before notifying

    if (any_change) this->schedule_notify();
  }
  void schedule_notify() {
    std::lock_guard lock(this->scheduler_mutex_);
    if (this->scheduler_pending_) return;  // already pending, nothing to do
    this->scheduler_pending_ = true;
    this->scheduler_cv_.notify_one();
  }
  void scheduler_run() {
    std::unique_lock lock(this->scheduler_mutex_);
    while (true) {
      this->scheduler_cv_.wait(lock, [this] { return this->scheduler_stop_ || this->scheduler_pending_; });
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
  void notify_subscribers() {
    std::vector<std::function<void()>> copied_callbacks;
    {
      std::lock_guard lock(this->subscribers_mutex_);
      copied_callbacks.reserve(this->subscribers_.size());
      for (auto& [id, callback] : this->subscribers_) copied_callbacks.push_back(callback);
    }
    for (auto& callback : copied_callbacks) callback();
  }

  std::vector<std::unique_ptr<AudioBackend>> backends_;

  std::shared_mutex devices_mutex_;
  std::unordered_map<DeviceId, Device, DeviceIdHash> devices_;

  // backend update event subscriber map: subscription_id -> callback
  std::mutex subscribers_mutex_;
  std::unordered_map<std::size_t, std::function<void()>> subscribers_;
  std::atomic<std::size_t> next_subscription_id_{0};

  std::mutex pending_refresh_mutex_;
  std::shared_ptr<PendingRefresh> pending_refresh;

  std::mutex scheduler_mutex_;
  std::condition_variable scheduler_cv_;
  bool scheduler_pending_ = false;
  bool scheduler_stop_    = false;
  std::chrono::steady_clock::time_point last_notify_{};
  std::chrono::milliseconds notify_rate_limit_;
  std::thread scheduler_thread_;
};

inline void register_audio_backends(AudioDeviceManager& manager) {
#if defined(__linux__)
  bool success = manager.register_backend(std::make_unique<PulseAudioBackend>());
  if (!success) manager.register_backend(std::make_unique<AlsaBackend>());  // fallback
#elif defined(_WIN32)
  manager.register_backend(std::make_unique<WasapiBackend>());
#endif
}

inline AudioDeviceManager& get() {
  static AudioDeviceManager instance;
  // value `initialized` is irrelevant, but needed for the one-time initialization
  static bool initialized = (register_audio_backends(instance), true);
  return instance;
}

}  // namespace audio_device_manager
