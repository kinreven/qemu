/* Core DSP support for Broxton audio DSP.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "qemu/io-bridge.h"

#include "hw/audio/adsp-dev.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/log.h"
#include "hw/ssi/ssp.h"
#include "hw/dma/dw-dma.h"
#include "hw/adsp/cavs.h"
#include "mbox.h"
#include "cavs.h"
#include "common.h"
#include "manifest.h"

static void adsp_reset(void *opaque)
{
}

static uint64_t io_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    log_read(adsp->log, &adsp->desc->io_dev[info->io_dev], addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

/* SHIM IO from ADSP */
static void io_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    log_write(adsp->log, &adsp->desc->io_dev[info->io_dev], addr, val, size,
         info->region[addr >> 2]);
}

static const MemoryRegionOps io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void init_io(struct adsp_dev *adsp)
{
    const struct adsp_desc *board = adsp->desc;
    struct adsp_io_info *info;
    char shm_name[32];
    void *ptr = NULL;
    int i, err;

    for (i = 0; i < board->num_io; i++) {

        /* SHIM and all IO  - shared via SHM */
        info = g_malloc(sizeof(*info));

        sprintf(shm_name, "%s-io", board->io_dev[i].name);
        err = qemu_io_register_shm(shm_name, ADSP_IO_SHM_IO(i),
                board->io_dev[i].desc.size, &ptr);
        if (err < 0)
            fprintf(stderr, "error: cant alloc IO %s SHM %d\n", shm_name, err);

        info->region = ptr;
        info->adsp = adsp;
        info->io_dev = i;

        memory_region_init_io(&info->io, NULL, &io_ops, info,
            board->io_dev[i].name, board->io_dev[i].desc.size);
        memory_region_add_subregion(adsp->system_memory,
            board->io_dev[i].desc.base, &info->io);
    }
}

static void init_memory(struct adsp_dev *adsp, const char *name)
{
    MemoryRegion *iram, *dram0, *lp_sram, *rom, *uncache, *imr;
    const struct adsp_desc *board = adsp->desc;
    void *ptr = NULL;
    char shm_name[32];
    int err, i;
    uint32_t *uptr;

    /* SRAM -shared via SHM (not shared on real HW) */
    sprintf(shm_name, "%s-l2-sram", name);
    err = qemu_io_register_shm(shm_name, ADSP_IO_SHM_IRAM,
        board->iram.size, &ptr);
    if (err < 0)
        fprintf(stderr, "error: cant alloc L2 SRAM SHM %d\n", err);
    iram = g_malloc(sizeof(*iram));
    memory_region_init_ram_ptr(iram, NULL, "lpe.l2_sram", board->iram.size, ptr);
    vmstate_register_ram_global(iram);
    memory_region_add_subregion(adsp->system_memory,
        board->iram.base, iram);
    adsp->sram = ptr;

    /* set memory to non zero values */
    uptr = ptr;
    for (i = 0; i < board->iram.size >> 2; i++)
        uptr[i] = 0x5a5a5a5a;

    /* HP SRAM - shared via SHM (not shared on real HW) */
    sprintf(shm_name, "%s-hp-sram", name);
    err = qemu_io_register_shm(shm_name, ADSP_IO_SHM_DRAM,
        board->dram0.size, &ptr);
    if (err < 0)
        fprintf(stderr, "error: cant alloc HP SRAM SHM %d\n", err);
    dram0 = g_malloc(sizeof(*dram0));
    memory_region_init_ram_ptr(dram0, NULL, "lpe.hp_sram", board->dram0.size, ptr);
    vmstate_register_ram_global(dram0);
    memory_region_add_subregion(adsp->system_memory,
        board->dram0.base, dram0);
    adsp->hp_sram = ptr;

    /* Uncache HP-SRAM (same HP-SRAM, just mapped at different addr)  */
    sprintf(shm_name, "%s-uncache", name);
    uncache = g_malloc(sizeof(*uncache));
    memory_region_init_ram_ptr(uncache, NULL, "lpe.uncache", board->uncache.size, ptr);
    vmstate_register_ram_global(uncache);
    memory_region_add_subregion(adsp->system_memory,
        board->uncache.base, uncache);
    adsp->uncache = ptr;

    /* set memory to non zero values */
    uptr = ptr;
    for (i = 0; i < board->dram0.size >> 2; i++)
        uptr[i] = 0x6b6b6b6b;

    /* LP SRAM - shared via SHM (not shared on real HW) */
    sprintf(shm_name, "%s-lp-sram", name);
    err = qemu_io_register_shm(shm_name, ADSP_IO_SHM_LP_SRAM,
        board->lp_sram.size, &ptr);
    if (err < 0)
        fprintf(stderr, "error: cant alloc LP SRAM SHM %d\n", err);
    lp_sram = g_malloc(sizeof(*lp_sram));
    memory_region_init_ram_ptr(lp_sram, NULL, "lpe.lp_sram", board->lp_sram.size, ptr);
    vmstate_register_ram_global(lp_sram);
    memory_region_add_subregion(adsp->system_memory,
        board->lp_sram.base, lp_sram);
    adsp->lp_sram = ptr;

    /* set memory to non zero values */
    uptr = ptr;
    for (i = 0; i < board->lp_sram.size >> 2; i++)
        uptr[i] = 0x7c7c7c7c;

    /* IMR - shared via SHM (not shared on real HW) */
    sprintf(shm_name, "%s-imr", name);
    err = qemu_io_register_shm(shm_name, ADSP_IO_SHM_IMR,
        board->imr.size, &ptr);
    if (err < 0)
        fprintf(stderr, "error: cant alloc IMR SHM %d\n", err);
    imr = g_malloc(sizeof(*imr));
    memory_region_init_ram_ptr(imr, NULL, "lpe.imr", board->imr.size, ptr);
    vmstate_register_ram_global(imr);
    memory_region_add_subregion(adsp->system_memory,
        board->imr.base, imr);
    adsp->imr = ptr;

   /* set memory to non zero values */
    uptr = ptr;
    for (i = 0; i < board->imr.size >> 2; i++)
        uptr[i] = 0x9e9e9e9e;

    /* ROM - shared via SHM (not shared on real HW) */
    sprintf(shm_name, "%s-rom", name);
    err = qemu_io_register_shm(shm_name, ADSP_IO_SHM_ROM,
        board->rom.size, &ptr);
    if (err < 0)
        fprintf(stderr, "error: cant alloc ROM SHM %d\n", err);
    rom = g_malloc(sizeof(*rom));
    memory_region_init_ram_ptr(rom, NULL, "lpe.rom", board->rom.size, ptr);
    vmstate_register_ram_global(rom);
    memory_region_add_subregion(adsp->system_memory,
        board->rom.base, rom);
    adsp->rom = ptr;

   /* set memory to non zero values */
    uptr = ptr;
    for (i = 0; i < board->rom.size >> 2; i++)
        uptr[i] = 0;

    init_io(adsp);
}

static void adsp_pm_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
}

static int bridge_cb(void *data, struct qemu_io_msg *msg)
{
    struct adsp_dev *adsp = (struct adsp_dev *)data;

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        adsp_cavs_shim_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_IRQ:
        adsp_cavs_irq_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_PM:
        adsp_pm_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_DMA:
        dw_dma_msg(msg);
        break;
    case QEMU_IO_TYPE_MEM:
    default:
        break;
    }

    return 0;
}

static void copy_man_modules(const struct adsp_desc *board, struct adsp_dev *adsp,
    struct adsp_fw_desc *desc)
{
    struct module *mod;
    struct adsp_fw_header *hdr = &desc->header;
    unsigned long foffset, soffset, ssize;
    void *base_ptr = desc;
    int i, j;

    base_ptr -= board->file_offset;

    printf("found %d modules\n", hdr->num_module_entries);
    printf("using file offset 0x%x\n", board->file_offset);

    /* copy modules to SRAM */
    for (i = 0; i < hdr->num_module_entries; i++) {

        mod = &desc->module[i];
        printf("checking module %d : %s\n", i, mod->name);

        for (j = 0; j < 3; j++) {

            if (mod->segment[j].flags.r.load == 0)
                continue;

            foffset = mod->segment[j].file_offset;
            ssize = mod->segment[j].flags.r.length * 4096;

            /* L2 cache */
            if (mod->segment[j].v_base_addr >= board->iram.base &&
                mod->segment[j].v_base_addr < board->iram.base + board->iram.size) {

                soffset = mod->segment[j].v_base_addr - board->iram.base;

                printf(" L2 segment %d file offset 0x%lx L2$ addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(adsp->sram + soffset,
                    (void*)base_ptr + foffset, ssize);
                continue;
            }

            /* HP SRAM */
            if (mod->segment[j].v_base_addr >= board->dram0.base &&
                mod->segment[j].v_base_addr < board->dram0.base + board->dram0.size) {

                soffset =
                mod->segment[j].v_base_addr - board->dram0.base;

                printf(" HP segment %d file offset 0x%lx HP-SRAM addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(adsp->hp_sram + soffset,
                    (void*)base_ptr + foffset, ssize);
                continue;
            }

            /* LP SRAM */
            if (mod->segment[j].v_base_addr >= board->lp_sram.base &&
                mod->segment[j].v_base_addr < board->lp_sram.base + board->lp_sram.size) {

                soffset = mod->segment[j].v_base_addr - board->lp_sram.base;

                printf(" LP segment %d file offset 0x%lx LP-SRAM addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(adsp->lp_sram + soffset,
                    (void*)base_ptr + foffset, ssize);
                continue;
            }

            /* Uncache */
            if (mod->segment[j].v_base_addr >= board->uncache.base &&
                mod->segment[j].v_base_addr < board->uncache.base +
                board->uncache.size) {

                soffset = mod->segment[j].v_base_addr - board->uncache.base;

                printf(" Uncache segment %d file offset 0x%lx Uncache addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(adsp->uncache + soffset,
                    (void*)base_ptr + foffset, ssize);
                continue;
            }

             /* IMR */
            if (mod->segment[j].v_base_addr >= board->imr.base &&
                mod->segment[j].v_base_addr < board->imr.base +
                board->imr.size) {

                soffset = mod->segment[j].v_base_addr - board->imr.base;

                printf(" IMR segment %d file offset 0x%lx IMR addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(adsp->imr + soffset,
                    (void*)base_ptr + foffset, ssize);
                continue;
            }

            printf(" Unmatched segment %d file offset 0x%lx SRAM addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);
        }
    }
}

static void copy_man_to_imr(const struct adsp_desc *board, struct adsp_dev *adsp,
    struct adsp_fw_desc *desc)
{
   struct adsp_fw_header *hdr = &desc->header;

    /* copy manifest to IMR */
    memcpy(adsp->imr + board->imr_boot_ldr_offset, (void*)hdr,
                 hdr->preload_page_count * 4096);
    printf("ROM loader: copy %d kernel pages to IMR\n", hdr->preload_page_count);
}

static struct adsp_dev *adsp_init(const struct adsp_desc *board,
    MachineState *machine, const char *name, int copy_modules)
{
    struct adsp_dev *adsp;
    void *man_ptr, *desc_ptr;
    int n, skip = 0, size;
    void *rom;

    adsp = g_malloc(sizeof(*adsp));
    adsp->log = log_init(NULL);    /* TODO: add log name to cmd line */
    adsp->desc = board;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->cpu_model = machine->cpu_model;
    adsp->kernel_filename = qemu_opt_get(adsp->machine_opts, "kernel");
    adsp->rom_filename = qemu_opt_get(adsp->machine_opts, "rom");

    /* initialise CPU */
    if (!adsp->cpu_model) {
        adsp->cpu_model = XTENSA_DEFAULT_CPU_MODEL;
    }

    for (n = 0; n < smp_cpus; n++) {

        adsp->xtensa[n] = g_malloc(sizeof(struct adsp_xtensa));
        adsp->xtensa[n]->cpu = XTENSA_CPU(cpu_create(machine->cpu_type));

        if (adsp->xtensa[n]->cpu == NULL) {
            error_report("unable to find CPU definition '%s'",
                adsp->cpu_model);
            exit(EXIT_FAILURE);
        }

        adsp->xtensa[n]->env = &adsp->xtensa[n]->cpu->env;
        adsp->xtensa[n]->env->sregs[PRID] = n;

        qemu_register_reset(adsp_reset, adsp->xtensa[n]->cpu);

        /* Need MMU initialized prior to ELF loading,
        * so that ELF gets loaded into virtual addresses
        */
        cpu_reset(CPU(adsp->xtensa[n]->cpu));

    }

    init_memory(adsp, name);

    /* init peripherals */
    adsp_cavs_shim_init(adsp, name);
    dw_dma_init_dev(adsp, adsp->system_memory, board->gp_dmac_dev, 2);
    adsp_ssp_init(adsp->system_memory, board->ssp_dev, 2);

    /* reset all devices to init state */
    qemu_devices_reset();

    /* initialise bridge to x86 host driver */
    qemu_io_register_child(name, &bridge_cb, (void*)adsp);

    /* load binary file if one is specified on cmd line otherwise finish */
    if (adsp->kernel_filename == NULL) {
        printf(" ** CAVS Xtensa HiFi3 DSP initialised.\n"
            " ** Waiting for host to load firmware...\n");
        return adsp;
    }

    printf("now loading:\n kernel %s\n ROM %s\n",
        adsp->kernel_filename, adsp->rom_filename);

    if (adsp->rom_filename != NULL) {
        /* load ROM image and copy to ROM */
        rom = g_malloc(ADSP_CAVS_DSP_ROM_SIZE);
        load_image_size(adsp->rom_filename, rom,
            ADSP_CAVS_DSP_ROM_SIZE);
        memcpy(adsp->rom, rom, ADSP_CAVS_DSP_ROM_SIZE);
    }

    /* load the binary image and copy to SRAM */
    man_ptr = g_malloc(board->iram.size);
    size = load_image_size(adsp->kernel_filename, man_ptr,
        board->iram.size);

    /* Search for manifest ID = "$AEM" */
    desc_ptr = man_ptr;
    while (*((uint32_t*)desc_ptr) != 0x314d4124) {
        desc_ptr = desc_ptr + sizeof(uint32_t);
        skip += sizeof(uint32_t);
        if (skip >= size) {
            printf("error: failed to find FW manifest header $AM1\n");
            exit(0);
        }
    }

    printf("Header $AM1 found at offset 0x%x bytes\n", skip);

    /* does ROM or VM load manifest */
    if (adsp->rom_filename != NULL && !copy_modules) {

         /* copy whole manifest if required */
         copy_man_to_imr(board, adsp, desc_ptr);

    } else {

        /* copy manifest modules if required */
        copy_man_modules(board, adsp, desc_ptr);
    }

    return adsp;
}

/* CAVS 1.5 IO devices */
static struct adsp_reg_space cavs_1_5_io[] = {
        { .name = "cmd", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_CMD_BASE, .size = ADSP_CAVS_1_5_DSP_CMD_SIZE},},
        { .name = "res", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_RES_BASE, .size = ADSP_CAVS_1_5_DSP_RES_SIZE},},
        { .name = "ipc", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_5_DSP_IPC_HOST_SIZE},},
        { .name = "idc0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_DSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_IPC_DSP_SIZE},},
        { .name = "idc1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_DSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_IPC_DSP_SIZE},},
        { .name = "hostwin0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "irq", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IRQ_BASE, .size = ADSP_CAVS_1_5_DSP_IRQ_SIZE},},
        { .name = "timer", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_TIME_BASE, .size = ADSP_CAVS_1_5_DSP_TIME_SIZE},},
        { .name = "mn", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_MN_BASE, .size = ADSP_CAVS_1_5_DSP_MN_SIZE},},
        { .name = "l2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_L2_BASE, .size = ADSP_CAVS_1_5_DSP_L2_SIZE},},
};

/* hardware memory map for APL */
static const struct adsp_desc cavs_1_5p_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,
    .pmc_irq = IRQ_NUM_EXT_PMC,

    .num_ssp = 3,
    .num_dmac = 2,
    .iram = {.base = ADSP_CAVS_1_5_DSP_SRAM_BASE, .size = ADSP_CAVS_1_5_DSP_SRAM_SIZE},
    .dram0 = {.base = ADSP_CAVS_1_5_DSP_HP_SRAM_BASE, .size = ADSP_CAVS_1_5_DSP_HP_SRAM_SIZE},
    .lp_sram = {.base = ADSP_CAVS_1_5_DSP_LP_SRAM_BASE, .size = ADSP_CAVS_1_5_DSP_LP_SRAM_SIZE},
    .uncache = {.base = ADSP_CAVS_1_5_DSP_UNCACHE_BASE, .size = ADSP_CAVS_1_5_DSP_UNCACHE_SIZE},
    .imr = {.base = ADSP_CAVS_1_5_DSP_IMR_BASE, .size = ADSP_CAVS_1_5_DSP_IMR_SIZE},
    .rom = {.base = ADSP_CAVS_DSP_ROM_BASE, .size = ADSP_CAVS_DSP_ROM_SIZE},
    .imr_boot_ldr_offset = ADSP_CAVS_1_5P_DSP_IMR_MAN_OFFSET,

    .shim_dev = {
        .name = "shim",
        .reg_count = ARRAY_SIZE(adsp_bxt_shim_map),
        .reg = adsp_bxt_shim_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SHIM_BASE, .size = ADSP_CAVS_1_5_SHIM_SIZE},
    },

    .gp_dmac_dev[0] = {
        .name = "dmac0",
        .reg_count = ARRAY_SIZE(adsp_gp_dma_map),
        .reg = adsp_gp_dma_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_LP_GP_DMA_BASE(0), .size = ADSP_CAVS_1_5_DSP_LP_GP_DMA_SIZE},
    },
    .gp_dmac_dev[1] = {
        .name = "dmac1",
        .reg_count = ARRAY_SIZE(adsp_gp_dma_map),
        .reg = adsp_gp_dma_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_HP_GP_DMA_BASE(0), .size = ADSP_CAVS_1_5_DSP_HP_GP_DMA_SIZE},
    },
    .ssp_dev[0] = {
        .name = "ssp0",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},
    },
    .ssp_dev[1] = {
        .name = "ssp1",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},
    },
    .ssp_dev[2] = {
        .name = "ssp2",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},
    },

    .num_io = ARRAY_SIZE(cavs_1_5_io),
    .io_dev = cavs_1_5_io,
};

/* hardware memory map for SKL, KBL */
static const struct adsp_desc cavs_1_5_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,
    .pmc_irq = IRQ_NUM_EXT_PMC,

    .num_ssp = 3,
    .num_dmac = 2,
    .iram = {.base = ADSP_CAVS_1_5_DSP_SRAM_BASE, .size = ADSP_CAVS_1_5_DSP_SRAM_SIZE},
    .dram0 = {.base = ADSP_CAVS_1_5_DSP_HP_SRAM_BASE, .size = ADSP_CAVS_1_5_DSP_HP_SRAM_SIZE},
    .lp_sram = {.base = ADSP_CAVS_1_5_DSP_LP_SRAM_BASE, .size = ADSP_CAVS_1_5_DSP_LP_SRAM_SIZE},
    .uncache = {.base = ADSP_CAVS_1_5_DSP_UNCACHE_BASE, .size = ADSP_CAVS_1_5_DSP_UNCACHE_SIZE},
    .imr = {.base = ADSP_CAVS_1_5_DSP_IMR_BASE, .size = ADSP_CAVS_1_5_DSP_IMR_SIZE},
    .rom = {.base = ADSP_CAVS_DSP_ROM_BASE, .size = ADSP_CAVS_DSP_ROM_SIZE},
    .imr_boot_ldr_offset = ADSP_CAVS_1_5_DSP_IMR_MAN_OFFSET,
    .file_offset = sizeof(struct fw_image_manifest_v1_5) - sizeof(struct adsp_fw_desc),

    .shim_dev = {
        .name = "shim",
        .reg_count = ARRAY_SIZE(adsp_bxt_shim_map),
        .reg = adsp_bxt_shim_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SHIM_BASE, .size = ADSP_CAVS_1_5_SHIM_SIZE},
    },

    .gp_dmac_dev[0] = {
        .name = "dmac0",
        .reg_count = ARRAY_SIZE(adsp_gp_dma_map),
        .reg = adsp_gp_dma_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_LP_GP_DMA_BASE(0), .size = ADSP_CAVS_1_5_DSP_LP_GP_DMA_SIZE},
    },
    .gp_dmac_dev[1] = {
        .name = "dmac1",
        .reg_count = ARRAY_SIZE(adsp_gp_dma_map),
        .reg = adsp_gp_dma_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_HP_GP_DMA_BASE(0), .size = ADSP_CAVS_1_5_DSP_HP_GP_DMA_SIZE},
    },
    .ssp_dev[0] = {
        .name = "ssp0",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},
    },
    .ssp_dev[1] = {
        .name = "ssp1",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},
    },
    .ssp_dev[2] = {
        .name = "ssp2",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},
    },

    .num_io = ARRAY_SIZE(cavs_1_5_io),
    .io_dev = cavs_1_5_io,
};

/* CAVS 1.8 IO devices */
static struct adsp_reg_space cavs_1_8_io[] = {
        { .name = "cap", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_CAP_BASE, .size = ADSP_CAVS_1_8_DSP_CAP_SIZE},},
        { .name = "hp-gpdma-shim", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_SIZE},},
        { .name = "idc0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "hostwin0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "irq", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_IRQ_SIZE},},
        { .name = "timer", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_TIME_BASE, .size = ADSP_CAVS_1_8_DSP_TIME_SIZE},},
        { .name = "mn", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_MN_BASE, .size = ADSP_CAVS_1_8_DSP_MN_SIZE},},
        { .name = "l2m", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2M_BASE, .size = ADSP_CAVS_1_8_DSP_L2M_SIZE},},
        { .name = "l2c", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2C_BASE, .size = ADSP_CAVS_1_8_DSP_L2C_SIZE},},
        { .name = "res", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_RES_BASE, .size = ADSP_CAVS_1_8_DSP_RES_SIZE},},
        { .name = "cmd", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_CMD_BASE, .size = ADSP_CAVS_1_8_DSP_CMD_SIZE},},
        { .name = "dmic", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_DMIC_BASE, .size = ADSP_CAVS_1_8_DSP_DMIC_SIZE},},
        { .name = "ipc", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_8_DSP_IPC_HOST_SIZE},},
        { .name = "gtw-lout", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-lin", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_SIZE * 14},},
        { .name = "gtw-hout", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-hin", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_SIZE * 14},},
        { .name = "cl", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_BASE, .size = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_SIZE},},
        { .name = "lp-gpda-shim", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_SIZE * 4},},
};

/* SUE and CNL */
static const struct adsp_desc cavs_1_8_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,
    .pmc_irq = IRQ_NUM_EXT_PMC,

    .num_ssp = 3,
    .num_dmac = 2,
    .iram = {.base = ADSP_CAVS_1_8_DSP_SRAM_BASE, .size = ADSP_CAVS_1_8_DSP_SRAM_SIZE},
    .dram0 = {.base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE, .size = ADSP_CAVS_1_8_DSP_HP_SRAM_SIZE},
    .lp_sram = {.base = ADSP_CAVS_1_8_DSP_LP_SRAM_BASE, .size = ADSP_CAVS_1_8_DSP_LP_SRAM_SIZE},
    .uncache = {.base = ADSP_CAVS_1_8_DSP_UNCACHE_BASE, .size = ADSP_CAVS_1_8_DSP_UNCACHE_SIZE},
    .imr = {.base = ADSP_CAVS_1_8_DSP_IMR_BASE, .size = ADSP_CAVS_1_8_DSP_IMR_SIZE},
    .rom = {.base = ADSP_CAVS_DSP_ROM_BASE, .size = ADSP_CAVS_DSP_ROM_SIZE},
    .imr_boot_ldr_offset = ADSP_CAVS_1_8_DSP_IMR_MAN_OFFSET,

    .shim_dev = {
        .name = "shim",
        .reg_count = ARRAY_SIZE(adsp_bxt_shim_map),
        .reg = adsp_bxt_shim_map,
        .desc = {.base = ADSP_CAVS_1_8_DSP_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_SHIM_SIZE},
    },

    .gp_dmac_dev[0] = {
        .name = "dmac0",
        .reg_count = ARRAY_SIZE(adsp_gp_dma_map),
        .reg = adsp_gp_dma_map,
        .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},
    },
    .gp_dmac_dev[1] = {
        .name = "dmac1",
        .reg_count = ARRAY_SIZE(adsp_gp_dma_map),
        .reg = adsp_gp_dma_map,
        .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},
    },
    .ssp_dev[0] = {
        .name = "ssp0",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},
    },
    .ssp_dev[1] = {
        .name = "ssp1",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},
    },
    .ssp_dev[2] = {
        .name = "ssp2",
        .reg_count = ARRAY_SIZE(adsp_ssp_map),
        .reg = adsp_ssp_map,
        .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},
    },

    .num_io = ARRAY_SIZE(cavs_1_8_io),
    .io_dev = cavs_1_8_io,
};

static void bxt_adsp_init(MachineState *machine)
{
    struct adsp_dev *adsp;

    adsp = adsp_init(&cavs_1_5p_dsp_desc, machine, "bxt", 0);

    adsp->ext_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb, adsp);
    adsp->ext_clk_kHz = 2500;
}

static void xtensa_bxt_machine_init(MachineClass *mc)
{
    mc->desc = "Broxton HiFi3";
    mc->is_default = true;
    mc->init = bxt_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_bxt", xtensa_bxt_machine_init)

static void skl_adsp_init(MachineState *machine)
{
    struct adsp_dev *adsp;

    adsp = adsp_init(&cavs_1_5_dsp_desc, machine, "skl", 1);

    adsp->ext_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb, adsp);
    adsp->ext_clk_kHz = 2500;
}

static void xtensa_skl_machine_init(MachineClass *mc)
{
    mc->desc = "Skylake HiFi3";
    mc->is_default = true;
    mc->init = skl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_skl", xtensa_skl_machine_init)

static void kbl_adsp_init(MachineState *machine)
{
    struct adsp_dev *adsp;

    adsp = adsp_init(&cavs_1_5_dsp_desc, machine, "kbl", 1);

    adsp->ext_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb, adsp);
    adsp->ext_clk_kHz = 2500;
}

static void xtensa_kbl_machine_init(MachineClass *mc)
{
    mc->desc = "Kabylake HiFi3";
    mc->is_default = true;
    mc->init = kbl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_kbl", xtensa_kbl_machine_init)

static void sue_adsp_init(MachineState *machine)
{
    struct adsp_dev *adsp;

    adsp = adsp_init(&cavs_1_8_dsp_desc, machine, "sue", 0);

    adsp->ext_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb, adsp);
    adsp->ext_clk_kHz = 2500;
}

static void xtensa_sue_machine_init(MachineClass *mc)
{
    mc->desc = "Sue HiFi3";
    mc->is_default = true;
    mc->init = sue_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_sue", xtensa_sue_machine_init)

static void cnl_adsp_init(MachineState *machine)
{
    struct adsp_dev *adsp;

    adsp = adsp_init(&cavs_1_8_dsp_desc, machine, "cnl", 0);

    adsp->ext_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb, adsp);
    adsp->ext_clk_kHz = 2500;
}

static void xtensa_cnl_machine_init(MachineClass *mc)
{
    mc->desc = "Cannonlake HiFi3";
    mc->is_default = true;
    mc->init = cnl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_cnl", xtensa_cnl_machine_init)

static void icl_adsp_init(MachineState *machine)
{
    struct adsp_dev *adsp;

    adsp = adsp_init(&cavs_1_8_dsp_desc, machine, "icl", 0);

    adsp->ext_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb, adsp);
    adsp->ext_clk_kHz = 2500;
}

static void xtensa_icl_machine_init(MachineClass *mc)
{
    mc->desc = "Icelake HiFi3";
    mc->is_default = true;
    mc->init = icl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_icl", xtensa_icl_machine_init)
