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
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espnow_sense.hpp"
#include "mqtt_sense.hpp"
#include "wifi_sense.hpp"
#include "wake_watchdog.hpp"
#include "HiveConfig.hpp"

static const char* TAG = "espnow_gw";

struct EspNowMessage {
    uint8_t payload[EspNow::kMaxPayloadV2];
    size_t len;
};

static EspNowMessage s_publishQueue[kPublishQueueLen];

static void logMac(const char* p_label, const uint8_t* p_mac) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X", p_label, p_mac[0], p_mac[1],
             p_mac[2], p_mac[3], p_mac[4], p_mac[5]);
}

// Returns the delivery result (ESP_OK only if every message published), not
// the ESP-NOW-restore result - the caller decides whether to clear the queue
// based on delivery, and a restore failure is logged here regardless since
// it means the device can no longer receive ESP-NOW at all.
static esp_err_t publishBatch(Wifi& wifi, EspNow& espNow,
                              const EspNowMessage* p_messages, size_t count) {
    esp_err_t err = espNow.deinit();
    wake_watchdog::feed();
    if (err != ESP_OK) {
        return err;
    }

    const Wifi::StaCredentials credentials = {kWifiSsid, kWifiPassword};
    esp_err_t deliverErr = wifi.connect(credentials, kSyncTimeTimeoutMs);
    wake_watchdog::feed();
    if (deliverErr == ESP_OK) {
        // First SNTP round trip after a fresh connect can miss a single 15s
        // window (DNS lookup + first packet loss); retry before giving up.
        deliverErr = ESP_FAIL;
        for (uint8_t attempt = 1; attempt <= kSyncTimeRetries; ++attempt) {
            deliverErr = wifi.syncTime(kSyncTimeTimeoutMs);
            wake_watchdog::feed();
            if (deliverErr == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "syncTime attempt %u/%u: %s", attempt, kSyncTimeRetries,
                     esp_err_to_name(deliverErr));
        }
    }

    if (deliverErr != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi/time sync failed after retries: %s",
                 esp_err_to_name(deliverErr));
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
        deliverErr = mqtt.init();
        wake_watchdog::feed();
        if (deliverErr == ESP_OK) {
            deliverErr = mqtt.waitConnected(kMqttConnectTimeoutMs);
            wake_watchdog::feed();
        }
        if (deliverErr == ESP_OK) {
            for (size_t index = 0; index < count; ++index) {
                esp_err_t publishErr = ESP_FAIL;
                for (uint8_t attempt = 1; attempt <= kMqttPublishRetries; ++attempt) {
                    publishErr = mqtt.publish(kMqttDataTopic, p_messages[index].payload,
                                              p_messages[index].len, MQTT::Qos::k1, false,
                                              kPublishAckMs);
                    wake_watchdog::feed();
                    if (publishErr == ESP_OK) {
                        ESP_LOGI(TAG, "MQTT publish %u/%u acknowledged",
                                 static_cast<unsigned>(index + 1U),
                                 static_cast<unsigned>(count));
                        break;
                    }
                    ESP_LOGW(TAG, "MQTT publish %u/%u attempt %u/%u: %s",
                             static_cast<unsigned>(index + 1U),
                             static_cast<unsigned>(count), static_cast<unsigned>(attempt),
                             static_cast<unsigned>(kMqttPublishRetries),
                             esp_err_to_name(publishErr));
                    if (attempt < kMqttPublishRetries) {
                        vTaskDelay(pdMS_TO_TICKS(kMqttPublishRetryDelayMs));
                    }
                }
                if (publishErr != ESP_OK) {
                    deliverErr = publishErr;
                }
            }
        } else {
            ESP_LOGE(TAG, "MQTT connect: %s", esp_err_to_name(deliverErr));
        }
        (void)mqtt.deinit();
    }

    (void)wifi.disconnect();
    esp_err_t restoreErr = wifi.setChannel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
    if (restoreErr == ESP_OK) {
        restoreErr = espNow.init();
    }
    wake_watchdog::feed();
    if (restoreErr != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW restore failed, gateway cannot receive: %s",
                 esp_err_to_name(restoreErr));
    }

    return deliverErr;
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
    TickType_t lastReceiveTicks = 0;
    TickType_t lastAnyPacketTicks = xTaskGetTickCount();
    uint8_t srcMac[EspNow::kMacLen];
    char prefix[kPrefixLogLen + 1];

    wake_watchdog::arm(kWakeWatchdogTimeoutMs);

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
            lastReceiveTicks = xTaskGetTickCount();
            lastAnyPacketTicks = lastReceiveTicks;
            wake_watchdog::feed();
            if (espNow.addPeer(srcMac) == ESP_OK) {
                err = espNow.send(srcMac, kAckMessage, sizeof(kAckMessage) - 1U);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "ACK send: %s", esp_err_to_name(err));
                }
                (void)espNow.removePeer(srcMac);
            }
        }

        const bool queueFull = queuedCount == kPublishQueueLen;
        const bool partialBatchTimedOut =
            queuedCount > 0U &&
            (xTaskGetTickCount() - lastReceiveTicks >=
             pdMS_TO_TICKS(kPartialBatchTimeoutMs));
        if (queueFull || partialBatchTimedOut) {
            ESP_LOGI(TAG, "%s; publishing %u messages",
                     queueFull ? "Queue full" : "10-minute receive timeout",
                     static_cast<unsigned>(queuedCount));
            err = publishBatch(wifi, espNow, s_publishQueue, queuedCount);
            if (err == ESP_OK) {
                queuedCount = 0;
                lastReceiveTicks = 0;
            } else {

                ESP_LOGW(TAG, "Batch publish failed (%s); keeping %u messages queued for retry",
                         esp_err_to_name(err), static_cast<unsigned>(queuedCount));
                vTaskDelay(pdMS_TO_TICKS(kBatchRetryDelayMs));
            }
        }

        if (xTaskGetTickCount() - lastAnyPacketTicks >= pdMS_TO_TICKS(kNoDataTimeoutMs)) {

            ESP_LOGE(TAG, "No ESP-NOW data for %lu ms - restarting",
                     (unsigned long)kNoDataTimeoutMs);
            esp_restart();
        }

        wake_watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {
    gpio_reset_pin(kBootPulsePin);
    gpio_set_direction(kBootPulsePin, GPIO_MODE_OUTPUT);
    gpio_set_level(kBootPulsePin, 1);
    vTaskDelay(pdMS_TO_TICKS(kBootPulseDurationMs));
    gpio_set_level(kBootPulsePin, 0);

    ESP_LOGI(TAG, "ESP-NOW channel 1 gateway (batch MQTT bridge)");
    if (xTaskCreatePinnedToCore(gatewayTask, "espnow_gw", kAppTaskStackBytes, nullptr,
                                 kAppTaskPriority, nullptr, kAppTaskCore) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
