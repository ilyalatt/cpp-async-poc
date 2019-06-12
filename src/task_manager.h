#pragma once

#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <atomic>

namespace base {

using Callable = std::function<void()>;

class TaskManager {
public:
  TaskManager() = default;

  virtual ~TaskManager() = default;

  virtual void Start() = 0;

  virtual void Stop() = 0;

  virtual bool AddTask(Callable task) = 0;

  virtual bool IsStopped() = 0;
};

class ThreadTaskManager : public TaskManager {
public:
  ThreadTaskManager() = default;

  void Start() override;

  bool AddTask(Callable task) override;

  void Stop() override;

  bool IsStopped() override;

  ~ThreadTaskManager() override;

private:
  void OnPoll();

  std::mutex task_queue_mutex_, start_stop_mutex_;
  std::queue<Callable> task_queue_;
  std::condition_variable task_waiter_;

  std::atomic_bool is_stopped_ = { true };

  std::unique_ptr<std::thread> poll_thread_;
};

} // base
