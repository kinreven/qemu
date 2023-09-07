/* Virtualization support for DesignWare DMA Engine.
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
#include "elf.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/thread.h"
#include "qemu/io-bridge.h"

#include "hw/pci/pci.h"
#include "hw/audio/adsp-dev.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/log.h"
#include "hw/dma/hda-dma.h"

/* registers */
#define DGCS(x)     (0x0 + (x * 0x20))
#define DGBBA(x)    (0x4 + (x * 0x20))
#define DGBS(x)     (0x8 + (x * 0x20))
#define DGBFPI(x)   (0xc + (x * 0x20))
#define DGBRP(x)    (0x10 + (x * 0x20))
#define DGBWP(x)    (0x14 + (x * 0x20))
#define DGBSP(x)    (0x18 + (x * 0x20))

/* DGCS bits */
#define DGCS_GEN    (0x1 << 26)
#define DGCS_GBUSY    (0x1 << 15)
#define DGCS_BSC    (0x1 << 11)
#define DGCS_BF    (0x1 << 9)
#define DGCS_BNE    (0x1 << 8)
#define DGCS_FIFORDY    (0x1 << 5)

static inline void dma_sleep(long nsec)
{
    struct timespec req;

    req.tv_sec = 0;
    req.tv_nsec = nsec;

    if (nanosleep(&req, NULL) < 0)
        fprintf(stderr, "failed to sleep %d\n", -errno);
}

static void dma_host_req(struct adsp_hda_dmac *dmac, uint32_t chan,
    uint32_t direction)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];
    struct qemu_io_msg_dma32 *dma_msg = &hda_dma_chan->dma_msg;

    /* send IRQ to parent */
    dma_msg->hdr.type = QEMU_IO_TYPE_DMA;
    dma_msg->hdr.msg = QEMU_IO_DMA_REQ_NEW;
    dma_msg->hdr.size = sizeof(*dma_msg);
    dma_msg->host_data = 0;
    dma_msg->src = dmac->io[DGBRP(chan) >> 2]; // link
    dma_msg->dest = dmac->io[DGBWP(chan) >> 2]; // link
    dma_msg->size = dmac->io[DGBFPI(chan) >> 2];
    dma_msg->dmac_id = dmac->id;
    dma_msg->chan_id = chan;
    dma_msg->client_data = (uint64_t)hda_dma_chan;
    dma_msg->direction = direction;

    log_text(dmac->log, LOG_DMA_M2M,
        "DMA req: src 0x%x dest 0x%x size 0x%x\n",
        dma_msg->src, dma_msg->dest, dma_msg->size);

    qemu_io_send_msg(&dma_msg->hdr);
}

static void dma_host_complete(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];
    struct qemu_io_msg_dma32 *dma_msg = &hda_dma_chan->dma_msg;

    /* send IRQ to parent */
    dma_msg->hdr.type = QEMU_IO_TYPE_DMA;
    dma_msg->hdr.msg = QEMU_IO_DMA_REQ_COMPLETE;
    dma_msg->size = sizeof(struct qemu_io_msg_dma32);

    log_text(dmac->log, LOG_DMA_M2M,
        "DMA req complete: src 0x%x dest 0x%x size 0x%x\n",
        dma_msg->src, dma_msg->dest, dma_msg->size);

    qemu_io_send_msg(&dma_msg->hdr);
}

static void chan_update_rptr(struct adsp_hda_dmac *dmac, int chan, int bytes)
{
    dmac->io[DGBRP(chan) >> 2] += bytes;

    /* check R ptr wrt to W ptr */
    if (dmac->io[DGBRP(chan) >> 2] == dmac->io[DGBWP(chan) >> 2])
        dmac->io[DGCS(chan) >> 2] &= ~DGCS_BNE; /* buffer is empty */
    else
        dmac->io[DGCS(chan) >> 2] |= DGCS_BNE; /* buffer not empty */

    /* buffer wrap ? */
    if (dmac->io[DGBRP(chan) >> 2] >= dmac->io[DGBS(chan) >> 2])
      dmac->io[DGBRP(chan) >> 2] =  dmac->io[DGBBA(chan) >> 2];
}

static void chan_update_wptr(struct adsp_hda_dmac *dmac, int chan, int bytes)
{
    dmac->io[DGBWP(chan) >> 2] += bytes;

    /* check R ptr wrt to W ptr */
    if (dmac->io[DGBRP(chan) >> 2] == dmac->io[DGBWP(chan) >> 2])
        dmac->io[DGCS(chan) >> 2] |= ~DGCS_BF; /* buffer is full */
    else
        dmac->io[DGCS(chan) >> 2] &= ~DGCS_BF; /* buffer not full */

    /* buffer wrap ? */
    if (dmac->io[DGBWP(chan) >> 2] >= dmac->io[DGBS(chan) >> 2])
      dmac->io[DGBWP(chan) >> 2] =  dmac->io[DGBBA(chan) >> 2];
}

static void chan_reset(struct adsp_hda_dmac *dmac, int chan)
{
    dmac->io[DGBWP(chan) >> 2] =  dmac->io[DGBBA(chan) >> 2];
    dmac->io[DGBRP(chan) >> 2] =  dmac->io[DGBBA(chan) >> 2];
    dmac->io[DGCS(chan) >> 2] &= ~DGCS_BNE; /* buffer is empty */
    dmac->io[DGCS(chan) >> 2] &= ~DGCS_BF; /* buffer not full */
}

/* read from MEM and write to SSP - audio playback */
static int dma_M2P_copy_burst(struct hda_dma_chan *hda_dma_chan)
{
    struct adsp_hda_dmac *dmac = hda_dma_chan->dmac;
    uint32_t chan = hda_dma_chan->chan;
    hwaddr burst_size;
    uint32_t buffer[0x1000];
    hwaddr size, sar;

    /* transfer complete ?*/
    if (hda_dma_chan->stop)
        return 0;

    /* no data ? */
    if (!(dmac->io[DGCS(chan) >> 2] & DGCS_BNE))
        return 1;

    sar = dmac->io[DGBRP(chan) >> 2];
    size = dmac->io[DGBFPI(chan) >> 2];
    burst_size = size / 2;

    /* copy burst from SAR to DAR */
    cpu_physical_memory_read(sar, buffer, burst_size);

    /* copy buffer to files */
    if (hda_dma_chan->fd > 0) {
        if (write(hda_dma_chan->fd, buffer, burst_size) != burst_size)
            fprintf(stderr, "error: writing to DMAC file %d\n", -errno);
    }

    /* update SAR, DAR and bytes copied */
    hda_dma_chan->ptr += burst_size;
    hda_dma_chan->bytes += burst_size;
    hda_dma_chan->tbytes += burst_size;
    chan_update_rptr(dmac, chan, burst_size);

    log_text(dmac->log, LOG_DMA_M2P,
        "dma: %d:%d: completed SAR 0x%lx DAR 0x%lx size 0x%x total bytes 0x%x\n",
        dmac->id, chan, sar, dmac->io[DGBWP(chan) >> 2],
        size,
        hda_dma_chan->tbytes);

     return 1;
}

/* read from SSP and write to MEM - audio capture */
static int dma_P2M_copy_burst(struct hda_dma_chan *hda_dma_chan)
{
    struct adsp_hda_dmac *dmac = hda_dma_chan->dmac;
    uint32_t chan = hda_dma_chan->chan;
    hwaddr burst_size;
    uint32_t buffer[0x1000];
    hwaddr size, dar;

    /* transfer complete ?*/
    if (hda_dma_chan->stop)
        return 0;

    /* buffer full ? */
    if (!(dmac->io[DGCS(chan) >> 2] & DGCS_BF))
        return 1;

    dar = dmac->io[DGBWP(chan) >> 2];
    size = dmac->io[DGBFPI(chan) >> 2];
    burst_size = size / 2;

    /* copy burst from SAR to DAR */
    cpu_physical_memory_read(dar, buffer, burst_size);

    /* copy buffer to files */
    if (hda_dma_chan->fd > 0) {
        if (read(hda_dma_chan->fd, buffer, burst_size) != burst_size)
            fprintf(stderr, "error: reading to DMAC file %d\n", -errno);
    }

    /* update SAR, DAR and bytes copied */
    hda_dma_chan->ptr += burst_size;
    hda_dma_chan->bytes += burst_size;
    hda_dma_chan->tbytes += burst_size;
    chan_update_wptr(dmac, chan, burst_size);

    log_text(dmac->log, LOG_DMA_P2M,
        "dma: %d:%d: completed SAR 0x%lx DAR 0x%lx size 0x%x total bytes 0x%x\n",
        dmac->id, chan, dar, dmac->io[DGBRP(chan) >> 2],
        size,
        hda_dma_chan->tbytes);

     return 1;
}

/* read from host and write to DSP - audio playback */
static int dma_M2M_read_host_burst(struct hda_dma_chan *hda_dma_chan)
{
    struct adsp_hda_dmac *dmac = hda_dma_chan->dmac;
    uint32_t chan = hda_dma_chan->chan;
    hwaddr burst_size = 32; // TODO read from DMAC
    uint32_t dar;

    /* transfer complete ?*/
    if (hda_dma_chan->stop) {
     /* tell host we are complete */
        dma_host_complete(dmac, chan);

        /* free SHM */
        qemu_io_free_shm(ADSP_IO_SHM_DMA(dmac->id, chan), 0);
        return 0;
    }

    /* buffer full ? */
    if (!(dmac->io[DGCS(chan) >> 2] & DGCS_BF))
        return 1;

    dar = dmac->io[DGBWP(chan) >> 2];

    /* copy burst from SAR to DAR */
    cpu_physical_memory_write(dar, hda_dma_chan->ptr, burst_size);

    hda_dma_chan->ptr += burst_size;
    hda_dma_chan->bytes += burst_size;
    hda_dma_chan->tbytes += burst_size;
    chan_update_wptr(dmac, chan, burst_size);

    if (hda_dma_chan->fd > 0) {
        if (write(hda_dma_chan->fd, hda_dma_chan->ptr, burst_size) != burst_size)
            fprintf(stderr, "error: writing to DMAC file %d\n", -errno);
    }

    log_text(dmac->log, LOG_DMA_M2M,
        "dma: %d:%d: completed SAR 0x%x DAR 0x%x size 0x%x total bytes 0x%x\n",
        dmac->id, chan, dmac->io[DGBRP(chan) >> 2], dar,
        dmac->io[DGBFPI(chan) >> 2],
        hda_dma_chan->tbytes);

    return 1;
}

/* read from DSP and write to host - audio capture */
static int dma_M2M_write_host_burst(struct hda_dma_chan *hda_dma_chan)
{
    struct adsp_hda_dmac *dmac = hda_dma_chan->dmac;
    uint32_t chan = hda_dma_chan->chan;
    hwaddr burst_size = 32; // TODO read from DMAC
    uint32_t sar;

    /* transfer complete ?*/
    if (hda_dma_chan->stop) {
     /* tell host we are complete */
        dma_host_complete(dmac, chan);

        /* free SHM */
        qemu_io_free_shm(ADSP_IO_SHM_DMA(dmac->id, chan), 0);
        return 0;
    }

     /* no data ? */
    if (!(dmac->io[DGCS(chan) >> 2] & DGCS_BNE))
        return 1;

    sar = dmac->io[DGBRP(chan) >> 2];

    /* copy burst from SAR to DAR */
    cpu_physical_memory_read(sar, hda_dma_chan->ptr, burst_size);

    hda_dma_chan->ptr += burst_size;
    hda_dma_chan->bytes += burst_size;
    hda_dma_chan->tbytes += burst_size;
    chan_update_rptr(dmac, chan, burst_size);

    log_text(dmac->log, LOG_DMA_M2M,
        "dma: %d:%d: completed SAR 0x%x DAR 0x%x size 0x%x total bytes 0x%x\n",
        dmac->id, chan, sar, dmac->io[DGBWP(chan) >> 2],
        dmac->io[DGBFPI(chan) >> 2],
        hda_dma_chan->tbytes);

    return 1;
}

static void open_dmac_file(struct hda_dma_chan *hda_dma_chan)
{
    struct adsp_hda_dmac *dmac = hda_dma_chan->dmac;
    char name[32];

    /* create filename */
    sprintf(name, "/tmp/hda-dmac%d-%d-%d.wav", dmac->id, hda_dma_chan->chan,
        hda_dma_chan->file_idx++);
    unlink(name);

    if (hda_dma_chan->fd == 0)
        hda_dma_chan->fd = open(name, O_WRONLY | O_CREAT,
            S_IRUSR | S_IWUSR | S_IXUSR);

    if (hda_dma_chan->fd < 0)
        fprintf(stderr, "error: can't open file %s %d\n", name, -errno);
}

static void close_dmac_file(struct hda_dma_chan *hda_dma_chan)
{
    if (hda_dma_chan->fd > 0) {
        close(hda_dma_chan->fd);
        hda_dma_chan->fd = 0;
    }
}

/* TODO: make these configurable on rate etc */
#define DMA_M2M_BURST_SLEEP    (200 * 1000)    /* 200us */
#define DMA_M2P_BURST_SLEEP    (6667 * 1000)    /* 6.66ms */
#define DMA_P2M_BURST_SLEEP    (6667 * 1000)    /* 6.66ms */

/* dsp mem to host mem work for capture */
static void * hda_dma_channel_Mhost2Mdsp_work(void *data)
{
    struct hda_dma_chan *hda_dma_chan = data;

    open_dmac_file(hda_dma_chan);

    do {
        dma_sleep(DMA_M2M_BURST_SLEEP);
    } while (dma_M2M_write_host_burst(hda_dma_chan));

    close_dmac_file(hda_dma_chan);

    return NULL;
}

/* dsp mem to host mem work for capture */
static void * hda_dma_channel_Mdsp2Mhost_work(void *data)
{
    struct hda_dma_chan *hda_dma_chan = data;

    open_dmac_file(hda_dma_chan);

    do {
        dma_sleep(DMA_M2M_BURST_SLEEP);
    } while (dma_M2M_read_host_burst(hda_dma_chan));

    close_dmac_file(hda_dma_chan);

    return NULL;
}

/* peripheral to mem channel work */
static void * hda_dma_channel_P2M_work(void *data)
{
    struct hda_dma_chan *hda_dma_chan = data;

    open_dmac_file(hda_dma_chan);

    do {
        dma_sleep(DMA_P2M_BURST_SLEEP);
    } while (dma_P2M_copy_burst(hda_dma_chan));

    close_dmac_file(hda_dma_chan);

    return NULL;
}

/* mem to peripheral channel work */
static void * hda_dma_channel_M2P_work(void *data)
{
    struct hda_dma_chan *hda_dma_chan = data;

    open_dmac_file(hda_dma_chan);

    do {
        dma_sleep(DMA_M2P_BURST_SLEEP);
    } while (dma_M2P_copy_burst(hda_dma_chan));

    close_dmac_file(hda_dma_chan);

    return NULL;
}

static void dma_P2M_start(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];

    /* prepare timer context */
    hda_dma_chan->bytes = 0;
    hda_dma_chan->tbytes = 0;
    chan_reset(dmac, chan);

    qemu_thread_create(&hda_dma_chan->thread, hda_dma_chan->thread_name,
        hda_dma_channel_P2M_work, hda_dma_chan, QEMU_THREAD_DETACHED);
}

static void dma_M2P_start(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];

    /* prepare timer context */
    hda_dma_chan->bytes = 0;
    hda_dma_chan->tbytes = 0;
    chan_reset(dmac, chan);

    qemu_thread_create(&hda_dma_chan->thread, hda_dma_chan->thread_name,
        hda_dma_channel_M2P_work, hda_dma_chan, QEMU_THREAD_DETACHED);
}

static void dma_Mdsp2Mhost_start(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];

    /* prepare timer context */
    hda_dma_chan->bytes = 0;
    hda_dma_chan->tbytes = 0;
    chan_reset(dmac, chan);

    qemu_thread_create(&hda_dma_chan->thread, hda_dma_chan->thread_name,
        hda_dma_channel_Mdsp2Mhost_work, hda_dma_chan, QEMU_THREAD_DETACHED);
}

static void dma_Mhost2Mdsp_start(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];

    /* prepare timer context */
    hda_dma_chan->bytes = 0;
    hda_dma_chan->tbytes = 0;
    chan_reset(dmac, chan);

    qemu_thread_create(&hda_dma_chan->thread, hda_dma_chan->thread_name,
        hda_dma_channel_Mhost2Mdsp_work, hda_dma_chan, QEMU_THREAD_DETACHED);
}

/* init new DMA mem to mem transfer after host is ready */
static void dma_M2M_do_transfer(struct hda_dma_chan *hda_dma_chan,
    uint32_t chan, int direction)
{
    struct adsp_hda_dmac *dmac = hda_dma_chan->dmac;
    void *ptr = NULL;
    uint32_t size;
    int err;

    size = dmac->io[DGBFPI(chan) >> 2];

    /* create SHM region for DMA */
    err = qemu_io_register_shm(hda_dma_chan->thread_name,
        ADSP_IO_SHM_DMA(dmac->id, chan), size, &ptr);
    if (err < 0) {
        fprintf(stderr, "error: can't create SHM size 0x%x for DMAC %d chan %d\n", size,
            dmac->id, chan);
        return;
    }

    /* prepare timer context */
    hda_dma_chan->ptr = ptr;

    /* allocate new timer for DMAC channel and direction */
    if (direction == QEMU_IO_DMA_DIR_READ)
        dma_Mdsp2Mhost_start(dmac, chan);
    else
        dma_Mhost2Mdsp_start(dmac, chan);
}

/* stop DMA transaction */
static void dma_stop_transfer(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan = &dmac->hda_dma_chan[chan];

    /* prepare timer context */
    hda_dma_chan->stop = 1;

    log_text(dmac->log, LOG_DMA,
        "dma: %d:%d: stop SAR 0x%x DAR 0x%x size 0x%x total bytes 0x%x\n",
        dmac->id, chan, dmac->io[DGBRP(chan) >> 2], dmac->io[DGBWP(chan) >> 2],
        dmac->io[DGBFPI(chan) >> 2],
        hda_dma_chan->tbytes);
}

/* init new DMA mem to mem playback transfer */
static void dma_start_transfer(struct adsp_hda_dmac *dmac, uint32_t chan)
{
    struct hda_dma_chan *hda_dma_chan;

    /* prepare timer context */
    hda_dma_chan = &dmac->hda_dma_chan[chan];
    hda_dma_chan->stop = 0;
    hda_dma_chan->tbytes = 0;

    log_text(dmac->log, LOG_DMA,
        "dma: %d:%d:  size 0x%x\n",
        dmac->id, chan,
        dmac->io[DGBFPI(chan) >> 2]);

    switch (dmac->dir) {
    case DIR_HOST_IN:   /* host capture */
        dma_host_req(dmac, chan, QEMU_IO_DMA_DIR_READ); /* capture */
        break;
    case DIR_HOST_OUT:   /* host playback */
         dma_host_req(dmac, chan, QEMU_IO_DMA_DIR_WRITE); /* playback */
         break;
    case DIR_LINK_IN:   /* link capture */
         dma_P2M_start(dmac, chan);
         break;
    case DIR_LINK_OUT:   /* link playback */
        dma_M2P_start(dmac, chan);
        break;
    default:
        /* should not get here */
        log_text(dmac->log, LOG_DMA,
            "error: invalid DMA request %d - cant find source/target\n",
            dmac->dir);
        break;
    }
}

void hda_dmac_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_hda_dmac *dmac = info->private;
    const struct adsp_reg_space *gp_dmac_dev = dmac->desc;

    memset(dmac->io, 0, gp_dmac_dev->desc.size);
}

static uint64_t dmac_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_hda_dmac *dmac = info->private;
    const struct adsp_reg_space *gp_dmac_dev = dmac->desc;

    /* only print IO from guest */
    log_read(dmac->log, gp_dmac_dev, addr, size,
            dmac->io[addr >> 2]);

    return dmac->io[addr >> 2];
}

static void dmac_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_hda_dmac *dmac = info->private;
    const struct adsp_reg_space *gp_dmac_dev = dmac->desc;
    uint32_t set, clear;

    set = val & ~dmac->io[addr >> 2];
    clear = ~val & dmac->io[addr >> 2];

    log_write(dmac->log, gp_dmac_dev, addr, val, size,
            dmac->io[addr >> 2]);

    /* special case registers */
    switch (addr) {
    case DGCS(0):

        if ((set & DGCS_GEN) && (set & DGCS_FIFORDY)) {
            /* channel enabled */
             dmac->io[addr >> 2] = val;
             dma_start_transfer(dmac, 0);
        }
        if ((clear & DGCS_GEN) || (clear & DGCS_FIFORDY)) {
            dmac->io[addr >> 2] = val;
            dma_stop_transfer(dmac, 0);
        }
        dmac->io[addr >> 2] = val;
        break;

    case DGBRP(0):
        dmac->io[addr >> 2] = val;

    default:
        dmac->io[addr >> 2] = val;
        break;
    }
}

void hda_dma_msg(struct qemu_io_msg *msg)
{
    struct qemu_io_msg_dma32 *dma_msg = (struct qemu_io_msg_dma32 *)msg;
    struct hda_dma_chan *hda_dma_chan = (struct hda_dma_chan*)dma_msg->client_data;
    struct qemu_io_msg_dma32 *local_dma_msg = &hda_dma_chan->dma_msg;

    /* get host buffer address */
    local_dma_msg->host_data = dma_msg->host_data;

    if (msg->msg == QEMU_IO_DMA_REQ_READY) {

        dma_M2M_do_transfer(
            (struct hda_dma_chan*)dma_msg->client_data,
            dma_msg->chan_id, dma_msg->direction);
    }
}

const MemoryRegionOps hda_dmac_ops = {
    .read = dmac_read,
    .write = dmac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};
