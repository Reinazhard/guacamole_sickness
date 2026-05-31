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

#define EH_POLL_DELAY_MS 500

struct eh_device {
	/* === Hot path fields (compression submission) === */
	void *fifo;
	struct eh_completion *completions;
	void **compr_buffers;

	spinlock_t fifo_prod_lock;
	unsigned int write_index;
	unsigned short fifo_size;
	unsigned short fifo_index_mask;
	unsigned short fifo_color_mask;

	/* === Hot path fields (completion processing) === */
	unsigned int complete_index;
	atomic_t nr_request;
	eh_cb_fn comp_callback;

	/* === IRQ / thread wakeup === */
	struct swait_queue_head cirq_wq;
	bool sync_comp_irq;
	int comp_irq;
	struct task_struct *comp_thread;
	wait_queue_head_t comp_wq;

	/* === SW FIFO for overflow === */
	struct eh_sw_fifo sw_fifo;

	/* === Cold path fields === */
	struct list_head eh_dev_list;
	unsigned int decompr_cmd_count;
	__iomem unsigned char *regs;
	void *fifo_alloc;

#ifdef CONFIG_GOOGLE_EH_DCMD_STATUS_IN_MEMORY
	unsigned long decompr_status[EH_MAX_DCMD];
#endif
	unsigned long __percpu *bounce_buffer;

	struct device *dev;
	struct clk *clk;
	int error_irq;
	unsigned short quirks;

#ifdef CONFIG_SOC_ZUMA
	int ip_index;
#endif

	struct pm_qos_request pm_qos_req;
};
#endif
