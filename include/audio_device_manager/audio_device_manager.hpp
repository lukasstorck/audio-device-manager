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
    {
      std::lock_guard lock(this->scheduler_mutex_);
      this->scheduler_stop_ = true;
    }
    this->scheduler_cv_.notify_all();
    this->scheduler_thread_.join();
  }

  void register_backend(std::unique_ptr<AudioBackend> backend) {
    if (!backend->available()) return;

    // check for duplicate backend type
    for (const auto& existing : this->backends_) {
      if (existing->type() == backend->type()) {
        throw std::runtime_error("AudioDeviceManager::register_backend(): backend type already registered");
      }
    }

    backend->subscribe([this, ptr = backend.get()](std::vector<DeviceSnapshot> snapshots) { this->on_backend_event(*ptr, std::move(snapshots)); });
    this->backends_.push_back(std::move(backend));
  }

  // Count of registered backends, diagnostic helper
  std::size_t backend_count() const { return this->backends_.size(); }

  // Returns a list of all currently or previously connected devices
  std::vector<std::reference_wrapper<Device>> get_devices() {
    std::shared_lock lock(this->devices_mutex_);
    std::vector<std::reference_wrapper<Device>> result;
    result.reserve(this->devices_.size());
    for (auto& [key, device] : this->devices_) result.push_back(device);
    return result;
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

 private:
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

  // --- callback scheduler ---
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
  // manager.register_backend(std::make_unique<AlsaBackend>());
  // manager.register_backend(std::make_unique<PulseAudioBackend>());
#elif defined(_WIN32)
  // manager.register_backend(std::make_unique<WasapiBackend>());
#endif
}

inline AudioDeviceManager& get() {
  static AudioDeviceManager instance;
  // value `initialized` is irrelevant, but needed for the one-time initialization
  static bool initialized = (register_audio_backends(instance), true);
  return instance;
}

}  // namespace audio_device_manager
