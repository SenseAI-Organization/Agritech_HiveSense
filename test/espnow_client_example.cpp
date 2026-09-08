/*******************************************************************************
 * @file espnow_client_example.cpp
 * @brief EspNowClient sensor demo (ESP-NOW v2 large payload).
 *
 * The gateway reuses whatever channel its Wi-Fi STA associates to (see
 * main.cpp), which varies with the AP and isn't known to this device ahead
 * of time. So instead of a fixed channel, this sweeps 1..13 to find the
 * gateway: a fresh/unpaired device broadcasts DISCOVER on each channel (only
 * answered while the gateway's pairing window is open); a device with a
 * gateway MAC already saved from a previous pairing sends a zero-length DATA
 * probe instead, which the gateway ACKs for any known peer regardless of
 * pairing window. The same sweep re-runs from the main loop after a run of
 * send failures, so a gateway reboot onto a new channel is recovered from
 * without a manual reflash.
 *
 * Stack: 8192 bytes (required for v2). Discovers gateway, then sends
 * "ping-N " + Lorem ipsum body each cycle.
 * Flash as client on /dev/ttyACM0 (MAC E4:B0:63:B3:F6:F8).
 *
 * Flow diagram: examples/diagrams/espnow_client_example.mmd
 *******************************************************************************/

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espnow_client_sense.hpp"
#include "espnow_gateway_proto.hpp"
#include "espnow_sense.hpp"
#include "flash_sense.hpp"
#include "wifi_sense.hpp"

static const char* TAG = "espnow_cli_ex";

static constexpr uint8_t kChannelMin = 1;
static constexpr uint8_t kChannelMax = 13;
static constexpr uint32_t kDiscoverDwellMs = 1200;
static constexpr uint32_t kConsecutiveFailuresBeforeRescan = 3;
static constexpr uint32_t kTaskStack = 8192;
static constexpr UBaseType_t kTaskPrio = 5;

static const char kLoremBody[] =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Phasellus "
    "consequat varius lacus id viverra. Nulla magna lacus, aliquet eget libero "
    "sed, lacinia hendrerit ante. Nunc nec est vel urna semper fringilla. "
    "Maecenas vel venenatis purus, cursus laoreet sapien. Donec rhoncus, lacus "
    "ut aliquet volutpat, nulla libero pharetra erat, mattis lacinia felis quam "
    "in velit. Duis quis maximus nulla. Sed sed maximus augue. Nullam id congue "
    "orci, nec malesuada velit. Mauris posuere diam et libero mattis ultricies. "
    "Nullam nec eros in sem finibus vulputate. Aliquam a scelerisque lectus. "
    "Sed interdum molestie massa congue sagittis. Pellentesque vestibulum "
    "cursus tortor, in gravida ipsum gravida eget. Vestibulum mauris nunc, "
    "iaculis tempus tincidunt eu, gravida non diam. Maecenas quis velit ut "
    "metus eleifend tempor.\n\n"
    "Sed nec nisl libero. Aliquam ut dolor sed neque rutrum tempor. Praesent "
    "id commodo velit. Pellentesque ut mi tincidunt, eleifend ex sed, "
    "vestibulum dolor. Donec ac iaculis lectus. Donec sit amet varius nunc "
    "cras. ";

static void logMac(const char* p_label, const uint8_t* p_mac) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X", p_label, p_mac[0], p_mac[1],
             p_mac[2], p_mac[3], p_mac[4], p_mac[5]);
}

// Sweeps channels 1..13 looking for the gateway. A previously-paired client
// (gateway MAC restored from NVS) probes each channel with a zero-length
// DATA send, ACKed by the gateway for any known peer at any time. An
// unpaired client instead broadcasts DISCOVER and dwells briefly per channel,
// which the gateway only answers while its pairing window is open. Leaves
// the radio on the matching channel on success; on failure the radio is left
// on the last channel tried (harmless - the next sweep sets it again).
static bool findGatewayChannel(Wifi& wifi, EspNowClient& client) {
    const bool wasPaired = client.isPaired();
    ESP_LOGI(TAG, "Scanning channels %u-%u for gateway (%s)", kChannelMin, kChannelMax,
             wasPaired ? "known peer, probing" : "unpaired, broadcasting DISCOVER");

    for (uint8_t ch = kChannelMin; ch <= kChannelMax; ++ch) {
        if (wifi.setChannel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            continue;  // channel not valid for this device's regulatory domain
        }

        if (wasPaired) {
            if (client.sendData(nullptr, 0) == ESP_OK) {
                ESP_LOGI(TAG, "Gateway responded on channel %u", ch);
                return true;
            }
        } else {
            (void)client.startDiscovery();
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kDiscoverDwellMs);
            while (xTaskGetTickCount() < deadline) {
                (void)client.process();
                if (client.isPaired()) {
                    ESP_LOGI(TAG, "Gateway discovered on channel %u", ch);
                    return true;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }

    ESP_LOGW(TAG, "Gateway not found scanning channels %u-%u", kChannelMin, kChannelMax);
    return false;
}

static void sensorTask(void* /*p_arg*/) {
    ESP_LOGI(TAG, "EspNowClient example (sensor, ESP-NOW v2)");

    Wifi::Config wifiCfg;
    wifiCfg.mode = Wifi::Mode::kStation;
    wifiCfg.powerSave = Wifi::PowerSave::kNone;
    wifiCfg.initNvs = true;
    Wifi wifi(wifiCfg);

    esp_err_t err = wifi.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    err = wifi.start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi.start: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    // Starting channel before the sweep below picks the real one; ESP-NOW
    // itself doesn't care which channel is active at init time.
    err = wifi.setChannel(kChannelMin, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi.setChannel: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    uint8_t selfMac[6] = {};
    if (wifi.getMac(selfMac) == ESP_OK) {
        logMac("Self STA MAC", selfMac);
    }

    FlashStorage store("espnow_cli");
    err = store.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FlashStorage.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    EspNowClient::Config cliCfg;
    cliCfg.espnowVersion2 = true;
    cliCfg.discoverIntervalMs = 1000;
    cliCfg.discoverTimeoutMs = 90000;
    cliCfg.dataAckTimeoutMs = 800;
    cliCfg.dataRetries = 5;
    cliCfg.rxQueueLen = 4;

    EspNowClient client(wifi, store, cliCfg);
    err = client.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    if (client.isPaired()) {
        uint8_t gw[6] = {};
        if (client.getServerMac(gw) == ESP_OK) {
            logMac("Restored gateway", gw);
        }
    }
    (void)findGatewayChannel(wifi, client);

    uint8_t txBuf[EspNow::kMaxPayloadV2];
    uint32_t txCount = 0;
    uint32_t consecutiveFailures = 0;

    while (true) {
        (void)client.process();

        if (client.isPaired()) {
            const int prefixLen =
                std::snprintf(reinterpret_cast<char*>(txBuf), sizeof(txBuf),
                              "ping-%lu ", static_cast<unsigned long>(txCount));
            if (prefixLen > 0) {
                const size_t maxApp = espnow_gateway::kMaxAppPayload;
                size_t room = (static_cast<size_t>(prefixLen) < maxApp)
                                  ? (maxApp - static_cast<size_t>(prefixLen))
                                  : 0U;
                if (room > sizeof(txBuf) - static_cast<size_t>(prefixLen) - 1U) {
                    room = sizeof(txBuf) - static_cast<size_t>(prefixLen) - 1U;
                }
                const size_t bodyLen = std::strlen(kLoremBody);
                const size_t copyLen = (bodyLen < room) ? bodyLen : room;
                std::memcpy(txBuf + prefixLen, kLoremBody, copyLen);
                const size_t total = static_cast<size_t>(prefixLen) + copyLen;

                err = client.sendData(txBuf, total);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "TX ok: ping-%lu total=%u bytes",
                             static_cast<unsigned long>(txCount),
                             static_cast<unsigned>(total));
                    ++txCount;
                    consecutiveFailures = 0;
                } else {
                    ESP_LOGW(TAG, "sendData: %s", esp_err_to_name(err));
                    // The gateway may have rebooted onto a different channel
                    // (its Wi-Fi STA channel follows whatever AP it joins).
                    // Re-sweep instead of retrying forever on a dead channel.
                    if (++consecutiveFailures >= kConsecutiveFailuresBeforeRescan) {
                        consecutiveFailures = 0;
                        (void)findGatewayChannel(wifi, client);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            if (!findGatewayChannel(wifi, client)) {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "EspNowClient Sense example (v2)");
    if (xTaskCreatePinnedToCore(sensorTask, "espnow_cli", kTaskStack, nullptr,
                                kTaskPrio, nullptr, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
