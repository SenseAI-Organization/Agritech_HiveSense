/*******************************************************************************
 * @file espnow_server_example.cpp
 * @brief EspNowServer gateway demo: bridges received ESP-NOW payloads to MQTT
 *        over TLS (ESP-NOW v2 large payload RX). Pairing window 60 s.
 *
 * Stack: 8192 bytes (required for v2 + MQTT TLS in the same task).
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
#include "mqtt_sense.hpp"
#include "HiveConfig.hpp"

static const char* TAG = "espnow_gw_ex";

static constexpr size_t kPrefixLogLen = 64;
static constexpr uint32_t kPublishAckMs = 5000;
static constexpr uint16_t kBrokerPort = 8883;
static constexpr uint32_t kAppTaskStackBytes = 8192;
static constexpr UBaseType_t kAppTaskPriority = 5;
static constexpr BaseType_t kAppTaskCore = tskNO_AFFINITY;

static void logMac(const char* p_label, const uint8_t* p_mac) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X", p_label, p_mac[0], p_mac[1],
             p_mac[2], p_mac[3], p_mac[4], p_mac[5]);
}

static bool configLooksUnset(void) {
    return (std::strcmp(kWifiSsid, "YOUR_WIFI_SSID") == 0) ||
           (std::strcmp(kWifiPassword, "YOUR_WIFI_PASSWORD") == 0) ||
           (std::strstr(kBrokerHost, "YOUR_ENDPOINT") != nullptr) ||
           (std::strcmp(kThingName, "YOUR_THING_NAME") == 0);
}


static void gatewayTask(void* /*p_arg*/) {
    ESP_LOGI(TAG, "EspNowServer example (gateway, ESP-NOW v2 -> MQTT bridge)");

    if (configLooksUnset()) {
        ESP_LOGE(TAG, "Edit CONFIG block: Wi-Fi, broker, clientId");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    // ESP-NOW must reuse the AP's channel once STA is associated, so connect
    // to the AP (for MQTT) before starting the ESP-NOW server; never setChannel().
    Wifi::Config wifiCfg;
    wifiCfg.mode = Wifi::Mode::kStation;
    wifiCfg.powerSave = Wifi::PowerSave::kMinModem;
    wifiCfg.initNvs = true;
    wifiCfg.connectTimeoutMs = 15000;
    Wifi wifi(wifiCfg);

    esp_err_t err = wifi.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    const Wifi::StaCredentials credentials = {kWifiSsid, kWifiPassword};
    err = wifi.connect(credentials);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi.connect: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    uint8_t selfMac[6] = {};
    if (wifi.getMac(selfMac) == ESP_OK) {
        logMac("Self STA MAC", selfMac);
    }

    err = wifi.syncTime(15000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "syncTime: %s (TLS needs wall clock)", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    MQTT::Config mqttConfig;
    mqttConfig.p_host = kBrokerHost;
    mqttConfig.port = kBrokerPort;
    mqttConfig.transport = MQTT::Transport::kSsl;
    mqttConfig.protocol = MQTT::Protocol::kMqtt311;
    mqttConfig.p_clientId = kDeviceID;
    mqttConfig.p_caFile = kAwsServerCA;
    mqttConfig.p_clientCertFile = kAwsClientCertificate;
    mqttConfig.p_clientKeyFile = kAwsClientKey;
    mqttConfig.p_spiffsBasePath = "/spiffs";
    mqttConfig.cleanSession = true;
    mqttConfig.publishAckTimeoutMs = kPublishAckMs;
    mqttConfig.connectTimeoutMs = 20000;
    mqttConfig.taskStackSize = MQTT::kMinTlsTaskStackBytes;

    static MQTT s_mqtt;
    err = s_mqtt.setConfig(mqttConfig);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt.setConfig: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    err = s_mqtt.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    err = s_mqtt.waitConnected(30000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt.waitConnected: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "MQTT connected; forwarding ESP-NOW payloads to %s", kMqttDataTopic);

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
            const size_t n = (len < kPrefixLogLen) ? len : kPrefixLogLen;
            std::memcpy(prefix, payload, n);
            prefix[n] = '\0';
            ESP_LOGI(TAG, "payload %u bytes, prefix: %s", static_cast<unsigned>(len),
                     prefix);

            err = s_mqtt.publish(kMqttDataTopic, payload, len, MQTT::Qos::k1, false,
                                  kPublishAckMs);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mqtt.publish: %s", esp_err_to_name(err));
            }
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
    ESP_LOGI(TAG, "EspNowServer Sense example (v2 -> MQTT bridge)");
    if (xTaskCreatePinnedToCore(gatewayTask, "espnow_gw", kAppTaskStackBytes, nullptr,
                                kAppTaskPriority, nullptr, kAppTaskCore) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
