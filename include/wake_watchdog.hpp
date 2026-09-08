/*******************************************************************************
 * @file wake_watchdog.hpp
 * @brief Software deadline for forward progress: if nothing calls feed() (or
 *        arm()/extend()) for the configured timeout, the device restarts via
 *        esp_restart().
 *
 * Rationale: a task can wedge on a mutex, a driver call that never returns,
 * or a peer that stops answering, without tripping the hardware/task
 * watchdogs unless that task is registered with them. This is an explicit,
 * single deadline any task can arm and feed to turn a silent hang into a
 * restart.
 *
 * OTA-safety property: feed() does no cleanup and never touches app state, so
 * it is safe to call from any task or state, including mid-OTA. On expiry the
 * callback also does no cleanup and just restarts - there is no partial-
 * cleanup path to get wrong while the device is in an unknown state.
 *
 * @note Singleton: one shared deadline for the whole device. If several tasks
 *       feed() it, expiry means "no task made progress," not "task X died."
 *       Use a generous timeout that covers the slowest normal step across all
 *       feeding tasks.
 ******************************************************************************/
#pragma once

#include <cstdint>

namespace wake_watchdog {

// Arms the deadline: esp_restart() after timeoutMs without feed()/extend().
// Safe to call again to re-arm with a different timeout.
void arm(uint32_t timeoutMs);

// Resets the deadline to the last armed timeout. Call once per unit of
// forward progress (e.g. once per loop iteration that did useful work). No-op
// if not armed.
void feed(void);

// Re-arms the deadline with a new timeout, replacing the previous one.
void extend(uint32_t timeoutMs);

// Cancels the deadline. No-op if not armed.
void disarm(void);

}  // namespace wake_watchdog
