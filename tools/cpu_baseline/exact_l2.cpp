// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Adapts the exact driver to the unchanged structural SystemC device.
#include "exact.h"
#include "pbqp_model_kernel.h"
#include "l2_accelerator.h"
#include "timing_model.h"
#include <limits>
#include <systemc>
namespace {
class L2Recorder final : public cpu_baseline::DeviceRecorder {
 public:
  L2Recorder() : model_(false, {}, std::numeric_limits<size_t>::max(), &config_) {
    model_.enable_measurements();
    sc_core::sc_start(sc_core::SC_ZERO_TIME);
  }
  void MakeKernel(pbqp_cost_kernel_t &kernel) override {
    model_.make_kernel(kernel);
  }
  void Before() override {
    before_ = model_.l2_statistics();
  }
  cpu_baseline::DeviceEvent After() override {
    const auto &after = model_.l2_statistics();
    const auto &host = model_.measurements().back();
    cpu_baseline::DeviceEvent event;
    event.cycles = after.cycles - before_.cycles;
    event.host_ns = host.host_ns;
    event.staging_ns = host.staging_ns;
    event.build_ns = host.submission_build_ns;
    event.readback_ns = host.readback_ns;
    for (size_t i = 0; i < after.phase_cycles.size(); ++i)
      event.phases.push_back(after.phase_cycles[i] - before_.phase_cycles[i]);
    for (size_t i = 0; i < after.primitives.size(); ++i)
      event.opcodes.push_back(after.primitives[i] - before_.primitives[i]);
    return event;
  }
  void WriteSummary(std::ostream &out) const override {
    l2_write_json(out, config_, model_.l2_statistics());
  }

 private:
  L2Config config_;
  ModelKernel model_;
  L2Statistics before_;
};
}  // namespace
int sc_main(int argc, char **argv) {
  L2Recorder recorder;
  return cpu_baseline::RunExact(argc, argv, &recorder);
}
