/*******************************************************************************
 * @file espnow_client_example.cpp
 * @brief EspNowClient sensor demo (ESP-NOW v2 large payload).
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

static constexpr uint8_t kChannel = 1;
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
    err = wifi.setChannel(kChannel, WIFI_SECOND_CHAN_NONE);
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

    if (!client.isPaired()) {
        (void)client.startDiscovery();
    } else {
        uint8_t gw[6] = {};
        if (client.getServerMac(gw) == ESP_OK) {
            logMac("Restored gateway", gw);
        }
    }

    uint8_t txBuf[EspNow::kMaxPayloadV2];
    uint32_t txCount = 0;

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
                } else {
                    ESP_LOGW(TAG, "sendData: %s", esp_err_to_name(err));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
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
