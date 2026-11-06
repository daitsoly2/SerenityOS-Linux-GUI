// SPDX-License-Identifier: BSD-2-Clause
/*
 * serenity_compat - the kernel bits the SerenityOS Linux port needs.
 *
 * Kept in one module so the host only has to load (and DKMS-build) a single
 * object: the PCM sink plus the virtual kernel files the applets read.
 */

#include <linux/init.h>
#include <linux/module.h>

int serenity_audio_init(void);
void serenity_audio_exit(void);
int serenity_sysfs_init(void);
void serenity_sysfs_exit(void);

static int __init serenity_compat_init(void)
{
	int ret;

	/* Audio is the important half: bring it up first. */
	ret = serenity_audio_init();
	if (ret) {
		pr_err("serenity_compat: audio device failed: %d\n", ret);
		return ret;
	}

	ret = serenity_sysfs_init();
	if (ret) {
		/* Not fatal: applets degrade, the desktop still works. */
		pr_warn("serenity_compat: sysfs files failed: %d\n", ret);
		ret = 0;
	}

	return 0;
}

static void __exit serenity_compat_exit(void)
{
	serenity_sysfs_exit();
	serenity_audio_exit();
}

module_init(serenity_compat_init);
module_exit(serenity_compat_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("SerenityOS Linux GUI port");
MODULE_DESCRIPTION("SerenityOS compatibility device nodes (/dev/audio) and virtual kernel files");
