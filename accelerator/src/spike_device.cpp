// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Bridges Spike's external MMIO device API to the SystemC accelerator module.

#include "accelerator.h"
#include "memory_interface.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "abstract_device.h"
#include "memif.h"
#include "sim.h"
#include "simif.h"

#include <sysc/communication/sc_port.h>
#include <sysc/kernel/sc_dynamic_processes.h>
#include <sysc/kernel/sc_module.h>
#include <sysc/kernel/sc_module_name.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_spawn.h>
#include <sysc/kernel/sc_time.h>
#include <sysc/kernel/sc_wait.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_phase.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

/* Keeps all guest-memory access at the Spike boundary; Accelerator only sees this ABI. */
class SpikeMemory final : public MemoryInterface {
 public:
  explicit SpikeMemory(simif_t *simulator) : simulator_(simulator) {}

  bool read(uint64_t address, void *destination, size_t size) override {
    return simulator_->mmio_load(address, size, static_cast<uint8_t *>(destination));
  }

  bool write(uint64_t address, const void *source, size_t size) override {
    return simulator_->mmio_store(address, size, static_cast<const uint8_t *>(source));
  }

 private:
  simif_t *simulator_;
};

class PcaaDevice final : public sc_core::sc_module, public abstract_device_t {
 public:
  PcaaDevice(simif_t *simulator, reg_t size, sc_core::sc_module_name name)
      : sc_core::sc_module(name),
        control_socket("control_socket"),
        memory_(simulator),
        accelerator_("pcaa_accelerator", memory_),
        size_(size) {
    control_socket.bind(accelerator_.target_socket);
  }

  bool load(reg_t address, size_t length, uint8_t *bytes) override {
    return transport(address, length, bytes, tlm::TLM_READ_COMMAND);
  }

  bool store(reg_t address, size_t length, const uint8_t *bytes) override {
    return transport(address, length, const_cast<uint8_t *>(bytes), tlm::TLM_WRITE_COMMAND);
  }

  reg_t size() override {
    return size_;
  }

 private:
  bool transport(reg_t address, size_t length, uint8_t *data, tlm::tlm_command command) {
    tlm::tlm_generic_payload transaction;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    transaction.set_command(command);
    transaction.set_address(address);
    transaction.set_data_ptr(data);
    transaction.set_data_length(length);
    transaction.set_streaming_width(length);
    control_socket->b_transport(transaction, delay);
    return transaction.get_response_status() == tlm::TLM_OK_RESPONSE;
  }

  tlm_utils::simple_initiator_socket<PcaaDevice> control_socket;
  SpikeMemory memory_;
  Accelerator accelerator_;
  reg_t size_;
};

// Spike's registration macro requires this exact conventional type name.
using pcaa_t = PcaaDevice;

struct DeviceConfiguration {
  reg_t base;
  reg_t size;
};

std::optional<DeviceConfiguration> parse_device_configuration(
    const std::vector<std::string> &arguments) {
  constexpr size_t kArgumentCount = 2;
  if (arguments.size() != kArgumentCount) {
    return std::nullopt;
  }

  try {
    return DeviceConfiguration{
        std::stoull(arguments[0], nullptr, 0),
        std::stoull(arguments[1], nullptr, 0),
    };
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

pcaa_t *pcaa_parse_from_fdt(const void *, const sim_t *sim, reg_t *base,
                            const std::vector<std::string> &arguments) {
  const auto configuration = parse_device_configuration(arguments);
  if (!configuration || configuration->size == 0) {
    return nullptr;
  }

  *base = configuration->base;
  // Spike's factory API uses const sim_t*, although the plugin is given the live simulator.
  auto *mutable_simulator = const_cast<sim_t *>(sim);
  return new pcaa_t(static_cast<simif_t *>(mutable_simulator), configuration->size,
                    sc_core::sc_module_name("pcaa_device"));
}

std::string pcaa_generate_dts(const sim_t *, const std::vector<std::string> &args) {
  const auto configuration = parse_device_configuration(args);
  if (!configuration || configuration->size == 0) {
    return {};
  }

  std::ostringstream dts;
  dts << std::hex << "    pcaa@" << configuration->base << " {\n"
      << "      compatible = \"pcaa\";\n"
      << "      reg = <0x0 0x" << configuration->base << " 0x0 0x" << configuration->size << ">;\n"
      << "      spike,plugin-params = \"" << args[0] << ',' << args[1] << "\";\n"
      << "    };\n";
  return dts.str();
}

REGISTER_DEVICE(pcaa, pcaa_parse_from_fdt, pcaa_generate_dts)
