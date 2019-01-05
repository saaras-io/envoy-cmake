#include "extensions/retry/priority/other_priority/other_priority.h"

namespace Envoy {
namespace Extensions {
namespace Retry {
namespace Priority {
const Upstream::PriorityLoad& OtherPriorityRetryPriority::determinePriorityLoad(
    const Upstream::PrioritySet& priority_set,
    const Upstream::PriorityLoad& original_priority_load) {
  // If we've not seen enough retries to modify the priority load, just
  // return the original.
  // If this retry should trigger an update, recalculate the priority load by excluding attempted
  // priorities.
  if (attempted_priorities_.size() < update_frequency_) {
    return original_priority_load;
  } else if (attempted_priorities_.size() % update_frequency_ == 0) {
    if (excluded_priorities_.size() < priority_set.hostSetsPerPriority().size()) {
      excluded_priorities_.resize(priority_set.hostSetsPerPriority().size());
    }

    for (const auto priority : attempted_priorities_) {
      excluded_priorities_[priority] = true;
    }

    adjustForAttemptedPriorities(priority_set);
  }

  return per_priority_load_;
}

void OtherPriorityRetryPriority::adjustForAttemptedPriorities(
    const Upstream::PrioritySet& priority_set) {
  for (auto& host_set : priority_set.hostSetsPerPriority()) {
    recalculatePerPriorityState(host_set->priority(), priority_set);
  }

  // If all priorities are unhealthy to begin with, there's nothing to do.
  if (!std::accumulate(per_priority_load_.begin(), per_priority_load_.end(), 0)) {
    return;
  }

  auto adjustedHealthAndSum = adjustedHealth();
  // If there are no healthy priorities left, we reset the attempted priorities and recompute the
  // adjusted health.
  // This allows us to fall back to the unmodified priority load when we run out of priorites
  // instead of failing to route requests.
  if (adjustedHealthAndSum.second == 0) {
    for (size_t i = 0; i < excluded_priorities_.size(); ++i) {
      excluded_priorities_[i] = false;
    }
    attempted_priorities_.clear();
    adjustedHealthAndSum = adjustedHealth();
  }

  const auto& adjusted_per_priority_health = adjustedHealthAndSum.first;
  auto total_health = adjustedHealthAndSum.second;

  std::fill(per_priority_load_.begin(), per_priority_load_.end(), 0);
  // We then adjust the load by rebalancing priorities with the adjusted health values.
  size_t total_load = 100;
  // The outer loop is used to eliminate rounding errors: any remaining load will be assigned to the
  // first healthy priority.
  while (total_load != 0) {
    for (size_t i = 0; i < adjusted_per_priority_health.size(); ++i) {
      // Now assign as much load as possible to the high priority levels and cease assigning load
      // when total_load runs out.
      auto delta =
          std::min<uint32_t>(total_load, adjusted_per_priority_health[i] * 100 / total_health);
      per_priority_load_[i] += delta;
      total_load -= delta;
    }
  }
}

std::pair<std::vector<uint32_t>, uint32_t> OtherPriorityRetryPriority::adjustedHealth() const {
  // Create an adjusted health view of the priorities, where attempted priorities are
  // given a zero weight.
  uint32_t total_health = 0;
  std::vector<uint32_t> adjusted_per_priority_health(per_priority_health_.size());
  adjusted_per_priority_health.resize(per_priority_health_.size());

  for (size_t i = 0; i < per_priority_health_.size(); ++i) {
    if (!excluded_priorities_[i]) {
      adjusted_per_priority_health[i] = per_priority_health_[i];
      total_health += per_priority_health_[i];
    }
  }

  return {std::move(adjusted_per_priority_health), std::min(total_health, 100u)};
}

} // namespace Priority
} // namespace Retry
} // namespace Extensions
} // namespace Envoy
