/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Kernel/API/KeyCode.h>
#include <Kernel/API/MousePacket.h>

// Input through libinput, the same library Mutter, KWin and wlroots use.
//
// It owns the ugly parts we were hand-rolling: device discovery, hotplug,
// absolute vs relative pointers, acceleration curves and axis normalisation.
// Everything below is a plain translation into Serenity's MousePacket /
// KeyEvent, which is all ScreenInput wants.
namespace WindowServer::LinuxInput {

// Width/height are only used to scale absolute (tablet/touch) positions into
// the 0..0xffff range ScreenInput expects.
bool initialize(int screen_width, int screen_height);
void set_screen_size(int screen_width, int screen_height);

// Drains queued libinput events and invokes the callbacks below.
void dispatch();

Function<void(MousePacket const&)>& on_mouse();
Function<void(::KeyEvent const&)>& on_key();

// Fed by the embedded wlroots compositor instead of libinput: the compositor
// owns the input devices, and only events that miss every foreign surface come
// back here. Same evdev codes / pixel coordinates as libinput would deliver.
void feed_pointer_motion_absolute(double x_pixels, double y_pixels);
void feed_pointer_button(uint32_t evdev_button, bool pressed);
void feed_pointer_axis(double vertical, double horizontal);
void feed_keyboard_key(uint32_t evdev_code, bool pressed);

bool is_initialized();

}
