#include "sd_storage_sense.hpp"

static const char* TAG = "HIVE_FIRST_STEPS";

const char* userSSID = "SENSE DEV";
const char* userPassword = "901798716SA";


// AWS IoT OTA Configuration
const char* AWS_IOT_ENDPOINT = "a1jlp9p9jotm7z-ats.iot.us-east-2.amazonaws.com";
const char* AWS_ServerCA = "AmazonCA1.pem";
const char* AWS_ClientCertificate = "SenseS.pem";
const char* AWS_ClientKey = "private.pem";
const char* THINGNAME = "BeeSense_Device";
const char* mqttCmdTopic = "cmd";
const char* mqttUpdateTopic = "ota/firmware";
const char* mqttFeedbackTopic = "ota/feedback";
const char* mqttDataTopic = "sense/dev/HIVE";
const char* deviceID = "HIVE_TEST";

SD* sdCard = nullptr;              
// std::string fileName = "testHive.txt";
std::string dailyFilename;  
std::string failedFilename;

// GPIO configuration for SPI
// constexpr gpio_num_t kCsPin = GPIO_NUM_39;
// constexpr gpio_num_t kSclPin = GPIO_NUM_36;
// constexpr gpio_num_t kMosiPin = GPIO_NUM_35;
// constexpr gpio_num_t kMisoPin = GPIO_NUM_37;
//BRAIN ZHANA
constexpr gpio_num_t kCsPin = GPIO_NUM_39;
constexpr gpio_num_t kSclPin = GPIO_NUM_12;
constexpr gpio_num_t kMosiPin = GPIO_NUM_11;
constexpr gpio_num_t kMisoPin = GPIO_NUM_13;

