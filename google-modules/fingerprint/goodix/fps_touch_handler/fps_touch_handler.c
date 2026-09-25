// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fingerprint Touch Handler
 *
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (C) 2024 Google, Inc.
 * Copyright (C) 2026 Sultan Alsawaf <sultan@kerneltoast.com>
 */

#define pr_fmt(fmt) "fth: " fmt

#include <linux/atomic.h>
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

#define FTH_FLAG_OPEN	0
#define FTH_FLAG_ZOMBIE	1

struct touch_event {
	int X;
	int Y;
	int major;
	int minor;
	int orientation;
	int id;
	ktime_t down_ktime_mono;
	bool updated;
};

struct finger_detect_touch {
	struct fth_touch_config_v6 config;
	struct fth_touch_config_v6 up_config;
	struct touch_event current_events[FTH_MAX_FINGERS];
	struct touch_event last_events[FTH_MAX_FINGERS];
	int delta_X[FTH_MAX_FINGERS];
	int delta_Y[FTH_MAX_FINGERS];
	bool is_finger_in[FTH_MAX_FINGERS];
	int current_slot;
};

struct fth_drvdata {
	struct class *class;
	struct cdev cdev;
	dev_t dev_no;
	struct device *dev;
	struct input_dev *in_dev;
	struct input_dev *input_touch_dev;
	struct input_handle *handle;
	struct mutex ioctl_lock;
	struct mutex read_lock;
	spinlock_t fifo_lock;
	spinlock_t touch_lock;
	unsigned long flags;
	struct finger_detect_touch fd_touch;
	DECLARE_KFIFO(fd_events, struct fth_touch_event_v7, FTH_MAX_FD_EVENTS);
	wait_queue_head_t wait;
	struct fth_fd_buf_v7 scratch_buf;
	int wakelock_count;
	bool lptw_enabled;
};

static struct input_handler fth_touch_handler;

static void fth_fd_report_event(struct fth_drvdata *drvdata,
				const struct fth_touch_event_v7 *event)
{
	unsigned long flags;

	spin_lock_irqsave(&drvdata->fifo_lock, flags);
	kfifo_put(&drvdata->fd_events, *event);
	spin_unlock_irqrestore(&drvdata->fifo_lock, flags);

	wake_up_interruptible(&drvdata->wait);
}

static bool fth_touch_filter_aoi_region(const struct touch_event *event,
					const struct fth_touch_config_v6 *config)
{
	return event->X >= config->left && event->X <= config->right &&
	       event->Y >= config->top && event->Y <= config->bottom;
}

static bool fth_touch_filter_by_radius(struct fth_drvdata *drvdata,
				       const struct touch_event *cur,
				       const struct touch_event *last,
				       int slot)
{
	struct finger_detect_touch *fd = &drvdata->fd_touch;
	int del_x, del_y;

	fd->delta_X[slot] += cur->X - last->X;
	fd->delta_Y[slot] += cur->Y - last->Y;

	del_x = abs(fd->delta_X[slot]);
	del_y = abs(fd->delta_Y[slot]);

	if (!fd->config.rad_filter_enable ||
	    del_x > fd->config.rad_x || del_y > fd->config.rad_y) {
		fd->delta_X[slot] = 0;
		fd->delta_Y[slot] = 0;
		return true;
	}

	return false;
}

static void fth_touch_process_frame(struct fth_drvdata *drvdata,
				    ktime_t timestamp)
{
	struct finger_detect_touch *fd_touch = &drvdata->fd_touch;
	struct fth_touch_config_v6 *config = &fd_touch->config;
	struct fth_touch_config_v6 *large_config = &fd_touch->up_config;
	struct fth_touch_event_v7 base_event = { .touch_valid = true };
	int slot;

	for (slot = 0; slot < FTH_MAX_FINGERS; slot++) {
		struct touch_event *cur = &fd_touch->current_events[slot];

		if (cur->id >= 0) {
			base_event.X[slot] = cur->X;
			base_event.Y[slot] = cur->Y;
			base_event.down_time_us[slot] =
				ktime_to_us(cur->down_ktime_mono);
			base_event.updated[slot] = true;
			base_event.num_fingers++;
		}
	}

	for (slot = 0; slot < FTH_MAX_FINGERS; slot++) {
		struct touch_event *cur = &fd_touch->current_events[slot];
		struct touch_event *last = &fd_touch->last_events[slot];
		bool *is_finger_in = &fd_touch->is_finger_in[slot];
		struct fth_touch_event_v7 finger_event;
		bool in_small_aoi, in_large_aoi;

		if (!cur->updated)
			continue;
		cur->updated = false;

		finger_event = base_event;

		if (cur->id < 0)
			finger_event.state = FTH_TOUCH_STATE_UP;
		else if (last->id < 0)
			finger_event.state = FTH_TOUCH_STATE_DOWN;
		else if (last->id == cur->id)
			finger_event.state = FTH_TOUCH_STATE_MOVE;
		else
			finger_event.state = FTH_TOUCH_STATE_DOWN;

		in_small_aoi = fth_touch_filter_aoi_region(cur, config);
		in_large_aoi = fth_touch_filter_aoi_region(cur, large_config);

		if (!*is_finger_in) {
			if (in_small_aoi && cur->id >= 0) {
				finger_event.state = FTH_TOUCH_STATE_DOWN;
				*is_finger_in = true;
			} else if (drvdata->lptw_enabled) {
				if (finger_event.state == FTH_TOUCH_STATE_DOWN)
					finger_event.state = FTH_TOUCH_STATE_MOVE;
			} else {
				*last = *cur;
				continue;
			}
		} else {
			if (cur->id < 0) {
				*is_finger_in = false;
			} else if (!in_large_aoi) {
				finger_event.state = FTH_TOUCH_STATE_UP;
				*is_finger_in = false;
			}
		}

		if (finger_event.state == FTH_TOUCH_STATE_MOVE &&
		    !fth_touch_filter_by_radius(drvdata, cur, last, slot)) {
			*last = *cur;
			continue;
		}

		*last = *cur;

		finger_event.slot = slot;
		if (cur->id >= 0) {
			finger_event.major = cur->major;
			finger_event.minor = cur->minor;
			finger_event.orientation = cur->orientation;
		}
		finger_event.time_us = ktime_to_us(timestamp ? timestamp :
							       ktime_get());

		if (config->touch_fd_enable)
			fth_fd_report_event(drvdata, &finger_event);
	}
}

static void fth_touch_report_event(struct input_handle *handle,
				   unsigned int type, unsigned int code,
				   int value)
{
	struct fth_drvdata *drvdata = handle->handler->private;
	struct finger_detect_touch *fd_touch;
	struct touch_event *event;
	struct input_dev *dev = handle->dev;
	unsigned long flags;

	if (!drvdata)
		return;

	fd_touch = &drvdata->fd_touch;

	spin_lock_irqsave(&drvdata->touch_lock, flags);

	if (!fd_touch->config.touch_fd_enable)
		goto out_unlock;

	if (type != EV_SYN && type != EV_ABS)
		goto out_unlock;

	if (code == ABS_MT_SLOT) {
		if (value >= 0 && value < FTH_MAX_FINGERS)
			fd_touch->current_slot = value;
		else
			fd_touch->current_slot = -1;
		goto out_unlock;
	}

	if (code == SYN_REPORT) {
		fth_touch_process_frame(drvdata, dev->timestamp[INPUT_CLK_MONO]);
		goto out_unlock;
	}

	if (fd_touch->current_slot < 0 || fd_touch->current_slot >= FTH_MAX_FINGERS)
		goto out_unlock;

	event = &fd_touch->current_events[fd_touch->current_slot];

	switch (code) {
	case ABS_MT_TRACKING_ID:
		event->id = value;
		event->down_ktime_mono =
			(value >= 0) ? dev->timestamp[INPUT_CLK_MONO] : 0;
		event->updated = true;
		break;
	case ABS_MT_POSITION_X:
		event->X = abs(value);
		event->updated = true;
		break;
	case ABS_MT_POSITION_Y:
		event->Y = abs(value);
		event->updated = true;
		break;
	case ABS_MT_TOUCH_MAJOR:
		event->major = value;
		event->updated = true;
		break;
	case ABS_MT_TOUCH_MINOR:
		event->minor = value;
		event->updated = true;
		break;
	case ABS_MT_ORIENTATION:
		event->orientation = value;
		event->updated = true;
		break;
	case ABS_MT_TOOL_TYPE:
		if (value == MT_TOOL_PALM) {
			int slot;

			for (slot = 0; slot < FTH_MAX_FINGERS; slot++) {
				fd_touch->current_events[slot].id = -1;
				fd_touch->current_events[slot].updated = true;
			}
		}
		break;
	default:
		break;
	}

out_unlock:
	spin_unlock_irqrestore(&drvdata->touch_lock, flags);
}

static int fth_touch_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct fth_drvdata *drvdata = handler->private;
	struct input_handle *handle;
	int ret;

	if (!drvdata || !dev->uniq ||
	    strncmp(dev->uniq, "google_touchscreen", 18) != 0)
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

	drvdata->handle = handle;
	drvdata->input_touch_dev = dev;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void fth_touch_disconnect(struct input_handle *handle)
{
	struct fth_drvdata *drvdata = handle->handler->private;

	input_close_device(handle);
	input_unregister_handle(handle);
	if (drvdata && drvdata->input_touch_dev == handle->dev) {
		drvdata->input_touch_dev = NULL;
		drvdata->handle = NULL;
	}
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
	.event = fth_touch_report_event,
	.connect = fth_touch_connect,
	.disconnect = fth_touch_disconnect,
	.name = "fth_touch",
	.id_table = fth_touch_ids,
};

static int fth_open(struct inode *inode, struct file *file)
{
	struct fth_drvdata *drvdata =
		container_of(inode->i_cdev, struct fth_drvdata, cdev);

	if (test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags))
		return -ENODEV;

	if (test_and_set_bit(FTH_FLAG_OPEN, &drvdata->flags))
		return -EBUSY;

	file->private_data = drvdata;
	return 0;
}

static int fth_release(struct inode *inode, struct file *file)
{
	struct fth_drvdata *drvdata = file->private_data;
	unsigned long flags;
	int slot;

	mutex_lock(&drvdata->ioctl_lock);
	if (drvdata->wakelock_count) {
		pm_relax(drvdata->dev);
		drvdata->wakelock_count = 0;
	}
	mutex_unlock(&drvdata->ioctl_lock);

	spin_lock_irqsave(&drvdata->touch_lock, flags);
	drvdata->fd_touch.config.touch_fd_enable = false;
	for (slot = 0; slot < FTH_MAX_FINGERS; slot++) {
		drvdata->fd_touch.current_events[slot].id = -1;
		drvdata->fd_touch.last_events[slot].id = -1;
		drvdata->fd_touch.is_finger_in[slot] = false;
		drvdata->fd_touch.delta_X[slot] = 0;
		drvdata->fd_touch.delta_Y[slot] = 0;
	}
	spin_unlock_irqrestore(&drvdata->touch_lock, flags);

	spin_lock_irqsave(&drvdata->fifo_lock, flags);
	kfifo_reset(&drvdata->fd_events);
	spin_unlock_irqrestore(&drvdata->fifo_lock, flags);

	clear_bit(FTH_FLAG_OPEN, &drvdata->flags);
	return 0;
}

static ssize_t fth_read(struct file *filp, char __user *ubuf, size_t cnt,
			loff_t *ppos)
{
	struct fth_drvdata *drvdata = filp->private_data;
	int i;

	if (cnt < sizeof(drvdata->scratch_buf))
		return -EINVAL;

	if (mutex_lock_interruptible(&drvdata->read_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&drvdata->fd_events)) {
		mutex_unlock(&drvdata->read_lock);

		if (test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags))
			return -ENODEV;

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(drvdata->wait,
					     !kfifo_is_empty(&drvdata->fd_events) ||
					     test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags)))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&drvdata->read_lock))
			return -ERESTARTSYS;
	}

	if (test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags)) {
		mutex_unlock(&drvdata->read_lock);
		return -ENODEV;
	}

	memset(&drvdata->scratch_buf, 0, sizeof(drvdata->scratch_buf));

	spin_lock_irq(&drvdata->fifo_lock);
	for (i = 0; i < FTH_MAX_FD_EVENTS; i++) {
		if (!kfifo_get(&drvdata->fd_events,
			       &drvdata->scratch_buf.fd_events[i]))
			break;
	}
	drvdata->scratch_buf.num_events = i;
	spin_unlock_irq(&drvdata->fifo_lock);

	if (copy_to_user(ubuf, &drvdata->scratch_buf, sizeof(drvdata->scratch_buf))) {
		mutex_unlock(&drvdata->read_lock);
		return -EFAULT;
	}

	mutex_unlock(&drvdata->read_lock);
	return sizeof(drvdata->scratch_buf);
}

static __poll_t fth_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct fth_drvdata *drvdata = filp->private_data;
	__poll_t mask = 0;

	if (test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags))
		return EPOLLHUP | EPOLLERR;

	poll_wait(filp, &drvdata->wait, wait);
	if (!kfifo_is_empty(&drvdata->fd_events))
		mask |= (EPOLLIN | EPOLLRDNORM);

	if (test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags))
		mask |= (EPOLLHUP | EPOLLERR);

	return mask;
}

static long fth_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fth_drvdata *drvdata = file->private_data;
	void __user *priv_arg = (void __user *)arg;
	long rc = 0;

	if (test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags))
		return -ENODEV;

	mutex_lock(&drvdata->ioctl_lock);

	switch (cmd) {
	case FTH_IOCTL_SEND_KEY_EVENT: {
		struct fth_key_event key_event;

		if (!drvdata->in_dev) {
			rc = -ENODEV;
			break;
		}

		if (copy_from_user(&key_event, priv_arg, sizeof(key_event))) {
			rc = -EFAULT;
			break;
		}

		input_event(drvdata->in_dev, EV_KEY, key_event.key,
			    key_event.value);
		input_sync(drvdata->in_dev);
		break;
	}
	case FTH_IOCTL_ENABLE_LPTW_EVENT_REPORT:
		drvdata->lptw_enabled = true;
		break;
	case FTH_IOCTL_DISABLE_LPTW_EVENT_REPORT:
		drvdata->lptw_enabled = false;
		break;
	case FTH_IOCTL_ACQUIRE_WAKELOCK:
		if (!drvdata->wakelock_count++)
			pm_stay_awake(drvdata->dev);
		break;
	case FTH_IOCTL_RELEASE_WAKELOCK:
		if (drvdata->wakelock_count && !--drvdata->wakelock_count)
			pm_relax(drvdata->dev);
		break;
	case FTH_IOCTL_GET_TOUCH_FD_VERSION: {
		struct fth_touch_fd_version version = {
			.version = FTH_TOUCH_FD_VERSION_7
		};

		if (copy_to_user(priv_arg, &version, sizeof(version)))
			rc = -EFAULT;
		break;
	}
	case FTH_IOCTL_CONFIGURE_TOUCH_FD_V7: {
		struct fth_touch_config_v6 config;
		unsigned long flags;
		s64 width, height;
		int half_w, half_h, slot;

		if (copy_from_user(&config, priv_arg, sizeof(config))) {
			rc = -EFAULT;
			break;
		}

		if (config.version.version != FTH_TOUCH_FD_VERSION_7) {
			rc = -EINVAL;
			break;
		}

		if (config.left > config.right || config.top > config.bottom ||
		    config.rad_x < 0 || config.rad_y < 0) {
			rc = -EINVAL;
			break;
		}

		width = (s64)config.right - config.left;
		height = (s64)config.bottom - config.top;
		if (width > INT_MAX || height > INT_MAX) {
			rc = -EINVAL;
			break;
		}

		half_w = (int)(width / 2);
		half_h = (int)(height / 2);

		spin_lock_irqsave(&drvdata->touch_lock, flags);
		drvdata->fd_touch.config = config;
		drvdata->fd_touch.up_config = config;
		drvdata->fd_touch.up_config.left -= half_w;
		drvdata->fd_touch.up_config.right += half_w;
		drvdata->fd_touch.up_config.top -= half_h;
		drvdata->fd_touch.up_config.bottom += half_h;

		for (slot = 0; slot < FTH_MAX_FINGERS; slot++) {
			drvdata->fd_touch.current_events[slot].id = -1;
			drvdata->fd_touch.last_events[slot].id = -1;
			drvdata->fd_touch.is_finger_in[slot] = false;
			drvdata->fd_touch.delta_X[slot] = 0;
			drvdata->fd_touch.delta_Y[slot] = 0;
		}
		spin_unlock_irqrestore(&drvdata->touch_lock, flags);
		break;
	}
	case FTH_IOCTL_GET_TOUCH_DEVICE_STATUS: {
		struct fth_touch_device_status status = {
			.is_connected = drvdata->input_touch_dev != NULL
		};

		if (copy_to_user(priv_arg, &status, sizeof(status)))
			rc = -EFAULT;
		break;
	}
	default:
		rc = -ENOIOCTLCMD;
		break;
	}

	mutex_unlock(&drvdata->ioctl_lock);
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

static int fth_dev_register(struct fth_drvdata *drvdata)
{
	struct device *device;
	int ret;

	ret = alloc_chrdev_region(&drvdata->dev_no, 0, 1, "fth");
	if (ret)
		return ret;

	cdev_init(&drvdata->cdev, &fth_fops);
	drvdata->cdev.owner = THIS_MODULE;

	ret = cdev_add(&drvdata->cdev, drvdata->dev_no, 1);
	if (ret)
		goto err_chrdev;

	drvdata->class = class_create(THIS_MODULE, "fth");
	if (IS_ERR(drvdata->class)) {
		ret = PTR_ERR(drvdata->class);
		goto err_cdev;
	}

	device = device_create(drvdata->class, drvdata->dev, drvdata->dev_no,
			       drvdata, "fth_fd");
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto err_class;
	}

	return 0;

err_class:
	class_destroy(drvdata->class);
err_cdev:
	cdev_del(&drvdata->cdev);
err_chrdev:
	unregister_chrdev_region(drvdata->dev_no, 1);
	return ret;
}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static void fth_lptw_report_event(int state, int x, int y, int major, int minor,
				  int orientation)
{
	struct fth_drvdata *drvdata;
	struct fth_touch_event_v7 event = {
		.state = state,
		.X = { abs(x) },
		.Y = { abs(y) },
		.major = major,
		.minor = minor,
		.orientation = orientation,
		.slot = FTH_LPTW_FINGER_SLOT,
		.touch_valid = true,
		.time_us = ktime_to_us(ktime_get()),
	};

	rcu_read_lock();
	drvdata = rcu_dereference(fth_touch_handler.private);
	if (!drvdata || test_bit(FTH_FLAG_ZOMBIE, &drvdata->flags)) {
		rcu_read_unlock();
		return;
	}

	fth_fd_report_event(drvdata, &event);
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
	struct fth_drvdata *drvdata;
	int ret, slot;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = dev;
	platform_set_drvdata(pdev, drvdata);

	mutex_init(&drvdata->ioctl_lock);
	mutex_init(&drvdata->read_lock);
	spin_lock_init(&drvdata->fifo_lock);
	spin_lock_init(&drvdata->touch_lock);
	INIT_KFIFO(drvdata->fd_events);
	init_waitqueue_head(&drvdata->wait);

	for (slot = 0; slot < FTH_MAX_FINGERS; slot++) {
		drvdata->fd_touch.current_events[slot].id = -1;
		drvdata->fd_touch.last_events[slot].id = -1;
	}

	ret = fth_dev_register(drvdata);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);

	rcu_assign_pointer(fth_touch_handler.private, drvdata);
	ret = input_register_handler(&fth_touch_handler);
	if (ret)
		goto err_dev;

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_lptw_notifier_register(&fth_notifier_block, true);
#endif

	return 0;

err_dev:
	rcu_assign_pointer(fth_touch_handler.private, NULL);
	synchronize_rcu();
	device_init_wakeup(dev, false);
	device_destroy(drvdata->class, drvdata->dev_no);
	class_destroy(drvdata->class);
	cdev_del(&drvdata->cdev);
	unregister_chrdev_region(drvdata->dev_no, 1);
	return ret;
}

static int fth_remove(struct platform_device *pdev)
{
	struct fth_drvdata *drvdata = platform_get_drvdata(pdev);

	set_bit(FTH_FLAG_ZOMBIE, &drvdata->flags);
	wake_up_all(&drvdata->wait);

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_lptw_notifier_register(&fth_notifier_block, false);
#endif
	rcu_assign_pointer(fth_touch_handler.private, NULL);
	synchronize_rcu();

	input_unregister_handler(&fth_touch_handler);

	mutex_lock(&drvdata->ioctl_lock);
	if (drvdata->wakelock_count) {
		pm_relax(drvdata->dev);
		drvdata->wakelock_count = 0;
	}
	mutex_unlock(&drvdata->ioctl_lock);

	device_init_wakeup(drvdata->dev, false);
	device_destroy(drvdata->class, drvdata->dev_no);
	class_destroy(drvdata->class);
	cdev_del(&drvdata->cdev);
	unregister_chrdev_region(drvdata->dev_no, 1);
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
