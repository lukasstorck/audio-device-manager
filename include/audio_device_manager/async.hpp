#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace audio_device_manager {

class AsyncWorker {
 public:
  /// @brief Create and start a worker thread
  /// @param on_thread_start optional callback that runs before the main worker loop
  /// @param on_thread_end optional callback that runs after the main worker loop
  explicit AsyncWorker(std::function<void()> on_thread_start = nullptr, std::function<void()> on_thread_end = nullptr)
      : thread_([this, on_thread_start = std::move(on_thread_start), on_thread_end = std::move(on_thread_end)] {
          if (on_thread_start) on_thread_start();
          this->run();
          if (on_thread_end) on_thread_end();
        }) {}
  ~AsyncWorker() {
    {
      std::lock_guard lock(this->mutex_);
      this->stop_ = true;
    }
    this->cv_.notify_all();
    this->thread_.join();
  }

  void post(std::function<void()> task) {
    {
      std::lock_guard lock(this->mutex_);
      this->queue_.push(std::move(task));
    }
    this->cv_.notify_one();
  }

 private:
  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lock(this->mutex_);
        cv_.wait(lock, [this] { return this->stop_ || !this->queue_.empty(); });
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

/// @brief A background thread that periodically invokes a callback at a fixed
/// interval. Can be started and stopped at will.
class IntervalWorker {
 public:
  IntervalWorker() = default;
  ~IntervalWorker() { this->stop(); }

  IntervalWorker(const IntervalWorker&)            = delete;
  IntervalWorker& operator=(const IntervalWorker&) = delete;
  IntervalWorker(IntervalWorker&&)                 = delete;
  IntervalWorker& operator=(IntervalWorker&&)      = delete;

  /// @brief Start the background thread. Will run callback every `interval_ms`
  /// @param interval_ms interval in milliseconds, must be > 0 to start
  /// @param callback callback to invoke
  void start(int interval_ms, std::function<void()> callback) {
    if (this->thread_.joinable() || interval_ms <= 0) return;

    this->stop_   = false;
    this->thread_ = std::thread([this, interval = std::chrono::milliseconds(interval_ms), callback = std::move(callback)] { this->run(interval, callback); });
  }

  void stop() {
    if (!this->thread_.joinable()) return;
    {
      std::lock_guard lock(this->mutex_);
      this->stop_ = true;
    }
    this->cv_.notify_all();
    this->thread_.join();
  }

 private:
  void run(std::chrono::milliseconds interval, const std::function<void()>& callback) {
    std::unique_lock lock(this->mutex_);
    while (!this->cv_.wait_for(lock, interval, [this] { return this->stop_; })) {
      lock.unlock();
      callback();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace audio_device_manager
