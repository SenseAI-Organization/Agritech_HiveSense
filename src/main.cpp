/*******************************************************************************
 * @file main.cpp
 * @brief ESP-NOW channel 1 gateway: queues broadcasts, then publishes a batch
 *        to AWS MQTT before returning to ESP-NOW reception.
 *
 * Each packet is ACKed after it is copied into the queue. The gateway cannot
 * receive channel 1 ESP-NOW packets while connected to a channel 11 Wi-Fi AP;
 * sensor nodes must retry until they receive the ACK.
 *
 * Stack: 8192 bytes for ESP-NOW v2 and the Wi-Fi/MQTT publish cycle.
 * Flow diagram: examples/diagrams/espnow_server_example.mmd
 *******************************************************************************/

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espnow_sense.hpp"
#include "mqtt_sense.hpp"
#include "wifi_sense.hpp"
#include "HiveConfig.hpp"

static const char* TAG = "espnow_gw";

static constexpr size_t kPrefixLogLen = 64;
static constexpr uint8_t kEspNowChannel = 1;
static constexpr size_t kPublishQueueLen = 8;
static constexpr uint32_t kPublishAckMs = 5000;
static constexpr uint16_t kBrokerPort = 8883;
static constexpr uint32_t kSyncTimeTimeoutMs = 15000;
static constexpr uint32_t kMqttConnectTimeoutMs = 30000;
static constexpr uint32_t kAppTaskStackBytes = 8192;
static constexpr UBaseType_t kAppTaskPriority = 5;
static constexpr BaseType_t kAppTaskCore = tskNO_AFFINITY;

static const uint8_t kAckMessage[] = "ACK";

struct EspNowMessage {
    uint8_t payload[EspNow::kMaxPayloadV2];
    size_t len;
};

static EspNowMessage s_publishQueue[kPublishQueueLen];

static void logMac(const char* p_label, const uint8_t* p_mac) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X", p_label, p_mac[0], p_mac[1],
             p_mac[2], p_mac[3], p_mac[4], p_mac[5]);
}

static esp_err_t publishBatch(Wifi& wifi, EspNow& espNow,
                              const EspNowMessage* p_messages, size_t count) {
    esp_err_t err = espNow.deinit();
    if (err != ESP_OK) {
        return err;
    }

    const Wifi::StaCredentials credentials = {kWifiSsid, kWifiPassword};
    err = wifi.connect(credentials, kSyncTimeTimeoutMs);
    if (err == ESP_OK) {
        err = wifi.syncTime(kSyncTimeTimeoutMs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi/time sync: %s", esp_err_to_name(err));
    } else {
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
        mqttConfig.connectTimeoutMs = kMqttConnectTimeoutMs;
        mqttConfig.taskStackSize = MQTT::kMinTlsTaskStackBytes;

        MQTT mqtt(mqttConfig);
        err = mqtt.init();
        if (err == ESP_OK) {
            err = mqtt.waitConnected(kMqttConnectTimeoutMs);
        }
        if (err == ESP_OK) {
            for (size_t index = 0; index < count; ++index) {
                err = mqtt.publish(kMqttDataTopic, p_messages[index].payload,
                                   p_messages[index].len, MQTT::Qos::k1, false,
                                   kPublishAckMs);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "MQTT publish %u: %s", static_cast<unsigned>(index),
                             esp_err_to_name(err));
                }
            }
        } else {
            ESP_LOGE(TAG, "MQTT connect: %s", esp_err_to_name(err));
        }
        (void)mqtt.deinit();
    }

    (void)wifi.disconnect();
    err = wifi.setChannel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
    return (err == ESP_OK) ? espNow.init() : err;
}

static void gatewayTask(void* /*p_arg*/) {
    ESP_LOGI(TAG, "ESP-NOW channel %u gateway with batch MQTT upload",
             static_cast<unsigned>(kEspNowChannel));

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
    err = wifi.setChannel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi.setChannel: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    EspNow::Config espNowCfg;
    espNowCfg.espnowVersion2 = true;
    espNowCfg.rxQueueLen = 8;
    EspNow espNow(espNowCfg);
    err = espNow.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "espNow.init: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    size_t queuedCount = 0;
    uint8_t srcMac[EspNow::kMacLen];
    char prefix[kPrefixLogLen + 1];

    while (true) {
        EspNowMessage message = {};
        while (queuedCount < kPublishQueueLen &&
               espNow.read(message.payload, sizeof(message.payload), &message.len,
                           srcMac) == ESP_OK) {
            logMac("Broadcast from", srcMac);
            const size_t prefixLen = (message.len < kPrefixLogLen) ? message.len : kPrefixLogLen;
            std::memcpy(prefix, message.payload, prefixLen);
            prefix[prefixLen] = '\0';
            ESP_LOGI(TAG, "payload %u bytes, prefix: %s",
                     static_cast<unsigned>(message.len), prefix);

            s_publishQueue[queuedCount++] = message;
            if (espNow.addPeer(srcMac) == ESP_OK) {
                err = espNow.send(srcMac, kAckMessage, sizeof(kAckMessage) - 1U);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "ACK send: %s", esp_err_to_name(err));
                }
                (void)espNow.removePeer(srcMac);
            }
        }

        if (queuedCount == kPublishQueueLen) {
            ESP_LOGI(TAG, "Queue full; publishing %u messages", static_cast<unsigned>(queuedCount));
            err = publishBatch(wifi, espNow, s_publishQueue, queuedCount);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ESP-NOW restore: %s", esp_err_to_name(err));
            }
            queuedCount = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP-NOW channel 1 gateway (batch MQTT bridge)");
    if (xTaskCreatePinnedToCore(gatewayTask, "espnow_gw", kAppTaskStackBytes, nullptr,
                                 kAppTaskPriority, nullptr, kAppTaskCore) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
