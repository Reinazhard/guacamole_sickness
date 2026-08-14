/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZRAM_VH_H_
#define _ZRAM_VH_H_

#if IS_ENABLED(CONFIG_ZRAM_GS_VENDOR_HOOK)
extern int zram_vh_init(void);
#else
static inline int zram_vh_init(void) { return 0; }
#endif

#endif /* _ZRAM_VH_H_ */
