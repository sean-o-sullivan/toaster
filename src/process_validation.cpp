#include "process_validation.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

const uint32_t kMaximumEvidenceGapMs = 1000U;

bool finite(float value) { return std::isfinite(value); }

bool nonEmptyTerminated(const char* text, size_t size) {
  return text[0] != '\0' && std::memchr(text, '\0', size) != nullptr;
}

bool validCriterion(const CriterionRequirement& criterion) {
  if (criterion.required == criterion.not_applicable) {
    return false;
  }
  return criterion.not_applicable ||
         (finite(criterion.minimum) && finite(criterion.maximum) &&
          criterion.minimum <= criterion.maximum);
}

uint32_t boundedMilliseconds(double milliseconds) {
  if (!(milliseconds > 0.0)) {
    return 0U;
  }
  const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(milliseconds >= maximum ? maximum : milliseconds + 0.5);
}

void addSaturated(uint32_t& destination, uint32_t value) {
  const uint32_t maximum = std::numeric_limits<uint32_t>::max();
  destination = value > maximum - destination ? maximum : destination + value;
}

double crossingMilliseconds(uint64_t interval_start_ms, uint32_t delta_ms,
                            float previous, float current, float threshold) {
  const double fraction = static_cast<double>(threshold - previous) /
                          static_cast<double>(current - previous);
  return static_cast<double>(interval_start_ms) + fraction * delta_ms;
}

ValidationStatus combine(ValidationStatus current, ValidationStatus next) {
  if (current == ValidationStatus::Incomplete || next == ValidationStatus::Incomplete) {
    return ValidationStatus::Incomplete;
  }
  if (current == ValidationStatus::CriteriaMissing ||
      next == ValidationStatus::CriteriaMissing) {
    return ValidationStatus::CriteriaMissing;
  }
  if (current == ValidationStatus::Fail || next == ValidationStatus::Fail) {
    return ValidationStatus::Fail;
  }
  return ValidationStatus::Pass;
}

bool validIdentity(const RunIdentity& identity) {
  return identity.setup_revision != 0U && identity.gain_revision != 0U &&
         identity.recipe_revision != 0U && identity.criteria_revision != 0U &&
         identity.run_id != 0U;
}

CriterionResult assessScalar(const CriterionRequirement& requirement,
                             bool measured, float value, bool evidence_complete) {
  CriterionResult result;
  result.not_applicable = requirement.not_applicable;
  result.measured = measured;
  result.value = measured ? value : 0.0F;
  if (requirement.not_applicable) {
    result.status = ValidationStatus::NotRun;
  } else if (!requirement.required) {
    result.status = ValidationStatus::CriteriaMissing;
  } else if (!evidence_complete || !measured) {
    result.status = ValidationStatus::Incomplete;
  } else {
    result.status = value >= requirement.minimum && value <= requirement.maximum
                        ? ValidationStatus::Pass
                        : ValidationStatus::Fail;
  }
  return result;
}

}  // namespace

bool validRequirements(const Requirements& requirements) {
  if (!requirements.valid ||
      !nonEmptyTerminated(requirements.criteria_id, Requirements::kIdSize) ||
      !nonEmptyTerminated(requirements.criteria_revision, Requirements::kRevisionSize) ||
      !nonEmptyTerminated(requirements.source_reference, Requirements::kSourceSize)) {
    return false;
  }

  if (requirements.profile == ValidationProfile::Reflow) {
    if (!validCriterion(requirements.heating_slope.criterion) ||
        !validCriterion(requirements.soak.criterion) ||
        !validCriterion(requirements.peak_celsius) ||
        !validCriterion(requirements.liquidus.criterion) ||
        !validCriterion(requirements.cooling_slope.criterion)) {
      return false;
    }
    if (!requirements.heating_slope.criterion.required &&
        !requirements.soak.criterion.required && !requirements.peak_celsius.required &&
        !requirements.liquidus.criterion.required &&
        !requirements.cooling_slope.criterion.required) {
      return false;
    }
    if (requirements.heating_slope.criterion.required &&
        (!finite(requirements.heating_slope.window_start_celsius) ||
         !finite(requirements.heating_slope.window_end_celsius) ||
         requirements.heating_slope.window_start_celsius >=
             requirements.heating_slope.window_end_celsius)) {
      return false;
    }
    if (requirements.soak.criterion.required &&
        (!finite(requirements.soak.band_minimum_celsius) ||
         !finite(requirements.soak.band_maximum_celsius) ||
         requirements.soak.band_minimum_celsius > requirements.soak.band_maximum_celsius)) {
      return false;
    }
    if (requirements.liquidus.criterion.required &&
        !finite(requirements.liquidus.threshold_celsius)) {
      return false;
    }
    if (requirements.cooling_slope.criterion.required &&
        (!finite(requirements.cooling_slope.window_start_celsius) ||
         !finite(requirements.cooling_slope.window_end_celsius) ||
         requirements.cooling_slope.window_start_celsius <=
             requirements.cooling_slope.window_end_celsius)) {
      return false;
    }
    return true;
  }

  const AnnealRequirement& anneal = requirements.anneal;
  if (anneal.required == anneal.not_applicable) {
    return false;
  }
  if (anneal.not_applicable) return false;
  return finite(anneal.target_celsius) && finite(anneal.band_celsius) &&
         finite(anneal.minimum_hold_seconds) && finite(anneal.maximum_warmup_seconds) &&
         finite(anneal.maximum_settling_seconds) &&
         finite(anneal.settling_continuous_seconds) &&
         finite(anneal.maximum_overshoot_celsius) &&
         finite(anneal.maximum_mean_error_celsius) &&
         finite(anneal.maximum_rms_error_celsius) &&
         finite(anneal.maximum_error_celsius) &&
         finite(anneal.maximum_abs_drift_celsius_per_second) &&
         anneal.band_celsius >= 0.0F && anneal.minimum_hold_seconds >= 0.0F &&
         anneal.maximum_warmup_seconds >= 0.0F &&
         anneal.maximum_settling_seconds >= 0.0F &&
         anneal.settling_continuous_seconds > 0.0F &&
         anneal.maximum_overshoot_celsius >= 0.0F &&
         anneal.maximum_mean_error_celsius >= 0.0F &&
         anneal.maximum_rms_error_celsius >= 0.0F &&
         anneal.maximum_error_celsius >= 0.0F &&
         anneal.maximum_abs_drift_celsius_per_second >= 0.0F;
}

bool sameValidationConfiguration(const RunIdentity& a, const RunIdentity& b) {
  return a.setup_revision == b.setup_revision && a.gain_revision == b.gain_revision &&
         a.recipe_revision == b.recipe_revision &&
         a.criteria_revision == b.criteria_revision;
}

const char* toString(ValidationStatus status) {
  switch (status) {
    case ValidationStatus::NotRun: return "NOT RUN";
    case ValidationStatus::Running: return "RUNNING";
    case ValidationStatus::Pass: return "PASS";
    case ValidationStatus::Fail: return "FAIL";
    case ValidationStatus::Incomplete: return "INCOMPLETE";
    case ValidationStatus::CriteriaMissing: return "CRITERIA MISSING";
  }
  return "UNKNOWN";
}

void ProcessValidationAssessor::reset() {
  requirements_ = Requirements();
  report_ = ValidationReport();
  running_ = false;
  finalized_ = false;
  evidence_complete_ = false;
  last_ms_ = 0;
  elapsed_ms_ = 0;
  last_temperature_ = 0.0F;
  last_target_ = 0.0F;
  last_demand_ = 0.0F;
  last_stage_ = ValidationStage::Ramp;
  heat_start_seen_ = false;
  heat_end_seen_ = false;
  heat_start_ms_ = 0.0;
  heat_end_ms_ = 0.0;
  cool_start_seen_ = false;
  cool_end_seen_ = false;
  cool_start_ms_ = 0.0;
  cool_end_ms_ = 0.0;
  soak_current_ms_ = 0;
  liquidus_current_ms_ = 0;
  hold_current_in_band_ms_ = 0;
  hold_seen_ = false;
  warmup_seen_ = false;
  settling_current_ms_ = 0;
  hold_error_time_sum_ = 0.0;
  hold_error_squared_time_sum_ = 0.0;
  hold_demand_time_sum_ = 0.0;
  hold_time_seconds_ = 0.0;
  regression_n_ = 0.0;
  regression_t_sum_ = 0.0;
  regression_error_sum_ = 0.0;
  regression_tt_sum_ = 0.0;
  regression_te_sum_ = 0.0;
}

bool ProcessValidationAssessor::begin(const Requirements& requirements,
                                      const RunIdentity& identity, uint32_t now_ms,
                                      float temperature_celsius) {
  reset();
  requirements_ = requirements;
  report_.identity = identity;
  report_.profile = requirements.profile;
  report_.status = ValidationStatus::Running;
  last_ms_ = now_ms;
  last_temperature_ = temperature_celsius;
  evidence_complete_ = finite(temperature_celsius);
  running_ = evidence_complete_;
  if (evidence_complete_) {
    report_.metrics.sample_count = 1U;
    report_.metrics.peak_measured = true;
    report_.metrics.peak_celsius = temperature_celsius;
    if (validRequirements(requirements_) &&
        requirements_.profile == ValidationProfile::Anneal &&
        requirements_.anneal.required) {
      const float low = requirements_.anneal.target_celsius - requirements_.anneal.band_celsius;
      const float high = requirements_.anneal.target_celsius + requirements_.anneal.band_celsius;
      if (temperature_celsius >= low && temperature_celsius <= high) {
        warmup_seen_ = true;
        report_.metrics.warmup_measured = true;
        report_.metrics.warmup_ms = 0U;
      }
    }
  } else {
    report_.status = ValidationStatus::Incomplete;
  }
  return running_;
}

void ProcessValidationAssessor::invalidateEvidence() {
  evidence_complete_ = false;
  running_ = false;
  report_.status = ValidationStatus::Incomplete;
}

void ProcessValidationAssessor::accumulateBand(uint32_t delta_ms, float previous,
                                               float current, float low, float high,
                                               uint32_t& accumulated,
                                               uint32_t& uninterrupted,
                                               uint32_t& longest) {
  double start = 0.0;
  double end = 1.0;
  if (current == previous) {
    if (current < low || current > high) {
      uninterrupted = 0U;
      return;
    }
  } else {
    double at_low = static_cast<double>(low - previous) / (current - previous);
    double at_high = static_cast<double>(high - previous) / (current - previous);
    if (at_low > at_high) {
      const double swap = at_low;
      at_low = at_high;
      at_high = swap;
    }
    start = at_low > 0.0 ? at_low : 0.0;
    end = at_high < 1.0 ? at_high : 1.0;
    if (end <= start) {
      uninterrupted = 0U;
      return;
    }
  }
  const uint32_t inside = boundedMilliseconds((end - start) * delta_ms);
  addSaturated(accumulated, inside);
  if (start > 0.0) {
    uninterrupted = inside;
  } else {
    addSaturated(uninterrupted, inside);
  }
  if (uninterrupted > longest) {
    longest = uninterrupted;
  }
  if (end < 1.0) {
    uninterrupted = 0U;
  }
}

void ProcessValidationAssessor::accumulateAbove(uint32_t delta_ms, float previous,
                                                float current, float threshold,
                                                uint32_t& accumulated,
                                                uint32_t& uninterrupted,
                                                uint32_t& longest) {
  if (previous == current) {
    if (current <= threshold) {
      uninterrupted = 0U;
      return;
    }
    addSaturated(accumulated, delta_ms);
    addSaturated(uninterrupted, delta_ms);
    if (uninterrupted > longest) longest = uninterrupted;
    return;
  }

  double start = 0.0;
  double end = 1.0;
  const double crossing = static_cast<double>(threshold - previous) / (current - previous);
  if (previous < threshold && current >= threshold) {
    start = crossing;
  } else if (previous >= threshold && current < threshold) {
    end = crossing;
  } else if (previous < threshold && current < threshold) {
    uninterrupted = 0U;
    return;
  }
  const uint32_t above = boundedMilliseconds((end - start) * delta_ms);
  addSaturated(accumulated, above);
  if (start > 0.0) uninterrupted = above;
  else addSaturated(uninterrupted, above);
  if (uninterrupted > longest) longest = uninterrupted;
  if (end < 1.0) uninterrupted = 0U;
}

bool ProcessValidationAssessor::sample(uint32_t now_ms, float temperature_celsius,
                                       float target_celsius, ValidationStage stage,
                                       float demand_percent, bool valid) {
  if (!running_ || finalized_) {
    return false;
  }
  if (!valid || !finite(temperature_celsius) || !finite(target_celsius) ||
      !finite(demand_percent)) {
    invalidateEvidence();
    return false;
  }
  const uint32_t delta_ms = now_ms - last_ms_;
  if (delta_ms > report_.metrics.maximum_gap_ms) {
    report_.metrics.maximum_gap_ms = delta_ms;
  }
  if (delta_ms > kMaximumEvidenceGapMs) {
    invalidateEvidence();
    return false;
  }

  const uint64_t interval_start_ms = elapsed_ms_;
  elapsed_ms_ += delta_ms;
  ++report_.metrics.sample_count;
  if (!report_.metrics.peak_measured || temperature_celsius > report_.metrics.peak_celsius) {
    report_.metrics.peak_measured = true;
    report_.metrics.peak_celsius = temperature_celsius;
  }

  const bool requirements_valid = validRequirements(requirements_);
  if (requirements_valid && requirements_.profile == ValidationProfile::Reflow) {
    // Ramp/cooling rates are endpoint rates across declared temperature
    // windows. Linear crossing interpolation avoids adjacent-sample derivatives
    // amplifying the MAX31855's 0.25 C quantization.
    if (requirements_.heating_slope.criterion.required && stage == ValidationStage::Ramp &&
        last_stage_ == ValidationStage::Ramp && temperature_celsius != last_temperature_) {
      const float start = requirements_.heating_slope.window_start_celsius;
      const float end = requirements_.heating_slope.window_end_celsius;
      if (!heat_start_seen_ && last_temperature_ <= start && temperature_celsius >= start) {
        heat_start_ms_ = crossingMilliseconds(interval_start_ms, delta_ms, last_temperature_,
                                              temperature_celsius, start);
        heat_start_seen_ = true;
      }
      if (heat_start_seen_ && !heat_end_seen_ && last_temperature_ <= end &&
          temperature_celsius >= end) {
        heat_end_ms_ = crossingMilliseconds(interval_start_ms, delta_ms, last_temperature_,
                                            temperature_celsius, end);
        heat_end_seen_ = heat_end_ms_ > heat_start_ms_;
      }
    }
    if (requirements_.soak.criterion.required && stage == ValidationStage::Hold &&
        last_stage_ == ValidationStage::Hold) {
      // Soak is deliberately phase-scoped. Ramp/cooling crossings through the
      // same temperatures cannot inflate qualified soak time.
      accumulateBand(delta_ms, last_temperature_, temperature_celsius,
                     requirements_.soak.band_minimum_celsius,
                     requirements_.soak.band_maximum_celsius,
                     report_.metrics.soak_accumulated_ms, soak_current_ms_,
                     report_.metrics.soak_longest_continuous_ms);
    } else if (stage != ValidationStage::Hold || last_stage_ != ValidationStage::Hold) {
      soak_current_ms_ = 0U;
    }
    if (requirements_.liquidus.criterion.required) {
      // TAL is physical time above the threshold, independent of recipe stage;
      // each boundary interval is linearly interpolated.
      accumulateAbove(delta_ms, last_temperature_, temperature_celsius,
                      requirements_.liquidus.threshold_celsius,
                      report_.metrics.liquidus_accumulated_ms, liquidus_current_ms_,
                      report_.metrics.liquidus_longest_continuous_ms);
    }
    if (requirements_.cooling_slope.criterion.required &&
        stage == ValidationStage::Cooling && temperature_celsius != last_temperature_) {
      const float start = requirements_.cooling_slope.window_start_celsius;
      const float end = requirements_.cooling_slope.window_end_celsius;
      if (!cool_start_seen_ && last_temperature_ >= start && temperature_celsius <= start) {
        cool_start_ms_ = crossingMilliseconds(interval_start_ms, delta_ms, last_temperature_,
                                              temperature_celsius, start);
        cool_start_seen_ = true;
      }
      if (cool_start_seen_ && !cool_end_seen_ && last_temperature_ >= end &&
          temperature_celsius <= end) {
        cool_end_ms_ = crossingMilliseconds(interval_start_ms, delta_ms, last_temperature_,
                                            temperature_celsius, end);
        cool_end_seen_ = cool_end_ms_ > cool_start_ms_;
      }
    }
  }

  if (requirements_valid && requirements_.profile == ValidationProfile::Anneal &&
      requirements_.anneal.required && !warmup_seen_ &&
      stage != ValidationStage::Cooling) {
    const float low = requirements_.anneal.target_celsius - requirements_.anneal.band_celsius;
    const float high = requirements_.anneal.target_celsius + requirements_.anneal.band_celsius;
    if (last_temperature_ < low && temperature_celsius >= low &&
        temperature_celsius != last_temperature_) {
      warmup_seen_ = true;
      report_.metrics.warmup_measured = true;
      report_.metrics.warmup_ms = boundedMilliseconds(crossingMilliseconds(
          interval_start_ms, delta_ms, last_temperature_, temperature_celsius, low));
    } else if (temperature_celsius >= low && temperature_celsius <= high) {
      warmup_seen_ = true;
      report_.metrics.warmup_measured = true;
      report_.metrics.warmup_ms = boundedMilliseconds(elapsed_ms_);
    }
  }

  if (stage == ValidationStage::Hold) {
    const float error = temperature_celsius - target_celsius;
    const float absolute_error = std::fabs(error);
    if (!hold_seen_ || last_stage_ != ValidationStage::Hold) {
      hold_seen_ = true;
      report_.metrics.hold_peak_celsius = temperature_celsius;
      report_.metrics.hold_max_error_celsius = absolute_error;
      regression_n_ = 1.0;
      regression_error_sum_ = error;
      last_target_ = target_celsius;
      last_demand_ = demand_percent;
    } else {
      const double seconds = delta_ms / 1000.0;
      const float previous_error = last_temperature_ - last_target_;
      hold_time_seconds_ += seconds;
      hold_error_time_sum_ += 0.5 * (previous_error + error) * seconds;
      hold_error_squared_time_sum_ +=
          0.5 * (previous_error * previous_error + error * error) * seconds;
      hold_demand_time_sum_ += 0.5 * (last_demand_ + demand_percent) * seconds;
      report_.metrics.hold_elapsed_ms = boundedMilliseconds(hold_time_seconds_ * 1000.0);
      const double t = hold_time_seconds_;
      regression_n_ += 1.0;
      regression_t_sum_ += t;
      regression_error_sum_ += error;
      regression_tt_sum_ += t * t;
      regression_te_sum_ += t * error;
      if (temperature_celsius > report_.metrics.hold_peak_celsius) {
        report_.metrics.hold_peak_celsius = temperature_celsius;
      }
      if (absolute_error > report_.metrics.hold_max_error_celsius) {
        report_.metrics.hold_max_error_celsius = absolute_error;
      }
    }
    if (demand_percent > report_.metrics.hold_max_demand_percent) {
      report_.metrics.hold_max_demand_percent = demand_percent;
    }
    report_.metrics.hold_error_measured = true;

    const float reference_target = requirements_valid &&
                                           requirements_.profile == ValidationProfile::Anneal &&
                                           requirements_.anneal.required
                                       ? requirements_.anneal.target_celsius
                                       : target_celsius;
    const float target_mismatch = std::fabs(target_celsius - reference_target);
    if (target_mismatch > report_.metrics.hold_max_target_mismatch_celsius) {
      report_.metrics.hold_max_target_mismatch_celsius = target_mismatch;
    }

    if (requirements_valid && requirements_.profile == ValidationProfile::Anneal &&
        requirements_.anneal.required) {
      const float low = requirements_.anneal.target_celsius - requirements_.anneal.band_celsius;
      const float high = requirements_.anneal.target_celsius + requirements_.anneal.band_celsius;
      if (last_stage_ == ValidationStage::Hold) {
        accumulateBand(delta_ms, last_temperature_, temperature_celsius, low, high,
                       report_.metrics.hold_in_band_accumulated_ms,
                       hold_current_in_band_ms_, report_.metrics.hold_longest_in_band_ms);
      }
      settling_current_ms_ = hold_current_in_band_ms_;
      const uint32_t required_settling =
          boundedMilliseconds(requirements_.anneal.settling_continuous_seconds * 1000.0);
      if (!report_.metrics.settling_measured && settling_current_ms_ >= required_settling) {
        report_.metrics.settling_measured = true;
        // Settling is established when the declared uninterrupted observation
        // interval completes, not backdated to when that interval began.
        report_.metrics.settling_ms = boundedMilliseconds(elapsed_ms_);
      }
    }
  } else {
    hold_current_in_band_ms_ = 0U;
    settling_current_ms_ = 0U;
  }

  last_ms_ = now_ms;
  last_temperature_ = temperature_celsius;
  last_target_ = target_celsius;
  last_demand_ = demand_percent;
  last_stage_ = stage;
  return true;
}

void ProcessValidationAssessor::assess() {
  report_.evidence_complete = evidence_complete_ && report_.run_completed;

  if (heat_start_seen_ && heat_end_seen_) {
    const double seconds = (heat_end_ms_ - heat_start_ms_) / 1000.0;
    if (seconds > 0.0) {
      report_.metrics.heating_slope_measured = true;
      report_.metrics.heating_slope_celsius_per_second =
          (requirements_.heating_slope.window_end_celsius -
           requirements_.heating_slope.window_start_celsius) /
          seconds;
    }
  }
  if (cool_start_seen_ && cool_end_seen_) {
    const double seconds = (cool_end_ms_ - cool_start_ms_) / 1000.0;
    if (seconds > 0.0) {
      report_.metrics.cooling_slope_measured = true;
      report_.metrics.cooling_slope_celsius_per_second =
          (requirements_.cooling_slope.window_start_celsius -
           requirements_.cooling_slope.window_end_celsius) /
          seconds;
    }
  }
  if (hold_time_seconds_ > 0.0) {
    // Mean/RMS/demand are trapezoidal time-weighted values. Drift is the
    // least-squares slope of all timestamped hold samples, not end-minus-start.
    report_.metrics.hold_mean_error_celsius = hold_error_time_sum_ / hold_time_seconds_;
    report_.metrics.hold_rms_error_celsius =
        std::sqrt(hold_error_squared_time_sum_ / hold_time_seconds_);
    report_.metrics.hold_mean_demand_percent = hold_demand_time_sum_ / hold_time_seconds_;
    const double denominator =
        regression_n_ * regression_tt_sum_ - regression_t_sum_ * regression_t_sum_;
    if (denominator > 0.0) {
      report_.metrics.hold_drift_celsius_per_second =
          (regression_n_ * regression_te_sum_ -
           regression_t_sum_ * regression_error_sum_) /
          denominator;
    }
  }

  const bool requirements_valid = validRequirements(requirements_);
  if (requirements_valid && !validIdentity(report_.identity)) {
    report_.evidence_complete = false;
  }
  if (!report_.evidence_complete) {
    report_.status = ValidationStatus::Incomplete;
    if (requirements_valid && requirements_.profile == ValidationProfile::Reflow) {
      report_.heating_slope = assessScalar(requirements_.heating_slope.criterion,
                                           report_.metrics.heating_slope_measured,
                                           report_.metrics.heating_slope_celsius_per_second,
                                           false);
      report_.soak = assessScalar(requirements_.soak.criterion, false, 0.0F, false);
      report_.peak = assessScalar(requirements_.peak_celsius,
                                  report_.metrics.peak_measured,
                                  report_.metrics.peak_celsius, false);
      report_.liquidus =
          assessScalar(requirements_.liquidus.criterion, false, 0.0F, false);
      report_.cooling_slope = assessScalar(requirements_.cooling_slope.criterion,
                                           report_.metrics.cooling_slope_measured,
                                           report_.metrics.cooling_slope_celsius_per_second,
                                           false);
    } else if (requirements_valid && requirements_.profile == ValidationProfile::Anneal) {
      report_.anneal.not_applicable = requirements_.anneal.not_applicable;
      report_.anneal.measured = report_.metrics.hold_error_measured;
      report_.anneal.status = requirements_.anneal.not_applicable
                                  ? ValidationStatus::NotRun
                                  : ValidationStatus::Incomplete;
    }
    return;
  }
  if (!requirements_valid) {
    report_.status = ValidationStatus::CriteriaMissing;
    report_.heating_slope.status = ValidationStatus::CriteriaMissing;
    report_.soak.status = ValidationStatus::CriteriaMissing;
    report_.peak.status = ValidationStatus::CriteriaMissing;
    report_.liquidus.status = ValidationStatus::CriteriaMissing;
    report_.cooling_slope.status = ValidationStatus::CriteriaMissing;
    report_.anneal.status = ValidationStatus::CriteriaMissing;
    return;
  }
  if (requirements_.profile == ValidationProfile::Reflow) {
    report_.heating_slope = assessScalar(requirements_.heating_slope.criterion,
                                         report_.metrics.heating_slope_measured,
                                         report_.metrics.heating_slope_celsius_per_second, true);
    const uint32_t soak_ms = requirements_.soak.mode == DurationMode::Continuous
                                 ? report_.metrics.soak_longest_continuous_ms
                                 : report_.metrics.soak_accumulated_ms;
    report_.soak = assessScalar(requirements_.soak.criterion, true, soak_ms / 1000.0F, true);
    report_.peak = assessScalar(requirements_.peak_celsius,
                                report_.metrics.peak_measured,
                                report_.metrics.peak_celsius, true);
    const uint32_t liquidus_ms = requirements_.liquidus.mode == DurationMode::Continuous
                                     ? report_.metrics.liquidus_longest_continuous_ms
                                     : report_.metrics.liquidus_accumulated_ms;
    report_.liquidus = assessScalar(requirements_.liquidus.criterion, true,
                                    liquidus_ms / 1000.0F, true);
    report_.cooling_slope = assessScalar(requirements_.cooling_slope.criterion,
                                         report_.metrics.cooling_slope_measured,
                                         report_.metrics.cooling_slope_celsius_per_second, true);
    ValidationStatus status = ValidationStatus::Pass;
    for (const CriterionResult* result : {&report_.heating_slope, &report_.soak,
                                         &report_.peak, &report_.liquidus,
                                         &report_.cooling_slope}) {
      if (!result->not_applicable) status = combine(status, result->status);
    }
    report_.status = status;
    return;
  }

  report_.anneal.not_applicable = requirements_.anneal.not_applicable;
  if (requirements_.anneal.not_applicable) {
    report_.anneal.status = ValidationStatus::NotRun;
    report_.status = ValidationStatus::Pass;
    return;
  }
  report_.anneal.measured = report_.metrics.hold_error_measured;
  report_.anneal.value = report_.metrics.hold_longest_in_band_ms / 1000.0F;
  const AnnealRequirement& requirement = requirements_.anneal;
  const bool measurements_present = report_.metrics.hold_error_measured &&
                                    report_.metrics.warmup_measured &&
                                    report_.metrics.settling_measured;
  if (!measurements_present) {
    report_.anneal.status = ValidationStatus::Incomplete;
  } else {
    const float overshoot = report_.metrics.peak_celsius - requirement.target_celsius;
    const bool passed =
        report_.metrics.hold_longest_in_band_ms >=
            boundedMilliseconds(requirement.minimum_hold_seconds * 1000.0) &&
        report_.metrics.warmup_ms <=
            boundedMilliseconds(requirement.maximum_warmup_seconds * 1000.0) &&
        report_.metrics.settling_ms <=
            boundedMilliseconds(requirement.maximum_settling_seconds * 1000.0) &&
        overshoot <= requirement.maximum_overshoot_celsius &&
        std::fabs(report_.metrics.hold_mean_error_celsius) <=
            requirement.maximum_mean_error_celsius &&
        report_.metrics.hold_rms_error_celsius <= requirement.maximum_rms_error_celsius &&
        report_.metrics.hold_max_error_celsius <= requirement.maximum_error_celsius &&
        std::fabs(report_.metrics.hold_drift_celsius_per_second) <=
            requirement.maximum_abs_drift_celsius_per_second &&
        report_.metrics.hold_max_target_mismatch_celsius <= 0.001F;
    report_.anneal.status = passed ? ValidationStatus::Pass : ValidationStatus::Fail;
  }
  report_.status = report_.anneal.status;
}

ValidationReport ProcessValidationAssessor::finish(bool completed) {
  if (finalized_) {
    return report_;
  }
  running_ = false;
  finalized_ = true;
  report_.run_completed = completed;
  assess();
  return report_;
}

void ValidationSequence::reset() {
  identity_ = RunIdentity();
  identity_set_ = false;
  last_run_id_ = 0U;
  passes_ = 0U;
  commissioned_ = false;
  for (uint8_t i = 0; i < kRequiredPasses; ++i) pass_run_ids_[i] = 0U;
}

bool ValidationSequence::record(const ValidationReport& report) {
  if (identity_set_ && sameValidationConfiguration(identity_, report.identity)) {
    if (last_run_id_ == report.identity.run_id) return false;
    for (uint8_t i = 0; i < passes_; ++i) {
      if (pass_run_ids_[i] == report.identity.run_id) return false;
    }
  }
  if (!identity_set_ || !sameValidationConfiguration(identity_, report.identity)) {
    identity_ = report.identity;
    identity_set_ = true;
    passes_ = 0U;
    for (uint8_t i = 0; i < kRequiredPasses; ++i) pass_run_ids_[i] = 0U;
  }
  commissioned_ = false;
  last_run_id_ = report.identity.run_id;
  identity_.run_id = report.identity.run_id;
  if (report.status != ValidationStatus::Pass || !report.run_completed ||
      !report.evidence_complete) {
    passes_ = 0U;
    for (uint8_t i = 0; i < kRequiredPasses; ++i) pass_run_ids_[i] = 0U;
    return true;
  }
  if (!validIdentity(report.identity)) {
    passes_ = 0U;
    for (uint8_t i = 0; i < kRequiredPasses; ++i) pass_run_ids_[i] = 0U;
    return true;
  }
  if (passes_ < kRequiredPasses) {
    pass_run_ids_[passes_++] = report.identity.run_id;
  } else {
    for (uint8_t i = 1U; i < kRequiredPasses; ++i) {
      pass_run_ids_[i - 1U] = pass_run_ids_[i];
    }
    pass_run_ids_[kRequiredPasses - 1U] = report.identity.run_id;
  }
  return true;
}

bool ValidationSequence::eligible(const RunIdentity& identity) const {
  return validIdentity(identity) && identity_set_ && passes_ >= kRequiredPasses &&
         sameValidationConfiguration(identity_, identity);
}

bool ValidationSequence::commission(const RunIdentity& identity) {
  if (!eligible(identity)) {
    return false;
  }
  commissioned_ = true;
  return true;
}

uint32_t ValidationSequence::stateChecksum(const State& state) {
  uint32_t hash = 2166136261U;
  const uint32_t words[] = {
      state.magic,
      state.version,
      state.identity.setup_revision,
      state.identity.gain_revision,
      state.identity.recipe_revision,
      state.identity.criteria_revision,
      state.identity.run_id,
      state.pass_run_ids[0],
      state.pass_run_ids[1],
      state.pass_run_ids[2],
      state.passes,
      state.commissioned ? 1U : 0U,
  };
  for (uint8_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
    uint32_t word = words[i];
    for (uint8_t byte = 0; byte < 4U; ++byte) {
      hash ^= word & 0xFFU;
      hash *= 16777619U;
      word >>= 8U;
    }
  }
  return hash;
}

ValidationSequence::State ValidationSequence::snapshot() const {
  State state;
  state.identity = identity_;
  state.identity.run_id = last_run_id_;
  for (uint8_t i = 0; i < kRequiredPasses; ++i) {
    state.pass_run_ids[i] = pass_run_ids_[i];
  }
  state.passes = passes_;
  state.commissioned = commissioned_;
  state.checksum = stateChecksum(state);
  return state;
}

bool ValidationSequence::restore(const State& state,
                                 const RunIdentity& expected_identity) {
  reset();
  if (state.magic != State::kMagic || state.version != State::kVersion ||
      state.checksum != stateChecksum(state) || state.passes > kRequiredPasses ||
      (state.commissioned && state.passes != kRequiredPasses) ||
      !sameValidationConfiguration(state.identity, expected_identity)) {
    return false;
  }
  if (state.passes > 0U) {
    if (state.identity.setup_revision == 0U || state.identity.gain_revision == 0U ||
        state.identity.recipe_revision == 0U || state.identity.criteria_revision == 0U ||
        state.identity.run_id == 0U) {
      return false;
    }
    for (uint8_t i = 0; i < state.passes; ++i) {
      if (state.pass_run_ids[i] == 0U) return false;
      for (uint8_t j = 0; j < i; ++j) {
        if (state.pass_run_ids[i] == state.pass_run_ids[j]) return false;
      }
    }
    if (state.identity.run_id != state.pass_run_ids[state.passes - 1U]) return false;
  }
  identity_ = state.identity;
  identity_set_ = state.passes > 0U || state.commissioned;
  last_run_id_ = state.identity.run_id;
  passes_ = state.passes;
  commissioned_ = state.commissioned;
  for (uint8_t i = 0; i < kRequiredPasses; ++i) {
    pass_run_ids_[i] = state.pass_run_ids[i];
  }
  return true;
}
