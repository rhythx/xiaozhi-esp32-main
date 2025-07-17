
#ifndef MQTT_DATA_H
#define MQTT_DATA_H

#include <string>
#include <functional>
#include <mutex>
#include <thread>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "cJSON.h"

// **修改**: 定义MQTT和TCP相关常量
#define MQTT_BROKER_URI "mqtt://test.mosquitto.org:1883"
#define MQTT_TOPIC_TX "topic/esp32_tx"
#define MQTT_TOPIC_RX "topic/esp32_rx"
#define TCP_PORT 5000

// **修改**: 包裹数据结构体，字段大小调整
typedef struct {
    int operation;
    char name[32];
    char express_number[32];
    char location_code[32];
    char phone_number[32];
} PackageData;

class MQTTData {
public:
    MQTTData();
    ~MQTTData();

    void StartServer();
    void PublishMessage(const std::string& topic, const std::string& message);
    void OnMessage(const std::string& topic, const std::string& message);
    void SendMessage(const std::string& message);

private:
    void TcpServerTask();

    esp_mqtt_client_handle_t mqtt_client;
    std::thread tcp_server_thread;
    int server_fd;
    int client_socket; // **修改**: 使用更清晰的变量名
    std::mutex client_socket_mutex; // **修改**: 用于保护client_socket访问的互斥锁
};

#endif // MQTT_DATA_H