// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <functional>
extern std::atomic<int> helmEngineGate;
// Main-thread entrypoints retry without waiting on an AudioWorklet.
bool helmTryControl();
bool helmControlsPending();
void helmReleaseControl();
void helmQueueControl(std::function<void()> work);
void helmPumpControls();
