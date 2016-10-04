// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <trace.h>

#include <dev/pcie.h>
#include <mxtl/limits.h>
#include "pch_thermal.h"

#define LOCAL_TRACE 0

/* Vendor and device IDs */
#define INTEL_VID 0x8086
static const uint16_t intel_dids[] = { 0x3a32, 0x9ca4 };

/* Driver state */
struct pch_thermal_context g_pch_thermal_context;

static void pch_thermal_cleanup(struct pch_thermal_context* ctx,
                                const mxtl::RefPtr<pcie_device_state_t>& pci_device) {
    if (pci_device)
        pcie_set_irq_mode_disabled(pci_device);

    if (ctx->regs) {
        /* Disable thermal sensor */
        ctx->regs->tsel = static_cast<uint8_t>(ctx->regs->tsel & ~1u);

        /* Unmap our registers */
        vmm_free_region(ctx->aspace, (vaddr_t)ctx->regs);
    }

    ctx->aspace = NULL;
    ctx->regs   = NULL;
}

static pcie_irq_handler_retval_t pch_thermal_irq_handler(const pcie_device_state_t& dev,
                                                         uint irq_id,
                                                         void* ctx)
{
    TRACEF("Thermal interrupt\n");
    return PCIE_IRQRET_NO_ACTION;
}

static void* pch_thermal_probe(const mxtl::RefPtr<pcie_device_state_t>& pci_device)
{
    /* If we've already claimed a device, do not claim another */
    if (g_pch_thermal_context.device_claimed) {
        return NULL;
    }

    bool claim = false;
    if (pci_device->vendor_id == INTEL_VID) {
        for (uint i = 0; i < countof(intel_dids); ++i) {
            if (pci_device->device_id == intel_dids[i]) {
                claim = true;
                break;
            }
        }
    }

    if (!claim) {
        return NULL;
    }

    g_pch_thermal_context.device_claimed = true;
    return &g_pch_thermal_context;
}

static status_t pch_thermal_startup(const mxtl::RefPtr<pcie_device_state_t>& pci_device)
{
    DEBUG_ASSERT(!g_pch_thermal_context.regs);
    DEBUG_ASSERT(&g_pch_thermal_context == pci_device->driver_ctx);
    status_t status;
    uint64_t size;
    void *vaddr;
    int16_t current_ctt;

    g_pch_thermal_context.aspace = vmm_get_kernel_aspace();

    const pcie_bar_info_t* bar_info = pcie_get_bar_info(*pci_device, 0);
    DEBUG_ASSERT(bar_info && bar_info->bus_addr);

    /* Select legacy IRQ Mode */
    status = pcie_set_irq_mode(pci_device, PCIE_IRQ_MODE_LEGACY, 1);
    if (status != NO_ERROR) {
        TRACEF("Failed to configure PCIe device for Legacy IRQ mode (err = %d)\n", status);
        goto finished;
    }

    /* Register our IRQ handler */
    status = pcie_register_irq_handler(pci_device, 0, pch_thermal_irq_handler, NULL);
    if (status != NO_ERROR) {
        TRACEF("Failed to register Legacy IRQ handler (err = %d)\n", status);
        goto finished;
    }

    size   = ROUNDUP(bar_info->size, PAGE_SIZE);
    vaddr  = nullptr;

    if ((bar_info->bus_addr > mxtl::numeric_limits<paddr_t>::max()) ||
        (size > mxtl::numeric_limits<size_t>::max())) {
        status = ERR_INVALID_ARGS;
        goto finished;
    }

    status = vmm_alloc_physical(
            g_pch_thermal_context.aspace,
            "pch_therm",
            static_cast<size_t>(size),
            &vaddr,
            PAGE_SIZE_SHIFT,
            0,
            static_cast<paddr_t>(bar_info->bus_addr),
            0,
            ARCH_MMU_FLAG_UNCACHED_DEVICE | ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE);
    if (status != NO_ERROR) {
        TRACEF("Failed to map registers (err = %d)\n", status);
        goto finished;
    }
    DEBUG_ASSERT(vaddr);

    pcie_enable_mmio(pci_device, true);

    g_pch_thermal_context.regs = reinterpret_cast<pch_thermal_registers*>(vaddr);

    /* Enable thermal sensor */
    g_pch_thermal_context.regs->tsel |= 1;

    /* Set the catastrophic trip threshold */
    current_ctt = decode_temp(g_pch_thermal_context.regs->ctt & 0x1ff);
    if (current_ctt >= 113) {
        /* The PCH spec suggests we should avoid 120C, but the sensor might be
         * 2C off due to the location of the sensor.  In the range 90C to 120C,
         * the sensor has +- 5C accuracy, so take that into account, too. */
        g_pch_thermal_context.regs->ctt = encode_temp(113);
    }

    /* Enable poweroff on catastrophic threshold trip */
    g_pch_thermal_context.regs->tsc |= 1;

    /* Enable our interrupt */
    status = pcie_unmask_irq(pci_device, 0);
    if (status != NO_ERROR) {
        TRACEF("Failed to unmask IRQ (err = %d)\n", status);
        goto finished;
    }

finished:
    if (status != NO_ERROR)
        pch_thermal_cleanup(&g_pch_thermal_context, pci_device);

    return status;
}

static void pch_thermal_shutdown(const mxtl::RefPtr<pcie_device_state_t>& pci_device)
{
    DEBUG_ASSERT(&g_pch_thermal_context == pci_device->driver_ctx);
    pch_thermal_cleanup(&g_pch_thermal_context, pci_device);
}

static void pch_thermal_release(void* ctx)
{
    DEBUG_ASSERT(&g_pch_thermal_context == ctx);
    DEBUG_ASSERT(!g_pch_thermal_context.regs);
    g_pch_thermal_context.device_claimed = false;
}

static pcie_driver_fn_table_t DRV_FN_TABLE = {
    .pcie_probe_fn = pch_thermal_probe,
    .pcie_startup_fn = pch_thermal_startup,
    .pcie_shutdown_fn = pch_thermal_shutdown,
    .pcie_release_fn = pch_thermal_release,
};

STATIC_PCIE_DRIVER(intel_pch_thermal, "Intel PCH Thermal Sensors", DRV_FN_TABLE);
