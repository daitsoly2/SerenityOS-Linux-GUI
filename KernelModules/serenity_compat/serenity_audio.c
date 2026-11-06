// SPDX-License-Identifier: BSD-2-Clause
/*
 * /dev/audio for the SerenityOS Linux port.
 *
 * SerenityOS' AudioServer opens /dev/audio/0 write-only, mixes everything into
 * S16_LE stereo at (by default) 44100 Hz and expects the write to block at
 * roughly real time, exactly like a hardware sink. So this driver is a virtual
 * sound card:
 *
 *   - write() appends PCM to a circular buffer and blocks once it is full.
 *   - read() lets a userspace bridge (ALSA/PipeWire) drain that buffer; the
 *     amount drained is the playback position.
 *   - A 10ms timer provides the playback clock when nobody is draining, so the
 *     device still behaves like real hardware without the bridge running.
 */

#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "serenity_compat.h"

#define DRIVER_NAME "serenity_audio"

/*
 * Ring size is latency: with a reader attached this is the most audio that can
 * sit unplayed, and for a writer with no reader it is the most that can be
 * buffered before write() blocks. 32 KiB is ~0.18s at 44.1 kHz stereo S16,
 * roughly one mixer buffer plus slack for VM scheduling jitter. Must be a power
 * of two.
 */
#define DEFAULT_RING_KIB 32
#define MIN_RING_KIB 8
#define MAX_RING_KIB 1024
#define BYTES_PER_FRAME 4 /* S16_LE, 2 channels */

#define DEFAULT_RATE 44100
#define MIN_RATE 8000
#define MAX_RATE 192000

#define TICK_MS 10
/* Without a reader for this long, fall back to timer-driven playback. */
#define READER_IDLE_MS 100

struct serenity_audio {
	struct miscdevice misc;
	u8 *ring;
	size_t size;
	size_t mask;

	/* Playback position and write position, in bytes since open. */
	atomic64_t written;
	atomic64_t played;

	atomic_t rate;
	atomic_t writers;
	atomic_t readers;

	atomic64_t underruns;
	unsigned long last_reader_active;
	ktime_t last_tick;

	spinlock_t lock;
	wait_queue_head_t read_queue;
	wait_queue_head_t write_queue;
	struct timer_list timer;
};

static unsigned int ring_kib = DEFAULT_RING_KIB;
module_param(ring_kib, uint, 0444);
MODULE_PARM_DESC(ring_kib, "Size of the PCM ring buffer in KiB (power of two)");

static struct serenity_audio *s_audio;

static inline size_t a_pending(struct serenity_audio *a)
{
	s64 pending = atomic64_read(&a->written) - atomic64_read(&a->played);
	return pending > 0 ? (size_t)pending : 0;
}

static inline size_t a_space(struct serenity_audio *a)
{
	size_t pending = a_pending(a);
	return pending >= a->size ? 0 : a->size - pending;
}

/**
 * a_advance_playback - consume bytes from the ring
 * @a: device
 * @bytes: how many bytes were played
 *
 * Never advances past the write position, so the accounting cannot drift into
 * negative pending counts if the timer and the bridge disagree.
 */
static void a_advance_playback(struct serenity_audio *a, size_t bytes)
{
	u64 written = atomic64_read(&a->written);
	u64 played = atomic64_read(&a->played);

	if (bytes > written - played)
		bytes = (size_t)(written - played);

	atomic64_add(bytes, &a->played);
}

/**
 * a_tick - playback clock for when no bridge is attached
 *
 * With a bridge reading the device, the read() calls are the clock. Without
 * one we still have to pretend the sound card is playing, otherwise AudioServer
 * would block forever after the first ring-full.
 */
static void a_tick(struct timer_list *t)
{
	struct serenity_audio *a = timer_container_of(a, t, timer);
	ktime_t now = ktime_get();
	s64 elapsed_ns;
	unsigned int rate;
	u64 bytes;

	elapsed_ns = ktime_to_ns(ktime_sub(now, a->last_tick));
	if (elapsed_ns <= 0)
		goto rearm;
	a->last_tick = now;

	/* A reader drives the clock itself; only writers need the fallback. */
	if (atomic_read(&a->writers) == 0)
		goto rearm;

	if (!time_after(jiffies, a->last_reader_active +
				  msecs_to_jiffies(READER_IDLE_MS)))
		goto rearm;

	/*
	 * Advance by the time that actually elapsed rather than assuming the
	 * timer fired exactly TICK_MS ago: mod_timer() is quantised to HZ (on a
	 * 250 Hz kernel a "10 ms" timer really fires every 12-20 ms), so a fixed
	 * per-tick byte count would play back slower than real time.
	 */
	rate = atomic_read(&a->rate);
	bytes = (u64)rate * BYTES_PER_FRAME * (u64)elapsed_ns;
	do_div(bytes, NSEC_PER_SEC);

	if (bytes > a_pending(a)) {
		/* Only a real drop is an underrun; an empty ring is just idle. */
		if (a_pending(a) > 0)
			atomic64_inc(&a->underruns);
		bytes = a_pending(a);
	}
	a_advance_playback(a, (size_t)bytes);
	wake_up_interruptible(&a->write_queue);

rearm:
	mod_timer(&a->timer, jiffies + msecs_to_jiffies(TICK_MS));
}

static ssize_t serenity_audio_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct serenity_audio *a = file->private_data;
	size_t copied = 0;

	if (count == 0)
		return 0;

	while (copied < count) {
		size_t space, chunk, offset, room;
		u64 written;

		space = a_space(a);
		if (space == 0) {
			if (file->f_flags & O_NONBLOCK)
				return copied ? copied : -EAGAIN;
			if (wait_event_interruptible_timeout(
				    a->write_queue, a_space(a) > 0,
				    msecs_to_jiffies(TICK_MS * 5)) < 0)
				return copied ? copied : -ERESTARTSYS;
			continue;
		}

		written = atomic64_read(&a->written);
		offset = (size_t)(written & a->mask);
		room = a->size - offset;
		chunk = min3(count - copied, space, room);

		if (copy_from_user(a->ring + offset, ubuf + copied, chunk))
			return copied ? copied : -EFAULT;

		atomic64_add(chunk, &a->written);
		copied += chunk;
	}

	wake_up_interruptible(&a->read_queue);
	return copied;
}

static ssize_t serenity_audio_read(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct serenity_audio *a = file->private_data;
	size_t pending, chunk, offset;
	u64 played;

	if (count == 0)
		return 0;

	wait_event_interruptible_timeout(a->read_queue, a_pending(a) > 0,
					 msecs_to_jiffies(50));
	if (signal_pending(current))
		return -ERESTARTSYS;

	/*
	 * Count the reader as alive even when it found nothing: it polls every
	 * 50ms, and if we only refreshed the timestamp on non-empty reads the
	 * playback timer would decide the device is unattended in the gaps and
	 * steal samples from under it.
	 */
	a->last_reader_active = jiffies;

	pending = a_pending(a);
	if (pending == 0)
		return 0; /* bridge polls again */

	played = atomic64_read(&a->played);
	offset = (size_t)(played & a->mask);
	chunk = min3(count, pending, a->size - offset);

	if (copy_to_user(ubuf, a->ring + offset, chunk))
		return -EFAULT;

	a_advance_playback(a, chunk);
	a->last_reader_active = jiffies;
	wake_up_interruptible(&a->write_queue);
	return chunk;
}

static __poll_t serenity_audio_poll(struct file *file, poll_table *wait)
{
	struct serenity_audio *a = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &a->read_queue, wait);
	poll_wait(file, &a->write_queue, wait);

	if (a_pending(a) > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (a_space(a) > 0)
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static long serenity_audio_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct serenity_audio *a = file->private_data;

	switch (cmd) {
	case SOUNDCARD_IOCTL_SET_SAMPLE_RATE: {
		/*
		 * Serenity passes the rate by value (ioctl(fd, req, rate)), not as
		 * a pointer - see Kernel/Devices/Audio/Channel.cpp.
		 */
		u32 rate = (u32)arg;

		if (rate == 0)
			return -EINVAL;
		rate = clamp_val(rate, MIN_RATE, MAX_RATE);
		atomic_set(&a->rate, rate);
		a->last_reader_active = jiffies;
		return 0;
	}
	case SOUNDCARD_IOCTL_GET_SAMPLE_RATE: {
		u32 rate = (u32)atomic_read(&a->rate);

		return copy_to_user((void __user *)arg, &rate, sizeof(rate)) ?
			       -EFAULT :
			       0;
	}
	case SERENITY_AUDIO_IOCTL_STATS: {
		struct serenity_audio_stats stats;

		stats.bytes_written = atomic64_read(&a->written);
		stats.bytes_played = atomic64_read(&a->played);
		stats.underruns = atomic64_read(&a->underruns);
		stats.sample_rate = (u32)atomic_read(&a->rate);
		stats.pending_bytes = (u32)a_pending(a);
		return copy_to_user((void __user *)arg, &stats, sizeof(stats)) ?
			       -EFAULT :
			       0;
	}
	default:
		return -ENOTTY;
	}
}

static int serenity_audio_open(struct inode *inode, struct file *file)
{
	struct serenity_audio *a = s_audio;

	if (file->f_mode & FMODE_WRITE) {
		/* One mixer owns the sink, like a real sound card. */
		if (atomic_inc_return(&a->writers) > 1) {
			atomic_dec(&a->writers);
			return -EBUSY;
		}
	}
	if (file->f_mode & FMODE_READ)
		atomic_inc(&a->readers);

	if (file->f_mode & FMODE_WRITE) {
		/* Start fresh so a restarted AudioServer does not inherit latency. */
		atomic64_set(&a->written, 0);
		atomic64_set(&a->played, 0);
		atomic64_set(&a->underruns, 0);
	}

	a->last_reader_active = jiffies;
	file->private_data = a;
	return 0;
}

static int serenity_audio_release(struct inode *inode, struct file *file)
{
	struct serenity_audio *a = file->private_data;

	if (file->f_mode & FMODE_WRITE)
		atomic_dec(&a->writers);
	if (file->f_mode & FMODE_READ)
		atomic_dec(&a->readers);

	wake_up_interruptible(&a->write_queue);
	wake_up_interruptible(&a->read_queue);
	return 0;
}

static const struct file_operations serenity_audio_fops = {
	.owner = THIS_MODULE,
	.open = serenity_audio_open,
	.release = serenity_audio_release,
	.read = serenity_audio_read,
	.write = serenity_audio_write,
	.poll = serenity_audio_poll,
	.unlocked_ioctl = serenity_audio_ioctl,
	.llseek = noop_llseek,
};

int serenity_audio_init(void)
{
	struct serenity_audio *a;
	int ret;

	if (ring_kib < MIN_RING_KIB || ring_kib > MAX_RING_KIB ||
	    !is_power_of_2(ring_kib))
		return -EINVAL;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	a->size = (size_t)ring_kib * 1024;
	a->mask = a->size - 1;
	a->ring = vzalloc(a->size);
	if (!a->ring) {
		kfree(a);
		return -ENOMEM;
	}

	spin_lock_init(&a->lock);
	init_waitqueue_head(&a->read_queue);
	init_waitqueue_head(&a->write_queue);
	atomic64_set(&a->written, 0);
	atomic64_set(&a->played, 0);
	atomic64_set(&a->underruns, 0);
	atomic_set(&a->rate, DEFAULT_RATE);
	atomic_set(&a->writers, 0);
	atomic_set(&a->readers, 0);
	a->last_reader_active = jiffies - msecs_to_jiffies(READER_IDLE_MS) - 1;
	a->last_tick = ktime_get();
	timer_setup(&a->timer, a_tick, 0);

	a->misc.minor = MISC_DYNAMIC_MINOR;
	a->misc.name = SERENITY_AUDIO_DEVICE_NAME;
	a->misc.fops = &serenity_audio_fops;
	a->misc.mode = 0666;

	ret = misc_register(&a->misc);
	if (ret) {
		vfree(a->ring);
		kfree(a);
		return ret;
	}

	s_audio = a;
	mod_timer(&a->timer, jiffies + msecs_to_jiffies(TICK_MS));

	pr_info(DRIVER_NAME ": /dev/%s ready (%d Hz, S16_LE stereo, %u KiB ring)\n",
		SERENITY_AUDIO_DEVICE_NAME, DEFAULT_RATE, ring_kib);
	return 0;
}

void serenity_audio_exit(void)
{
	struct serenity_audio *a = s_audio;

	if (!a)
		return;

	s_audio = NULL;
	timer_delete_sync(&a->timer);
	misc_deregister(&a->misc);
	wake_up_interruptible(&a->write_queue);
	wake_up_interruptible(&a->read_queue);
	vfree(a->ring);
	kfree(a);
}
