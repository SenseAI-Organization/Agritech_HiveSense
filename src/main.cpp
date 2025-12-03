/*******************************************************************************
 * @file main.cpp
 * @brief Server_example to use BLE library
 * @version 0.0.5
 * @date 2025-06-19
 * @author isa@sense-ai.co
 *******************************************************************************
 *******************************************************************************/

#include "ble_sense.hpp"
#include "wifi_sense.hpp"
#include "ble_server.hpp"
// #include "device_configurator.hpp"
#include "HiveConfig.hpp"
#include "aws_sense.hpp"
#include "actuators_sense.hpp"

#include "esp_log.h"
#include <inttypes.h>

#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

// Estructura para pasar datos entre tareas
struct DataMessage {
    char data[256];
    size_t length;
    uint64_t timestamp;
};

WifiHandler* wifiHandler = nullptr;
awsHandler* awsHelper = nullptr;
BLEServer* globalServer = nullptr;
QueueHandle_t dataQueue = nullptr;
RGB* brainLED = nullptr;
SemaphoreHandle_t sdMutex = nullptr;
bool sdCardReady = false;
TaskHandle_t sdTaskHandle = nullptr;

// Función para obtener el nombre del archivo basado en la fecha actual
std::string getDateBasedFilename(const char* prefix) {
    //TODO: Time synchronization debe ser en UNIX
    time_t now;
    struct tm timeinfo;
    char buffer[64];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    snprintf(buffer, sizeof(buffer), "%s_%04d-%02d-%02d.txt", 
             prefix,
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday);
    
    return std::string(buffer);
}

// Función auxiliar mejorada para escribir en SD con mutex
bool writeToSD(const char* filename, const char* data) {
    if (!sdCard || !sdCardReady) {
        ESP_LOGW(TAG, "SD Card not ready, data not saved");
        return false;
    }
    
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire SD mutex");
        return false;
    }
    
    FRESULT error = sdCard->openFile(filename, SD::OpenMode::kOpenAppend);
    if (error) {
        ESP_LOGE(TAG, "SD error at openFile: %s", sdCard->getFastFsErrName(error));
        xSemaphoreGive(sdMutex);
        return false;
    }
    
    error = sdCard->fileWrite(data);
    if (error) {
        ESP_LOGE(TAG, "SD error at fileWrite: %s", sdCard->getFastFsErrName(error));
        sdCard->closeFile();
        xSemaphoreGive(sdMutex);
        return false;
    }
    
    sdCard->closeFile();
    xSemaphoreGive(sdMutex);
    return true;
}

// Tarea dedicada para la SD card con manejo de errores y reintentos
void sdCardTask(void* pvParameters) {
    ESP_LOGI(TAG, "SD Card Task started");
    
    const int MAX_RETRIES = 5;
    const int RETRY_DELAY_MS = 2000;
    int retryCount = 0;
    bool initialized = false;
    
    while (!initialized && retryCount < MAX_RETRIES) {
        ESP_LOGI(TAG, "SD Card initialization attempt %d/%d", retryCount + 1, MAX_RETRIES);
        
        // Crear SPI master
        SPI* spiMaster = new SPI(SPI::SpiMode::kMaster, SPI2_HOST, kMosiPin, kMisoPin, kSclPin);
        
        esp_err_t err = spiMaster->init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
            delete spiMaster;
            retryCount++;
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        
        ESP_LOGI(TAG, "SPI initialized successfully");
        vTaskDelay(pdMS_TO_TICKS(100)); // Dar tiempo al SPI
        
        // Crear objeto SD
        if (sdCard == nullptr) {
            sdCard = new SD(*spiMaster, kCsPin);
        }
        
        // Intentar inicializar SD
        err = sdCard->init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD init failed: %s", esp_err_to_name(err));
            delete sdCard;
            sdCard = nullptr;
            delete spiMaster;
            retryCount++;
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        
        ESP_LOGI(TAG, "SD Card hardware initialized");
        vTaskDelay(pdMS_TO_TICKS(100)); // Dar tiempo antes de montar
        
        // Intentar montar la tarjeta
        FRESULT error = sdCard->mountCard();
        if (error != FR_OK) {
            ESP_LOGE(TAG, "SD mount failed: %s", sdCard->getFastFsErrName(error));
            delete sdCard;
            sdCard = nullptr;
            delete spiMaster;
            retryCount++;
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        
        // Éxito!
        std::string currentPath = sdCard->getCurrentDir();
        ESP_LOGI(TAG, " SD Card mounted successfully at: %s", currentPath.c_str());
        
        initialized = true;
        sdCardReady = true;
        
        // Crear el mutex para acceso a SD
        if (sdMutex == nullptr) {
            sdMutex = xSemaphoreCreateMutex();
        }
    }
    
    if (!initialized) {
        ESP_LOGE(TAG, "Failed to initialize SD Card after %d attempts", MAX_RETRIES);
        ESP_LOGE(TAG, "System will continue WITHOUT SD Card support");
        sdCardReady = false;
    }
    
    // Monitoreo continuo de la SD
    while (initialized) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Verificar cada 30 segundos
        
        // Verificar si la SD sigue accesible
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Intentar acceso simple
            FRESULT result = sdCard->goToDir(sdCard->getCurrentDir());
            if (result != FR_OK) {
                ESP_LOGW(TAG, "SD Card seems to be disconnected, attempting recovery...");
                sdCardReady = false;
                
                // Intentar recuperación
                sdCard->unmountCard();
                vTaskDelay(pdMS_TO_TICKS(500));
                
                FRESULT mountResult = sdCard->mountCard();
                if (mountResult == FR_OK) {
                    ESP_LOGI(TAG, "SD Card recovered successfully");
                    sdCardReady = true;
                } else {
                    ESP_LOGE(TAG, "SD Card recovery failed: %s", 
                            sdCard->getFastFsErrName(mountResult));
                }
            }
            xSemaphoreGive(sdMutex);
        }
    }
    
    vTaskDelete(NULL);
}

// Tarea WiFi/AWS - maneja conexión y envío de datos
void wifiAwsTask(void* pvParameters) {
    ESP_LOGI(TAG, "WiFi/AWS Task started");
    
    DataMessage msg;
    char timestampedData[512];
    
    // Variables para controlar la creación de archivos diarios
    std::string currentDailyFile = "";
    std::string currentFailedFile = "";
    
    while (true) {
        // Esperar por datos en la cola
        if (xQueueReceive(dataQueue, &msg, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Processing: %.*s", msg.length, msg.data);
            
            // Solo intentar crear archivo si SD está lista
            if (sdCardReady) {
                std::string todayDailyFile = getDateBasedFilename("data");
                if (todayDailyFile != currentDailyFile) {
                    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                        FRESULT error = sdCard->createFile(todayDailyFile);
                        if (error && error != FR_EXIST) {
                            ESP_LOGE(TAG, "Failed to create daily file: %s", 
                                    sdCard->getFastFsErrName(error));
                        } else {
                            currentDailyFile = todayDailyFile;
                            ESP_LOGI(TAG, "Daily file ready: %s", currentDailyFile.c_str());
                        }
                        xSemaphoreGive(sdMutex);
                    }
                }
            }
            
            // Formatear datos con timestamp
            time_t now = msg.timestamp / 1000000;
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            
            snprintf(timestampedData, sizeof(timestampedData), 
                    "[%04d-%02d-%02d %02d:%02d:%02d] %.*s\n",
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                    (int)msg.length, msg.data);
            
            // Guardar en SD solo si está lista
            if (sdCardReady && !currentDailyFile.empty()) {
                if (writeToSD(currentDailyFile.c_str(), timestampedData)) {
                    ESP_LOGI(TAG, "Saved to: %s", currentDailyFile.c_str());
                } else {
                    ESP_LOGE(TAG, "Failed to save to SD");
                }
            } else {
                ESP_LOGW(TAG, "SD not ready, skipping file write");
            }
            
            // Intentar conectar a AWS y enviar
            bool mqttSuccess = false;
            
            if (awsHelper->connect() == ESP_OK) {
                if (awsHelper->publish(mqttDataTopic, msg.data) == ESP_OK) {
                    ESP_LOGI(TAG, "Published to AWS");
                    mqttSuccess = true;
                } else {
                    ESP_LOGE(TAG, "Failed to publish to AWS");
                }
                awsHelper->disconnect();
            } else {
                ESP_LOGE(TAG, "Failed to connect to AWS");
            }
            
            // Si falló MQTT, guardar en archivo de fallidos
            if (!mqttSuccess && sdCardReady) {
                std::string todayFailedFile = getDateBasedFilename("failed");
                if (todayFailedFile != currentFailedFile) {
                    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                        FRESULT error = sdCard->createFile(todayFailedFile);
                        if (error && error != FR_EXIST) {
                            ESP_LOGE(TAG, "Failed to create failed file");
                        } else {
                            currentFailedFile = todayFailedFile;
                        }
                        xSemaphoreGive(sdMutex);
                    }
                }
                
                if (!currentFailedFile.empty()) {
                    writeToSD(currentFailedFile.c_str(), timestampedData);
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// Tarea BLE - maneja el servidor BLE y advertising
void bleTask(void* pvParameters) {
    ESP_LOGI(TAG, "BLE Task started");
    
    BLELibrary* ble = (BLELibrary*)pvParameters;
    
    if (!globalServer) {
        ESP_LOGE(TAG, "Global server is NULL");
        vTaskDelete(NULL);
        return;
    }
    
    int loopCount = 0;
    int lastConnectedClients = 0;
    
    while (true) {
        loopCount++;

        // Check for connection changes every loop
        bleServerStatus_t status = globalServer->getStatus();
        if ((int)status.connectedClients != lastConnectedClients) {
            ESP_LOGI(TAG, " Connection count changed: %d -> %d", 
                    lastConnectedClients, (int)status.connectedClients);
            lastConnectedClients = (int)status.connectedClients;
        }
        
        // Verificar y forzar advertising si no está activo y no hay clientes máximos
        if (!status.advertisingActive && status.connectedClients < 4) {
            ESP_LOGW(TAG, "  Advertising not active with %d clients - forcing restart...", 
                    (int)status.connectedClients);
            esp_err_t ret = globalServer->startAdvertising();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, " Advertising restarted successfully");
            } else {
                ESP_LOGE(TAG, " Failed to restart advertising: %s", esp_err_to_name(ret));
            }
        }

        if (loopCount % 10 == 0) {
            
            if (globalServer->hasConnectedClients()) {
                char timeBuffer[128];
                if (wifiHandler->getCurrentTime(timeBuffer, sizeof(timeBuffer)) == ESP_OK) {
                    ESP_LOGI(TAG, "Current Time: %s", timeBuffer);
                } else {
                    ESP_LOGE(TAG, "Failed to get current time");
                }
                
                char jsonData[64];
                snprintf(jsonData, sizeof(jsonData), "{\"a\":1}");
                
                // MÉTODO 1: Configurar datos en custom y notificar
                esp_err_t setResult = globalServer->setCustomData(jsonData);
                ESP_LOGI(TAG, "setCustomData resultado: %s", esp_err_to_name(setResult));
                
                if (setResult == ESP_OK) {
                    // Obtener todas las sesiones de clientes
                    bleClientSession_t sessions[4];
                    uint8_t sessionCount = globalServer->getAllClientSessions(sessions, 4);
                    
                    for (uint8_t i = 0; i < sessionCount; i++) {
                        if (sessions[i].connID != 0) {
                            ESP_LOGI(TAG, "Notificando cliente %d con custom data", sessions[i].connID);
                            esp_err_t result = globalServer->notifyClient(sessions[i].connID, "custom");
                            ESP_LOGI(TAG, "notifyClient custom resultado: %s", esp_err_to_name(result));
                        }
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t synchroTime(WifiHandler* wifiHandler, int maxAttempts = 5) {
    
    if (!wifiHandler) {
        printf("No WiFi handler available for time sync\n");
        return ESP_FAIL;
    }
    
    printf("Starting NTP synchronization (%d attempts max)...\n", maxAttempts);
    wifiHandler->syncTime();
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        // Check for reed interrupt during sync
        
        char timeBuffer[64];
        printf("Time sync attempt %d/%d...\n", attempt, maxAttempts);
        
        if (wifiHandler->getCurrentTime(timeBuffer, sizeof(timeBuffer)) == ESP_OK) {
            printf("Time retrieved: %s\n", timeBuffer);
            
            int day, month, year, hour, min, sec;
            if (sscanf(timeBuffer, "%d/%d/%d-%d:%d:%d", &day, &month, &year, &hour, &min, &sec) == 6) {
                struct tm tm_time = {};
                tm_time.tm_mday = day;
                tm_time.tm_mon = month - 1;
                tm_time.tm_year = year - 1900;
                tm_time.tm_hour = hour;
                tm_time.tm_min = min;
                tm_time.tm_sec = sec;
                tm_time.tm_isdst = -1;
                
                time_t wifiUnixTime = mktime(&tm_time);
  
            } else {
                printf("Failed to parse time string: %s\n", timeBuffer);
            }
        } else {
            printf("Time sync attempt %d failed\n", attempt);
        }
        
        if (attempt < maxAttempts) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    printf("Failed to synchronize RTC after %d attempts\n", maxAttempts);
    return ESP_FAIL;
}

extern "C" void app_main() {
    //TODO: Implementar DeviceConfig
    //TODO: cORREGIR ntp trouble
    //TODO: Enviar la hora al cliente BLE cuando se conecte y ha pasado un día
    brainLED = new RGB(kLedPin,255, 255, 255);
    esp_err_t err = brainLED->init();
    if (err) {
        printf("LED couldn't be initialized.\n");
        printf("%s\n", esp_err_to_name(err));
        // while(1);
    }

    ESP_LOGI(TAG, "=== HIVE FIRST STEPS ===");

    // Inicializar NVS ANTES de todo
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS flash erased");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized correctly");

    esp_err_t mtu_ret = esp_ble_gatt_set_local_mtu(512);
    if (mtu_ret == ESP_OK) {
        ESP_LOGI(TAG, "Server local MTU set to 512 bytes");
    } else {
        ESP_LOGW(TAG, "Failed to set local MTU: %s", esp_err_to_name(mtu_ret));
    }

    wifiHandler = new WifiHandler(userSSID, userPassword);

    if (wifiHandler->connectWifi() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi.");
        // return;
    }

    ESP_LOGI(TAG, "Connected to WiFi!");

    // Synchronize time
    ret = synchroTime(wifiHandler, 5);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to synchronize time: %s", esp_err_to_name(ret));
        // return;
    }

    // ESP_LOGI(TAG, "Synchronized time!");
    
    awsHelper = new awsHandler(*wifiHandler);

    if (awsHelper->init(AWS_IOT_ENDPOINT, THINGNAME, AWS_ServerCA, AWS_ClientCertificate, AWS_ClientKey) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize AWS helper");
        return;
    }

    // Crear tarea dedicada para SD Card con manejo robusto de errores
    BaseType_t sdTaskCreated = xTaskCreatePinnedToCore(
        sdCardTask,"SDTask",8192,NULL,6,&sdTaskHandle,1 );
    
    if (sdTaskCreated != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SD Card task");
        // Continuar sin SD
    } else {
        ESP_LOGI(TAG, "SD Card task created on core 1");
    }
    
    // Dar tiempo a que la SD se inicialice
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Mostrar datos de la librería
    BLELibrary::printLibraryInfo();

    // Crear configuración personalizada
    bleLibraryConfig_t config;
    ret = BLELibrary::createDefaultConfig(&config, BLE_MODE_SERVER_ONLY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail creating configuration: %s", esp_err_to_name(ret));
        return;
    }
    
    // Personalizar configuración
    strncpy(config.deviceName, "HiveSense", BLE_MAX_DEVICE_NAME_LEN - 1);
    config.autoStart = false; // Controlaremos manualmente
    config.enableLogging = true;
    config.logLevel = ESP_LOG_INFO;

    BLELibrary* ble = new BLELibrary(config);
    if (ble == nullptr) {
        ESP_LOGE(TAG, "Fail creating ble_library");
        return;
    }

    ret = ble->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail initializating ble_library: %s", esp_err_to_name(ret));
        delete ble;
        return;
    }

    BLEServer* server = ble->getServer();
    if (server != nullptr) {
        ESP_LOGI(TAG, "Settings server");

        esp_err_t name_ret = server->setDeviceName("HiveSense");
        if (name_ret == ESP_OK) {
            ESP_LOGI(TAG, "Device name successfully set to HiveSense");
        } else {
            ESP_LOGW(TAG, "Failed to set device name: %s", esp_err_to_name(name_ret));
        }
        
        bleServerConfig_t server_config = server->getConfig();
        server_config.dataUpdateInterval = 0;    
        server_config.clientTimeout = 60000;     // 60 segundos timeout en lugar de 0
        server_config.maxClients = 4;
        server_config.enableNotifications = true;
        server_config.autoStartAdvertising = true;
        server_config.enableJsonCommands = true;
        strncpy(server_config.deviceName, "HiveSense", BLE_MAX_DEVICE_NAME_LEN - 1);

        ret = server->setConfig(server_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Warnings in settings server: %s", esp_err_to_name(ret));
        }

        // Configurar callbacks básicos con más logging
        server->setClientConnectedCallback([](uint16_t connID, const bleDeviceInfo_t* clientInfo) {
            ESP_LOGI(TAG, " Client connected - ID: %d, MAC: %02x:%02x:%02x:%02x:%02x:%02x", 
                    connID,
                    clientInfo->address[0], clientInfo->address[1], clientInfo->address[2],
                    clientInfo->address[3], clientInfo->address[4], clientInfo->address[5]);
        });

        server->setClientDisconnectedCallback([](uint16_t connID, int reason) {
            ESP_LOGI(TAG, " Client disconnected - ID: %d, Reason: %d", connID, reason);
        });

        server->setAdvertisingCallback([](bool isStarted, esp_err_t result) {
            if (isStarted && result == ESP_OK) {
                ESP_LOGI(TAG, " Advertising STARTED successfully");
            } else if (!isStarted || result != ESP_OK) {
                ESP_LOGE(TAG, " Advertising STOPPED or FAILED: %s", esp_err_to_name(result));
            }
        });

        server->setDataWrittenCallback([](uint16_t connID, const uint8_t* data, uint16_t length) {
            ESP_LOGI(TAG, " Client %d data received (%d bytes)", connID, length);
            
            if (length > 0 && length < 256) {
                // Solo preparar el mensaje y enviarlo a la cola
                // La escritura en SD se hace en la tarea WiFi/AWS para no bloquear el callback
                if(brainLED != nullptr){
                    brainLED->setColor(0, 255, 0); // Verde al recibir datos
                    brainLED->pulse(200); // Pulso breve
                }
                // vTaskDelay(pdMS_TO_TICKS(100));
                DataMessage msg;
                memcpy(msg.data, data, length);
                msg.data[length] = '\0';
                msg.length = length;
                
                struct timeval tv;
                gettimeofday(&tv, NULL);
                msg.timestamp = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
                
                if (xQueueSend(dataQueue, &msg, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Queue full, data dropped");
                }
            }
        });


        // Manual Advertising
        ESP_LOGI(TAG, "Init advertising...");
        ret = server->startAdvertising();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Fail init advertising: %s", esp_err_to_name(ret));
            delete ble;
            return;
        }
        
        // Guardar referencia global al servidor
        globalServer = server;
        
        // Crear cola para comunicación entre tareas
        dataQueue = xQueueCreate(20, sizeof(DataMessage));
        if (dataQueue == NULL) {
            ESP_LOGE(TAG, "Failed to create data queue");
            delete ble;
            return;
        }
        ESP_LOGI(TAG, "Data queue created successfully");
        
        // Crear tarea WiFi/AWS con más stack
        BaseType_t wifiTaskCreated = xTaskCreatePinnedToCore(
            wifiAwsTask,
            "WiFi_AWS_Task",
            12288,  // 12KB de stack
            NULL,
            5,
            NULL,
            1  // Core 1
        );
        
        if (wifiTaskCreated != pdPASS) {
            ESP_LOGE(TAG, "Failed to create WiFi/AWS task");
            delete ble;
            return;
        }
        ESP_LOGI(TAG, "WiFi/AWS task created on core 1");
        
        // Crear tarea BLE con más stack
        BaseType_t bleTaskCreated = xTaskCreatePinnedToCore(
            bleTask,
            "BLE_Task",
            6144,  // 6KB de stack
            (void*)ble,
            4,
            NULL,
            0  // Core 0
        );
        
        if (bleTaskCreated != pdPASS) {
            ESP_LOGE(TAG, "Failed to create BLE task");
            delete ble;
            return;
        }
        ESP_LOGI(TAG, "BLE task created on core 0");
        
        ESP_LOGI(TAG, "=== System running with separate tasks ===");
        ESP_LOGI(TAG, "BLE Task: Handling connections and data reception");
        ESP_LOGI(TAG, "WiFi/AWS Task: Publishing data to MQTT and saving failed attempts");
        
    } else {
        ESP_LOGE(TAG, "Get server not found");
        delete ble;
        return;
    }
}
