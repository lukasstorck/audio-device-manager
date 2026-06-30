#pragma once

#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>

namespace audio_device_manager {

class AsyncWorker {
 public:
  AsyncWorker() : thread_([this] { this->run(); }) {}
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
