/* Virtualization support for SSP.
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
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"

#include "qemu/io-bridge.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/log.h"
#include "hw/ssi/ssp.h"

const struct adsp_reg_desc adsp_ssp_map[ADSP_SSP_REGS] = {
    {.name = "ssp", .enable = LOG_SSP,
        .offset = 0x00000000, .size = 0x4000},
};

static void ssp_reset(void *opaque)
{
     struct adsp_io_info *info = opaque;
     struct adsp_reg_space *space = info->space;

     memset(info->region, 0, space->desc.size);
}

static uint64_t ssp_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;
    struct adsp_ssp *ssp = info->private;

    log_read(ssp->log, space, addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

static void ssp_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;
    struct adsp_ssp *ssp = info->private;
    uint32_t set, clear;

    log_write(ssp->log, space, addr, val, size,
        info->region[addr >> 2]);

    switch (addr) {
    case SSCR1:
        set = val & ~info->region[addr >> 2];
        clear = ~val & info->region[addr >> 2];

        info->region[addr >> 2] = val;

        /* open file if playback has been enabled */
        if (set & SSCR1_TSRE) {

            /* create filename */
            sprintf(ssp->tx.file_name, "/tmp/%s-play%d.wav",
                ssp->name, ssp->tx.index++);

            unlink(ssp->tx.file_name);
            ssp->tx.fd = open(ssp->tx.file_name, O_WRONLY | O_CREAT,
                S_IRUSR | S_IWUSR | S_IXUSR);

            if (ssp->tx.fd < 0) {
                fprintf(stderr, "cant open file %s %d\n",
                    ssp->tx.file_name, -errno);
                return;
            } else
                printf("%s opened %s for playback\n",
                    ssp->name, ssp->tx.file_name);

            ssp->tx.total_frames = 0;
        }

        /* close file if playback has finished */
        if (clear & SSCR1_TSRE) {
            printf("%s closed %s for playback at %d frames\n",
                ssp->name,
                ssp->tx.file_name, ssp->tx.total_frames);
            close(ssp->tx.fd);
            ssp->tx.fd = 0;
        }

        /* open file if capture has been enabled */
        if (set & SSCR1_RSRE) {

            /* create filename */
            sprintf(ssp->rx.file_name, "/tmp/%s-capture.wav",
                ssp->name);

            ssp->rx.fd = open(ssp->rx.file_name, O_RDONLY,
                S_IRUSR | S_IWUSR | S_IXUSR);

            if (ssp->rx.fd < 0) {
                fprintf(stderr, "cant open file %s %d\n",
                    ssp->rx.file_name, -errno);
                return;
            } else
                printf("%s opened %s for capture\n",
                    ssp->name, ssp->rx.file_name);

            ssp->rx.total_frames = 0;
        }

        /* close file if capture has finished */
        if (clear & SSCR1_RSRE) {
            printf("%s closed %s for capture at %d frames\n", ssp->name,
                ssp->rx.file_name, ssp->rx.total_frames);
            close(ssp->rx.fd);
            ssp->rx.fd = 0;
        }
        break;
    case SSDR:
        /* update counters */
        ssp->tx.total_frames += size;
        info->region[addr >> 2] = val;

        break;
    default:
        info->region[addr >> 2] = val;
        break;
    }
}

const MemoryRegionOps ssp_ops = {
    .read = ssp_read,
    .write = ssp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

#define MAX_SSP     6
struct adsp_ssp *_ssp[MAX_SSP] = {NULL, NULL, NULL, NULL, NULL, NULL};

struct adsp_ssp *ssp_get_port(int port)
{
    port += 2; //skip dma
    // printf("ssp 0x%x with port:%d\n", _ssp[port], port);
    if (port >= 0  && port < MAX_SSP)
        return _ssp[port];

    // TODO find an alternative
    fprintf(stderr, "cant get SSP port %d\n", port);
    return NULL;
}

void adsp_ssp_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    struct adsp_ssp *ssp;

    ssp = g_malloc(sizeof(*ssp));

    ssp->tx.level = 0;
    ssp->rx.level = 0;
    sprintf(ssp->name, "%s.io", info->space->name);

    ssp->log = log_init(NULL);
    info->private = ssp;
    ssp_reset(info);
    ssp->ssp_dev = info->space;
    _ssp[info->io_dev] = ssp;
}
