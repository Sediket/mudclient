#pragma once

namespace mudclient::platform {

// Prepares the process console for UTF-8 output with ANSI escape sequences.
// On Windows this enables ENABLE_VIRTUAL_TERMINAL_PROCESSING and sets the
// output code page to UTF-8; elsewhere it is a no-op.
void init_console();

} // namespace mudclient::platform
