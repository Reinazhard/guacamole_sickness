// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (C) 2024 Google, Inc.
 */

#define pr_fmt(fmt) "fth: " fmt

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
#include <goog_touch_interface.h>
#include <linux/notifier.h>
#endif
#include "fps_touch_handler.h"

#define FTH_FLAG_OPEN	0	/* /dev/fth_fd is open; only ever once */
#define FTH_FLAG_ZOMBIE	1	/* the platform device is going away */

/*
 * A touchscreen reports one frame at a time: it sets a slot's properties,
 * moves on to the next slot with ABS_MT_SLOT, and closes the frame with
 * SYN_REPORT.  Properties are latched into ->cur as they arrive; ->last is
 * the same slot as it looked at the end of the previous frame.
 */
struct touch_event {
	int x, y;
	int major, minor, orientation;
	int id;			/* tracking ID, negative once the finger lifts */
	ktime_t down;		/* ktime of the press, 0 while lifted */
	bool dirty;		/* a property arrived during this frame */
};

struct finger {
	struct touch_event cur, last;
	int dx, dy;		/* travel accumulated since the last report */
	bool in_aoi;		/* inside the configured area of interest */
};

struct fth_drvdata {
	struct device *dev;
	struct class *class;
	struct cdev cdev;
	dev_t dev_no;

	/* Serialises the input path against the ioctl and release paths. */
	spinlock_t lock;
	struct fth_touch_config_v6 config, up_config;
	struct finger fingers[FTH_MAX_FINGERS];
	int slot;
	int wakelocks;
	bool lptw_enabled;
	unsigned long flags;

	/* Serialises the staging buffer against concurrent readers. */
	struct mutex read_lock;
	DECLARE_KFIFO(fifo, struct fth_touch_event_v7, FTH_MAX_FD_EVENTS);
	wait_queue_head_t wait;
	struct fth_fd_buf_v7 buf;

	struct input_dev *touch_dev;
};

static struct input_handler fth_touch_handler;

static void fth_reset_fingers(struct fth_drvdata *d)
{
	int i;

	for (i = 0; i < FTH_MAX_FINGERS; i++) {
		memset(&d->fingers[i], 0, sizeof(d->fingers[i]));
		d->fingers[i].cur.id = -1;
		d->fingers[i].last.id = -1;
	}
}

static bool fth_in_aoi(const struct fth_touch_config_v6 *cfg,
		       const struct touch_event *e)
{
	return e->x >= cfg->left && e->x <= cfg->right &&
	       e->y >= cfg->top && e->y <= cfg->bottom;
}

/*
 * Accumulate a finger's travel and say whether it has moved far enough to be
 * worth reporting.  The accumulator is only cleared when an event is actually
 * reported, so slow drift still adds up.
 */
static bool fth_moved_enough(struct fth_drvdata *d, struct finger *f,
			     const struct touch_event *last)
{
	f->dx += f->cur.x - last->x;
	f->dy += f->cur.y - last->y;

	if (!d->config.rad_filter_enable ||
	    abs(f->dx) > d->config.rad_x || abs(f->dy) > d->config.rad_y) {
		f->dx = 0;
		f->dy = 0;
		return true;
	}

	return false;
}

/*
 * Resolve the frame that SYN_REPORT just closed.  Called with d->lock held.
 * Returns true if at least one event was queued for the HAL.
 */
static bool fth_process_frame(struct fth_drvdata *d, ktime_t ts)
{
	struct fth_touch_event_v7 ev = { .touch_valid = true };
	bool queued = false;
	int i;

	/*
	 * Every event carries the position of every finger that is down, not
	 * just the one the event is about, so take that snapshot up front.
	 */
	for (i = 0; i < FTH_MAX_FINGERS; i++) {
		const struct touch_event *c = &d->fingers[i].cur;

		if (c->id < 0)
			continue;

		ev.X[i] = c->x;
		ev.Y[i] = c->y;
		ev.down_time_us[i] = ktime_to_us(c->down);
		ev.updated[i] = true;
		ev.num_fingers++;
	}

	ev.time_us = ktime_to_us(ts);

	for (i = 0; i < FTH_MAX_FINGERS; i++) {
		struct finger *f = &d->fingers[i];
		struct touch_event last;

		if (!f->cur.dirty)
			continue;

		f->cur.dirty = false;
		last = f->last;
		f->last = f->cur;

		if (f->cur.id < 0)
			ev.state = FTH_TOUCH_STATE_UP;
		else if (last.id < 0)
			ev.state = FTH_TOUCH_STATE_DOWN;
		else if (last.id == f->cur.id)
			ev.state = FTH_TOUCH_STATE_MOVE;
		else	/* a lift went missing, so treat this as a fresh press */
			ev.state = FTH_TOUCH_STATE_DOWN;

		if (!f->in_aoi) {
			if (fth_in_aoi(&d->config, &f->cur) && f->cur.id >= 0) {
				ev.state = FTH_TOUCH_STATE_DOWN;
				f->in_aoi = true;
			} else if (!d->lptw_enabled) {
				/* Outside the AOI with no gesture to pass on. */
				continue;
			} else if (ev.state == FTH_TOUCH_STATE_DOWN) {
				/*
				 * The finger landed outside the AOI, so the
				 * HAL only ever hears about it as a move.
				 */
				ev.state = FTH_TOUCH_STATE_MOVE;
			}
		} else if (f->cur.id < 0) {
			f->in_aoi = false;
		} else if (!fth_in_aoi(&d->up_config, &f->cur)) {
			/* Left even the enlarged AOI: the finger is gone. */
			ev.state = FTH_TOUCH_STATE_UP;
			f->in_aoi = false;
		}

		if (ev.state == FTH_TOUCH_STATE_MOVE &&
		    !fth_moved_enough(d, f, &last))
			continue;

		ev.slot = i;
		if (f->cur.id >= 0) {
			ev.major = f->cur.major;
			ev.minor = f->cur.minor;
			ev.orientation = f->cur.orientation;
		}

		kfifo_put(&d->fifo, ev);
		queued = true;
	}

	return queued;
}

static void fth_touch_event(struct input_handle *handle, unsigned int type,
			    unsigned int code, int value)
{
	struct fth_drvdata *d = handle->handler->private;
	struct input_dev *dev = handle->dev;
	struct finger *f;
	unsigned long flags;
	bool queued = false;
	int i;

	if (!d)
		return;

	spin_lock_irqsave(&d->lock, flags);

	if (!d->config.touch_fd_enable)
		goto unlock;

	if (type != EV_SYN && type != EV_ABS)
		goto unlock;

	if (code == ABS_MT_SLOT) {
		/*
		 * The touchscreen is not supposed to name a slot the HAL has
		 * no room for, and a bogus value would index out of bounds
		 * for the rest of the frame.
		 */
		d->slot = (unsigned int)value < FTH_MAX_FINGERS ? value : -1;
		goto unlock;
	}

	if (code == SYN_REPORT) {
		queued = fth_process_frame(d, dev->timestamp[INPUT_CLK_MONO]);
		goto unlock;
	}

	if (d->slot < 0)
		goto unlock;

	f = &d->fingers[d->slot];

	switch (code) {
	case ABS_MT_TRACKING_ID:
		f->cur.id = value;
		f->cur.down = value >= 0 ? dev->timestamp[INPUT_CLK_MONO] : 0;
		break;
	case ABS_MT_POSITION_X:
		f->cur.x = abs(value);
		break;
	case ABS_MT_POSITION_Y:
		f->cur.y = abs(value);
		break;
	case ABS_MT_TOUCH_MAJOR:
		f->cur.major = value;
		break;
	case ABS_MT_TOUCH_MINOR:
		f->cur.minor = value;
		break;
	case ABS_MT_ORIENTATION:
		f->cur.orientation = value;
		break;
	case ABS_MT_TOOL_TYPE:
		/*
		 * A palm means the contacts the touchscreen just reported are
		 * not fingers at all, so lift every one of them.
		 */
		if (value != MT_TOOL_PALM)
			goto unlock;

		for (i = 0; i < FTH_MAX_FINGERS; i++) {
			d->fingers[i].cur.id = -1;
			d->fingers[i].cur.dirty = true;
		}
		goto unlock;
	default:
		goto unlock;
	}

	f->cur.dirty = true;

unlock:
	spin_unlock_irqrestore(&d->lock, flags);

	if (queued)
		wake_up_interruptible(&d->wait);
}

static int fth_touch_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct fth_drvdata *d = handler->private;
	struct input_handle *handle;
	int ret;

	/* Only the built-in touchscreen drives the fingerprint sensor. */
	if (!d || !dev->uniq || strncmp(dev->uniq, "google_touchscreen", 18))
		return -ENODEV;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "fth_touch";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;

	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;

	d->touch_dev = dev;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void fth_touch_disconnect(struct input_handle *handle)
{
	struct fth_drvdata *d = handle->handler->private;

	input_close_device(handle);
	input_unregister_handle(handle);
	if (d && d->touch_dev == handle->dev)
		d->touch_dev = NULL;
	kfree(handle);
}

static const struct input_device_id fth_touch_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_ABS) },
	},
	{}
};
MODULE_DEVICE_TABLE(input, fth_touch_ids);

static struct input_handler fth_touch_handler = {
	.event = fth_touch_event,
	.connect = fth_touch_connect,
	.disconnect = fth_touch_disconnect,
	.name = "fth_touch",
	.id_table = fth_touch_ids,
};

static int fth_open(struct inode *inode, struct file *file)
{
	struct fth_drvdata *d = container_of(inode->i_cdev,
					     struct fth_drvdata, cdev);

	if (test_bit(FTH_FLAG_ZOMBIE, &d->flags))
		return -ENODEV;

	if (test_and_set_bit(FTH_FLAG_OPEN, &d->flags))
		return -EBUSY;

	file->private_data = d;
	return 0;
}

static int fth_release(struct inode *inode, struct file *file)
{
	struct fth_drvdata *d = file->private_data;
	unsigned long flags;
	bool held;

	spin_lock_irqsave(&d->lock, flags);
	d->config.touch_fd_enable = false;
	fth_reset_fingers(d);
	kfifo_reset(&d->fifo);
	held = d->wakelocks;
	d->wakelocks = 0;
	spin_unlock_irqrestore(&d->lock, flags);

	if (held)
		pm_relax(d->dev);

	clear_bit(FTH_FLAG_OPEN, &d->flags);
	return 0;
}

static ssize_t fth_read(struct file *file, char __user *ubuf, size_t count,
			loff_t *ppos)
{
	struct fth_drvdata *d = file->private_data;
	unsigned long flags;
	ssize_t ret;
	int n;

	if (count < sizeof(d->buf))
		return -EINVAL;

	if (mutex_lock_interruptible(&d->read_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&d->fifo)) {
		if (test_bit(FTH_FLAG_ZOMBIE, &d->flags)) {
			ret = -ENODEV;
			goto out;
		}

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(d->wait,
					     !kfifo_is_empty(&d->fifo) ||
					     test_bit(FTH_FLAG_ZOMBIE, &d->flags))) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	memset(&d->buf, 0, sizeof(d->buf));

	spin_lock_irqsave(&d->lock, flags);
	for (n = 0; n < FTH_MAX_FD_EVENTS; n++) {
		if (!kfifo_get(&d->fifo, &d->buf.fd_events[n]))
			break;
	}
	spin_unlock_irqrestore(&d->lock, flags);

	d->buf.num_events = n;
	ret = sizeof(d->buf);

	if (copy_to_user(ubuf, &d->buf, sizeof(d->buf)))
		ret = -EFAULT;

out:
	mutex_unlock(&d->read_lock);
	return ret;
}

static __poll_t fth_poll(struct file *file, struct poll_table_struct *wait)
{
	struct fth_drvdata *d = file->private_data;
	__poll_t mask = 0;

	if (test_bit(FTH_FLAG_ZOMBIE, &d->flags))
		return EPOLLHUP | EPOLLERR;

	poll_wait(file, &d->wait, wait);

	if (!kfifo_is_empty(&d->fifo))
		mask |= EPOLLIN | EPOLLRDNORM;

	if (test_bit(FTH_FLAG_ZOMBIE, &d->flags))
		mask |= EPOLLHUP | EPOLLERR;

	return mask;
}

static long fth_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fth_drvdata *d = file->private_data;
	void __user *uarg = (void __user *)arg;
	unsigned long flags;
	long rc = 0;
	bool edge;

	if (test_bit(FTH_FLAG_ZOMBIE, &d->flags))
		return -ENODEV;

	switch (cmd) {
	case FTH_IOCTL_SEND_KEY_EVENT:
		/*
		 * This driver has no input device to inject into, so the
		 * request can only be refused.  Say so rather than handing a
		 * NULL pointer to input_event().
		 */
		rc = -ENODEV;
		break;

	case FTH_IOCTL_ENABLE_LPTW_EVENT_REPORT:
	case FTH_IOCTL_DISABLE_LPTW_EVENT_REPORT:
		spin_lock_irqsave(&d->lock, flags);
		d->lptw_enabled = cmd == FTH_IOCTL_ENABLE_LPTW_EVENT_REPORT;
		spin_unlock_irqrestore(&d->lock, flags);
		break;

	case FTH_IOCTL_ACQUIRE_WAKELOCK:
		spin_lock_irqsave(&d->lock, flags);
		edge = !d->wakelocks++;
		spin_unlock_irqrestore(&d->lock, flags);
		if (edge)
			pm_stay_awake(d->dev);
		break;

	case FTH_IOCTL_RELEASE_WAKELOCK:
		spin_lock_irqsave(&d->lock, flags);
		edge = d->wakelocks && !--d->wakelocks;
		spin_unlock_irqrestore(&d->lock, flags);
		if (edge)
			pm_relax(d->dev);
		break;

	case FTH_IOCTL_GET_TOUCH_FD_VERSION: {
		const struct fth_touch_fd_version version = {
			.version = FTH_TOUCH_FD_VERSION_7,
		};

		if (copy_to_user(uarg, &version, sizeof(version)))
			rc = -EFAULT;
		break;
	}

	case FTH_IOCTL_CONFIGURE_TOUCH_FD_V7: {
		struct fth_touch_config_v6 cfg;
		int half_w, half_h;

		if (copy_from_user(&cfg, uarg, sizeof(cfg))) {
			rc = -EFAULT;
			break;
		}

		if (cfg.version.version != FTH_TOUCH_FD_VERSION_7) {
			rc = -EINVAL;
			break;
		}

		/*
		 * A finger may wander half the AOI's size outside it before it
		 * counts as lifted, so grow the box by that much.  The edges
		 * are signed 32-bit, so do the subtraction in 64 bits.
		 */
		half_w = (int)(((s64)cfg.right - cfg.left) / 2);
		half_h = (int)(((s64)cfg.bottom - cfg.top) / 2);

		spin_lock_irqsave(&d->lock, flags);
		d->config = cfg;
		d->up_config = cfg;
		d->up_config.left -= half_w;
		d->up_config.right += half_w;
		d->up_config.top -= half_h;
		d->up_config.bottom += half_h;
		/* A fresh configuration starts with no fingers down. */
		fth_reset_fingers(d);
		spin_unlock_irqrestore(&d->lock, flags);
		break;
	}

	case FTH_IOCTL_GET_TOUCH_DEVICE_STATUS: {
		const struct fth_touch_device_status status = {
			.is_connected = !!d->touch_dev,
		};

		if (copy_to_user(uarg, &status, sizeof(status)))
			rc = -EFAULT;
		break;
	}

	default:
		rc = -ENOIOCTLCMD;
		break;
	}

	return rc;
}

static const struct file_operations fth_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fth_ioctl,
	.open = fth_open,
	.release = fth_release,
	.read = fth_read,
	.poll = fth_poll,
};

static int fth_dev_register(struct fth_drvdata *d)
{
	struct device *dev;
	int ret;

	ret = alloc_chrdev_region(&d->dev_no, 0, 1, "fth");
	if (ret)
		return ret;

	cdev_init(&d->cdev, &fth_fops);
	d->cdev.owner = THIS_MODULE;

	ret = cdev_add(&d->cdev, d->dev_no, 1);
	if (ret)
		goto err_region;

	d->class = class_create(THIS_MODULE, "fth");
	if (IS_ERR(d->class)) {
		ret = PTR_ERR(d->class);
		goto err_cdev;
	}

	dev = device_create(d->class, NULL, d->dev_no, d, "fth_fd");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_class;
	}

	return 0;

err_class:
	class_destroy(d->class);
err_cdev:
	cdev_del(&d->cdev);
err_region:
	unregister_chrdev_region(d->dev_no, 1);
	return ret;
}

static void fth_dev_unregister(struct fth_drvdata *d)
{
	device_destroy(d->class, d->dev_no);
	class_destroy(d->class);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->dev_no, 1);
}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static void fth_lptw_report_event(int state, int x, int y, int major, int minor,
				  int orientation)
{
	struct fth_touch_event_v7 ev = {
		.time_us = ktime_to_us(ktime_get()),
		.X = { abs(x) },
		.Y = { abs(y) },
		.major = major,
		.minor = minor,
		.orientation = orientation,
		.slot = FTH_LPTW_FINGER_SLOT,
		.state = state,
		.touch_valid = true,
	};
	struct fth_drvdata *d;
	unsigned long flags;

	/*
	 * The notifier chain is walked under RCU and unregistering does not
	 * wait for a walk already in progress, so take the reference under
	 * rcu_read_lock() and let fth_remove() synchronise.
	 */
	rcu_read_lock();
	d = rcu_dereference(fth_touch_handler.private);
	if (d) {
		spin_lock_irqsave(&d->lock, flags);
		kfifo_put(&d->fifo, ev);
		spin_unlock_irqrestore(&d->lock, flags);
		wake_up_interruptible(&d->wait);
	}
	rcu_read_unlock();
}

static int fth_lptw_notifier_callback(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	int *param = data;

	fth_lptw_report_event(action, param[0], param[1], param[2], param[3],
			      param[4]);
	return NOTIFY_OK;
}

static struct notifier_block fth_notifier_block = {
	.notifier_call = fth_lptw_notifier_callback,
};
#endif

static int fth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fth_drvdata *d;
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->dev = dev;
	platform_set_drvdata(pdev, d);

	spin_lock_init(&d->lock);
	mutex_init(&d->read_lock);
	INIT_KFIFO(d->fifo);
	init_waitqueue_head(&d->wait);
	fth_reset_fingers(d);

	ret = fth_dev_register(d);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);
	rcu_assign_pointer(fth_touch_handler.private, d);

	ret = input_register_handler(&fth_touch_handler);
	if (ret) {
		rcu_assign_pointer(fth_touch_handler.private, NULL);
		synchronize_rcu();
		device_init_wakeup(dev, false);
		fth_dev_unregister(d);
		return ret;
	}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_lptw_notifier_register(&fth_notifier_block, true);
#endif

	return 0;
}

static int fth_remove(struct platform_device *pdev)
{
	struct fth_drvdata *d = platform_get_drvdata(pdev);
	unsigned long flags;
	bool held;

	set_bit(FTH_FLAG_ZOMBIE, &d->flags);
	wake_up_all(&d->wait);

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_lptw_notifier_register(&fth_notifier_block, false);
#endif

	rcu_assign_pointer(fth_touch_handler.private, NULL);
	synchronize_rcu();

	input_unregister_handler(&fth_touch_handler);

	spin_lock_irqsave(&d->lock, flags);
	held = d->wakelocks;
	d->wakelocks = 0;
	spin_unlock_irqrestore(&d->lock, flags);
	if (held)
		pm_relax(d->dev);

	device_init_wakeup(d->dev, false);
	fth_dev_unregister(d);
	return 0;
}

static const struct of_device_id fth_match[] = {
	{ .compatible = "google,fps-touch-handler" },
	{}
};
MODULE_DEVICE_TABLE(of, fth_match);

static struct platform_driver fth_plat_driver = {
	.probe = fth_probe,
	.remove = fth_remove,
	.driver = {
		.name = "fps_touch_handler",
		.of_match_table = fth_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(fth_plat_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Fingerprint Touch Handler");
