/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for WindowServer.
//
// The SerenityOS main.cpp talks to /dev/gpu, /dev/tty and uses pledge/unveil.
// None of that exists on Linux, so this entry point:
//   1. loads the theme + fonts from a normal resource directory,
//   2. creates a single "virtual" screen (presented by LinuxScreenBackend),
//   3. feeds SDL input events into ScreenInput.

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/Try.h>
#include <Kernel/API/KeyCode.h>
#include <Kernel/API/MousePacket.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <Ports/LinuxGUI/LinuxInput.h>
#include <Ports/LinuxGUI/LinuxScreenBackend.h>
#include <Ports/LinuxGUI/Notifications.h>
#include <Ports/LinuxGUI/Recorder.h>
#include <WindowServer/AppletManager.h>
#include <WindowServer/Compositor.h>
#include <WindowServer/EventLoop.h>
#include <WindowServer/MenuManager.h>
#include <WindowServer/Screen.h>
#include <WindowServer/ScreenLayout.h>
#include <WindowServer/WindowManager.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

namespace WindowServer {
RefPtr<Core::ConfigFile> g_config;
}

static ByteString resource_root()
{
    if (auto* value = getenv("SERENITY_RESOURCE_ROOT"); value && *value)
        return ByteString { value };
    return ByteString { "/res" };
}

// The kernel exposes each GPU connector under /sys/class/drm/<card>-<conn>.
// A connected connector provides a readable `edid` file containing the raw
// EDID binary. We point DisplaySettings at the first connected connector so it
// can show the real monitor identity and supported modes instead of the VESA
// fallback list. This is the Linux equivalent of SerenityOS's graphics
// connector device node.
static ByteString discover_connector_edid_path()
{
    auto dir = opendir("/sys/class/drm");
    if (!dir)
        return {};
    ByteString found;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        ByteString name { ent->d_name };
        if (!name.starts_with("card"sv))
            continue;
        auto status_path = ByteString::formatted("/sys/class/drm/{}/status", name);
        int fd = open(status_path.characters(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char buf[32] = { 0 };
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        if (!StringView { buf, (size_t)n }.starts_with("connected"sv))
            continue;
        auto edid_path = ByteString::formatted("/sys/class/drm/{}/edid", name);
        struct stat st;
        if (stat(edid_path.characters(), &st) == 0 && st.st_size > 0 && found.is_empty())
            found = edid_path;
    }
    closedir(dir);
    return found;
}

// Cached connector EDID path; empty when no connected connector was found.
static Optional<ByteString> edid_device_for_layout()
{
    static ByteString path = discover_connector_edid_path();
    if (path.is_empty())
        return {};
    return path;
}

static KeyCode sdl_keycode_to_key_code(SDL_Keycode sym)
{
    if (sym >= 32 && sym < 127)
        return code_point_to_key_code(static_cast<u32>(sym));

    switch (sym) {
    case SDLK_ESCAPE:
        return Key_Escape;
    case SDLK_TAB:
        return Key_Tab;
    case SDLK_BACKSPACE:
        return Key_Backspace;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return Key_Return;
    case SDLK_INSERT:
        return Key_Insert;
    case SDLK_DELETE:
        return Key_Delete;
    case SDLK_HOME:
        return Key_Home;
    case SDLK_END:
        return Key_End;
    case SDLK_LEFT:
        return Key_Left;
    case SDLK_RIGHT:
        return Key_Right;
    case SDLK_UP:
        return Key_Up;
    case SDLK_DOWN:
        return Key_Down;
    case SDLK_PAGEUP:
        return Key_PageUp;
    case SDLK_PAGEDOWN:
        return Key_PageDown;
    case SDLK_LSHIFT:
        return Key_LeftShift;
    case SDLK_RSHIFT:
        return Key_RightShift;
    case SDLK_LCTRL:
        return Key_LeftControl;
    case SDLK_RCTRL:
        return Key_RightControl;
    case SDLK_LALT:
        return Key_LeftAlt;
    case SDLK_RALT:
        return Key_RightAlt;
    case SDLK_LGUI:
        return Key_LeftSuper;
    case SDLK_RGUI:
        return Key_RightSuper;
    case SDLK_SPACE:
        return Key_Space;
    case SDLK_CAPSLOCK:
        return Key_CapsLock;
    case SDLK_F1:
        return Key_F1;
    case SDLK_F2:
        return Key_F2;
    case SDLK_F3:
        return Key_F3;
    case SDLK_F4:
        return Key_F4;
    case SDLK_F5:
        return Key_F5;
    case SDLK_F6:
        return Key_F6;
    case SDLK_F7:
        return Key_F7;
    case SDLK_F8:
        return Key_F8;
    case SDLK_F9:
        return Key_F9;
    case SDLK_F10:
        return Key_F10;
    case SDLK_F11:
        return Key_F11;
    case SDLK_F12:
        return Key_F12;
    default:
        return Key_Invalid;
    }
}

static unsigned sdl_modifiers()
{
    auto mod = SDL_GetModState();
    unsigned modifiers = 0;
    if (mod & KMOD_SHIFT)
        modifiers |= Mod_Shift;
    if (mod & KMOD_CTRL)
        modifiers |= Mod_Ctrl;
    if (mod & KMOD_ALT)
        modifiers |= Mod_Alt;
    if (mod & KMOD_GUI)
        modifiers |= Mod_Super;
    return modifiers;
}

// Input normally comes from the kernel side (serenity_compat exposes
// /dev/mouse/0 and /dev/keyboard/0 carrying Serenity's own MousePacket and
// KeyEvent records). SDL events stay as a fallback so the port still works on a
// host without those devices.
static int s_mouse_device { -1 };
static int s_keyboard_device { -1 };
static u8 s_mouse_buffer[sizeof(MousePacket)] {};
static size_t s_mouse_fill { 0 };
static u8 s_keyboard_buffer[sizeof(::KeyEvent)] {};
static size_t s_keyboard_fill { 0 };

// Re-applies the screen layout at runtime. Serenity's apply_layout already
// handles buffer remapping, compositor invalidation and client relayout, which
// is exactly what a modern desktop does when the display changes.
static bool s_layout_ready { false };
static volatile sig_atomic_t s_quit_requested { 0 };
// Set by SIGUSR1, acted on by the input timer (a handler cannot allocate).
static volatile sig_atomic_t s_screenshot_requested { 0 };

// Without SDL there is no SDL_QUIT event, so termination comes from signals.
static void on_termination_signal(int)
{
    s_quit_requested = 1;
}

// A crash in the compositor takes the entire desktop with it, and without a
// handler the segfault is completely silent: the session manager only records
// "window-server exited (signal 11)" and the backtrace is lost. Dump one to
// stderr before dying so the next report has something to work with.
static void on_fatal_signal(int signo)
{
    void* frames[64];
    int count = backtrace(frames, 64);
    char header[128];
    int header_length = snprintf(header, sizeof(header), "\nWindowServer: fatal signal %d, backtrace:\n", signo);
    if (header_length > 0)
        (void)!write(STDERR_FILENO, header, static_cast<size_t>(header_length));
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    // Restore the default action and re-raise so the exit status still says
    // "killed by signal" (the session manager and systemd rely on it).
    signal(signo, SIG_DFL);
    raise(signo);
}

static bool using_sdl_presenter()
{
    auto* backend = WindowServer::LinuxScreenBackend::last_instance();
    return backend && backend->is_using_sdl_presenter();
}

static void apply_screen_resolution(int width, int height)
{
    // Reconfiguring screens before the window manager exists corrupts its
    // state (and trips a ref-count assert on shutdown), so wait for startup.
    if (!s_layout_ready)
        return;
    if (width <= 0 || height <= 0)
        return;
    auto const& current = WindowServer::Screen::layout();
    if (!current.screens.is_empty() && current.screens[0].resolution == Gfx::IntSize { width, height })
        return;

    WindowServer::ScreenLayout screen_layout;
    screen_layout.screens.append({
        .mode = WindowServer::ScreenLayout::Screen::Mode::Virtual,
        .device = edid_device_for_layout(),
        .location = { 0, 0 },
        .resolution = { width, height },
        .scale_factor = 1,
    });
    screen_layout.main_screen_index = 0;

    ByteString error_msg;
    if (!WindowServer::Screen::apply_layout(move(screen_layout), error_msg))
        warnln("WindowServer: cannot apply resolution {}x{}: {}", width, height, error_msg);
    else
        outln("WindowServer: resolution now {}x{}", width, height);
}

static void try_open_input_devices()
{
    s_mouse_device = open("/dev/mouse/0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    s_keyboard_device = open("/dev/keyboard/0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s_mouse_device >= 0)
        outln("WindowServer: pointer input from /dev/mouse/0");
    if (s_keyboard_device >= 0)
        outln("WindowServer: keyboard input from /dev/keyboard/0");
    if (s_mouse_device < 0 && s_keyboard_device < 0)
        outln("WindowServer: no kernel input devices, falling back to SDL events");
}

static bool device_input_active()
{
    return s_mouse_device >= 0 || s_keyboard_device >= 0;
}

// A non-blocking read can return a partial record, so accumulate until whole.
static void drain_record(int fd, u8* buffer, size_t& fill, size_t record_size, Function<void(u8 const*)> dispatch)
{
    for (;;) {
        auto bytes = read(fd, buffer + fill, record_size - fill);
        if (bytes <= 0)
            return;
        fill += static_cast<size_t>(bytes);
        if (fill < record_size)
            continue;
        dispatch(buffer);
        fill = 0;
    }
}

static void pump_device_events()
{
    if (s_mouse_device >= 0) {
        drain_record(s_mouse_device, s_mouse_buffer, s_mouse_fill, sizeof(MousePacket), [](u8 const* raw) {
            MousePacket packet;
            memcpy(&packet, raw, sizeof(packet));
            WindowServer::ScreenInput::the().on_receive_mouse_data(packet);
        });
    }
    if (s_keyboard_device >= 0) {
        drain_record(s_keyboard_device, s_keyboard_buffer, s_keyboard_fill, sizeof(::KeyEvent), [](u8 const* raw) {
            ::KeyEvent key_event {};
            memcpy(&key_event, raw, sizeof(key_event));
            WindowServer::ScreenInput::the().on_receive_keyboard_data(key_event);
        });
    }
}

static void pump_sdl_events()
{
    static u8 mouse_buttons { 0 };

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            Core::EventLoop::current().quit(0);
            return;

        case SDL_WINDOWEVENT: {
            if (event.window.event != SDL_WINDOWEVENT_SIZE_CHANGED)
                break;
            // Only windowed backends (X11/Wayland) treat the window size as the
            // screen resolution. Under kmsdrm the window is a surface the driver
            // scales onto the real mode, so its size is not the display size.
            auto const* driver = SDL_GetCurrentVideoDriver();
            if (driver && !strcmp(driver, "kmsdrm"))
                break;
            apply_screen_resolution(event.window.data1, event.window.data2);
            break;
        }

        case SDL_MOUSEMOTION: {
            if (device_input_active())
                break;
            MousePacket packet;
            packet.is_relative = false;
            packet.x = event.motion.x;
            packet.y = event.motion.y;
            packet.buttons = mouse_buttons;
            WindowServer::ScreenInput::the().on_receive_mouse_data(packet);
            break;
        }

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (device_input_active())
                break;
            u8 bit = 0;
            switch (event.button.button) {
            case SDL_BUTTON_LEFT:
                bit = MousePacket::LeftButton;
                break;
            case SDL_BUTTON_RIGHT:
                bit = MousePacket::RightButton;
                break;
            case SDL_BUTTON_MIDDLE:
                bit = MousePacket::MiddleButton;
                break;
            case SDL_BUTTON_X1:
                bit = MousePacket::BackwardButton;
                break;
            case SDL_BUTTON_X2:
                bit = MousePacket::ForwardButton;
                break;
            default:
                break;
            }

            if (event.type == SDL_MOUSEBUTTONDOWN)
                mouse_buttons |= bit;
            else
                mouse_buttons &= static_cast<u8>(~bit);

            MousePacket packet;
            packet.is_relative = false;
            packet.x = event.button.x;
            packet.y = event.button.y;
            packet.buttons = mouse_buttons;
            WindowServer::ScreenInput::the().on_receive_mouse_data(packet);
            break;
        }

        case SDL_MOUSEWHEEL: {
            if (device_input_active())
                break;
            int x = 0;
            int y = 0;
            SDL_GetMouseState(&x, &y);

            MousePacket packet;
            packet.is_relative = false;
            packet.x = x;
            packet.y = y;
            packet.buttons = mouse_buttons;
            packet.z = event.wheel.y;
            WindowServer::ScreenInput::the().on_receive_mouse_data(packet);
            break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (device_input_active())
                break;
            ::KeyEvent key_event {};
            key_event.key = sdl_keycode_to_key_code(event.key.keysym.sym);
            key_event.scancode = event.key.keysym.scancode;
            key_event.code_point = (event.key.keysym.sym >= 32 && event.key.keysym.sym < 127)
                ? static_cast<u32>(event.key.keysym.sym)
                : 0;
            key_event.flags = static_cast<u8>(sdl_modifiers()
                | (event.type == SDL_KEYDOWN ? Is_Press : 0));
            WindowServer::ScreenInput::the().on_receive_keyboard_data(key_event);
            break;
        }

        default:
            break;
        }
    }
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    int width = 1280;
    int height = 800;
    ByteString theme_name = ByteString { "Default" };
    ByteString config_path = ByteString { "/etc/WindowServer.ini" };
    ByteString screenshot_path;
    int screenshot_delay_ms = 1500;

    for (int i = 1; i < arguments.argc; ++i) {
        StringView arg { arguments.argv[i], strlen(arguments.argv[i]) };
        auto next = [&]() -> StringView {
            if (i + 1 >= arguments.argc)
                return {};
            ++i;
            return { arguments.argv[i], strlen(arguments.argv[i]) };
        };

        if (arg == "--width"sv) {
            if (auto value = next().to_number<int>(); value.has_value())
                width = *value;
        } else if (arg == "--height"sv) {
            if (auto value = next().to_number<int>(); value.has_value())
                height = *value;
        } else if (arg == "--theme"sv) {
            theme_name = ByteString { next() };
        } else if (arg == "--config"sv) {
            config_path = ByteString { next() };
        } else if (arg == "--screenshot"sv) {
            screenshot_path = ByteString { next() };
        } else if (arg == "--screenshot-delay"sv) {
            if (auto value = next().to_number<int>(); value.has_value())
                screenshot_delay_ms = *value;
        } else if (arg == "--help"sv) {
            outln("usage: serenity-window-server [--width N] [--height N] [--theme NAME] [--config PATH] [--screenshot PATH] [--screenshot-delay MS]");
            return 0;
        }
    }

    auto root = resource_root();
    outln("WindowServer (Linux port): resource root = {}", root);

    // Fonts, emoji data and icons are loaded through the "resource://" scheme,
    // which defaults to a hardcoded "/res". Point it at our host directory so
    // the port runs without needing a root-owned /res symlink.
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(root))));


    auto config_or_error = Core::ConfigFile::open(config_path, Core::ConfigFile::AllowWriting::Yes);
    if (config_or_error.is_error())
        config_or_error = Core::ConfigFile::open(config_path, Core::ConfigFile::AllowWriting::No);
    if (config_or_error.is_error()) {
        warnln("Could not open {}: {}", config_path, config_or_error.error());
        return 1;
    }
    WindowServer::g_config = config_or_error.release_value();

    if (auto configured_theme = WindowServer::g_config->read_entry("Theme", "Name", {}); !configured_theme.is_empty())
        theme_name = configured_theme;

    auto theme_path = ByteString::formatted("{}/themes/{}.ini", root, theme_name);
    auto theme = TRY(Gfx::load_system_theme(theme_path));
    Gfx::set_system_theme(theme);
    auto palette = Gfx::PaletteImpl::create_with_anonymous_buffer(theme);

    // Lagom only ships the Liberation font family, not Serenity's Katica/Csilla.
    auto default_font_query = WindowServer::g_config->read_entry("Fonts", "Default", ByteString { "Liberation Sans 10 400 0" });
    auto fixed_width_font_query = WindowServer::g_config->read_entry("Fonts", "FixedWidth", ByteString { "Liberation Mono 10 400 0" });
    auto window_title_font_query = WindowServer::g_config->read_entry("Fonts", "WindowTitle", ByteString { "Liberation Sans 10 700 0" });

    Gfx::FontDatabase::set_default_font_query(default_font_query);
    Gfx::FontDatabase::set_fixed_width_font_query(fixed_width_font_query);
    Gfx::FontDatabase::set_window_title_font_query(window_title_font_query);

    // Build the initial layout at the mode the display will actually run. Doing
    // this up front avoids a resolution change right after startup, which would
    // make the backend close and reopen the DRM device.
    auto const* presenter_env = getenv("SERENITY_PRESENTER");
    auto const* ws_backend_env = getenv("SERENITY_WS_BACKEND");
    bool const want_gbm_backend = ws_backend_env && strcmp(ws_backend_env, "gbm") == 0;
    // The GBM backend drives /dev/dri directly (no SDL, no libdrm probe), so do
    // not probe the display through LinuxScreenBackend when it is in use.
    if (!want_gbm_backend && (!presenter_env || strcmp(presenter_env, "sdl"))) {
        auto display_size = WindowServer::LinuxScreenBackend::probe_display_size(width, height);
        warnln("WindowServer: probe_display_size -> {}x{}", display_size.width(), display_size.height());
        if (display_size.width() > 0 && display_size.height() > 0) {
            width = display_size.width();
            height = display_size.height();
            warnln("WindowServer: using {}x{} as the initial layout", width, height);
        }
    }

    WindowServer::ScreenLayout screen_layout;
    screen_layout.screens.append({
        .mode = WindowServer::ScreenLayout::Screen::Mode::Virtual,
        .device = edid_device_for_layout(),
        .location = { 0, 0 },
        .resolution = { width, height },
        .scale_factor = 1,
    });
    screen_layout.main_screen_index = 0;

    ByteString error_msg;
    if (!WindowServer::Screen::apply_layout(move(screen_layout), error_msg)) {
        warnln("Failed to apply screen layout: {}", error_msg);
        return 1;
    }

    // libinput is the primary input path; the raw device files are only a
    // fallback for hosts where libinput cannot start.
    //
    // A headless wlroots compositor (WSWLR_HEADLESS=1) is a *development*
    // instance that runs side by side with the live desktop on the same host.
    // Initializing libinput there opens the same evdev devices twice, so the
    // user's mouse and keyboard get split between two WindowServers — the live
    // session then drops events. Headless runs therefore own no input at all;
    // SERENITY_NO_INPUT=1 forces the same for any other dev instance.
    // The callbacks are registered unconditionally: with the embedded wlroots
    // presenter the compositor owns libinput and feeds these same entry points,
    // so ScreenInput must be listening either way.
    WindowServer::LinuxInput::on_mouse() = [](MousePacket const& packet) {
        WindowServer::ScreenInput::the().on_receive_mouse_data(packet);
    };
    WindowServer::LinuxInput::on_key() = [](::KeyEvent const& key_event) {
        WindowServer::ScreenInput::the().on_receive_keyboard_data(key_event);
    };

    auto const* headless_env = getenv("WSWLR_HEADLESS");
    auto const* no_input_env = getenv("SERENITY_NO_INPUT");
    auto const* presenter_env_for_input = getenv("SERENITY_PRESENTER");
    bool const no_input_forced = no_input_env && strcmp(no_input_env, "0");
    // wlroots IS the input consumer: opening evdev a second time would split
    // events between two readers (and the compositor would lose pointer state).
    bool const wlroots_owns_input = presenter_env_for_input && !strcmp(presenter_env_for_input, "wlroots");
    bool const input_disabled = no_input_forced
        || (headless_env && !strcmp(headless_env, "1"));

    if (wlroots_owns_input) {
        warnln("WindowServer: input via embedded wlroots compositor (libinput owned by the compositor)");
    } else if (input_disabled) {
        warnln("WindowServer: input disabled (headless dev instance; unset WSWLR_HEADLESS / SERENITY_NO_INPUT for real input)");
    } else if (WindowServer::LinuxInput::initialize(width, height)) {
        warnln("WindowServer: input via libinput");
    } else {
        warnln("WindowServer: libinput unavailable, falling back to /dev input devices");
        try_open_input_devices();
    }

    auto& screen_input = WindowServer::ScreenInput::the();
    screen_input.set_cursor_location(WindowServer::Screen::main().rect().center());

    // Upstream WindowServer restores these from the config file; the Linux port
    // used to force 1.0 / 4 on every start, which silently discarded the values
    // MouseSettings had just persisted via WindowManager::set_*. Mirror upstream
    // so pointer speed and scroll step survive a session restart.
    double acceleration_factor = atof(WindowServer::g_config->read_entry("Mouse", "AccelerationFactor", "1.0").characters());
    if (acceleration_factor < WindowServer::mouse_accel_min || acceleration_factor > WindowServer::mouse_accel_max) {
        warnln("WindowServer: Mouse.AccelerationFactor out of range, resetting to 1.0");
        acceleration_factor = 1.0;
        WindowServer::g_config->write_entry("Mouse", "AccelerationFactor", "1.0");
    }
    screen_input.set_acceleration_factor(acceleration_factor);
    screen_input.set_scroll_step_size(WindowServer::g_config->read_num_entry<unsigned>("Mouse", "ScrollStepSize", 4));

    WindowServer::Compositor::the();
    auto window_manager = WindowServer::WindowManager::construct(*palette);
    s_layout_ready = true;

    // Screenshot / recording / notifications can be switched off wholesale with
    // SERENITY_NO_CAPTURE_EXTRAS=1, which is the A/B switch used to tell a bug
    // in them apart from a bug in the compositor itself.
    if (!getenv("SERENITY_NO_CAPTURE_EXTRAS")) {
        // Desktop notifications: cards are compositor overlays, and the same
        // server owns org.freedesktop.Notifications on the session bus so Linux
        // apps running inside the port post into the Serenity desktop.
        WindowServer::NotificationCenter::initialize();

        // Screenshot + screen recording for the Serenity desktop. The recorder
        // captures straight out of the compositor's front bitmap and owns the
        // /tmp/serenity-recorder.sock control channel the Screenshot app talks
        // to.
        WindowServer::Recorder::initialize();
    }


    // Wallpaper: on SerenityOS this is pushed by DisplaySettings over IPC. There
    // is no such client at host session start, so load it from disk. The user's
    // live choice lives in ConfigServer's "WindowManager" domain
    // (~/.config/WindowManager.ini, [Background] Wallpaper=); the legacy Linux
    // port location is [Background] Path= of /etc/WindowServer.ini. Prefer the
    // former so a restart does not briefly show a stale wallpaper behind the
    // Shell desktop (which reads the same ConfigServer value).
    ByteString wallpaper_path = WindowServer::g_config->read_entry("Background"sv, "Path"sv, {});
    if (auto config_server_file = Core::ConfigFile::open(
            ByteString::formatted("{}/WindowManager.ini", Core::StandardPaths::config_directory()));
        !config_server_file.is_error() && config_server_file.value()->has_key("Background", "Wallpaper")) {
        wallpaper_path = config_server_file.value()->read_entry("Background", "Wallpaper", "");
    }
    if (!wallpaper_path.is_empty()) {
        if (auto wallpaper = Gfx::Bitmap::load_from_file(wallpaper_path); wallpaper.is_error())
            warnln("Failed to load wallpaper '{}': {}", wallpaper_path, wallpaper.error());
        else
            WindowServer::Compositor::the().set_wallpaper(wallpaper.release_value());
    }
    auto applet_manager = WindowServer::AppletManager::construct();
    auto menu_manager = WindowServer::MenuManager::construct();


    WindowServer::EventLoop event_loop;

    signal(SIGINT, on_termination_signal);
    signal(SIGTERM, on_termination_signal);

    signal(SIGSEGV, on_fatal_signal);
    signal(SIGBUS, on_fatal_signal);
    signal(SIGFPE, on_fatal_signal);
    signal(SIGILL, on_fatal_signal);
    signal(SIGABRT, on_fatal_signal);

    signal(SIGUSR1, [](int) { s_screenshot_requested = 1; });

    auto input_timer = Core::Timer::create_repeating(8, [] {
        if (s_quit_requested) {
            Core::EventLoop::current().quit(0);
            return;
        }
        // `kill -USR1 <window-server>` grabs the desktop as the guest sees it.
        // A signal handler cannot allocate, so it only sets the flag and the
        // dump happens here, on the event loop.
        if (s_screenshot_requested) {
            s_screenshot_requested = 0;
            if (auto* backend = WindowServer::LinuxScreenBackend::last_instance()) {
                if (auto result = backend->dump_to_ppm("/tmp/serenity-shot.ppm"sv); result.is_error())
                    warnln("Screenshot failed: {}", result.error());
                else
                    outln("Wrote screenshot to /tmp/serenity-shot.ppm");
            }
        }
        if (WindowServer::LinuxInput::is_initialized())
            WindowServer::LinuxInput::dispatch();
        else
            pump_device_events();
        // SDL is only pumped when it is actually presenting. Under KMS the port
        // must not touch SDL at all, otherwise it initialises the SDL video/evdev
        // backend and grabs every input device for nothing.
        if (using_sdl_presenter())
            pump_sdl_events();
        // KMS hotplug / mode change: follow the display instead of assuming
        // the resolution we started with is still valid.
        if (auto* backend = WindowServer::LinuxScreenBackend::last_instance()) {
            if (auto size = backend->take_display_change(); size.has_value())
                apply_screen_resolution(size->width(), size->height());
        }
    });
    input_timer->start();

    // NOTE: The timer must outlive this scope, otherwise it is destroyed before
    //       the event loop gets a chance to fire it.
    RefPtr<Core::Timer> screenshot_timer;
    if (!screenshot_path.is_empty()) {
        screenshot_timer = Core::Timer::create_single_shot(screenshot_delay_ms, [screenshot_path] {
            if (auto* backend = WindowServer::LinuxScreenBackend::last_instance()) {
                if (auto result = backend->dump_to_ppm(screenshot_path); result.is_error())
                    warnln("Failed to write screenshot: {}", result.error());
                else
                    outln("Wrote screenshot to {}", screenshot_path);
            }
            Core::EventLoop::current().quit(0);
        });
        screenshot_timer->start();
    }

    outln("WindowServer (Linux port) entering main loop ({}x{})", width, height);

    auto result = event_loop.exec();

    // Hand the display back and leave immediately: the port's full destructor
    // chain trips a ref-count assert, and a crashed exit makes systemd restart
    // while we still hold DRM master (the next instance would fall back to SDL).
    if (auto* backend = WindowServer::LinuxScreenBackend::last_instance())
        backend->restore_display();
    _exit(result);
}
