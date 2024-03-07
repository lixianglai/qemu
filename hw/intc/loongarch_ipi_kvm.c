/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LoongArch IPI interrupt support
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#include "qemu/osdep.h"
#include "hw/boards.h"
#include "sysemu/kvm.h"
#include "qapi/error.h"
#include "hw/intc/loongarch_ipi.h"
#include "target/loongarch/cpu.h"

static AddressSpace *get_iocsr_as(CPUState *cpu)
{
    return LOONGARCH_CPU(cpu)->env.address_space_iocsr;
}

static void kvm_ipi_access_regs(int fd, uint64_t addr,
                                uint32_t *val, bool is_write)
{
    kvm_device_access(fd, KVM_DEV_LOONGARCH_IPI_GRP_REGS,
                          addr, val, is_write, &error_abort);
}
static void kvm_loongarch_ipi_save_load_regs(void *opaque, bool is_write)
{
    LoongsonIPICommonState *ipi = (LoongsonIPICommonState *)opaque;
    KVMLoongarchIPIState *s = KVM_LOONGARCH_IPI(opaque);
    IPICore *cpu;
    uint64_t attr;
    int cpu_id = 0;
    int fd = s->dev_fd;

    for (cpu_id = 0; cpu_id < ipi->num_cpu; cpu_id++) {
        cpu = &ipi->cpu[cpu_id];
        attr = (cpu_id << 16) | CORE_STATUS_OFF;
        kvm_ipi_access_regs(fd, attr, &cpu->status, is_write);

        attr = (cpu_id << 16) | CORE_EN_OFF;
        kvm_ipi_access_regs(fd, attr, &cpu->en, is_write);

        attr = (cpu_id << 16) | CORE_SET_OFF;
        kvm_ipi_access_regs(fd, attr, &cpu->set, is_write);

        attr = (cpu_id << 16) | CORE_CLEAR_OFF;
        kvm_ipi_access_regs(fd, attr, &cpu->clear, is_write);

        attr = (cpu_id << 16) | CORE_BUF_20;
        kvm_ipi_access_regs(fd, attr, &cpu->buf[0], is_write);

        attr = (cpu_id << 16) | CORE_BUF_28;
        kvm_ipi_access_regs(fd, attr, &cpu->buf[2], is_write);

        attr = (cpu_id << 16) | CORE_BUF_30;
        kvm_ipi_access_regs(fd, attr, &cpu->buf[4], is_write);

        attr = (cpu_id << 16) | CORE_BUF_38;
        kvm_ipi_access_regs(fd, attr, &cpu->buf[6], is_write);
    }
}

static void kvm_loongarch_ipi_pre_save(LoongsonIPICommonState *opaque)
{
    kvm_loongarch_ipi_save_load_regs(opaque, false);
}

static void kvm_loongarch_ipi_post_load(LoongsonIPICommonState *opaque,
                                        int version_id)
{
    kvm_loongarch_ipi_save_load_regs(opaque, true);
}

static void kvm_loongarch_ipi_realize(DeviceState *dev, Error **errp)
{
    KVMLoongarchIPIState *s = KVM_LOONGARCH_IPI(dev);
    KVMLoongArchIPIClass *klic = KVM_LOONGARCH_IPI_GET_CLASS(dev);
    struct kvm_create_device cd = {0};
    Error *local_err = NULL;
    int ret;

    klic->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    cd.type = KVM_DEV_TYPE_LOONGARCH_IPI;
    ret = kvm_vm_ioctl(kvm_state, KVM_CREATE_DEVICE, &cd);
    if (ret < 0) {
        error_setg_errno(errp, errno, "Creating the KVM device failed");
        return;
    }
    s->dev_fd = cd.fd;
}

static void kvm_loongarch_ipi_unrealize(DeviceState *dev)
{
    KVMLoongArchIPIClass *klic = KVM_LOONGARCH_IPI_GET_CLASS(dev);
    klic->parent_unrealize(dev);
}

static void kvm_loongarch_ipi_class_init(ObjectClass *klass, void *data)
{
    LoongsonIPICommonClass *licc = LOONGSON_IPI_COMMON_CLASS(klass);
    KVMLoongArchIPIClass *klic = KVM_LOONGARCH_IPI_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_parent_realize(dc, kvm_loongarch_ipi_realize,
                                    &klic->parent_realize);
    device_class_set_parent_unrealize(dc, kvm_loongarch_ipi_unrealize,
                                    &klic->parent_unrealize);

    licc->get_iocsr_as = get_iocsr_as;
    licc->cpu_by_arch_id = cpu_by_arch_id;
    licc->pre_save =  kvm_loongarch_ipi_pre_save;
    licc->post_load = kvm_loongarch_ipi_post_load;
}

static const TypeInfo kvm_loongarch_ipi_types[] = {
    {
        .name               = TYPE_KVM_LOONGARCH_IPI,
        .parent             = TYPE_LOONGSON_IPI_COMMON,
        .class_init         = kvm_loongarch_ipi_class_init,
    }
};

DEFINE_TYPES(kvm_loongarch_ipi_types)
