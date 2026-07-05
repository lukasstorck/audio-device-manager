#pragma once

#include <condition_variable>
#include <functional>
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

}  // namespace audio_device_manager
