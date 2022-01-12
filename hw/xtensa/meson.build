xtensa_ss = ss.source_set()
xtensa_ss.add(files(
  'mx_pic.c',
  'pic_cpu.c',
  'xtensa_memory.c',
))
xtensa_ss.add(when: 'CONFIG_XTENSA_SIM', if_true: files('sim.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_VIRT', if_true: files('virt.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_XTFPGA', if_true: files('xtfpga.c'))

xtensa_ss.add(when: 'CONFIG_ADSP', if_true: files('adsp-common.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_INTEL_ADSP_BYT', if_true: files('byt.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_INTEL_ADSP_HSW', if_true: files('hsw.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_INTEL_ADSP_CAVS', if_true: files('cavs.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_NXP_ADSP', if_true: files('irqstr.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_NXP_ADSP_IMX8', if_true: files(
   'imx8-mu.c',
   'imx8.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_NXP_ADSP_IMX8M', if_true: files('imx8m.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_MTK_ADSP_MT8195', if_true: files('mt8195.c'))

hw_arch += {'xtensa': xtensa_ss}
