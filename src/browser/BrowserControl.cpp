// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowserControl.h"
#include <deque>
#include <emscripten.h>
std::atomic<int> helmEngineGate{0};
static std::deque<std::function<void()>> pending;
static unsigned depth = 0;
bool helmTryControl() {
    if (depth) { ++depth; return true; }
    int expected = 0;
    if (!helmEngineGate.compare_exchange_strong(expected, 2, std::memory_order_acquire)) return false;
    depth = 1;
    return true;
}
void helmReleaseControl() {
    if (--depth == 0) helmEngineGate.store(0, std::memory_order_release);
}
bool helmControlsPending() { return !pending.empty(); }
void helmQueueControl(std::function<void()> work) { pending.push_back(std::move(work)); }
void helmPumpControls() {
    if (!helmTryControl()) return;
    for (int n = 0; n < 256 && !pending.empty(); ++n) {
        auto next = std::move(pending.front()); pending.pop_front(); next();
    }
    helmReleaseControl();
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_control_enter() { return helmTryControl(); }
extern "C" EMSCRIPTEN_KEEPALIVE void helm_control_exit() { helmReleaseControl(); }
