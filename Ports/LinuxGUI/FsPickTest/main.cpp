// Throwaway repro harness: asks FileSystemAccessServer for a file, exactly like
// Notepad's File > Open does, so the crash can be reproduced without clicking.
#include <LibCore/EventLoop.h>
#include <LibFileSystemAccessClient/Client.h>
#include <LibGUI/Application.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    // open_file() dereferences the parent window, so give it a real one.
    auto window = GUI::Window::construct();
    window->set_title("fspicktest");
    window->resize(240, 120);
    window->show();
    outln("fspicktest: requesting a file from FileSystemAccessServer...");
    auto result = FileSystemAccessClient::Client::the().open_file(window.ptr());
    if (result.is_error()) {
        warnln("fspicktest: failed: {}", result.error());
        return 1;
    }
    outln("fspicktest: picked '{}'", result.value().filename());
    return 0;
}
