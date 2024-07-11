/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LoongArch kvm extioi interrupt support
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qemu/typedefs.h"
#include "hw/intc/loongarch_extioi.h"
#include "hw/sysbus.h"
#include "linux/kvm.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "sysemu/kvm.h"

static void kvm_extioi_access_regs(int fd, uint64_t addr,
                                       void *val, bool is_write)
{
        kvm_device_access(fd, KVM_DEV_LOONGARCH_EXTIOI_GRP_REGS,
                          addr, val, is_write, &error_abort);
}

static void kvm_extioi_access_sw_status(int fd, uint64_t addr,
                                       void *val, bool is_write)
{
        kvm_device_access(fd, KVM_DEV_LOONGARCH_EXTIOI_GRP_SW_STATUS,
                          addr, val, is_write, &error_abort);
}

static void kvm_extioi_save_load_sw_status(void *opaque, bool is_write)
{
    KVMLoongArchExtIOI *s = (KVMLoongArchExtIOI *)opaque;
    KVMLoongArchExtIOIClass *class = KVM_LOONGARCH_EXTIOI_GET_CLASS(s);
    int fd = class->dev_fd;
    int addr;

    addr = KVM_DEV_LOONGARCH_EXTIOI_SW_STATUS_NUM_CPU;
    kvm_extioi_access_sw_status(fd, addr, (void *)&s->num_cpu, is_write);

    addr = KVM_DEV_LOONGARCH_EXTIOI_SW_STATUS_FEATURE;
    kvm_extioi_access_sw_status(fd, addr, (void *)&s->features, is_write);

    addr = KVM_DEV_LOONGARCH_EXTIOI_SW_STATUS_STATE;
    kvm_extioi_access_sw_status(fd, addr, (void *)&s->status, is_write);
}

static void kvm_extioi_save_load_regs(void *opaque, bool is_write)
{
    KVMLoongArchExtIOI *s = (KVMLoongArchExtIOI *)opaque;
    KVMLoongArchExtIOIClass *class = KVM_LOONGARCH_EXTIOI_GET_CLASS(s);
    int fd = class->dev_fd;
    int addr, offset, cpuid;

    for (addr = EXTIOI_NODETYPE_START; addr < EXTIOI_NODETYPE_END; addr += 4) {
        offset = (addr - EXTIOI_NODETYPE_START) / 4;
        kvm_extioi_access_regs(fd, addr,
                               (void *)&s->nodetype[offset], is_write);
    }

    for (addr = EXTIOI_IPMAP_START; addr < EXTIOI_IPMAP_END; addr += 4) {
        offset = (addr - EXTIOI_IPMAP_START) / 4;
        kvm_extioi_access_regs(fd, addr, (void *)&s->ipmap[offset], is_write);
    }

    for (addr = EXTIOI_ENABLE_START; addr < EXTIOI_ENABLE_END; addr += 4) {
        offset = (addr - EXTIOI_ENABLE_START) / 4;
        kvm_extioi_access_regs(fd, addr,
                               (void *)&s->enable[offset], is_write);
    }

    for (addr = EXTIOI_BOUNCE_START; addr < EXTIOI_BOUNCE_END; addr += 4) {
        offset = (addr - EXTIOI_BOUNCE_START) / 4;
        kvm_extioi_access_regs(fd, addr,
                               (void *)&s->bounce[offset], is_write);
    }

    for (addr = EXTIOI_ISR_START; addr < EXTIOI_ISR_END; addr += 4) {
        offset = (addr - EXTIOI_ISR_START) / 4;
        kvm_extioi_access_regs(fd, addr,
                               (void *)&s->isr[offset], is_write);
    }

    for (addr = EXTIOI_COREMAP_START; addr < EXTIOI_COREMAP_END; addr += 4) {
        offset = (addr - EXTIOI_COREMAP_START) / 4;
        kvm_extioi_access_regs(fd, addr,
                               (void *)&s->coremap[offset], is_write);
    }

    for (cpuid = 0; cpuid < s->num_cpu; cpuid++) {
        for (addr = EXTIOI_COREISR_START;
             addr < EXTIOI_COREISR_END; addr += 4) {
            offset = (addr - EXTIOI_COREISR_START) / 4;
            addr = (cpuid << 16) | addr;
            kvm_extioi_access_regs(fd, addr,
                              (void *)&s->coreisr[cpuid][offset], is_write);
        }
    }
}

static int kvm_loongarch_extioi_pre_save(void *opaque)
{
    kvm_extioi_save_load_regs(opaque, false);
    kvm_extioi_save_load_sw_status(opaque, false);
    return 0;
}

static int kvm_loongarch_extioi_post_load(void *opaque, int version_id)
{
    KVMLoongArchExtIOI *s = (KVMLoongArchExtIOI *)opaque;
    KVMLoongArchExtIOIClass *class = KVM_LOONGARCH_EXTIOI_GET_CLASS(s);
    int fd = class->dev_fd;

    kvm_extioi_save_load_regs(opaque, true);
    kvm_extioi_save_load_sw_status(opaque, true);

    kvm_device_access(fd, KVM_DEV_LOONGARCH_EXTIOI_GRP_CTRL,
                      KVM_DEV_LOONGARCH_EXTIOI_CTRL_LOAD_FINISHED,
                      NULL, true, &error_abort);
    return 0;
}

static void kvm_loongarch_extioi_realize(DeviceState *dev, Error **errp)
{
    KVMLoongArchExtIOIClass *extioi_class = KVM_LOONGARCH_EXTIOI_GET_CLASS(dev);
    KVMLoongArchExtIOI *s = KVM_LOONGARCH_EXTIOI(dev);
    struct kvm_create_device cd = {0};
    Error *err = NULL;
    int ret, i;

    extioi_class->parent_realize(dev, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    if (s->num_cpu == 0) {
        error_setg(errp, "num-cpu must be at least 1");
        return;
    }


    if (extioi_class->is_created) {
        error_setg(errp, "extioi had be created");
        return;
    }

    if (s->features & BIT(EXTIOI_HAS_VIRT_EXTENSION)) {
        s->features |= EXTIOI_VIRT_HAS_FEATURES;
    }

    cd.type = KVM_DEV_TYPE_LOONGARCH_EXTIOI;
    ret = kvm_vm_ioctl(kvm_state, KVM_CREATE_DEVICE, &cd);
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "Creating the KVM extioi device failed");
        return;
    }
    extioi_class->is_created = true;
    extioi_class->dev_fd = cd.fd;

    ret = kvm_device_access(cd.fd, KVM_DEV_LOONGARCH_EXTIOI_GRP_CTRL,
                            KVM_DEV_LOONGARCH_EXTIOI_CTRL_INIT_NUM_CPU,
                            &s->num_cpu, true, NULL);
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "KVM EXTIOI: failed to set the num-cpu of EXTIOI");
        exit(1);
    }

    ret = kvm_device_access(cd.fd, KVM_DEV_LOONGARCH_EXTIOI_GRP_CTRL,
                            KVM_DEV_LOONGARCH_EXTIOI_CTRL_INIT_FEATURE,
                            &s->features, true, NULL);
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "KVM EXTIOI: failed to set the feature of EXTIOI");
        exit(1);
    }

    fprintf(stdout, "Create LoongArch extioi irqchip in KVM done!\n");

    kvm_async_interrupts_allowed = true;
    kvm_msi_via_irqfd_allowed = kvm_irqfds_enabled();
    if (kvm_has_gsi_routing()) {
        for (i = 0; i < 64; ++i) {
            kvm_irqchip_add_irq_route(kvm_state, i, 0, i);
        }
        kvm_gsi_routing_allowed = true;
    }
}

static const VMStateDescription vmstate_kvm_extioi_core = {
    .name = "kvm-extioi-single",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = kvm_loongarch_extioi_pre_save,
    .post_load = kvm_loongarch_extioi_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(nodetype, KVMLoongArchExtIOI,
                             EXTIOI_IRQS_NODETYPE_COUNT / 2),
        VMSTATE_UINT32_ARRAY(bounce, KVMLoongArchExtIOI,
                             EXTIOI_IRQS_GROUP_COUNT),
        VMSTATE_UINT32_ARRAY(isr, KVMLoongArchExtIOI, EXTIOI_IRQS / 32),
        VMSTATE_UINT32_2DARRAY(coreisr, KVMLoongArchExtIOI, EXTIOI_CPUS,
                               EXTIOI_IRQS_GROUP_COUNT),
        VMSTATE_UINT32_ARRAY(enable, KVMLoongArchExtIOI, EXTIOI_IRQS / 32),
        VMSTATE_UINT32_ARRAY(ipmap, KVMLoongArchExtIOI,
                             EXTIOI_IRQS_IPMAP_SIZE / 4),
        VMSTATE_UINT32_ARRAY(coremap, KVMLoongArchExtIOI, EXTIOI_IRQS / 4),
        VMSTATE_UINT8_ARRAY(sw_coremap, KVMLoongArchExtIOI, EXTIOI_IRQS),
        VMSTATE_UINT32(features, KVMLoongArchExtIOI),
        VMSTATE_UINT32(status, KVMLoongArchExtIOI),
        VMSTATE_END_OF_LIST()
    }
};

static Property extioi_properties[] = {
    DEFINE_PROP_UINT32("num-cpu", KVMLoongArchExtIOI, num_cpu, 1),
    DEFINE_PROP_BIT("has-virtualization-extension", KVMLoongArchExtIOI,
                    features, EXTIOI_HAS_VIRT_EXTENSION, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void kvm_loongarch_extioi_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    KVMLoongArchExtIOIClass *extioi_class = KVM_LOONGARCH_EXTIOI_CLASS(oc);

    extioi_class->parent_realize = dc->realize;
    dc->realize = kvm_loongarch_extioi_realize;
    extioi_class->is_created = false;
    device_class_set_props(dc, extioi_properties);
    dc->vmsd = &vmstate_kvm_extioi_core;
}

static const TypeInfo kvm_loongarch_extioi_info = {
    .name = TYPE_KVM_LOONGARCH_EXTIOI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KVMLoongArchExtIOI),
    .class_size = sizeof(KVMLoongArchExtIOIClass),
    .class_init = kvm_loongarch_extioi_class_init,
};

static void kvm_loongarch_extioi_register_types(void)
{
    type_register_static(&kvm_loongarch_extioi_info);
}

type_init(kvm_loongarch_extioi_register_types)
