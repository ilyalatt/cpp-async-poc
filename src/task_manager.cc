#include "src/task_manager.h"

namespace base {

void ThreadTaskManager::Start() {
  std::unique_lock<std::mutex> lock(start_stop_mutex_);
  if (!IsStopped()) {
    return;
  }
  is_stopped_ = false;
  poll_thread_ = std::make_unique<std::thread>(&ThreadTaskManager::OnPoll, this);
}

void ThreadTaskManager::OnPoll() {
  while (!IsStopped()) {
    std::unique_ptr<Callable> next_task;
    {
      std::unique_lock<std::mutex> lock(task_queue_mutex_);
      task_waiter_.wait(lock, [&]() { return !task_queue_.empty() || IsStopped(); });
      if (IsStopped())
        break;
      if (!task_queue_.empty()) {
        next_task = std::make_unique<Callable>(std::move(task_queue_.front()));
        task_queue_.pop();
      }
    }
    if (next_task) {
      (*next_task)();
    }
  }
}

bool ThreadTaskManager::AddTask(Callable task) {
  if (IsStopped())
    return false;
  std::unique_lock<std::mutex> lock(task_queue_mutex_);
  task_queue_.push(std::move(task));
  task_waiter_.notify_one();
  return true;
}

void ThreadTaskManager::Stop() {
  std::unique_lock<std::mutex> lock(start_stop_mutex_);
  if (IsStopped()) {
    return;
  }
  is_stopped_ = true;
  task_waiter_.notify_one();
  if (poll_thread_->joinable()) {
    poll_thread_->join();
  }
  poll_thread_.reset(nullptr);
}

bool ThreadTaskManager::IsStopped() {
  return is_stopped_;
}

ThreadTaskManager::~ThreadTaskManager() {
  Stop();
}

} // base
