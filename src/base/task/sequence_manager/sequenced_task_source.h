// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TASK_SEQUENCE_MANAGER_SEQUENCED_TASK_SOURCE_H_
#define BASE_TASK_SEQUENCE_MANAGER_SEQUENCED_TASK_SOURCE_H_

#include "base/base_export.h"
#include "base/callback_helpers.h"
#include "base/pending_task.h"
#include "base/task/common/lazy_now.h"
#include "base/task/sequence_manager/task_queue.h"
#include "base/task/sequence_manager/tasks.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace perfetto {
class EventContext;
}

namespace base {
namespace sequence_manager {
namespace internal {

// Interface to pass tasks to ThreadController.
class SequencedTaskSource {
 public:
  enum class SelectTaskOption { kDefault, kSkipDelayedTask };

  using TaskExecutionTraceLogger =
      RepeatingCallback<void(perfetto::EventContext&, const Task&)>;

  struct BASE_EXPORT SelectedTask {
    SelectedTask(const SelectedTask&);
    SelectedTask(Task& task,
                 TaskExecutionTraceLogger task_execution_trace_logger,
                 TaskQueue::QueuePriority priority);
    ~SelectedTask();

    Task& task;
    // Callback to fill trace event arguments associated with the task
    // execution. Can be null
    TaskExecutionTraceLogger task_execution_trace_logger =
        TaskExecutionTraceLogger();
    TaskQueue::QueuePriority priority;
  };

  virtual ~SequencedTaskSource() = default;

  // Returns the next task to run from this source or nullopt if
  // there're no more tasks ready to run. If a task is returned,
  // DidRunTask() must be invoked before the next call to SelectNextTask().
  // |option| allows control on which kind of tasks can be selected.
  virtual absl::optional<SelectedTask> SelectNextTask(
      LazyNow& lazy_now,
      SelectTaskOption option = SelectTaskOption::kDefault) = 0;

  // Notifies this source that the task previously obtained
  // from SelectNextTask() has been completed.
  virtual void DidRunTask(LazyNow& lazy_now) = 0;

  // Removes all canceled delayed tasks from the front of the queue. After
  // calling this, GetPendingWakeUp() is guaranteed to return a ready time for a
  // non-canceled task.
  virtual void RemoveAllCanceledDelayedTasksFromFront(LazyNow* lazy_now) = 0;

  // Returns a WakeUp for the next pending task, is_immediate() if the
  // next task can run immediately, or nullopt if there are no more immediate or
  // delayed tasks. |option| allows control on which kind of tasks can be
  // selected.
  virtual absl::optional<WakeUp> GetPendingWakeUp(
      LazyNow* lazy_now,
      SelectTaskOption option = SelectTaskOption::kDefault) const = 0;

  // Return true if there are any pending tasks in the task source which require
  // high resolution timing.
  virtual bool HasPendingHighResolutionTasks() = 0;

  // Called when we have run out of immediate work.  If more immediate work
  // becomes available as a result of any processing done by this callback,
  // return true to schedule a future DoWork.
  virtual bool OnSystemIdle() = 0;
};

}  // namespace internal
}  // namespace sequence_manager
}  // namespace base

#endif  // BASE_TASK_SEQUENCE_MANAGER_SEQUENCED_TASK_SOURCE_H_
