// #include "sd_storage_sense.hpp"

// static const char* TAG = "HIVE_FIRST_STEPS";

const char* kWifiSsid = "TP-Link_DDB22";
const char* kWifiPassword = "733624492";


// AWS IoT OTA Configuration
const char* kBrokerHost = "a1jlp9p9jotm7z-ats.iot.us-east-2.amazonaws.com";
const char* kAwsServerCA = "AmazonCA1.pem";
const char* kAwsClientCertificate = "SenseS.pem";
const char* kAwsClientKey = "private.pem";
const char* kThingName = "BeeSense_Device";
const char* kMqttCmdTopic = "cmd";
const char* kMqttUpdateTopic = "sense/dev/HIVEota/firmware";
const char* kMqttFeedbackTopic = "sense/dev/HIVEota/feedback";
const char* kMqttDataTopic = "sense/dev/HIVE";
const char* kDeviceID = "HIVE_TEST";
static constexpr const char* kTopicPub = "sense/demo/out";
static constexpr const char* kTopicSub = "sense/demo/in";

// SD* sdCard = nullptr;              
// // std::string fileName = "testHive.txt";
// std::string dailyFilename;  
// std::string failedFilename;

// GPIO configuration for SPI
// constexpr gpio_num_t kCsPin = GPIO_NUM_39;
// constexpr gpio_num_t kSclPin = GPIO_NUM_36;
// constexpr gpio_num_t kMosiPin = GPIO_NUM_35;
// constexpr gpio_num_t kMisoPin = GPIO_NUM_37;
//BRAIN ZHANA
// constexpr gpio_num_t kCsPin = GPIO_NUM_39;
// constexpr gpio_num_t kSclPin = GPIO_NUM_12;
// constexpr gpio_num_t kMosiPin = GPIO_NUM_11;
// constexpr gpio_num_t kMisoPin = GPIO_NUM_13;
// constexpr gpio_num_t kLedPin = GPIO_NUM_2; // Pin para el LED RGB del cerebro

