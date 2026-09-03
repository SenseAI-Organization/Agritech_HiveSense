/*******************************************************************************
 * @file espnow_server_example.cpp
 * @brief EspNowServer gateway demo (ESP-NOW v2 large payload RX).
 *
 * Stack: 8192 bytes (required for v2). Pairing window 60 s.
 * Flash as server on /dev/ttyUSB0 (MAC F0:9E:9E:23:4B:3C).
 *
 * Flow diagram: examples/diagrams/espnow_server_example.mmd
 *******************************************************************************/

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espnow_gateway_proto.hpp"
#include "espnow_server_sense.hpp"
#include "flash_sense.hpp"
#include "wifi_sense.hpp"

static const char* TAG = "espnow_gw_ex";

static constexpr uint8_t kChannel = 1;
static constexpr uint32_t kTaskStack = 8192;
static constexpr UBaseType_t kTaskPrio = 5;
static constexpr size_t kPrefixLogLen = 64;

static void logMac(const char* p_label, const uint8_t* p_mac) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X", p_label, p_mac[0], p_mac[1],
             p_mac[2], p_mac[3], p_mac[4], p_mac[5]);
}

static void gatewayTask(void* /*p_arg*/) {
    ESP_LOGI(TAG, "EspNowServer example (gateway, ESP-NOW v2)");

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

    FlashStorage store("espnow_gw");
    err = store.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FlashStorage.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    EspNowServer::Config srvCfg;
    srvCfg.espnowVersion2 = true;
    srvCfg.pairingTimeoutMs = 60000;
    srvCfg.rxAppQueueLen = 4;
    srvCfg.rxQueueLen = 4;

    EspNowServer server(wifi, store, srvCfg);
    err = server.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    (void)server.enablePairing(true);
    ESP_LOGI(TAG, "Pairing enabled for 60 s; known clients=%u",
             static_cast<unsigned>(server.getClientCount()));

    uint8_t payload[espnow_gateway::kMaxAppPayload];
    uint8_t srcMac[6];
    char prefix[kPrefixLogLen + 1];

    while (true) {
        (void)server.process();

        size_t len = 0;
        while (server.read(srcMac, payload, sizeof(payload), &len) == ESP_OK) {
            logMac("DATA from", srcMac);
            ESP_LOGI(TAG, "payload %u bytes (v2 large-frame check)",
                     static_cast<unsigned>(len));
            const size_t n = (len < kPrefixLogLen) ? len : kPrefixLogLen;
            std::memcpy(prefix, payload, n);
            prefix[n] = '\0';
            ESP_LOGI(TAG, "prefix: %s", prefix);
        }

        static bool loggedPairingEnd = false;
        if (!server.isPairing() && !loggedPairingEnd) {
            ESP_LOGI(TAG, "Pairing closed; clients=%u",
                     static_cast<unsigned>(server.getClientCount()));
            loggedPairingEnd = true;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "EspNowServer Sense example (v2)");
    if (xTaskCreatePinnedToCore(gatewayTask, "espnow_gw", kTaskStack, nullptr,
                                kTaskPrio, nullptr, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
