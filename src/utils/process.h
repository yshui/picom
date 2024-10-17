// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

struct x_connection;

/// Fork the process to create a detached underling process. A new connection to the X
/// server is created for the underling.
/// @return 0 in the child process, 1 in the parent process, -1 on failure
int spawn_picomling(struct x_connection *);
