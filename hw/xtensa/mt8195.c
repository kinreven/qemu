/* Core DSP support for mt8195 audio DSP.
 *
 * Copyright(c) 2022 Mediatek
 *
 * Author: Trevor Wu <trevor.wu@mediatek.com>
 */

#include <stdint.h>
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/io-bridge.h"
#include "sysemu/runstate.h"
#include "sysemu/reset.h"
#include "hw/audio/adsp-dev.h"
#include "hw/adsp/mt8195.h"
#include "common.h"
#include "hw/adsp/fw.h"
#include "trace.h"

#define CNT_EN_BIT BIT(0)
#define TIMER_CLK_KHZ 13000

static void clk_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;
    memset(info->region, 0, space->desc.size);
}

static uint64_t clk_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_clk_read(addr, info->region[addr >> 2]);

    return info->region[addr >> 2];
}

static void clk_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_clk_write(addr, val);

    info->region[addr >> 2] = val;
}

const MemoryRegionOps mt8195_dsp_clk_ops = {
    .read = clk_read,
    .write = clk_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mt8195_dsp_clk_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    clk_reset(info);
}

static void dspio_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;
    memset(info->region, 0, space->desc.size);
}

static inline uint64_t ns2ticks(uint64_t ns, uint64_t clk_kHz)
{
    return ns * clk_kHz * 1000 / ( 1000 * 1000 * 1000 );
}

static uint64_t mt8195_set_time(struct adsp_dev *adsp, struct adsp_io_info *info)
{
    uint64_t time = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adsp->timer[0].start);
    uint64_t ticks = ns2ticks(time, adsp->timer[0].clk_kHz);

    info->region[OSTIMER_CUR_H >> 2] = (uint32_t)(ticks >> 32);
    info->region[OSTIMER_CUR_L >> 2] = (uint32_t)(ticks & 0xffffffff);

    return time;
}

/* other IO from ADSP */
static uint64_t dspio_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    switch(addr) {
    case OSTIMER_CUR_H:
        mt8195_set_time(adsp, info);
        break;
    }
    trace_adsp_dsp_cfg_read(addr, info->region[addr >> 2]);

    return info->region[addr >> 2];
}

static void dspio_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    switch(addr) {
    case CNTCR:
        /* enable the timer ? */
        if (val & CNT_EN_BIT &&
            !(info->region[addr >> 2] & CNT_EN_BIT)) {
                adsp->timer[0].start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                mt8195_set_time(adsp, info);
        }
        break;
    default:
        break;
    }

    trace_adsp_dsp_cfg_write(addr, val);

    info->region[addr >> 2] = val;
}

const MemoryRegionOps mt8195_dsp_io_ops = {
    .read = dspio_read,
    .write = dspio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mt8195_dspio_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    dspio_reset(info);
    adsp->timer[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, NULL, info);
    adsp->timer[0].clk_kHz = TIMER_CLK_KHZ;
    adsp->timer[0].ns_to_clock = (1000000 / adsp->timer[0].clk_kHz);
}

static void mbox_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;

    memset(info->region, 0, space->desc.size);
}

static uint64_t mbox_read(void *opaque, hwaddr addr, unsigned size)
{
    struct adsp_io_info *info = opaque;
    uint64_t value;

    value = info->region[addr >> 2];
    trace_adsp_dsp_mbox_read(addr, value);

    return value;
}

static void mbox_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    struct adsp_io_info *info = opaque;

    info->region[addr >> 2] = value;
    trace_adsp_dsp_mbox_write(addr, value);
}

static const MemoryRegionOps mbox_io_ops = {
    .read = mbox_read,
    .write = mbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mt8195_mbox_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    mbox_reset(info);
}

static void mb_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;

    memset(info->region, 0, space->desc.size);
}

static uint64_t mb_read(void *opaque, hwaddr addr, unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_mb_read(addr, info->region[addr >> 2]);

    return info->region[addr >> 2];
}

static void mb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_mb_write(addr, val);

    info->region[addr >> 2] = val;
}

static const MemoryRegionOps mediatek_mb_ops = {
    .read = mb_read,
    .write = mb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mediatek_mb_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    mb_reset(info);
}

static int bridge_cb(void *data, struct qemu_io_msg *msg)
{
    trace_adsp_dsp_host_msg("received type", msg->type);

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        break;
    case QEMU_IO_TYPE_IRQ:
        break;
    case QEMU_IO_TYPE_PM:
        break;
    case QEMU_IO_TYPE_DMA:
        break;
    case QEMU_IO_TYPE_MEM:
    default:
        break;
    }

    return 0;
}

static struct adsp_dev *adsp_init(const struct adsp_desc *board,
    MachineState *machine, unsigned mem_size)
{
    struct adsp_dev *adsp;
    uint8_t *ldata;
    size_t lsize;

    adsp = g_malloc(sizeof(*adsp));
    adsp->desc = board;
    adsp->shm_idx = 0;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->kernel_filename = qemu_opt_get(adsp->machine_opts, "kernel");

    /* initialise CPU */
    adsp->xtensa[0] = g_malloc(sizeof(struct adsp_xtensa));
    adsp->xtensa[0]->cpu = XTENSA_CPU(cpu_create(machine->cpu_type));

    if (adsp->xtensa[0]->cpu == NULL) {
        error_report("unable to find CPU definition '%s'", machine->cpu_type);
        exit(EXIT_FAILURE);
    }

    adsp->xtensa[0]->env = &adsp->xtensa[0]->cpu->env;
    adsp->xtensa[0]->env->sregs[PRID] = 0;

    /* Need MMU initialized prior to ELF loading,
     * so that ELF gets loaded into virtual addresses
     */
    cpu_reset(CPU(adsp->xtensa[0]->cpu));

    adsp_create_memory_regions(adsp);
    adsp_create_io_devices(adsp, NULL);

    /* reset all devices to init state */
    qemu_devices_reset();

    /* initialise bridge to host driver */
    qemu_io_register_child(machine->cpu_type, &bridge_cb, (void *)adsp);

    /* load binary file if one is specified on cmd line otherwise finish */
    if (adsp->kernel_filename == NULL) {
        qemu_log_mask(CPU_LOG_RESET,"%s initialised, waiting for FW load",
	        board->name);
        return adsp;
    }
    /* load the binary image and copy to IRAM */
    ldata = g_malloc(mem_size);
    lsize = load_image_size(adsp->kernel_filename, ldata, mem_size);

    adsp_load_modules(adsp, ldata, lsize);

    return adsp;
}

static struct adsp_mem_desc mt8195_mem[] = {
    {.name = "iram", .base = ADSP_MT8195_DSP_SRAM_BASE,
        .size = ADSP_MT8195_DSP_SRAM_SIZE},
    {.name = "sram", .base = ADSP_MT8195_DSP_DRAM0_BASE,
        .size = ADSP_MT8195_DSP_DRAM0_SIZE},
};

static struct adsp_reg_space mt8195_io[] = {
    { .name = "mbox", .init = &mt8195_mbox_init, .ops = &mbox_io_ops,
        .desc = {.base = ADSP_MT8195_DSP_MAILBOX_BASE,
                 .size = ADSP_MT8195_DSP_MAILBOX_SIZE},},
    { .name = "mb0", .init = &mediatek_mb_init, .ops = &mediatek_mb_ops,
        .desc = {.base = ADSP_MT8195_MBOX0_REG_BASE,
                 .size = ADSP_MT8195_MBOX0_REG_SIZE},},
    { .name = "mb1", .init = &mediatek_mb_init, .ops = &mediatek_mb_ops,
        .desc = {.base = ADSP_MT8195_MBOX1_REG_BASE,
                 .size = ADSP_MT8195_MBOX1_REG_SIZE},},
    { .name = "mb2", .init = &mediatek_mb_init, .ops = &mediatek_mb_ops,
        .desc = {.base = ADSP_MT8195_MBOX2_REG_BASE,
                 .size = ADSP_MT8195_MBOX2_REG_SIZE},},
    { .name = "dspcfg",
        .init = &mt8195_dspio_init, .ops = &mt8195_dsp_io_ops,
        .desc = {.base = ADSP_MT8195_DSP_REG_BASE,
                 .size = ADSP_MT8195_DSP_REG_SIZE},},
    { .name = "scp",
        .init = &mt8195_dsp_clk_init, .ops = &mt8195_dsp_clk_ops,
        .desc = {.base = ADSP_MT8195_SCP_REG_BASE,
                 .size = ADSP_MT8195_SCP_REG_SIZE},},
    { .name = "topckgen",
        .init = &mt8195_dsp_clk_init, .ops = &mt8195_dsp_clk_ops,
        .desc = {.base = ADSP_MT8195_TOPCKGEN_REG_BASE,
                 .size = ADSP_MT8195_TOPCKGEN_REG_SIZE},},
    { .name = "apmixdsys",
        .init = &mt8195_dsp_clk_init, .ops = &mt8195_dsp_clk_ops,
        .desc = {.base = ADSP_MT8195_APMIXDSYS_REG_BASE,
                 .size = ADSP_MT8195_APMIXDSYS_REG_SIZE},},
};

/* hardware memory map */
static const struct adsp_desc mt8195_dsp_desc = {
    .num_mem = ARRAY_SIZE(mt8195_mem),
    .mem_region = mt8195_mem,
    .num_io = ARRAY_SIZE(mt8195_io),
    .io_dev = mt8195_io,

    .mem_zones = {
                [SOF_FW_BLK_TYPE_IRAM] = {
                        .base = ADSP_MT8195_DSP_SRAM_BASE,
                        .size = ADSP_MT8195_DSP_SRAM_SIZE,
                        .host_offset = 0,
                },
                [SOF_FW_BLK_TYPE_SRAM] = {
                        .base = ADSP_MT8195_DSP_DRAM0_BASE,
                        .size = ADSP_MT8195_DSP_DRAM0_SIZE,
                        .host_offset = 0,
                },
        },
};

static void mt8195_adsp_init(MachineState *machine)
{
    adsp_init(&mt8195_dsp_desc, machine,
    	ADSP_MT8195_DSP_DRAM0_SIZE + ADSP_MT8195_DSP_SRAM_SIZE);
}

static void xtensa_mt8195_machine_init(MachineClass *mc)
{
    mc->desc = "MT8195 HiFi4";
    mc->is_default = false;
    mc->init = mt8195_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_mt8195", xtensa_mt8195_machine_init)
