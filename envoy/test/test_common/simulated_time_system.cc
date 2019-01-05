#include "test/test_common/simulated_time_system.h"

#include <chrono>

#include "envoy/event/dispatcher.h"

#include "common/common/assert.h"
#include "common/common/lock_guard.h"
#include "common/event/real_time_system.h"
#include "common/event/timer_impl.h"

#include "event2/event.h"

namespace Envoy {
namespace Event {

// Our simulated alarm inherits from TimerImpl so that the same dispatching
// mechanism used in RealTimeSystem timers is employed for simulated alarms.
// Note that libevent is placed into thread-safe mode due to the call to
// evthread_use_pthreads() in source/common/event/libevent.cc.
class SimulatedTimeSystem::Alarm : public TimerImpl {
public:
  Alarm(SimulatedTimeSystem& time_system, Libevent::BasePtr& libevent, TimerCb cb)
      : TimerImpl(libevent, [this, cb] { runAlarm(cb); }), time_system_(time_system),
        index_(time_system.nextIndex()), armed_(false) {}

  virtual ~Alarm();

  // Timer
  void disableTimer() override;
  void enableTimer(const std::chrono::milliseconds& duration) override;

  void setTime(MonotonicTime time) { time_ = time; }

  /**
   * Activates the timer so it will be run the next time the libevent loop is run,
   * typically via Dispatcher::run().
   */
  void activate() {
    armed_ = false;
    std::chrono::milliseconds duration = std::chrono::milliseconds::zero();
    time_system_.incPending();
    TimerImpl::enableTimer(duration);
  }

  MonotonicTime time() const {
    ASSERT(armed_);
    return time_;
  }

  uint64_t index() const { return index_; }

private:
  void runAlarm(TimerCb cb) {
    time_system_.decPending();
    cb();
  }

  SimulatedTimeSystem& time_system_;
  MonotonicTime time_;
  uint64_t index_;
  bool armed_;
};

// Compare two alarms, based on wakeup time and insertion order. Returns true if
// a comes before b.
bool SimulatedTimeSystem::CompareAlarms::operator()(const Alarm* a, const Alarm* b) const {
  if (a != b) {
    if (a->time() < b->time()) {
      return true;
    } else if (a->time() == b->time() && a->index() < b->index()) {
      return true;
    }
  }
  return false;
};

// Each timer is maintained and ordered by a common TimeSystem, but is
// associated with a scheduler. The scheduler creates the timers with a libevent
// context, so that the timer callbacks can be executed via Dispatcher::run() in
// the expected thread.
class SimulatedTimeSystem::SimulatedScheduler : public Scheduler {
public:
  SimulatedScheduler(SimulatedTimeSystem& time_system, Libevent::BasePtr& libevent)
      : time_system_(time_system), libevent_(libevent) {}
  TimerPtr createTimer(const TimerCb& cb) override {
    return std::make_unique<SimulatedTimeSystem::Alarm>(time_system_, libevent_, cb);
  };

private:
  SimulatedTimeSystem& time_system_;
  Libevent::BasePtr& libevent_;
};

SimulatedTimeSystem::Alarm::~Alarm() {
  if (armed_) {
    disableTimer();
  }
}

void SimulatedTimeSystem::Alarm::disableTimer() {
  if (armed_) {
    time_system_.removeAlarm(this);
    armed_ = false;
  }
}

void SimulatedTimeSystem::Alarm::enableTimer(const std::chrono::milliseconds& duration) {
  disableTimer();
  armed_ = true;
  if (duration.count() == 0) {
    activate();
  } else {
    time_system_.addAlarm(this, duration);
  }
}

// It would be very confusing if there were more than one simulated time system
// extant at once. Technically this might be something we want, but more likely
// it indicates some kind of plumbing error in test infrastructure. So track
// the instance count with a simple int. In the future if there's a good reason
// to have more than one around at a time, this variable can be deleted.
static int instance_count = 0;

// When we initialize our simulated time, we'll start the current time based on
// the real current time. But thereafter, real-time will not be used, and time
// will march forward only by calling sleep().
SimulatedTimeSystem::SimulatedTimeSystem()
    : monotonic_time_(MonotonicTime(std::chrono::seconds(0))),
      system_time_(real_time_source_.systemTime()), index_(0), pending_alarms_(0) {
  ASSERT(++instance_count <= 1);
}

SimulatedTimeSystem::~SimulatedTimeSystem() { --instance_count; }

SystemTime SimulatedTimeSystem::systemTime() {
  Thread::LockGuard lock(mutex_);
  return system_time_;
}

MonotonicTime SimulatedTimeSystem::monotonicTime() {
  Thread::LockGuard lock(mutex_);
  return monotonic_time_;
}

void SimulatedTimeSystem::sleep(const Duration& duration) {
  mutex_.lock();
  MonotonicTime monotonic_time =
      monotonic_time_ + std::chrono::duration_cast<MonotonicTime::duration>(duration);
  setMonotonicTimeAndUnlock(monotonic_time);
}

Thread::CondVar::WaitStatus SimulatedTimeSystem::waitFor(Thread::MutexBasicLockable& mutex,
                                                         Thread::CondVar& condvar,
                                                         const Duration& duration) noexcept {
  const Duration real_time_poll_delay(std::chrono::milliseconds(50));
  const MonotonicTime end_time = monotonicTime() + duration;

  while (true) {
    // First check to see if the condition is already satisfied without advancing sim time.
    if (condvar.waitFor(mutex, real_time_poll_delay) == Thread::CondVar::WaitStatus::NoTimeout) {
      return Thread::CondVar::WaitStatus::NoTimeout;
    }

    // Wait for the libevent poll in another thread to catch up prior to advancing time.
    if (hasPending()) {
      continue;
    }

    mutex_.lock();
    if (monotonic_time_ < end_time) {
      if (alarms_.empty()) {
        // If no alarms are pending, sleep till the end time.
        setMonotonicTimeAndUnlock(end_time);
      } else {
        // If there's another alarm pending, sleep forward to it.
        MonotonicTime next_wakeup = (*alarms_.begin())->time();
        setMonotonicTimeAndUnlock(std::min(next_wakeup, end_time));
      }
    } else {
      // If we reached our end_time, break the loop and return timeout.
      mutex_.unlock();
      break;
    }
  }
  return Thread::CondVar::WaitStatus::Timeout;
}

int64_t SimulatedTimeSystem::nextIndex() {
  Thread::LockGuard lock(mutex_);
  return index_++;
}

void SimulatedTimeSystem::addAlarm(Alarm* alarm, const std::chrono::milliseconds& duration) {
  Thread::LockGuard lock(mutex_);
  alarm->setTime(monotonic_time_ + duration);
  alarms_.insert(alarm);
}

void SimulatedTimeSystem::removeAlarm(Alarm* alarm) {
  Thread::LockGuard lock(mutex_);
  alarms_.erase(alarm);
}

SchedulerPtr SimulatedTimeSystem::createScheduler(Libevent::BasePtr& libevent) {
  return std::make_unique<SimulatedScheduler>(*this, libevent);
}

void SimulatedTimeSystem::setMonotonicTimeAndUnlock(const MonotonicTime& monotonic_time) {
  // We don't have a convenient LockGuard construct that allows temporarily
  // dropping the lock to run a callback. The main issue here is that we must
  // be careful not to be holding mutex_ when an exception can be thrown.
  // That can only happen here in alarm->activate(), which is run with the mutex
  // released.
  if (monotonic_time >= monotonic_time_) {
    // Alarms is a std::set ordered by wakeup time, so pulling off begin() each
    // iteration gives you wakeup order. Also note that alarms may be added
    // or removed during the call to activate() so it would not be correct to
    // range-iterate over the set.
    while (!alarms_.empty()) {
      AlarmSet::iterator pos = alarms_.begin();
      Alarm* alarm = *pos;
      if (alarm->time() > monotonic_time) {
        break;
      }
      ASSERT(alarm->time() >= monotonic_time_);
      system_time_ +=
          std::chrono::duration_cast<SystemTime::duration>(alarm->time() - monotonic_time_);
      monotonic_time_ = alarm->time();
      alarms_.erase(pos);
      mutex_.unlock();
      // We don't want to activate the alarm under lock, as it will make a libevent call,
      // and libevent itself uses locks:
      // https://github.com/libevent/libevent/blob/29cc8386a2f7911eaa9336692a2c5544d8b4734f/event.c#L1917
      alarm->activate();
      mutex_.lock();
    }
    system_time_ +=
        std::chrono::duration_cast<SystemTime::duration>(monotonic_time - monotonic_time_);
    monotonic_time_ = monotonic_time;
  }
  mutex_.unlock();
}

void SimulatedTimeSystem::setSystemTime(const SystemTime& system_time) {
  mutex_.lock();
  if (system_time > system_time_) {
    MonotonicTime monotonic_time =
        monotonic_time_ +
        std::chrono::duration_cast<MonotonicTime::duration>(system_time - system_time_);
    setMonotonicTimeAndUnlock(monotonic_time);
  } else {
    system_time_ = system_time;
    mutex_.unlock();
  }
}

} // namespace Event
} // namespace Envoy
