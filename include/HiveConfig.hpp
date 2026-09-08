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


static constexpr size_t kPrefixLogLen = 64;
static constexpr uint8_t kEspNowChannel = 1;
static constexpr size_t kPublishQueueLen = 8;
static constexpr uint32_t kPartialBatchTimeoutMs = 1U * 60U * 1000U; // 1 minute
static constexpr uint32_t kPublishAckMs = 5000;
static constexpr uint8_t kMqttPublishRetries = 3;
static constexpr uint32_t kMqttPublishRetryDelayMs = 1000;
static constexpr uint16_t kBrokerPort = 8883;
static constexpr uint32_t kSyncTimeTimeoutMs = 15000;
static constexpr uint8_t kSyncTimeRetries = 3;
static constexpr uint32_t kMqttConnectTimeoutMs = 30000;
static constexpr uint32_t kBatchRetryDelayMs = 5000;
static constexpr uint32_t kWakeWatchdogTimeoutMs = 3U * 60U * 1000U; // 3 minutes
static constexpr uint32_t kAppTaskStackBytes = 8192;
static constexpr UBaseType_t kAppTaskPriority = 5;
static constexpr BaseType_t kAppTaskCore = tskNO_AFFINITY;

static const uint8_t kAckMessage[] = "ACK";