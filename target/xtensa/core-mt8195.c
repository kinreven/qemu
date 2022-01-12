#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/gdbstub.h"
#include "qemu-common.h"
#include "qemu/host-utils.h"

#include "core-mt8195/core-isa.h"
#include "core-mt8195/core-matmap.h"
#include "overlay_tool.h"

#define xtensa_modules xtensa_modules_mt8195
#include "core-mt8195/xtensa-modules.c.inc"

static XtensaConfig mt8195 __attribute__((unused)) = {
    .name = "mt8195",
    .gdb_regmap = {
        .reg = {
#include "core-mt8195/gdb-config.c.inc"
        }
    },
    .isa_internal = &xtensa_modules,
    .clock_freq_khz = 40000,
    DEFAULT_SECTIONS
};

REGISTER_CORE(mt8195)
