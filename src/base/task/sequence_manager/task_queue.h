// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TASK_SEQUENCE_MANAGER_TASK_QUEUE_H_
#define BASE_TASK_SEQUENCE_MANAGER_TASK_QUEUE_H_

#include <memory>

#include "base/base_export.h"
#include "base/check.h"
#include "base/memory/weak_ptr.h"
#include "base/task/common/checked_lock.h"
#include "base/task/common/lazy_now.h"
#include "base/task/sequence_manager/tasks.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_observer.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/trace_event/base_tracing_forward.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace perfetto {
class EventContext;
}

namespace base {

class TaskObserver;

namespace sequence_manager {

namespace internal {
class AssociatedThreadId;
class SequenceManagerImpl;
class TaskQueueImpl;
}  // namespace internal

// TODO(kraynov): Make TaskQueue to actually be an interface for TaskQueueImpl
// and stop using ref-counting because we're no longer tied to task runner
// lifecycle and there's no other need for ref-counting either.
// NOTE: When TaskQueue gets automatically deleted on zero ref-count,
// TaskQueueImpl gets gracefully shutdown. It means that it doesn't get
// unregistered immediately and might accept some last minute tasks until
// SequenceManager will unregister it at some point. It's done to ensure that
// task queue always gets unregistered on the main thread.
class BASE_EXPORT TaskQueue : public RefCountedThreadSafe<TaskQueue> {
 public:
  // Interface that lets a task queue be throttled by changing the wake up time
  // and optionally, by inserting fences. A wake up in this context is a
  // notification at a given time that lets this TaskQueue know of newly ripe
  // delayed tasks if it's enabled. By delaying the desired wake up time to a
  // different allowed wake up time, the Throttler can hold off delayed tasks
  // that would otherwise by allowed to run sooner.
  class BASE_EXPORT Throttler {
   public:
    // Invoked when the TaskQueue's next allowed wake up time is reached and is
    // enabled, even if blocked by a fence. That wake up is defined by the last
    // value returned from GetNextAllowedWakeUp().
    // This is always called on the thread this TaskQueue is associated with.
    virtual void OnWakeUp(LazyNow* lazy_now) = 0;

    // Invoked when the TaskQueue newly gets a pending immediate task and is
    // enabled, even if blocked by a fence. Redundant calls are possible when
    // the TaskQueue already had a pending immediate task.
    // The implementation may use this to:
    // - Restrict task execution by inserting/updating a fence.
    // - Update the TaskQueue's next delayed wake up via UpdateWakeUp().
    //   This allows the Throttler to perform additional operations later from
    //   OnWakeUp().
    // This is always called on the thread this TaskQueue is associated with.
    virtual void OnHasImmediateTask() = 0;

    // Invoked when the TaskQueue is enabled and wants to know when to schedule
    // the next delayed wake-up (which happens at least every time this queue is
    // about to cause the next wake up) provided |next_desired_wake_up|, the
    // wake-up for the next pending delayed task in this queue (pending delayed
    // tasks that are ripe may be ignored), or nullopt if there's no pending
    // delayed task. |has_ready_task| indicates whether there are immediate
    // tasks or ripe delayed tasks. The implementation should return the next
    // allowed wake up, or nullopt if no future wake-up is necessary.
    // This is always called on the thread this TaskQueue is associated with.
    virtual absl::optional<WakeUp> GetNextAllowedWakeUp(
        LazyNow* lazy_now,
        absl::optional<WakeUp> next_desired_wake_up,
        bool has_ready_task) = 0;

   protected:
    ~Throttler() = default;
  };

  // Shuts down the queue. All tasks currently queued will be discarded.
  virtual void ShutdownTaskQueue();

  // Shuts down the queue when there are no more tasks queued.
  void ShutdownTaskQueueGracefully();

  // Queues with higher priority are selected to run before queues of lower
  // priority. Note that there is no starvation protection, i.e., a constant
  // stream of high priority work can mean that tasks in lower priority queues
  // won't get to run.
  // TODO(scheduler-dev): Could we define a more clear list of priorities?
  // See https://crbug.com/847858.
  enum QueuePriority : uint8_t {
    // Queues with control priority will run before any other queue, and will
    // explicitly starve other queues. Typically this should only be used for
    // private queues which perform control operations.
    kControlPriority = 0,

    kHighestPriority = 1,
    kVeryHighPriority = 2,
    kHighPriority = 3,
    kNormalPriority = 4,  // Queues with normal priority are the default.
    kLowPriority = 5,

    // Queues with best effort priority will only be run if all other queues are
    // empty.
    kBestEffortPriority = 6,

    // Must be the last entry.
    kQueuePriorityCount = 7,
    kFirstQueuePriority = kControlPriority,
  };

  // Can be called on any thread.
  static const char* PriorityToString(QueuePriority priority);

  // Options for constructing a TaskQueue.
  struct Spec {
    explicit Spec(const char* name) : name(name) {}

    Spec SetShouldMonitorQuiescence(bool should_monitor) {
      should_monitor_quiescence = should_monitor;
      return *this;
    }

    Spec SetShouldNotifyObservers(bool run_observers) {
      should_notify_observers = run_observers;
      return *this;
    }

    // Delayed fences require Now() to be sampled when posting immediate tasks
    // which is not free.
    Spec SetDelayedFencesAllowed(bool allow_delayed_fences) {
      delayed_fence_allowed = allow_delayed_fences;
      return *this;
    }

    Spec SetNonWaking(bool non_waking_in) {
      non_waking = non_waking_in;
      return *this;
    }

    const char* name;
    bool should_monitor_quiescence = false;
    bool should_notify_observers = true;
    bool delayed_fence_allowed = false;
    bool non_waking = false;
  };

  // TODO(altimin): Make this private after TaskQueue/TaskQueueImpl refactoring.
  TaskQueue(std::unique_ptr<internal::TaskQueueImpl> impl,
            const TaskQueue::Spec& spec);
  TaskQueue(const TaskQueue&) = delete;
  TaskQueue& operator=(const TaskQueue&) = delete;

  // Information about task execution.
  //
  // Wall-time related methods (start_time, end_time, wall_duration) can be
  // called only when |has_wall_time()| is true.
  // Thread-time related mehtods (start_thread_time, end_thread_time,
  // thread_duration) can be called only when |has_thread_time()| is true.
  //
  // start_* should be called after RecordTaskStart.
  // end_* and *_duration should be called after RecordTaskEnd.
  class BASE_EXPORT TaskTiming {
   public:
    enum class State { NotStarted, Running, Finished };
    enum class TimeRecordingPolicy { DoRecord, DoNotRecord };

    TaskTiming(bool has_wall_time, bool has_thread_time);

    bool has_wall_time() const { return has_wall_time_; }
    bool has_thread_time() const { return has_thread_time_; }

    base::TimeTicks start_time() const {
      DCHECK(has_wall_time());
      return start_time_;
    }
    base::TimeTicks end_time() const {
      DCHECK(has_wall_time());
      return end_time_;
    }
    base::TimeDelta wall_duration() const {
      DCHECK(has_wall_time());
      return end_time_ - start_time_;
    }
    base::ThreadTicks start_thread_time() const {
      DCHECK(has_thread_time());
      return start_thread_time_;
    }
    base::ThreadTicks end_thread_time() const {
      DCHECK(has_thread_time());
      return end_thread_time_;
    }
    base::TimeDelta thread_duration() const {
      DCHECK(has_thread_time());
      return end_thread_time_ - start_thread_time_;
    }

    State state() const { return state_; }

    void RecordTaskStart(LazyNow* now);
    void RecordTaskEnd(LazyNow* now);

    // Protected for tests.
   protected:
    State state_ = State::NotStarted;

    bool has_wall_time_;
    bool has_thread_time_;

    base::TimeTicks start_time_;
    base::TimeTicks end_time_;
    base::ThreadTicks start_thread_time_;
    base::ThreadTicks end_thread_time_;
  };

  // An interface that lets the owner vote on whether or not the associated
  // TaskQueue should be enabled.
  class BASE_EXPORT QueueEnabledVoter {
   public:
    ~QueueEnabledVoter();

    QueueEnabledVoter(const QueueEnabledVoter&) = delete;
    const QueueEnabledVoter& operator=(const QueueEnabledVoter&) = delete;

    // Votes to enable or disable the associated TaskQueue. The TaskQueue will
    // only be enabled if all the voters agree it should be enabled, or if there
    // are no voters.
    // NOTE this must be called on the thread the associated TaskQueue was
    // created on.
    void SetVoteToEnable(bool enabled);

    bool IsVotingToEnable() const { return enabled_; }

   private:
    friend class TaskQueue;
    explicit QueueEnabledVoter(scoped_refptr<TaskQueue> task_queue);

    scoped_refptr<TaskQueue> const task_queue_;
    bool enabled_;
  };

  // Returns an interface that allows the caller to vote on whether or not this
  // TaskQueue is enabled. The TaskQueue will be enabled if there are no voters
  // or if all agree it should be enabled.
  // NOTE this must be called on the thread this TaskQueue was created by.
  std::unique_ptr<QueueEnabledVoter> CreateQueueEnabledVoter();

  // NOTE this must be called on the thread this TaskQueue was created by.
  bool IsQueueEnabled() const;

  // Returns true if the queue is completely empty.
  bool IsEmpty() const;

  // Returns the number of pending tasks in the queue.
  size_t GetNumberOfPendingTasks() const;

  // Returns true iff this queue has immediate tasks or delayed tasks that are
  // ripe for execution. Ignores the queue's enabled state and fences.
  // NOTE: this must be called on the thread this TaskQueue was created by.
  // TODO(etiennep): Rename to HasReadyTask() and add LazyNow parameter.
  bool HasTaskToRunImmediatelyOrReadyDelayedTask() const;

  // Returns a wake-up for the next pending delayed task (pending delayed tasks
  // that are ripe may be ignored), ignoring Throttler is any. If there are no
  // such tasks (immediate tasks don't count) or the queue is disabled it
  // returns nullopt.
  // NOTE: this must be called on the thread this TaskQueue was created by.
  absl::optional<WakeUp> GetNextDesiredWakeUp();

  // Can be called on any thread.
  virtual const char* GetName() const;

  // Serialise this object into a trace.
  void WriteIntoTrace(perfetto::TracedValue context) const;

  // Set the priority of the queue to |priority|. NOTE this must be called on
  // the thread this TaskQueue was created by.
  void SetQueuePriority(QueuePriority priority);

  // Returns the current queue priority.
  QueuePriority GetQueuePriority() const;

  // These functions can only be called on the same thread that the task queue
  // manager executes its tasks on.
  void AddTaskObserver(TaskObserver* task_observer);
  void RemoveTaskObserver(TaskObserver* task_observer);

  enum class InsertFencePosition {
    kNow,  // Tasks posted on the queue up till this point further may run.
           // All further tasks are blocked.
    kBeginningOfTime,  // No tasks posted on this queue may run.
  };

  // Inserts a barrier into the task queue which prevents tasks with an enqueue
  // order greater than the fence from running until either the fence has been
  // removed or a subsequent fence has unblocked some tasks within the queue.
  // Note: delayed tasks get their enqueue order set once their delay has
  // expired, and non-delayed tasks get their enqueue order set when posted.
  //
  // Fences come in three flavours:
  // - Regular (InsertFence(NOW)) - all tasks posted after this moment
  //   are blocked.
  // - Fully blocking (InsertFence(kBeginningOfTime)) - all tasks including
  //   already posted are blocked.
  // - Delayed (InsertFenceAt(timestamp)) - blocks all tasks posted after given
  //   point in time (must be in the future).
  //
  // Only one fence can be scheduled at a time. Inserting a new fence
  // will automatically remove the previous one, regardless of fence type.
  void InsertFence(InsertFencePosition position);

  // Delayed fences are only allowed for queues created with
  // SetDelayedFencesAllowed(true) because this feature implies sampling Now()
  // (which isn't free) for every PostTask, even those with zero delay.
  void InsertFenceAt(TimeTicks time);

  // Removes any previously added fence and unblocks execution of any tasks
  // blocked by it.
  void RemoveFence();

  // Returns true if the queue has a fence but it isn't necessarily blocking
  // execution of tasks (it may be the case if tasks enqueue order hasn't
  // reached the number set for a fence).
  bool HasActiveFence();

  // Returns true if the queue has a fence which is blocking execution of tasks.
  bool BlockedByFence() const;

  // Associates |throttler| to this queue. Only one throttler can be associated
  // with this queue. |throttler| must outlive this TaskQueue, or remain valid
  // until ResetThrottler().
  void SetThrottler(Throttler* throttler);
  // Disassociates the current throttler from this queue, if any.
  void ResetThrottler();

  // Updates the task queue's next wake up time in its time domain, taking into
  // account the desired run time of queued tasks and policies enforced by the
  // throttler if any.
  void UpdateWakeUp(LazyNow* lazy_now);

  // Controls whether or not the queue will emit traces events when tasks are
  // posted to it while disabled. This only applies for the current or next
  // period during which the queue is disabled. When the queue is re-enabled
  // this will revert back to the default value of false.
  void SetShouldReportPostedTasksWhenDisabled(bool should_report);

  // Create a task runner for this TaskQueue which will annotate all
  // posted tasks with the given task type.
  // May be called on any thread.
  // NOTE: Task runners don't hold a reference to a TaskQueue, hence,
  // it's required to retain that reference to prevent automatic graceful
  // shutdown. Unique ownership of task queues will fix this issue soon.
  scoped_refptr<SingleThreadTaskRunner> CreateTaskRunner(TaskType task_type);

  // Default task runner which doesn't annotate tasks with a task type.
  const scoped_refptr<SingleThreadTaskRunner>& task_runner() const {
    return default_task_runner_;
  }

  // Checks whether or not this TaskQueue has a TaskQueueImpl.
  // TODO(crbug.com/1143007): Remove this method when TaskQueueImpl inherits
  // from TaskQueue and TaskQueue no longer owns an Impl.
  bool HasImpl() { return !!impl_; }

  using OnTaskStartedHandler =
      RepeatingCallback<void(const Task&, const TaskQueue::TaskTiming&)>;
  using OnTaskCompletedHandler =
      RepeatingCallback<void(const Task&, TaskQueue::TaskTiming*, LazyNow*)>;
  using OnTaskPostedHandler = RepeatingCallback<void(const Task&)>;
  using TaskExecutionTraceLogger =
      RepeatingCallback<void(perfetto::EventContext&, const Task&)>;

  // Sets a handler to subscribe for notifications about started and completed
  // tasks.
  void SetOnTaskStartedHandler(OnTaskStartedHandler handler);

  // |task_timing| may be passed in Running state and may not have the end time,
  // so that the handler can run an additional task that is counted as a part of
  // the main task.
  // The handler can call TaskTiming::RecordTaskEnd, which is optional, to
  // finalize the task, and use the resulting timing.
  void SetOnTaskCompletedHandler(OnTaskCompletedHandler handler);

  // RAII handle associated with an OnTaskPostedHandler. Unregisters the handler
  // upon destruction.
  class OnTaskPostedCallbackHandle {
   public:
    OnTaskPostedCallbackHandle(const OnTaskPostedCallbackHandle&) = delete;
    OnTaskPostedCallbackHandle& operator=(const OnTaskPostedCallbackHandle&) =
        delete;
    virtual ~OnTaskPostedCallbackHandle() = default;

   protected:
    OnTaskPostedCallbackHandle() = default;
  };

  // Add a callback for adding custom functionality for processing posted task.
  // Callback will be dispatched while holding a scheduler lock. As a result,
  // callback should not call scheduler APIs directly, as this can lead to
  // deadlocks. For example, PostTask should not be called directly and
  // ScopedDeferTaskPosting::PostOrDefer should be used instead. `handler` must
  // not be a null callback. Must be called on the thread this task queue is
  // associated with, and the handle returned must be destroyed on the same
  // thread.
  [[nodiscard]] std::unique_ptr<OnTaskPostedCallbackHandle>
  AddOnTaskPostedHandler(OnTaskPostedHandler handler);

  // Set a callback to fill trace event arguments associated with the task
  // execution.
  void SetTaskExecutionTraceLogger(TaskExecutionTraceLogger logger);

  base::WeakPtr<TaskQueue> AsWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

 protected:
  virtual ~TaskQueue();

  internal::TaskQueueImpl* GetTaskQueueImpl() const { return impl_.get(); }

 private:
  friend class RefCountedThreadSafe<TaskQueue>;
  friend class internal::SequenceManagerImpl;
  friend class internal::TaskQueueImpl;

  void AddQueueEnabledVoter(bool voter_is_enabled);
  void RemoveQueueEnabledVoter(bool voter_is_enabled);
  bool AreAllQueueEnabledVotersEnabled() const;
  void OnQueueEnabledVoteChanged(bool enabled);

  bool IsOnMainThread() const;

  // TaskQueue has ownership of an underlying implementation but in certain
  // cases (e.g. detached frames) their lifetime may diverge.
  // This method should be used to take away the impl for graceful shutdown.
  // TaskQueue will disregard any calls or posting tasks thereafter.
  std::unique_ptr<internal::TaskQueueImpl> TakeTaskQueueImpl();

  // |impl_| can be written to on the main thread but can be read from
  // any thread.
  // |impl_lock_| must be acquired when writing to |impl_| or when accessing
  // it from non-main thread. Reading from the main thread does not require
  // a lock.
  mutable base::internal::CheckedLock impl_lock_{
      base::internal::UniversalPredecessor{}};
  std::unique_ptr<internal::TaskQueueImpl> impl_;

  const WeakPtr<internal::SequenceManagerImpl> sequence_manager_;

  const scoped_refptr<const internal::AssociatedThreadId> associated_thread_;
  const scoped_refptr<SingleThreadTaskRunner> default_task_runner_;

  int enabled_voter_count_ = 0;
  int voter_count_ = 0;
  const char* name_;

  base::WeakPtrFactory<TaskQueue> weak_ptr_factory_{this};
};

}  // namespace sequence_manager
}  // namespace base

#endif  // BASE_TASK_SEQUENCE_MANAGER_TASK_QUEUE_H_
