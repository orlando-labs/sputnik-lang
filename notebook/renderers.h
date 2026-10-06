#pragma once

namespace amber::notebook {
// Optional statically linked renderers shared by the native host and worker.
// Idempotent; does not load a project or execute module code.
void register_builtin_renderers();
}
