#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "process_validation.h"

namespace {

bool near(float actual, float expected, float tolerance = 0.001F) {
  return std::fabs(actual - expected) <= tolerance;
}

CriterionRequirement required(float minimum, float maximum) {
  CriterionRequirement result;
  result.required = true;
  result.minimum = minimum;
  result.maximum = maximum;
  return result;
}

CriterionRequirement notApplicable() {
  CriterionRequirement result;
  result.not_applicable = true;
  return result;
}

void provenance(Requirements& requirements) {
  requirements.valid = true;
  std::strcpy(requirements.criteria_id, "board-reflow");
  std::strcpy(requirements.criteria_revision, "r1");
  std::strcpy(requirements.source_reference, "approved paste data sheet section 4");
}

RunIdentity identity(uint32_t run_id, uint32_t setup = 1U) {
  RunIdentity result;
  result.setup_revision = setup;
  result.gain_revision = 2U;
  result.recipe_revision = 3U;
  result.criteria_revision = 4U;
  result.run_id = run_id;
  return result;
}

Requirements reflowRequirements() {
  Requirements requirements;
  provenance(requirements);
  requirements.profile = ValidationProfile::Reflow;
  requirements.heating_slope.criterion = required(9.9F, 10.1F);
  requirements.heating_slope.window_start_celsius = 100.0F;
  requirements.heating_slope.window_end_celsius = 150.0F;
  requirements.soak.criterion = required(3.0F, 5.0F);
  requirements.soak.band_minimum_celsius = 150.0F;
  requirements.soak.band_maximum_celsius = 180.0F;
  requirements.soak.mode = DurationMode::Continuous;
  requirements.peak_celsius = required(170.0F, 180.0F);
  requirements.liquidus.criterion = required(5.9F, 6.1F);
  requirements.liquidus.threshold_celsius = 160.0F;
  requirements.liquidus.mode = DurationMode::Accumulated;
  requirements.cooling_slope.criterion = required(9.9F, 10.1F);
  requirements.cooling_slope.window_start_celsius = 170.0F;
  requirements.cooling_slope.window_end_celsius = 120.0F;
  return requirements;
}

void testReflowMetrics() {
  ProcessValidationAssessor assessor;
  const Requirements requirements = reflowRequirements();
  assert(validRequirements(requirements));
  assert(assessor.begin(requirements, identity(10U), 0U, 90.0F));
  for (uint32_t second = 1U; second <= 6U; ++second) {
    assert(assessor.sample(second * 1000U, 90.0F + second * 10.0F, 150.0F,
                           ValidationStage::Ramp, 50.0F, true));
  }
  const float hold[] = {160.0F, 165.0F, 170.0F, 175.0F, 170.0F};
  for (uint32_t i = 0U; i < 5U; ++i) {
    assert(assessor.sample((7U + i) * 1000U, hold[i], 170.0F,
                           ValidationStage::Hold, 25.0F, true));
  }
  const float cooling[] = {170.0F, 160.0F, 150.0F, 140.0F, 130.0F, 120.0F, 110.0F};
  for (uint32_t i = 0U; i < 7U; ++i) {
    assert(assessor.sample((12U + i) * 1000U, cooling[i], 25.0F,
                           ValidationStage::Cooling, 0.0F, true));
  }
  const ValidationReport report = assessor.finish(true);
  assert(report.status == ValidationStatus::Pass);
  assert(report.evidence_complete);
  assert(near(report.metrics.heating_slope_celsius_per_second, 10.0F));
  assert(report.metrics.soak_longest_continuous_ms == 4000U);
  assert(near(report.metrics.peak_celsius, 175.0F));
  assert(report.metrics.liquidus_accumulated_ms == 6000U);
  assert(near(report.metrics.cooling_slope_celsius_per_second, 10.0F));
}

void testInterpolatedLiquidus() {
  Requirements requirements;
  provenance(requirements);
  requirements.profile = ValidationProfile::Reflow;
  requirements.heating_slope.criterion = notApplicable();
  requirements.soak.criterion = notApplicable();
  requirements.peak_celsius = notApplicable();
  requirements.cooling_slope.criterion = notApplicable();
  requirements.liquidus.criterion = required(0.99F, 1.01F);
  requirements.liquidus.threshold_celsius = 160.0F;

  ProcessValidationAssessor assessor;
  assert(assessor.begin(requirements, identity(11U), 0U, 150.0F));
  assert(assessor.sample(1000U, 170.0F, 170.0F, ValidationStage::Ramp, 10.0F, true));
  assert(assessor.sample(2000U, 150.0F, 150.0F, ValidationStage::Cooling, 0.0F, true));
  const ValidationReport report = assessor.finish(true);
  assert(report.status == ValidationStatus::Pass);
  assert(report.metrics.liquidus_accumulated_ms == 1000U);

  requirements.liquidus.criterion = required(0.0F, 0.0F);
  ProcessValidationAssessor threshold_only;
  assert(threshold_only.begin(requirements, identity(13U), 0U, 160.0F));
  assert(threshold_only.sample(1000U, 160.0F, 160.0F,
                               ValidationStage::Hold, 0.0F, true));
  const ValidationReport flat = threshold_only.finish(true);
  assert(flat.status == ValidationStatus::Pass);
  assert(flat.metrics.liquidus_accumulated_ms == 0U);
}

void testQuantizedWindowSlope() {
  Requirements requirements;
  provenance(requirements);
  requirements.profile = ValidationProfile::Reflow;
  requirements.heating_slope.criterion = required(0.99F, 1.01F);
  requirements.heating_slope.window_start_celsius = 100.0F;
  requirements.heating_slope.window_end_celsius = 101.0F;
  requirements.soak.criterion = notApplicable();
  requirements.peak_celsius = notApplicable();
  requirements.liquidus.criterion = notApplicable();
  requirements.cooling_slope.criterion = notApplicable();

  ProcessValidationAssessor assessor;
  assert(assessor.begin(requirements, identity(12U), 0U, 99.75F));
  for (uint32_t i = 1U; i <= 6U; ++i) {
    assert(assessor.sample(i * 250U, 99.75F + i * 0.25F, 105.0F,
                           ValidationStage::Ramp, 30.0F, true));
  }
  const ValidationReport report = assessor.finish(true);
  assert(report.status == ValidationStatus::Pass);
  assert(near(report.metrics.heating_slope_celsius_per_second, 1.0F));
}

Requirements annealRequirements() {
  Requirements requirements;
  provenance(requirements);
  requirements.profile = ValidationProfile::Anneal;
  requirements.anneal.required = true;
  requirements.anneal.target_celsius = 100.0F;
  requirements.anneal.band_celsius = 2.0F;
  requirements.anneal.minimum_hold_seconds = 3.0F;
  requirements.anneal.maximum_warmup_seconds = 3.0F;
  requirements.anneal.maximum_settling_seconds = 4.0F;
  requirements.anneal.settling_continuous_seconds = 1.0F;
  requirements.anneal.maximum_overshoot_celsius = 3.0F;
  requirements.anneal.maximum_mean_error_celsius = 1.0F;
  requirements.anneal.maximum_rms_error_celsius = 1.0F;
  requirements.anneal.maximum_error_celsius = 1.0F;
  requirements.anneal.maximum_abs_drift_celsius_per_second = 0.5F;
  return requirements;
}

void testAnnealMetricsAndTargetIdentity() {
  const Requirements requirements = annealRequirements();
  ProcessValidationAssessor assessor;
  assert(assessor.begin(requirements, identity(20U), 0U, 25.0F));
  assert(assessor.sample(1000U, 80.0F, 100.0F, ValidationStage::Ramp, 50.0F, true));
  assert(assessor.sample(2000U, 95.0F, 100.0F, ValidationStage::Ramp, 30.0F, true));
  assert(assessor.sample(3000U, 100.0F, 100.0F, ValidationStage::Hold, 20.0F, true));
  assert(assessor.sample(4000U, 100.0F, 100.0F, ValidationStage::Hold, 20.0F, true));
  assert(assessor.sample(5000U, 101.0F, 100.0F, ValidationStage::Hold, 20.0F, true));
  assert(assessor.sample(6000U, 100.0F, 100.0F, ValidationStage::Hold, 20.0F, true));
  assert(assessor.sample(7000U, 99.0F, 100.0F, ValidationStage::Hold, 20.0F, true));
  const ValidationReport report = assessor.finish(true);
  assert(report.status == ValidationStatus::Pass);
  assert(report.metrics.hold_longest_in_band_ms == 4000U);
  assert(report.metrics.warmup_ms == 2600U);
  assert(report.metrics.settling_ms == 4000U);
  assert(near(report.metrics.hold_mean_demand_percent, 20.0F));
  assert(report.metrics.hold_rms_error_celsius > 0.0F);

  ProcessValidationAssessor wrong_target;
  assert(wrong_target.begin(requirements, identity(21U), 0U, 25.0F));
  for (uint32_t second = 1U; second <= 5U; ++second) {
    assert(wrong_target.sample(second * 1000U, 100.0F, 99.0F,
                               ValidationStage::Hold, 20.0F, true));
  }
  assert(wrong_target.finish(true).status == ValidationStatus::Fail);
}

void testMissingCriteriaStillMeasures() {
  Requirements missing;
  ProcessValidationAssessor assessor;
  RunIdentity defaults;
  assert(assessor.begin(missing, defaults, 0U, 20.0F));
  assert(assessor.sample(500U, 21.0F, 20.0F, ValidationStage::Hold, 10.0F, true));
  assert(assessor.sample(1000U, 22.0F, 20.0F, ValidationStage::Hold, 20.0F, true));
  const ValidationReport report = assessor.finish(true);
  assert(report.status == ValidationStatus::CriteriaMissing);
  assert(report.metrics.peak_measured && near(report.metrics.peak_celsius, 22.0F));
  assert(report.metrics.hold_error_measured);
  assert(report.metrics.hold_mean_error_celsius > 1.0F);
  assert(!report.metrics.heating_slope_measured);
  assert(report.metrics.liquidus_accumulated_ms == 0U);
}

void testEvidenceGapsInvalidAndWrap() {
  ProcessValidationAssessor gap;
  assert(gap.begin(reflowRequirements(), identity(30U), 0U, 90.0F));
  assert(!gap.sample(1001U, 100.0F, 150.0F, ValidationStage::Ramp, 10.0F, true));
  assert(!gap.running());
  const ValidationReport gap_report = gap.finish(true);
  assert(gap_report.status == ValidationStatus::Incomplete);
  assert(gap_report.heating_slope.status == ValidationStatus::Incomplete);

  ProcessValidationAssessor invalid;
  assert(invalid.begin(reflowRequirements(), identity(31U), 0U, 90.0F));
  assert(!invalid.sample(100U, std::numeric_limits<float>::quiet_NaN(), 150.0F,
                         ValidationStage::Ramp, 10.0F, false));
  assert(invalid.finish(true).status == ValidationStatus::Incomplete);

  ProcessValidationAssessor wrap;
  Requirements missing;
  assert(wrap.begin(missing, RunIdentity(), UINT32_MAX - 500U, 20.0F));
  assert(wrap.sample(499U, 21.0F, 20.0F, ValidationStage::Ramp, 0.0F, true));
  const ValidationReport wrapped = wrap.finish(true);
  assert(wrapped.status == ValidationStatus::CriteriaMissing);
  assert(wrapped.metrics.maximum_gap_ms == 1000U);
}

ValidationReport passingReport(uint32_t run_id, uint32_t setup = 1U) {
  ValidationReport report;
  report.identity = identity(run_id, setup);
  report.status = ValidationStatus::Pass;
  report.run_completed = true;
  report.evidence_complete = true;
  return report;
}

void testSequenceAndPersistence() {
  ValidationSequence sequence;
  sequence.reset();
  assert(sequence.record(passingReport(100U)));
  assert(!sequence.record(passingReport(100U)));
  assert(sequence.record(passingReport(101U)));
  assert(!sequence.record(passingReport(100U)));
  assert(sequence.record(passingReport(102U)));
  assert(sequence.consecutivePasses() == 3U);
  assert(sequence.eligible(identity(999U)));
  assert(!sequence.commissioned());
  assert(sequence.commission(identity(999U)));
  assert(sequence.commissioned());

  assert(sequence.record(passingReport(103U)));
  assert(sequence.consecutivePasses() == 3U);
  assert(!sequence.commissioned());
  assert(sequence.commission(identity(999U)));

  const ValidationSequence::State saved = sequence.snapshot();
  ValidationSequence restored;
  assert(restored.restore(saved, identity(999U)));
  assert(restored.consecutivePasses() == 3U);
  assert(restored.commissioned());

  ValidationSequence::State corrupt = saved;
  corrupt.passes = 2U;
  assert(!restored.restore(corrupt, identity(999U)));
  assert(restored.consecutivePasses() == 0U);

  corrupt = saved;
  corrupt.commissioned = true;
  corrupt.passes = 2U;
  corrupt.checksum = ValidationSequence::stateChecksum(corrupt);
  assert(!restored.restore(corrupt, identity(999U)));

  assert(!restored.restore(saved, identity(999U, 2U)));

  sequence.record(passingReport(200U, 2U));
  assert(sequence.consecutivePasses() == 1U);
  ValidationReport failed = passingReport(201U, 2U);
  failed.status = ValidationStatus::Fail;
  assert(sequence.record(failed));
  assert(sequence.consecutivePasses() == 0U);
  assert(!sequence.commissioned());

  ValidationReport unidentified = passingReport(300U);
  unidentified.identity.criteria_revision = 0U;
  assert(sequence.record(unidentified));
  assert(sequence.consecutivePasses() == 0U);
}

void testInvalidRequirements() {
  Requirements requirements = reflowRequirements();
  requirements.source_reference[0] = '\0';
  assert(!validRequirements(requirements));
  requirements = reflowRequirements();
  requirements.heating_slope.window_start_celsius =
      std::numeric_limits<float>::quiet_NaN();
  assert(!validRequirements(requirements));
  requirements = reflowRequirements();
  requirements.soak.criterion.not_applicable = true;
  assert(!validRequirements(requirements));
  requirements = reflowRequirements();
  requirements.heating_slope.criterion = notApplicable();
  requirements.soak.criterion = notApplicable();
  requirements.peak_celsius = notApplicable();
  requirements.liquidus.criterion = notApplicable();
  requirements.cooling_slope.criterion = notApplicable();
  assert(!validRequirements(requirements));

  ProcessValidationAssessor unidentified;
  requirements = reflowRequirements();
  assert(unidentified.begin(requirements, RunIdentity(), 0U, 90.0F));
  assert(unidentified.sample(1000U, 100.0F, 150.0F,
                             ValidationStage::Ramp, 10.0F, true));
  const ValidationReport unidentified_report = unidentified.finish(true);
  assert(unidentified_report.status == ValidationStatus::Incomplete);
  assert(!unidentified_report.evidence_complete);
}

}  // namespace

int main() {
  testReflowMetrics();
  testInterpolatedLiquidus();
  testQuantizedWindowSlope();
  testAnnealMetricsAndTargetIdentity();
  testMissingCriteriaStillMeasures();
  testEvidenceGapsInvalidAndWrap();
  testSequenceAndPersistence();
  testInvalidRequirements();
  std::puts("process_validation tests passed");
  return 0;
}
