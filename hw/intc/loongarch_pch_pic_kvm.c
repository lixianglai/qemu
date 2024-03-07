/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LoongArch kvm pch pic interrupt support
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qemu/typedefs.h"
#include "hw/intc/loongarch_pch_pic.h"
#include "hw/sysbus.h"
#include "linux/kvm.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "sysemu/kvm.h"
#include "hw/loongarch/virt.h"
#include "hw/pci-host/ls7a.h"
#include "qemu/error-report.h"

static void kvm_pch_pic_access_regs(int fd, uint64_t addr,
                                       void *val, bool is_write)
{
        kvm_device_access(fd, KVM_DEV_LOONGARCH_PCH_PIC_GRP_REGS,
                          addr, val, is_write, &error_abort);
}

static void kvm_loongarch_pch_pic_save_load(void *opaque, bool is_write)
{
    KVMLoongArchPCHPIC *s = (KVMLoongArchPCHPIC *)opaque;
    KVMLoongArchPCHPICClass *class = KVM_LOONGARCH_PCH_PIC_GET_CLASS(s);
    int fd = class->dev_fd;
    int addr, offset;

    kvm_pch_pic_access_regs(fd, PCH_PIC_MASK_START,
                            (void *)&s->int_mask, is_write);
    kvm_pch_pic_access_regs(fd, PCH_PIC_HTMSI_EN_START,
                            (void *)&s->htmsi_en, is_write);
    kvm_pch_pic_access_regs(fd, PCH_PIC_EDGE_START,
                            (void *)&s->intedge, is_write);
    kvm_pch_pic_access_regs(fd, PCH_PIC_AUTO_CTRL0_START,
                            (void *)&s->auto_crtl0, is_write);
    kvm_pch_pic_access_regs(fd, PCH_PIC_AUTO_CTRL1_START,
                            (void *)&s->auto_crtl1, is_write);

    for (addr = PCH_PIC_ROUTE_ENTRY_START;
         addr < PCH_PIC_ROUTE_ENTRY_END; addr++) {
        offset = addr - PCH_PIC_ROUTE_ENTRY_START;
        kvm_pch_pic_access_regs(fd, addr,
                                (void *)&s->route_entry[offset], is_write);
    }

    for (addr = PCH_PIC_HTMSI_VEC_START; addr < PCH_PIC_HTMSI_VEC_END; addr++) {
        offset = addr - PCH_PIC_HTMSI_VEC_START;
        kvm_pch_pic_access_regs(fd, addr,
                                (void *)&s->htmsi_vector[offset], is_write);
    }

    kvm_pch_pic_access_regs(fd, PCH_PIC_INT_IRR_START,
                            (void *)&s->intirr, is_write);
    kvm_pch_pic_access_regs(fd, PCH_PIC_INT_ISR_START,
                            (void *)&s->intisr, is_write);
    kvm_pch_pic_access_regs(fd, PCH_PIC_POLARITY_START,
                            (void *)&s->int_polarity, is_write);
}

static int kvm_loongarch_pch_pic_pre_save(void *opaque)
{
    kvm_loongarch_pch_pic_save_load(opaque, false);
    return 0;
}

static int kvm_loongarch_pch_pic_post_load(void *opaque, int version_id)
{
    kvm_loongarch_pch_pic_save_load(opaque, true);
    return 0;
}

static void kvm_pch_pic_handler(void *opaque, int irq, int level)
{
    int kvm_irq;

    if (kvm_enabled()) {
        kvm_irq = \
            (KVM_LOONGARCH_IRQ_TYPE_IOAPIC << KVM_LOONGARCH_IRQ_TYPE_SHIFT)
            | (0 <<  KVM_LOONGARCH_IRQ_VCPU_SHIFT) | irq;
        kvm_set_irq(kvm_state, kvm_irq, !!level);
    }
}

static void kvm_loongarch_pch_pic_realize(DeviceState *dev, Error **errp)
{
    KVMLoongArchPCHPICClass *pch_pic_class =
            KVM_LOONGARCH_PCH_PIC_GET_CLASS(dev);
    struct kvm_create_device cd = {0};
    uint64_t pch_pic_base = VIRT_PCH_REG_BASE;
    Error *err = NULL;
    int ret;

    pch_pic_class->parent_realize(dev, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    if (!pch_pic_class->is_created) {
        cd.type = KVM_DEV_TYPE_LOONGARCH_PCH_PIC;
        ret = kvm_vm_ioctl(kvm_state, KVM_CREATE_DEVICE, &cd);
        if (ret < 0) {
            error_setg_errno(errp, errno,
                             "Creating the KVM pch pic device failed");
            return;
        }
        pch_pic_class->is_created = true;
        pch_pic_class->dev_fd = cd.fd;
        fprintf(stdout, "Create LoongArch pch pic irqchip in KVM done!\n");

        ret = kvm_device_access(cd.fd, KVM_DEV_LOONGARCH_PCH_PIC_GRP_CTRL,
                                KVM_DEV_LOONGARCH_PCH_PIC_CTRL_INIT,
                                &pch_pic_base, true, NULL);
        if (ret < 0) {
            error_report(
                "KVM PCH_PIC: failed to set the base address of PCH PIC");
            exit(1);
        }

        qdev_init_gpio_in(dev, kvm_pch_pic_handler, VIRT_PCH_PIC_IRQ_NUM);
    }
}

static const VMStateDescription vmstate_kvm_loongarch_pch_pic = {
    .name = TYPE_LOONGARCH_PCH_PIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = kvm_loongarch_pch_pic_pre_save,
    .post_load = kvm_loongarch_pch_pic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(int_mask, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(htmsi_en, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(intedge, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(intclr, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(auto_crtl0, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(auto_crtl1, KVMLoongArchPCHPIC),
        VMSTATE_UINT8_ARRAY(route_entry, KVMLoongArchPCHPIC, 64),
        VMSTATE_UINT8_ARRAY(htmsi_vector, KVMLoongArchPCHPIC, 64),
        VMSTATE_UINT64(last_intirr, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(intirr, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(intisr, KVMLoongArchPCHPIC),
        VMSTATE_UINT64(int_polarity, KVMLoongArchPCHPIC),
        VMSTATE_END_OF_LIST()
    }
};


static void kvm_loongarch_pch_pic_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    KVMLoongArchPCHPICClass *pch_pic_class = KVM_LOONGARCH_PCH_PIC_CLASS(oc);

    pch_pic_class->parent_realize = dc->realize;
    dc->realize = kvm_loongarch_pch_pic_realize;
    pch_pic_class->is_created = false;
    dc->vmsd = &vmstate_kvm_loongarch_pch_pic;

}

static const TypeInfo kvm_loongarch_pch_pic_info = {
    .name = TYPE_KVM_LOONGARCH_PCH_PIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KVMLoongArchPCHPIC),
    .class_size = sizeof(KVMLoongArchPCHPICClass),
    .class_init = kvm_loongarch_pch_pic_class_init,
};

static void kvm_loongarch_pch_pic_register_types(void)
{
    type_register_static(&kvm_loongarch_pch_pic_info);
}

type_init(kvm_loongarch_pch_pic_register_types)
