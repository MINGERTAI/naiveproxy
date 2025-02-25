// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/power_monitor/battery_state_sampler.h"

#if !BUILDFLAG(IS_MAC)
#include "base/power_monitor/timer_sampling_event_source.h"
#endif

namespace base {

namespace {

// Singleton instance of the BatteryStateSampler.
BatteryStateSampler* g_battery_state_sampler = nullptr;

}  // namespace

BatteryStateSampler::BatteryStateSampler(
    std::unique_ptr<SamplingEventSource> sampling_event_source,
    std::unique_ptr<BatteryLevelProvider> battery_level_provider)
    : sampling_event_source_(std::move(sampling_event_source)),
      battery_level_provider_(std::move(battery_level_provider)) {
  DCHECK(sampling_event_source_);
  DCHECK(battery_level_provider_);

  DCHECK(!g_battery_state_sampler);
  g_battery_state_sampler = this;

  // Get an initial sample.
  battery_level_provider_->GetBatteryState(
      base::BindOnce(&BatteryStateSampler::OnInitialBatteryStateSampled,
                     base::Unretained(this)));

  // Start the periodic sampling.
  sampling_event_source_->Start(base::BindRepeating(
      &BatteryStateSampler::OnSamplingEvent, base::Unretained(this)));
}

BatteryStateSampler::~BatteryStateSampler() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_EQ(g_battery_state_sampler, this);
  g_battery_state_sampler = nullptr;
}

// static
BatteryStateSampler* BatteryStateSampler::Get() {
  DCHECK(g_battery_state_sampler);
  return g_battery_state_sampler;
}

void BatteryStateSampler::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  observer_list_.AddObserver(observer);

  // Send the last sample available.
  if (has_last_battery_state_)
    observer->OnBatteryStateSampled(last_battery_state_);
}

void BatteryStateSampler::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observer_list_.RemoveObserver(observer);
}

#if !BUILDFLAG(IS_MAC)
// static
std::unique_ptr<SamplingEventSource>
BatteryStateSampler::CreateSamplingEventSource() {
  // On platforms where the OS does not provide a notification when an updated
  // battery level is available, simply sample on a regular 1 minute interval.
  return std::make_unique<TimerSamplingEventSource>(Minutes(1));
}
#endif  // !BUILDFLAG(IS_MAC)

void BatteryStateSampler::OnInitialBatteryStateSampled(
    const absl::optional<BatteryLevelProvider::BatteryState>& battery_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(!has_last_battery_state_);
  has_last_battery_state_ = true;
  last_battery_state_ = battery_state;

  for (auto& observer : observer_list_)
    observer.OnBatteryStateSampled(battery_state);
}

void BatteryStateSampler::OnSamplingEvent() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  battery_level_provider_->GetBatteryState(base::BindOnce(
      &BatteryStateSampler::OnBatteryStateSampled, base::Unretained(this)));
}

void BatteryStateSampler::OnBatteryStateSampled(
    const absl::optional<BatteryLevelProvider::BatteryState>& battery_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(has_last_battery_state_);
  last_battery_state_ = battery_state;

  for (auto& observer : observer_list_)
    observer.OnBatteryStateSampled(battery_state);
}

}  // namespace base
