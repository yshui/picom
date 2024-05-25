// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

struct backend_base;

/// Invoke all backend plugins for the specified backend.
void api_backend_plugins_invoke(const char *backend_name, struct backend_base *backend);
