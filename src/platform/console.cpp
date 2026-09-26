#include "platform/console.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace mudclient::platform {

void init_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(out, &mode)) {
            SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
}

} // namespace mudclient::platform
