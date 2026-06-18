# SPDX-License-Identifier: MIT

obj-$(CONFIG_ZRAM_GS) += zram_gs.o
zram_gs-y := zcomp.o zram_drv.o
zram_gs-$(CONFIG_ZRAM_GS_VENDOR_HOOK) += zram_vh.o
zram_gs-$(CONFIG_ZRAM_GS_ANDROID_IOCTL) += zram_ioctl.o

obj-$(CONFIG_ZCOMP_CPU) += zcomp_gs_cpu.o
zcomp_gs_cpu-y := zcomp_cpu.o
zcomp_gs_cpu-$(CONFIG_ZRAM_GS_BACKEND_LZO) += backend_lzo.o backend_lzorle.o
zcomp_gs_cpu-$(CONFIG_ZRAM_GS_BACKEND_LZ4) += backend_lz4.o
zcomp_gs_cpu-$(CONFIG_ZRAM_GS_BACKEND_LZ4HC) += backend_lz4hc.o
zcomp_gs_cpu-$(CONFIG_ZRAM_GS_BACKEND_ZSTD) += backend_zstd.o
zcomp_gs_cpu-$(CONFIG_ZRAM_GS_BACKEND_DEFLATE) += backend_deflate.o
zcomp_gs_cpu-$(CONFIG_ZRAM_GS_BACKEND_842) += backend_842.o

obj-$(CONFIG_ZCOMP_EH) += zcomp_gs_eh.o
zcomp_gs_eh-y := zcomp_eh.o
