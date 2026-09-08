/*******************************************************************************
 * @file wake_watchdog.cpp
 * @brief Wake-cycle deadline. See wake_watchdog.hpp for the rationale and the
 *        OTA-safety property.
 ******************************************************************************/
#include "wake_watchdog.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace wake_watchdog {
namespace {

constexpr const char* TAG = "WakeWatchdog";

esp_timer_handle_t s_timer = nullptr;
uint32_t s_timeoutMs = 0;   // 0 means "not armed"

// Runs on the esp_timer task, so a main task blocked on a mutex or a driver
// that never returns cannot prevent it firing. Deliberately does no cleanup:
// the device is in an unknown state and anything this touched could block too.
// The breadcrumb was already written by whichever stage was running.
void onDeadlineExpired(void* /*arg*/) {
    ESP_LOGE(TAG, "No progress for %lu ms - restarting", (unsigned long)s_timeoutMs);
    esp_restart();
}

esp_err_t ensureTimer(void) {
    if (s_timer != nullptr) return ESP_OK;

    const esp_timer_create_args_t args = {
        .callback = &onDeadlineExpired,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wake_deadline",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        s_timer = nullptr;
    }
    return err;
}

void restart(uint32_t timeoutMs) {
    if (ensureTimer() != ESP_OK) return;

    esp_timer_stop(s_timer);  // no-op when not running
    esp_err_t err = esp_timer_start_once(s_timer, (uint64_t)timeoutMs * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not arm deadline: %s", esp_err_to_name(err));
        s_timeoutMs = 0;
        return;
    }
    s_timeoutMs = timeoutMs;
}

}  // namespace

void arm(uint32_t timeoutMs) {
    restart(timeoutMs);
    ESP_LOGI(TAG, "Deadline armed: %lu ms", (unsigned long)timeoutMs);
}

void feed(void) {
    // Silent by design: called once per OTA chunk, and logging each one would
    // bury the transfer log.
    if (s_timeoutMs == 0) return;
    restart(s_timeoutMs);
}

void extend(uint32_t timeoutMs) {
    restart(timeoutMs);
    ESP_LOGI(TAG, "Deadline changed to %lu ms", (unsigned long)timeoutMs);
}

void disarm(void) {
    if (s_timer == nullptr) return;
    esp_timer_stop(s_timer);
    s_timeoutMs = 0;
    ESP_LOGI(TAG, "Deadline disarmed");
}

}  // namespace wake_watchdog
