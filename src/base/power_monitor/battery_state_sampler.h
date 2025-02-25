// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_POWER_MONITOR_BATTERY_STATE_SAMPLER_H_
#define BASE_POWER_MONITOR_BATTERY_STATE_SAMPLER_H_

#include <memory>
#include <vector>

#include "base/base_export.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/power_monitor/battery_level_provider.h"
#include "base/power_monitor/power_monitor_buildflags.h"
#include "base/power_monitor/sampling_event_source.h"
#include "base/sequence_checker.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {

// Periodically samples the battery and notifies its observers.
class BASE_EXPORT BatteryStateSampler {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Note: The first sample taken by the BatteryStateSampler may be out of
    // date (i.e. represent the battery state at an earlier time). Observers
    // that want to ignore those stale samples should ignore the first call to
    // OnBatteryStateSampled.
    virtual void OnBatteryStateSampled(
        const absl::optional<BatteryLevelProvider::BatteryState>&
            battery_state) = 0;
  };

  // Creates a BatteryStateSampler and initializes the global instance. Will
  // DCHECK if an instance already exists.
  BatteryStateSampler(
      std::unique_ptr<SamplingEventSource> sampling_event_source =
          CreateSamplingEventSource(),
      std::unique_ptr<BatteryLevelProvider> battery_level_provider =
          BatteryLevelProvider::Create());
  ~BatteryStateSampler();

  // Returns the unique instance.
  static BatteryStateSampler* Get();

  // Adds/removes an observer. `OnBatteryStateSampled` will be immediately
  // invoked upon adding an observer if an existing sample exists already.
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  // Returns a platform specific SamplingEventSource.
  static std::unique_ptr<SamplingEventSource> CreateSamplingEventSource();

  // Called when the first battery sampled is obtained. Notifies current
  // observers as they are waiting on the cached battery state.
  void OnInitialBatteryStateSampled(
      const absl::optional<BatteryLevelProvider::BatteryState>& battery_state);

  // Triggers the sampling of the battery state.
  void OnSamplingEvent();

  // Notifies observers of the sampled battery state.
  void OnBatteryStateSampled(
      const absl::optional<BatteryLevelProvider::BatteryState>& battery_state);

  std::unique_ptr<SamplingEventSource> sampling_event_source_
      GUARDED_BY_CONTEXT(sequence_checker_);

  std::unique_ptr<BatteryLevelProvider> battery_level_provider_
      GUARDED_BY_CONTEXT(sequence_checker_);

  base::ObserverList<Observer> observer_list_
      GUARDED_BY_CONTEXT(sequence_checker_);

  // Indicates if |last_battery_state_| contains an actual sample. Note: a
  // separate bool is used to avoid nested optionals.
  bool has_last_battery_state_ GUARDED_BY_CONTEXT(sequence_checker_) = false;

  // The value of the last sample taken.
  absl::optional<BatteryLevelProvider::BatteryState> last_battery_state_
      GUARDED_BY_CONTEXT(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace base

#endif  // BASE_POWER_MONITOR_BATTERY_STATE_SAMPLER_H_
