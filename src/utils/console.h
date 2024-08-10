#pragma once

/// Generate ANSI escape code
#define ANSI(x) "\033[" x "m"
/// Create a string that will print `str` in bold when output to terminal
#define BOLD(str) "\033[1m" str "\033[0m"
