/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// SerenityOS ioctl numbers that host-side code needs.
//
// On SerenityOS these come from <Kernel/API/Ioctl.h>, which is a kernel header
// the host does not have. They are plain sequential enum values (enum
// IOCtlNumber), i.e. small integers, so they survive the trip unchanged.
//
// Keep in sync with Kernel/API/Ioctl.h (and with
// KernelModules/serenity_compat/serenity_compat.h, which must match for the
// /dev/audio driver).

#define SOUNDCARD_IOCTL_SET_SAMPLE_RATE 55
#define SOUNDCARD_IOCTL_GET_SAMPLE_RATE 56
