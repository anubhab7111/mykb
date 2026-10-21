#pragma once
// Command line: one binary for the CLI, the Waybar stream and the daemon.
//
// Exit codes: 0 ok; 1 the daemon (or the keyboard write) reported an error; 2 the daemon is not
// running; 64 bad usage.

#include "ipc/protocol.hpp"

#include <optional>
#include <string>

namespace omenkbd {

int cli_main(int argc, char **argv);

/// The request a command line sends, or nullopt: with `*error` set on a usage error, empty for
/// commands that are not requests (`daemon`, `waybar`, `--help`).
std::optional<Request> parse_args_to_request(int argc, const char *const *argv, std::string *error);

} // namespace omenkbd
