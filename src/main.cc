#include <boost/coroutine/all.hpp>
#include <iostream>
#include <vector>
#include <thread>
#include "src/task_manager.h"

// TODO: use boost coroutines2
using namespace boost::coroutines;

class TaskRunner;

class AsyncContext {
public:
  std::function<void()> suspend; // called in an owner thread
  std::function<void()> resume; // called in any thread
};

template<typename T> using AsyncFn = std::function<T(const AsyncContext&)>;
using AsyncVoidFn = std::function<void(const AsyncContext&)>;

template<typename T> using AsyncResultHandler = std::function<void(const T&)>;

// it is not thread safe, but it used only in an event loop
// TODO: consider promise
template<typename T>
class TaskCompletionSource {

private:
  bool _has_result = false;
  std::vector<AsyncResultHandler<T>> _result_handlers;
  T _result;
public:
  void on_result(AsyncResultHandler<T> handler) {
    if (_has_result) {
      handler(_result);
    } else {
      _result_handlers.push_back(std::move(handler));
    }
  }

  void set_result(const T res) {
    assert(!_has_result);
    _has_result = true;
    _result = res;
    for (const auto& handler : _result_handlers) {
      handler(_result);
    }
    _result_handlers.clear();
  }

  bool try_get_result(T& out) const {
    if (_has_result) {
      out = _result;
      return true;
    } else {
      return false;
    }
  }
};

// TODO: consider future for VoidTask and Task<T>
class VoidTask {
  using ResultHandler = std::function<void()>;

private:
  std::function<void(ResultHandler)> _add_on_result_handler;

public:
  VoidTask(std::function<void(ResultHandler)> add_on_result_handler) {
    _add_on_result_handler = std::move(add_on_result_handler);
  }

  void on_result(const ResultHandler& handler) const {
    _add_on_result_handler(handler);
  };
};


template<typename T>
class Task {
private:
  std::shared_ptr<TaskCompletionSource<T>> _tcs;
public:
  Task(std::shared_ptr<TaskCompletionSource<T>> tcs) {
    _tcs = std::move(tcs);
  }

  operator VoidTask() {
    using ResultHandler = std::function<void()>;
    auto tcs_copy = _tcs;
    return VoidTask(
      [tcs_copy](
          const ResultHandler& handler
      ) {
        tcs_copy->on_result([handler](const T& result) {
          handler();
        });
      }
    );
  }

  void on_result(const AsyncResultHandler<T>& handler) const {
    _tcs->on_result(handler);
  }

  bool try_get_result(T& out) const {
    return _tcs->try_get_result(out);
  }

  T unsafe_get_result() const {
    T out;
    assert(try_get_result(out));
    return out;
  }
};

class TaskRunner {
private:
  int _coroutineIdCounter = 0;
  std::unordered_map<int, std::unique_ptr<coroutine<void>::pull_type>> _coroutines;
  base::ThreadTaskManager* _task_manager;
public:
  TaskRunner(base::ThreadTaskManager* task_manager) {
    _task_manager = task_manager;
  }

  template<typename T>
  Task<T> run(
      const AsyncFn<T>& task_fn
  ) {
    std::shared_ptr<TaskCompletionSource<T>> tcs = std::make_shared<TaskCompletionSource<T>>();
    _task_manager->AddTask([this, tcs, task_fn]() {
      auto coroutine_id = _coroutineIdCounter++;
      auto source = std::make_unique<coroutine<void>::pull_type>(
        [this, tcs, task_fn, coroutine_id](coroutine<void>::push_type& sink) {
          auto suspend = [&sink] { sink(); };
          auto resume = [this, coroutine_id] {
            _task_manager->AddTask([this, coroutine_id] {
              // TODO: enhance performance
              auto coroutine_it = _coroutines.find(coroutine_id);
              auto coroutine = (*coroutine_it).second.get();
              if (*coroutine) {
                try {
                  (*coroutine)();
                }
                catch (...) {
                  _coroutines.erase(coroutine_it);
                  // ignores all exceptions
                }
              } else {
                _coroutines.erase(coroutine_it);
              }
            });
          };
          const auto& context = AsyncContext { suspend, resume };

          const T& res = task_fn(context);
          tcs->set_result(res);
        });
      _coroutines[coroutine_id] = std::move(source);
    });
    return Task(tcs);
  }

  class Unit { };

  VoidTask runVoid(
      const AsyncVoidFn& task_fn
  ) {
    return run<Unit>(
        [task_fn](const AsyncContext& context) {
          task_fn(context);
          return Unit();
        }
    );
  }
};


void async_helper(
    const AsyncContext& context,
    const std::function<void(std::function<void()>)>& callback
) {
  bool needs_in_suspending = true;
  bool is_suspended = false;

  callback([&context, &needs_in_suspending, &is_suspended] {
    needs_in_suspending = false;
    if (is_suspended) {
      context.resume();
    }
  });

  if (needs_in_suspending) {
    is_suspended = true;
    context.suspend();
  }
}

// TODO: atomic is needed if tasks continuation are run in different threads

template <typename VoidTaskIterator>
void wait_all(const AsyncContext& context, const VoidTaskIterator begin, const VoidTaskIterator end) {
  int total_count = end - begin;
  int completed_count = 0;

  async_helper(context, [begin, end, total_count, &completed_count](auto resume) {
    for (auto it = begin; it != end; ++it) {
      const VoidTask& task = *it;
      task.on_result([resume, total_count, &completed_count] {
        if (++completed_count == total_count) {
          resume();
        }
      });
    }
  });
}

template <typename VoidTaskIterator>
int wait_any(const AsyncContext& context, const VoidTaskIterator begin, const VoidTaskIterator end) {
  int task_idx = -1;
  async_helper(context, [begin, end, &task_idx](auto resume) {
    for (auto it = begin; it != end; ++it) {
      const VoidTask& task = *it;
      task.on_result([resume, begin, it, &task_idx]() {
        if (task_idx == -1) {
          task_idx = it - begin;
          resume();
        }
      });
    }
  });
  return task_idx;
}

void wait_all(const AsyncContext& context, std::initializer_list<VoidTask> tasks) {
  wait_all(context, tasks.begin(), tasks.end());
}

int wait_any(const AsyncContext& context, std::initializer_list<VoidTask> tasks) {
  return wait_any(context, tasks.begin(), tasks.end());
}


// just a prototype
void launch_in_thread(const AsyncContext& context, std::function<void()> fn) {
  async_helper(context, [&fn](auto resume) {
    new std::thread([&fn, resume] {
      fn();
      resume();
    });
  });
}

/*
// just a prototype
void delay(int time, std::function<void()> callback) {
  new std::thread([time, callback]() {
    sleep(time);
    callback();
  });
}

void delay(const AsyncContext& context, int time) {
  async_helper(context, [time](auto resume) {
    delay(time, resume);
  });
}
*/

// just a prototype
void delay(const AsyncContext& context, int time) {
  launch_in_thread(context, [time] {
    sleep(time);
  });
}


std::string f1(const AsyncContext& context) {
  std::cout << "f1_in" << '\n';
  delay(context, 1);
  std::cout << "f1_out" << '\n';
  return "123";
}

int f2(const AsyncContext& context, TaskRunner& task_runner) {
  int state = 1;
  std::cout << "f2_in" << '\n';
  std::cout << "f2_state " << state << '\n';

  auto task1 = task_runner.run<std::string>(f1);
  auto task2 = task_runner.run<std::string>(f1);
  wait_all(context, {task1, task2});
  std::cout << "f1_res_1 " << task1.unsafe_get_result() << ", f1_res_2" << task2.unsafe_get_result() << '\n';

  /*
  auto task1 = run<std::string>(context, f1);
  auto task2 = run<std::string>(context, f1);
  std::cout << "f1 first completed task index " << wait_any(context, { task1, task2 }) << "\n";
  */

  state++;
  std::cout << "f2_state " << state << '\n';
  std::cout << "f2_out" << '\n';
  return state;
}


int main() {
  base::ThreadTaskManager task_manager;
  task_manager.Start();
  TaskRunner task_runner(&task_manager);
  task_runner.run<int>(
      [&task_runner](const AsyncContext& context) { return f2(context, task_runner); }
  ).on_result([](const int& res) {
    std::cout << "f2 ret: " << res << '\n';
  });
  sleep(4);
}
