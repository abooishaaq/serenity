/*
 * Copyright (c) 2021-2022, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/BuiltinWrappers.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <Kernel/Bus/PCI/API.h>
#include <Kernel/CommandLine.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Storage/ATA/AHCIController.h>
#include <Kernel/Storage/ATA/AHCIInterruptHandler.h>

namespace Kernel {

UNMAP_AFTER_INIT NonnullRefPtr<AHCIController> AHCIController::initialize(PCI::DeviceIdentifier const& pci_device_identifier)
{
    auto controller = adopt_ref_if_nonnull(new (nothrow) AHCIController(pci_device_identifier)).release_nonnull();
    controller->initialize_hba(pci_device_identifier);
    return controller;
}

bool AHCIController::reset()
{
    hba().control_regs.ghc = 1;

    dbgln_if(AHCI_DEBUG, "{}: AHCI Controller reset", pci_address());

    full_memory_barrier();
    size_t retry = 0;

    while (true) {
        if (retry > 1000)
            return false;
        if (!(hba().control_regs.ghc & 1))
            break;
        IO::delay(1000);
        retry++;
    }
    // The HBA is locked or hung if we waited more than 1 second!
    return true;
}

bool AHCIController::shutdown()
{
    TODO();
}

size_t AHCIController::devices_count() const
{
    size_t count = 0;
    for (auto port : m_ports) {
        if (port && port->connected_device())
            count++;
    }
    return count;
}

void AHCIController::start_request(ATADevice const& device, AsyncBlockDeviceRequest& request)
{
    auto port = m_ports[device.ata_address().port];
    VERIFY(port);
    port->start_request(request);
}

void AHCIController::complete_current_request(AsyncDeviceRequest::RequestResult)
{
    VERIFY_NOT_REACHED();
}

volatile AHCI::PortRegisters& AHCIController::port(size_t port_number) const
{
    VERIFY(port_number < (size_t)AHCI::Limits::MaxPorts);
    return static_cast<volatile AHCI::PortRegisters&>(hba().port_regs[port_number]);
}

volatile AHCI::HBA& AHCIController::hba() const
{
    return static_cast<volatile AHCI::HBA&>(*(volatile AHCI::HBA*)(m_hba_region->vaddr().as_ptr()));
}

UNMAP_AFTER_INIT AHCIController::AHCIController(PCI::DeviceIdentifier const& pci_device_identifier)
    : ATAController()
    , PCI::Device(pci_device_identifier.address())
    , m_hba_region(default_hba_region())
    , m_hba_capabilities(capabilities())
{
}

AHCI::HBADefinedCapabilities AHCIController::capabilities() const
{
    u32 capabilities = hba().control_regs.cap;
    u32 extended_capabilities = hba().control_regs.cap2;

    dbgln_if(AHCI_DEBUG, "{}: AHCI Controller Capabilities = {:#08x}, Extended Capabilities = {:#08x}", pci_address(), capabilities, extended_capabilities);

    return (AHCI::HBADefinedCapabilities) {
        (capabilities & 0b11111) + 1,
        ((capabilities >> 8) & 0b11111) + 1,
        (u8)((capabilities >> 20) & 0b1111),
        (capabilities & (u32)(AHCI::HBACapabilities::SXS)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::EMS)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::CCCS)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::PSC)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SSC)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::PMD)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::FBSS)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SPM)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SAM)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SCLO)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SAL)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SALP)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SSS)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SMPS)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SSNTF)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::SNCQ)) != 0,
        (capabilities & (u32)(AHCI::HBACapabilities::S64A)) != 0,
        (extended_capabilities & (u32)(AHCI::HBACapabilitiesExtended::BOH)) != 0,
        (extended_capabilities & (u32)(AHCI::HBACapabilitiesExtended::NVMP)) != 0,
        (extended_capabilities & (u32)(AHCI::HBACapabilitiesExtended::APST)) != 0,
        (extended_capabilities & (u32)(AHCI::HBACapabilitiesExtended::SDS)) != 0,
        (extended_capabilities & (u32)(AHCI::HBACapabilitiesExtended::SADM)) != 0,
        (extended_capabilities & (u32)(AHCI::HBACapabilitiesExtended::DESO)) != 0
    };
}

UNMAP_AFTER_INIT NonnullOwnPtr<Memory::Region> AHCIController::default_hba_region() const
{
    return MM.allocate_kernel_region(PhysicalAddress(PCI::get_BAR5(pci_address())).page_base(), Memory::page_round_up(sizeof(AHCI::HBA)).release_value_but_fixme_should_propagate_errors(), "AHCI HBA", Memory::Region::Access::ReadWrite).release_value();
}

AHCIController::~AHCIController() = default;

UNMAP_AFTER_INIT void AHCIController::initialize_hba(PCI::DeviceIdentifier const& pci_device_identifier)
{
    if (!reset()) {
        dmesgln("{}: AHCI controller reset failed", pci_address());
        return;
    }
    dmesgln("{}: AHCI controller reset", pci_address());
    dbgln("{}: AHCI command list entries count - {}", pci_address(), m_hba_capabilities.max_command_list_entries_count);

    u32 version = hba().control_regs.version;
    dbgln_if(AHCI_DEBUG, "{}: AHCI Controller Version = {:#08x}", pci_address(), version);

    hba().control_regs.ghc = 0x80000000; // Ensure that HBA knows we are AHCI aware.
    PCI::enable_interrupt_line(pci_address());
    PCI::enable_bus_mastering(pci_address());
    enable_global_interrupts();

    auto implemented_ports = AHCI::MaskedBitField((u32 volatile&)(hba().control_regs.pi));
    m_irq_handler = AHCIInterruptHandler::create(*this, pci_device_identifier.interrupt_line().value(), implemented_ports).release_value_but_fixme_should_propagate_errors();

    if (kernel_command_line().ahci_reset_mode() == AHCIResetMode::Aggressive) {
        for (auto index : implemented_ports.to_vector()) {
            auto port = AHCIPort::create(*this, m_hba_capabilities, static_cast<volatile AHCI::PortRegisters&>(hba().port_regs[index]), index).release_value_but_fixme_should_propagate_errors();
            m_ports[index] = port;
            port->reset();
        }
        return;
    }
    for (auto index : implemented_ports.to_vector()) {
        auto port = AHCIPort::create(*this, m_hba_capabilities, static_cast<volatile AHCI::PortRegisters&>(hba().port_regs[index]), index).release_value_but_fixme_should_propagate_errors();
        m_ports[index] = port;
        port->initialize_without_reset();
    }
}

void AHCIController::handle_interrupt_for_port(Badge<AHCIInterruptHandler>, u32 port_index) const
{
    auto port = m_ports[port_index];
    VERIFY(port);
    port->handle_interrupt();
}

void AHCIController::disable_global_interrupts() const
{
    hba().control_regs.ghc = hba().control_regs.ghc & 0xfffffffd;
}
void AHCIController::enable_global_interrupts() const
{
    hba().control_regs.ghc = hba().control_regs.ghc | (1 << 1);
}

RefPtr<StorageDevice> AHCIController::device_by_port(u32 port_index) const
{
    auto port = m_ports[port_index];
    if (!port)
        return {};
    return port->connected_device();
}

RefPtr<StorageDevice> AHCIController::device(u32 index) const
{
    NonnullRefPtrVector<StorageDevice> connected_devices;
    u32 pi = hba().control_regs.pi;
    u32 bit = bit_scan_forward(pi);
    while (bit) {
        dbgln_if(AHCI_DEBUG, "Checking implemented port {}, pi {:b}", bit - 1, pi);
        pi &= ~(1u << (bit - 1));
        auto checked_device = device_by_port(bit - 1);
        bit = bit_scan_forward(pi);
        if (checked_device.is_null())
            continue;
        connected_devices.append(checked_device.release_nonnull());
    }
    dbgln_if(AHCI_DEBUG, "Connected device count: {}, Index: {}", connected_devices.size(), index);
    if (index >= connected_devices.size())
        return nullptr;
    return connected_devices[index];
}

}
