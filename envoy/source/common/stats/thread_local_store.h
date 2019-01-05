#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "envoy/thread_local/thread_local.h"

#include "common/stats/heap_stat_data.h"
#include "common/stats/histogram_impl.h"
#include "common/stats/source_impl.h"
#include "common/stats/utility.h"

#include "circllhist.h"

namespace Envoy {
namespace Stats {

/**
 * A histogram that is stored in TLS and used to record values per thread. This holds two
 * histograms, one to collect the values and other as backup that is used for merge process. The
 * swap happens during the merge process.
 */
class ThreadLocalHistogramImpl : public Histogram, public MetricImpl {
public:
  ThreadLocalHistogramImpl(const std::string& name, std::string&& tag_extracted_name,
                           std::vector<Tag>&& tags);
  ~ThreadLocalHistogramImpl();

  void merge(histogram_t* target);

  /**
   * Called in the beginning of merge process. Swaps the histogram used for collection so that we do
   * not have to lock the histogram in high throughput TLS writes.
   */
  void beginMerge() {
    // This switches the current_active_ between 1 and 0.
    ASSERT(std::this_thread::get_id() == created_thread_id_);
    current_active_ = otherHistogramIndex();
  }

  // Stats::Histogram
  void recordValue(uint64_t value) override;
  bool used() const override { return flags_ & Flags::Used; }

  // Stats::Metric
  const std::string name() const override { return name_; }

private:
  uint64_t otherHistogramIndex() const { return 1 - current_active_; }
  uint64_t current_active_;
  histogram_t* histograms_[2];
  std::atomic<uint16_t> flags_;
  std::thread::id created_thread_id_;
  const std::string name_;
};

typedef std::shared_ptr<ThreadLocalHistogramImpl> TlsHistogramSharedPtr;

class TlsScope;

/**
 * Log Linear Histogram implementation that is stored in the main thread.
 */
class ParentHistogramImpl : public ParentHistogram, public MetricImpl {
public:
  ParentHistogramImpl(const std::string& name, Store& parent, TlsScope& tlsScope,
                      std::string&& tag_extracted_name, std::vector<Tag>&& tags);
  ~ParentHistogramImpl();

  void addTlsHistogram(const TlsHistogramSharedPtr& hist_ptr);
  bool used() const override;
  void recordValue(uint64_t value) override;

  /**
   * This method is called during the main stats flush process for each of the histograms. It
   * iterates through the TLS histograms and collects the histogram data of all of them
   * in to "interval_histogram". Then the collected "interval_histogram" is merged to a
   * "cumulative_histogram".
   */
  void merge() override;

  const HistogramStatistics& intervalStatistics() const override { return interval_statistics_; }
  const HistogramStatistics& cumulativeStatistics() const override {
    return cumulative_statistics_;
  }
  const std::string summary() const override;

  // Stats::Metric
  const std::string name() const override { return name_; }

private:
  bool usedLockHeld() const EXCLUSIVE_LOCKS_REQUIRED(merge_lock_);

  Store& parent_;
  TlsScope& tls_scope_;
  histogram_t* interval_histogram_;
  histogram_t* cumulative_histogram_;
  HistogramStatisticsImpl interval_statistics_;
  HistogramStatisticsImpl cumulative_statistics_;
  mutable Thread::MutexBasicLockable merge_lock_;
  std::list<TlsHistogramSharedPtr> tls_histograms_ GUARDED_BY(merge_lock_);
  bool merged_;
  const std::string name_;
};

typedef std::shared_ptr<ParentHistogramImpl> ParentHistogramImplSharedPtr;

/**
 * Class used to create ThreadLocalHistogram in the scope.
 */
class TlsScope : public Scope {
public:
  virtual ~TlsScope() {}

  // TODO(ramaraochavali): Allow direct TLS access for the advanced consumers.
  /**
   * @return a ThreadLocalHistogram within the scope's namespace.
   * @param name name of the histogram with scope prefix attached.
   */
  virtual Histogram& tlsHistogram(const std::string& name, ParentHistogramImpl& parent) PURE;
};

/**
 * Store implementation with thread local caching. This implementation supports the following
 * features:
 * - Thread local per scope stat caching.
 * - Overlapping scopes with proper reference counting (2 scopes with the same name will point to
 *   the same backing stats).
 * - Scope deletion.
 * - Lockless in the fast path.
 *
 * This implementation is complicated so here is a rough overview of the threading model.
 * - The store can be used before threading is initialized. This is needed during server init.
 * - Scopes can be created from any thread, though in practice they are only created from the main
 *   thread.
 * - Scopes can be deleted from any thread, and they are in practice as scopes are likely to be
 *   shared across all worker threads.
 * - Per thread caches are checked, and if empty, they are populated from the central cache.
 * - Scopes are entirely owned by the caller. The store only keeps weak pointers.
 * - When a scope is destroyed, a cache flush operation is run on all threads to flush any cached
 *   data owned by the destroyed scope.
 * - Scopes use a unique incrementing ID for the cache key. This ensures that if a new scope is
 *   created at the same address as a recently deleted scope, cache references will not accidentally
 *   reference the old scope which may be about to be cache flushed.
 * - Since it's possible to have overlapping scopes, we de-dup stats when counters() or gauges() is
 *   called since these are very uncommon operations.
 * - Though this implementation is designed to work with a fixed shared memory space, it will fall
 *   back to heap allocated stats if needed. NOTE: In this case, overlapping scopes will not share
 *   the same backing store. This is to keep things simple, it could be done in the future if
 *   needed.
 *
 * The threading model for managing histograms is as described below.
 * Each Histogram implementation will have 2 parts.
 *  - "main" thread parent which is called "ParentHistogram".
 *  - "per-thread" collector which is called "ThreadLocalHistogram".
 * Worker threads will write to ParentHistogram which checks whether a TLS histogram is available.
 * If there is one it will write to it, otherwise creates new one and writes to it.
 * During the flush process the following sequence is followed.
 *  - The main thread starts the flush process by posting a message to every worker which tells the
 *    worker to swap its "active" histogram with its "backup" histogram. This is achieved via a call
 *    to "beginMerge" method.
 *  - Each TLS histogram has 2 histograms it makes use of, swapping back and forth. It manages a
 *    current_active index via which it writes to the correct histogram.
 *  - When all workers have done, the main thread continues with the flush process where the
 *    "actual" merging happens.
 *  - As the active histograms are swapped in TLS histograms, on the main thread, we can be sure
 *    that no worker is writing into the "backup" histogram.
 *  - The main thread now goes through all histograms, collect them across each worker and
 *    accumulates in to "interval" histograms.
 *  - Finally the main "interval" histogram is merged to "cumulative" histogram.
 */
class ThreadLocalStoreImpl : Logger::Loggable<Logger::Id::stats>, public StoreRoot {
public:
  ThreadLocalStoreImpl(const Stats::StatsOptions& stats_options, StatDataAllocator& alloc);
  ~ThreadLocalStoreImpl();

  // Stats::Scope
  Counter& counter(const std::string& name) override { return default_scope_->counter(name); }
  ScopePtr createScope(const std::string& name) override;
  void deliverHistogramToSinks(const Histogram& histogram, uint64_t value) override {
    return default_scope_->deliverHistogramToSinks(histogram, value);
  }
  Gauge& gauge(const std::string& name) override { return default_scope_->gauge(name); }
  Histogram& histogram(const std::string& name) override {
    return default_scope_->histogram(name);
  };

  // Stats::Store
  std::vector<CounterSharedPtr> counters() const override;
  std::vector<GaugeSharedPtr> gauges() const override;
  std::vector<ParentHistogramSharedPtr> histograms() const override;

  // Stats::StoreRoot
  void addSink(Sink& sink) override { timer_sinks_.push_back(sink); }
  void setTagProducer(TagProducerPtr&& tag_producer) override {
    tag_producer_ = std::move(tag_producer);
  }
  void initializeThreading(Event::Dispatcher& main_thread_dispatcher,
                           ThreadLocal::Instance& tls) override;
  void shutdownThreading() override;

  void mergeHistograms(PostMergeCb mergeCb) override;

  Source& source() override { return source_; }

  const Stats::StatsOptions& statsOptions() const override { return stats_options_; }

private:
  struct TlsCacheEntry {
    std::unordered_map<std::string, CounterSharedPtr> counters_;
    std::unordered_map<std::string, GaugeSharedPtr> gauges_;
    std::unordered_map<std::string, TlsHistogramSharedPtr> histograms_;
    std::unordered_map<std::string, ParentHistogramSharedPtr> parent_histograms_;
  };

  struct CentralCacheEntry {
    std::unordered_map<std::string, CounterSharedPtr> counters_;
    std::unordered_map<std::string, GaugeSharedPtr> gauges_;
    std::unordered_map<std::string, ParentHistogramImplSharedPtr> histograms_;
  };

  struct ScopeImpl : public TlsScope {
    ScopeImpl(ThreadLocalStoreImpl& parent, const std::string& prefix)
        : scope_id_(next_scope_id_++), parent_(parent),
          prefix_(Utility::sanitizeStatsName(prefix)) {}
    ~ScopeImpl();

    // Stats::Scope
    Counter& counter(const std::string& name) override;
    ScopePtr createScope(const std::string& name) override {
      return parent_.createScope(prefix_ + name);
    }
    void deliverHistogramToSinks(const Histogram& histogram, uint64_t value) override;
    Gauge& gauge(const std::string& name) override;
    Histogram& histogram(const std::string& name) override;
    Histogram& tlsHistogram(const std::string& name, ParentHistogramImpl& parent) override;
    const Stats::StatsOptions& statsOptions() const override { return parent_.statsOptions(); }

    template <class StatType>
    using MakeStatFn =
        std::function<std::shared_ptr<StatType>(StatDataAllocator&, absl::string_view name,
                                                std::string&& tag_extracted_name,
                                                std::vector<Tag>&& tags)>;

    /**
     * Makes a stat either by looking it up in the central cache,
     * generating it from the the parent allocator, or as a last
     * result, creating it with the heap allocator.
     *
     * @param name the full name of the stat (not tag extracted).
     * @param central_cache_map a map from name to the desired object in the central cache.
     * @param make_stat a function to generate the stat object, called if it's not in cache.
     * @param tls_ref possibly null reference to a cache entry for this stat, which will be
     *     used if non-empty, or filled in if empty (and non-null).
     */
    template <class StatType>
    StatType&
    safeMakeStat(const std::string& name,
                 std::unordered_map<std::string, std::shared_ptr<StatType>>& central_cache_map,
                 MakeStatFn<StatType> make_stat, std::shared_ptr<StatType>* tls_ref);

    static std::atomic<uint64_t> next_scope_id_;

    const uint64_t scope_id_;
    ThreadLocalStoreImpl& parent_;
    const std::string prefix_;
    CentralCacheEntry central_cache_;
  };

  struct TlsCache : public ThreadLocal::ThreadLocalObject {
    // The TLS scope cache is keyed by scope ID. This is used to avoid complex circular references
    // during scope destruction. An ID is required vs. using the address of the scope pointer
    // because it's possible that the memory allocator will recyle the scope pointer immediately
    // upon destruction, leading to a situation in which a new scope with the same address is used
    // to reference the cache, and then subsequently cache flushed, leaving nothing in the central
    // store. See the overview for more information. This complexity is required for lockless
    // operation in the fast path.
    std::unordered_map<uint64_t, TlsCacheEntry> scope_cache_;
  };

  std::string getTagsForName(const std::string& name, std::vector<Tag>& tags) const;
  void clearScopeFromCaches(uint64_t scope_id);
  void releaseScopeCrossThread(ScopeImpl* scope);
  void mergeInternal(PostMergeCb mergeCb);
  absl::string_view truncateStatNameIfNeeded(absl::string_view name);

  const Stats::StatsOptions& stats_options_;
  StatDataAllocator& alloc_;
  Event::Dispatcher* main_thread_dispatcher_{};
  ThreadLocal::SlotPtr tls_;
  mutable Thread::MutexBasicLockable lock_;
  std::unordered_set<ScopeImpl*> scopes_ GUARDED_BY(lock_);
  ScopePtr default_scope_;
  std::list<std::reference_wrapper<Sink>> timer_sinks_;
  TagProducerPtr tag_producer_;
  std::atomic<bool> shutting_down_{};
  std::atomic<bool> merge_in_progress_{};
  Counter& num_last_resort_stats_;
  HeapStatDataAllocator heap_allocator_;
  SourceImpl source_;
};

} // namespace Stats
} // namespace Envoy
