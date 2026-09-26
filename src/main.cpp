#include <cstdio>
#include <string_view>

#include "platform/console.hpp"

#ifndef MUDCLIENT_VERSION
#error "MUDCLIENT_VERSION must be defined by the build system"
#endif

int main(int argc, char** argv) {
    mudclient::platform::init_console();
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            std::printf("mudclient %s\n", MUDCLIENT_VERSION);
            return 0;
        }
    }
    std::printf("mudclient %s\nusage: mudclient --version\n", MUDCLIENT_VERSION);
    return 0;
}
