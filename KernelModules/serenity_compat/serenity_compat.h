/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * serenity_compat - kernel side of the SerenityOS-on-Linux port.
 *
 * Linux obviously cannot grow SerenityOS' syscall interface, but most of what
 * the ported userland actually talks to is plain device nodes and a handful of
 * virtual files. Those we can provide from a single module:
 *
 *   /dev/audio/0                     S16_LE stereo PCM sink (AudioServer)
 *   /sys/kernel/stats                JSON, CPU times  (ResourceGraph)
 *   /sys/kernel/memstat              JSON, memory     (ResourceGraph)
 *   /sys/kernel/net/adapters         JSON, interfaces (ResourceGraph, Network)
 */

#ifndef _SERENITY_COMPAT_H
#define _SERENITY_COMPAT_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * SerenityOS ioctl numbers. Upstream these are plain sequential enum values
 * (see Kernel/API/Ioctl.h, enum IOCtlNumber); they land in the "old style"
 * Linux range, which the VFS passes through untouched. Keep these in sync with
 * Kernel/API/Ioctl.h or AudioServer's set/get sample rate will silently fail.
 */
#define SOUNDCARD_IOCTL_SET_SAMPLE_RATE 55
#define SOUNDCARD_IOCTL_GET_SAMPLE_RATE 56

/* Device name; udev links it into place as /dev/audio/0. */
#define SERENITY_AUDIO_DEVICE_NAME "serenity_audio0"

struct serenity_audio_stats {
	__u64 bytes_written;
	__u64 bytes_played;
	__u64 underruns;
	__u32 sample_rate;
	__u32 pending_bytes;
};

#define SERENITY_AUDIO_IOCTL_STATS _IOR('S', 0x10, struct serenity_audio_stats)

#ifdef __KERNEL__
int serenity_audio_init(void);
void serenity_audio_exit(void);
int serenity_sysfs_init(void);
void serenity_sysfs_exit(void);
#endif

#endif /* _SERENITY_COMPAT_H */
