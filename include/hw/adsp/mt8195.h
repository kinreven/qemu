/* Core Audio DSP
 *
 * Copyright(c) 2022 Mediatek
 *
 * Author: Trevor Wu <trevor.wu@mediatek.com>
 *
 */

#ifndef __HW_ADSP_MT8195_H__
#define __HW_ADSP_MT8195_H__

#define ADSP_MT8195_DSP_MAILBOX_BASE      0x60800000

/* Mailbox configuration */
#define ADSP_SRAM_OUTBOX_BASE       ADSP_MT8195_DSP_MAILBOX_BASE
#define ADSP_SRAM_OUTBOX_SIZE       0x1000
#define ADSP_SRAM_OUTBOX_OFFSET     0

#define ADSP_SRAM_INBOX_BASE        (ADSP_SRAM_OUTBOX_BASE \
                                    + ADSP_SRAM_OUTBOX_SIZE)
#define ADSP_SRAM_INBOX_SIZE        0x1000
#define ADSP_SRAM_INBOX_OFFSET      ADSP_SRAM_OUTBOX_SIZE

#define ADSP_SRAM_DEBUG_BASE        (ADSP_SRAM_INBOX_BASE \
                                    + ADSP_SRAM_INBOX_SIZE)
#define ADSP_SRAM_DEBUG_SIZE        0x800
#define ADSP_SRAM_DEBUG_OFFSET      (ADSP_SRAM_INBOX_OFFSET \
                                    + ADSP_SRAM_INBOX_SIZE)

#define ADSP_SRAM_EXCEPT_BASE       (ADSP_SRAM_DEBUG_BASE \
                                    + ADSP_SRAM_DEBUG_SIZE)
#define ADSP_SRAM_EXCEPT_SIZE       0x800
#define ADSP_SRAM_EXCEPT_OFFSET     (ADSP_SRAM_DEBUG_OFFSET \
                                    + ADSP_SRAM_DEBUG_SIZE)

#define ADSP_SRAM_STREAM_BASE       (ADSP_SRAM_EXCEPT_BASE \
                                    + ADSP_SRAM_EXCEPT_SIZE)
#define ADSP_SRAM_STREAM_SIZE       0x1000
#define ADSP_SRAM_STREAM_OFFSET     (ADSP_SRAM_EXCEPT_OFFSET \
                                    + ADSP_SRAM_EXCEPT_SIZE)

#define ADSP_SRAM_TRACE_BASE        (ADSP_SRAM_STREAM_BASE \
                                    + ADSP_SRAM_STREAM_SIZE)
#define ADSP_SRAM_TRACE_SIZE        0x1000
#define ADSP_SRAM_TRACE_OFFSET      (ADSP_SRAM_STREAM_OFFSET \
                                    + ADSP_SRAM_STREAM_SIZE)

#define ADSP_MT8195_DSP_MAILBOX_SIZE  (ADSP_SRAM_INBOX_SIZE \
                                    + ADSP_SRAM_OUTBOX_SIZE \
                                    + ADSP_SRAM_DEBUG_SIZE \
                                    + ADSP_SRAM_EXCEPT_SIZE \
                                    + ADSP_SRAM_STREAM_SIZE \
                                    + ADSP_SRAM_TRACE_SIZE)

#define ADSP_MT8195_DSP_DRAM0_BASE     0x60000000
#define ADSP_MT8195_DSP_DRAM0_SIZE     0x1000000

#define ADSP_MT8195_DSP_SRAM_BASE     0x40000000
#define ADSP_MT8195_DSP_SRAM_SIZE     0x40000

#define ADSP_MT8195_TOPCKGEN_REG_BASE	0x10000000
#define ADSP_MT8195_TOPCKGEN_REG_SIZE	0x200

#define ADSP_MT8195_APMIXDSYS_REG_BASE	0x1000C000
#define ADSP_MT8195_APMIXDSYS_REG_SIZE	0x800

#define ADSP_MT8195_SCP_REG_BASE		0x10700000
#define ADSP_MT8195_SCP_REG_SIZE		0x21000

#define ADSP_MT8195_MBOX0_REG_BASE  0x10816000
#define ADSP_MT8195_MBOX0_REG_SIZE  0x1000
#define ADSP_MT8195_MBOX1_REG_BASE  0x10817000
#define ADSP_MT8195_MBOX1_REG_SIZE  0x1000
#define ADSP_MT8195_MBOX2_REG_BASE  0x10818000
#define ADSP_MT8195_MBOX2_REG_SIZE  0x1000

#define ADSP_MT8195_DSP_REG_BASE     0x10800000
#define ADSP_MT8195_DSP_REG_SIZE     0xe000

#define MTK_DSP_TIMER_BASE (0x10800000 - ADSP_MT8195_DSP_REG_BASE)
#define CNTCR (MTK_DSP_TIMER_BASE + 0x00)

#define MTK_DSP_OSTIMER_BASE (0x1080D000 - ADSP_MT8195_DSP_REG_BASE)
#define OSTIMER_CUR_L (MTK_DSP_OSTIMER_BASE + 0x8C)
#define OSTIMER_CUR_H (MTK_DSP_OSTIMER_BASE + 0x90)

#endif
