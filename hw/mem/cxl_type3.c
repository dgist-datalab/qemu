#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "hw/mem/memory-device.h"
#include "hw/mem/pc-dimm.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/range.h"
#include "qemu/rcu.h"
#include "sysemu/hostmem.h"
#include "hw/cxl/cxl.h"

typedef struct cxl_type3_dev {
    /* Private */
    PCIDevice parent_obj;

    /* Properties */
    uint64_t size;
    HostMemoryBackend *hostmem;

    /* State */
    CXLComponentState cxl_cstate;
    CXLDeviceState cxl_dstate;
} CXLType3Dev;

#define CT3(obj) OBJECT_CHECK(CXLType3Dev, (obj), TYPE_CXL_TYPE3_DEV)

static void build_dvsecs(CXLType3Dev *ct3d)
{
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    uint8_t *dvsec;

    dvsec = (uint8_t *)&(struct cxl_dvsec_device){
        .cap = 0x1e,
        .ctrl = 0x6,
        .status2 = 0x2,
        .range1_size_hi = 0,
        .range1_size_lo = (2 << 5) | (2 << 2) | 0x3 | ct3d->size,
        .range1_base_hi = 0,
        .range1_base_lo = 0,
    };
    cxl_component_create_dvsec(cxl_cstate, PCIE_CXL_DEVICE_DVSEC_LENGTH,
                               PCIE_CXL_DEVICE_DVSEC,
                               PCIE_CXL2_DEVICE_DVSEC_REVID, dvsec);

    dvsec = (uint8_t *)&(struct cxl_dvsec_register_locator){
        .rsvd         = 0,
        .reg0_base_lo = RBI_COMPONENT_REG | CXL_COMPONENT_REG_BAR_IDX,
        .reg0_base_hi = 0,
        .reg1_base_lo = RBI_CXL_DEVICE_REG | CXL_DEVICE_REG_BAR_IDX,
        .reg1_base_hi = 0,
    };
    cxl_component_create_dvsec(cxl_cstate, REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                               REG_LOC_DVSEC_REVID, dvsec);
}

static void ct3_instance_init(Object *obj)
{
    /* MemoryDeviceClass *mdc = MEMORY_DEVICE_GET_CLASS(obj); */
}

static void ct3_finalize(Object *obj)
{
    CXLType3Dev *ct3d = CT3(obj);

    g_free(ct3d->cxl_dstate.pmem);
}

#ifdef SET_PMEM_PADDR
static void cxl_set_addr(CXLType3Dev *ct3d, hwaddr addr, Error **errp)
{
    MemoryDeviceClass *mdc = MEMORY_DEVICE_GET_CLASS(ct3d);
    mdc->set_addr(MEMORY_DEVICE(ct3d), addr, errp);
}
#endif

static void cxl_setup_memory(CXLType3Dev *ct3d, Error **errp)
{
    MemoryRegionSection mrs;
    MemoryRegion *pmem;
    MemoryRegion *mr;
    uint64_t offset = 0;
    size_t remaining_size;

    if (!ct3d->hostmem) {
        error_setg(errp, "memdev property must be set");
        return;
    }

    /* FIXME: need to check mr is the host bridge's MR */
    mr = host_memory_backend_get_memory(ct3d->hostmem);

    /* Create our new subregion */
    pmem = g_new(MemoryRegion, 1);

    /* Find the first free space in the window */
    WITH_RCU_READ_LOCK_GUARD()
    {
        mrs = memory_region_find(mr, offset, 1);
        while (mrs.mr && mrs.mr != mr) {
            offset += memory_region_size(mrs.mr);
            mrs = memory_region_find(mr, offset, 1);
        }
    }

    remaining_size = memory_region_size(mr) - offset;
    if (remaining_size < ct3d->size) {
        g_free(pmem);
        error_setg(errp,
                   "Not enough free space (%zd) required for device (%" PRId64  ")",
                   remaining_size, ct3d->size);
    }

    memory_region_set_nonvolatile(pmem, true);
    memory_region_set_enabled(pmem, false);
    memory_region_init_alias(pmem, OBJECT(ct3d), "cxl_type3-memory", mr, 0,
                             ct3d->size);
    ct3d->cxl_dstate.pmem = pmem;

#ifdef SET_PMEM_PADDR
    /* This path will initialize the memory device as if BIOS had done it */
    cxl_set_addr(ct3d, mr->addr + offset, errp);
    memory_region_set_enabled(pmem, true);
#endif
}

static MemoryRegion *cxl_md_get_memory_region(MemoryDeviceState *md,
                                              Error **errp)
{
    CXLType3Dev *ct3d = CT3(md);

    if (!ct3d->cxl_dstate.pmem) {
        cxl_setup_memory(ct3d, errp);
    }

    return ct3d->cxl_dstate.pmem;
}

static void ct3_realize(PCIDevice *pci_dev, Error **errp)
{
    CXLType3Dev *ct3d = CT3(pci_dev);
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    ComponentRegisters *regs = &cxl_cstate->crb;
    MemoryRegion *mr = &regs->component_registers;
    uint8_t *pci_conf = pci_dev->config;

    if (!ct3d->cxl_dstate.pmem) {
        cxl_setup_memory(ct3d, errp);
    }

    pci_config_set_prog_interface(pci_conf, 0x10);
    pci_config_set_class(pci_conf, PCI_CLASS_MEMORY_CXL);

    pcie_endpoint_cap_init(pci_dev, 0x80);
    cxl_cstate->dvsec_offset = 0x100;

    ct3d->cxl_cstate.pdev = pci_dev;
    build_dvsecs(ct3d);

    cxl_component_register_block_init(OBJECT(pci_dev), cxl_cstate,
                                      TYPE_CXL_TYPE3_DEV);

    pci_register_bar(
        pci_dev, CXL_COMPONENT_REG_BAR_IDX,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, mr);

    cxl_device_register_block_init(OBJECT(pci_dev), &ct3d->cxl_dstate);
    pci_register_bar(pci_dev, CXL_DEVICE_REG_BAR_IDX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &ct3d->cxl_dstate.device_registers);
}

static uint64_t cxl_md_get_addr(const MemoryDeviceState *md)
{
    CXLType3Dev *ct3d = CT3(md);
    MemoryRegion *pmem = ct3d->cxl_dstate.pmem;

    assert(pmem->alias);
    return pmem->alias_offset;
}

static void cxl_md_set_addr(MemoryDeviceState *md, uint64_t addr, Error **errp)
{
    CXLType3Dev *ct3d = CT3(md);
    MemoryRegion *pmem = ct3d->cxl_dstate.pmem;

    assert(pmem->alias);
    memory_region_set_alias_offset(pmem, addr);
    memory_region_set_address(pmem, addr);
}

static void ct3d_reset(DeviceState *dev)
{
    CXLType3Dev *ct3d = CT3(dev);
    uint32_t *reg_state = ct3d->cxl_cstate.crb.cache_mem_registers;

    cxl_component_register_init_common(reg_state, CXL2_TYPE3_DEVICE);
    cxl_device_register_init_common(&ct3d->cxl_dstate);
}

static Property ct3_props[] = {
    DEFINE_PROP_SIZE("size", CXLType3Dev, size, -1),
    DEFINE_PROP_LINK("memdev", CXLType3Dev, hostmem, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_END_OF_LIST(),
};

static void pc_dimm_md_fill_device_info(const MemoryDeviceState *md,
                                        MemoryDeviceInfo *info)
{
    PCDIMMDeviceInfo *di = g_new0(PCDIMMDeviceInfo, 1);
    const DeviceClass *dc = DEVICE_GET_CLASS(md);
    const DeviceState *dev = DEVICE(md);
    CXLType3Dev *ct3d = CT3(md);

    if (dev->id) {
        di->has_id = true;
        di->id = g_strdup(dev->id);
    }

    di->hotplugged = dev->hotplugged;
    di->hotpluggable = dc->hotpluggable;
    di->addr = cxl_md_get_addr(md);
    di->slot = 0;
    di->node = 0;
    di->size = memory_device_get_region_size(md, NULL);
    di->memdev = object_get_canonical_path(OBJECT(ct3d->hostmem));

    info->u.cxl.data = di;
    info->type = MEMORY_DEVICE_INFO_KIND_CXL;
}

static void ct3_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    MemoryDeviceClass *mdc = MEMORY_DEVICE_CLASS(oc);

    pc->realize = ct3_realize;
    pc->class_id = PCI_CLASS_STORAGE_EXPRESS;
    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->device_id = 0xd93; /* LVF for now */
    pc->revision = 1;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "CXL PMEM Device (Type 3)";
    dc->reset = ct3d_reset;
    device_class_set_props(dc, ct3_props);

    mdc->get_memory_region = cxl_md_get_memory_region;
    mdc->get_addr = cxl_md_get_addr;
    mdc->fill_device_info = pc_dimm_md_fill_device_info;
    mdc->get_plugged_size = memory_device_get_region_size;
    mdc->set_addr = cxl_md_set_addr;
}

static const TypeInfo ct3d_info = {
    .name = TYPE_CXL_TYPE3_DEV,
    .parent = TYPE_PCI_DEVICE,
    .class_init = ct3_class_init,
    .instance_size = sizeof(CXLType3Dev),
    .instance_init = ct3_instance_init,
    .instance_finalize = ct3_finalize,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_MEMORY_DEVICE },
        { INTERFACE_CXL_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        {}
    },
};

static void ct3d_registers(void)
{
    type_register_static(&ct3d_info);
}

type_init(ct3d_registers);
