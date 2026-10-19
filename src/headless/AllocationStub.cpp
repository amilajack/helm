// SPDX-License-Identifier: GPL-3.0-or-later
// Native CLAP builds use the system allocator; the Wasm targets link
// BrowserAllocationAudit.c instead, which provides a real-time pool.
#include <cstddef>
extern "C" thread_local int helm_audio_callback_depth = 0;
extern "C" int helm_browser_audio_pool_reserve(size_t) { return 1; }
