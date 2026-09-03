// SPDX-License-Identifier: GPL-2.0 only
/*
 *  Emerald Hill compression engine driver internal header
 *
 *  Copyright (C) 2020 Google LLC
 *  Author: Petri Gynther <pgynther@google.com>
 */
#ifndef _EH_INTERNAL_H
#define _EH_INTERNAL_H

#include <linux/eh.h>
#include "eh_regs.h"
#include <linux/pm_qos.h>
#include <linux/spinlock_types.h>
#include <linux/wait.h>

struct eh_completion {
	void *priv;
};

#define EH_MAX_DCMD 8

#define EH_QUIRK_IGNORE_GCTRL_RESET BIT(0)

struct eh_sw_fifo {
	struct list_head head;
	spinlock_t lock;
	bool has_reqs;
};

struct eh_device {
	struct list_head eh_dev_list;

	/* hardware characteristics */

	/* how many decompression command sets are implemented */
	unsigned int decompr_cmd_count;

	/* relating to the fifo and masks used to do related calculations */
	unsigned short fifo_size;
	unsigned short fifo_index_mask;
	unsigned short fifo_color_mask;

	/* cached copy of HW write index */
	unsigned int write_index;

	/* cached copy of HW complete index */
	unsigned int complete_index;

	__iomem unsigned char *regs;

	/* in-memory allocated location (not aligned) for cacheable fifo */
	void *fifo_alloc;

	/* 64B aligned compression command fifo of either type 0 or type 1 */
	void *fifo;

	spinlock_t fifo_prod_lock;

	/* Array of completions to keep track of each ongoing compression */
	struct eh_completion *completions;

	/* Array of pre-allocated buffers for compression */
	void **compr_buffers;

#ifdef CONFIG_GOOGLE_EH_DCMD_STATUS_IN_MEMORY
	unsigned long decompr_status[EH_MAX_DCMD];
#endif
	/* Array of pre-allocated bounce buffers for decompression */
	unsigned long __percpu *bounce_buffer;
	struct swait_queue_head cirq_wq;
	bool sync_comp_irq;
	int comp_irq;

	/* parent device */
	struct device *dev;

	/* EH clock */
	struct clk *clk;

	int error_irq;

	/*
	 * no interrupts, need to use a polling
	 * We use the polling for the HW validation testing .
	 */
#define EH_POLL_DELAY_MS 500

	unsigned short quirks;

	struct task_struct *comp_thread;
	wait_queue_head_t comp_wq;
	atomic_t nr_request;

	eh_cb_fn comp_callback;

	/*
	 * Every request the device has accepted and not yet fully completed:
	 * hardware ring + software FIFO + the in-callback window.
	 *
	 * nr_request covers only the ring, so it cannot answer "is the device
	 * idle" on its own. A request parked in the software FIFO in
	 * particular was invisible to eh_suspend() entirely, which is what let
	 * the clock be gated with work still outstanding and stranded the
	 * submitter's BIO reference forever.
	 *
	 * Incremented before the request becomes visible to the drain
	 * decision, and decremented only after the completion callback has
	 * released the BIO, so a non-zero value always means there is work
	 * that must not be abandoned.
	 */
	atomic_t nr_inflight;
	wait_queue_head_t idle_wq;

	/*
	 * Called by eh_suspend() to flush requests the upper layer is still
	 * holding. Required because cookies batched on a block plug are
	 * released only when the owning task unplugs, and a task the freezer
	 * has already stopped will never unplug.
	 */
	eh_drain_fn drain_cb;
	void *drain_priv;

	/* keep pending request */
	struct eh_sw_fifo sw_fifo;
#ifdef CONFIG_SOC_ZUMA
	int ip_index;
#endif

	struct pm_qos_request pm_qos_req;
};
#endif
