
#include "mqtt_data.h"
#include <driver/uart.h>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include "application.h" // 新增：包含Application头文件以访问其单例和方法
#include "assets/lang_config.h" // 新增：包含语言配置以使用预设字符串和声音
#include "protocol.h" 

#define TAG "MQTT_DATA"
#define TCP_RX_BUFFER_SIZE 128

// MQTT事件处理回调函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    MQTTData* mqtt_data_obj = static_cast<MQTTData*>(handler_args);
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            // 订阅接收主题
            esp_mqtt_client_subscribe(event->client, MQTT_TOPIC_RX, 1);
            ESP_LOGI(TAG, "Subscribed to topic: %s", MQTT_TOPIC_RX);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_DATA: {
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            std::string topic(event->topic, event->topic_len);
            std::string data(event->data, event->data_len);
            // 调用成员函数处理消息
            mqtt_data_obj->OnMessage(topic, data);
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

MQTTData::MQTTData() : mqtt_client(nullptr), server_fd(-1), client_socket(-1) {}

MQTTData::~MQTTData() {
    if (mqtt_client) {
        esp_mqtt_client_destroy(mqtt_client);
    }
    if (server_fd != -1) {
        close(server_fd);
    }
    if (client_socket != -1) {
        close(client_socket);
    }
}

void MQTTData::StartServer() {
    ESP_LOGI(TAG, "Starting services...");

    // 启动TCP服务器线程
    tcp_server_thread = std::thread(&MQTTData::TcpServerTask, this);
    tcp_server_thread.detach();

    // 初始化MQTT客户端
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, this);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT client started, connecting to broker: %s", MQTT_BROKER_URI);
}

void MQTTData::PublishMessage(const std::string& topic, const std::string& message) {
    if (mqtt_client) {
        int msg_id = esp_mqtt_client_publish(mqtt_client, topic.c_str(), message.c_str(), 0, 1, 0);
        ESP_LOGI(TAG, "Sent publish successful, msg_id=%d, topic=%s, data=%s", msg_id, topic.c_str(), message.c_str());
    }
}

void MQTTData::OnMessage(const std::string& topic, const std::string& message) {
    ESP_LOGI(TAG, "Received message on topic: %s, data: %s", topic.c_str(), message.c_str());
    if (topic == MQTT_TOPIC_RX) {
        cJSON *root = cJSON_Parse(message.c_str());
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse JSON: %s", message.c_str());
            return;
        }

        int operation = -1;
        cJSON *op_item = cJSON_GetObjectItem(root, "operation");
        if (cJSON_IsNumber(op_item)) {
            operation = op_item->valueint;
        }

        // --- 关键修改：根据操作码分发任务 ---
        if (operation == 1) { // 操作1：成功找到包裹，发送位置码到串口
            const char* location_code = "";
            cJSON *loc_item = cJSON_GetObjectItem(root, "location_code");
            if (cJSON_IsString(loc_item)) {
                location_code = loc_item->valuestring;
            }

            if (strlen(location_code) > 0) {
                std::stringstream ss;
                ss << "op:" << operation << ",lc:" << location_code << "\r\n";
                std::string uart_msg = ss.str();
                uart_write_bytes(UART_NUM_1, uart_msg.c_str(), uart_msg.length());
                ESP_LOGI(TAG, "Sent to UART: %s", uart_msg.c_str());
            }

        } else if (operation == 2) { // 操作2：系统通知
            const char* reason = "";
            cJSON *reason_item = cJSON_GetObjectItem(root, "reason");
            if (cJSON_IsString(reason_item)) {
                reason = reason_item->valuestring;
            }

            if (strcmp(reason, "not_found") == 0) {
                ESP_LOGI(TAG, "Reason is 'not_found'. Triggering alert.");
                // 调用Application的Alert功能来通知用户
                // 这会在屏幕上显示消息，改变表情，并播放提示音
                // 注意：这不会触发云端TTS，而是使用本地资源进行反馈，这在当前架构下是最高效的方式
                Application::GetInstance().Alert(
                    Lang::Strings::ERROR,                           // 状态标题: "错误"
                    "手机尾号不正确，请输入正确的手机尾号",         // 核心提示信息
                    "sad",                                          // AI表情: "悲伤"
                    Lang::Sounds::P3_EXCLAMATION                    // 提示音
                );
            }
        }

        cJSON_Delete(root);
    }
}

void MQTTData::SendMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(client_socket_mutex);
    if (client_socket != -1) {
        int to_write = message.length();
        const char* buffer = message.c_str();
        while (to_write > 0) {
            int written = send(client_socket, buffer, to_write, 0);
            if (written < 0) {
                ESP_LOGE(TAG, "Error sending to TCP client: errno %d", errno);
                // 发生错误，可能连接已断开
                close(client_socket);
                client_socket = -1;
                break;
            }
            to_write -= written;
            buffer += written;
        }
        if (to_write == 0) {
             ESP_LOGI(TAG, "Sent to TCP client: %s", message.c_str());
        }
    } else {
        ESP_LOGW(TAG, "No TCP client connected, cannot send message.");
    }
}

void MQTTData::TcpServerTask() {
    char rx_buffer[TCP_RX_BUFFER_SIZE];
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(TCP_PORT);

    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    if (bind(server_fd, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(server_fd);
        return;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", TCP_PORT);

    if (listen(server_fd, 1) != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(server_fd);
        return;
    }

    while (true) {
        ESP_LOGI(TAG, "Waiting for TCP connection on port %d...", TCP_PORT);
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(server_fd, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }
        ESP_LOGI(TAG, "Socket accepted");

        {
            std::lock_guard<std::mutex> lock(client_socket_mutex);
            // 如果已有连接，先关闭
            if(client_socket != -1) {
                close(client_socket);
            }
            client_socket = sock;
        }

        while (true) {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGW(TAG, "Connection closed");
                break;
            } else {
                rx_buffer[len] = '\0';
                //发送tcp回传数据
                SendMessage("Received ok!!!");
                ESP_LOGI(TAG, "Received from TCP: %s", rx_buffer);

                // 解析分号分隔的数据: "0;aa;000;0000;00000"
                PackageData package = {};
                char* p = rx_buffer;
                char* token;
                int field_count = 0;

                token = strtok_r(p, ";", &p); if (token) { package.operation = atoi(token); field_count++; }
                token = strtok_r(p, ";", &p); if (token) { strncpy(package.name, token, sizeof(package.name)-1); field_count++; }
                token = strtok_r(p, ";", &p); if (token) { strncpy(package.express_number, token, sizeof(package.express_number)-1); field_count++; }
                token = strtok_r(p, ";", &p); if (token) { strncpy(package.location_code, token, sizeof(package.location_code)-1); field_count++; }
                token = strtok_r(p, ";", &p); if (token) { strncpy(package.phone_number, token, sizeof(package.phone_number)-1); field_count++; }
                
                if (field_count != 5) {
                    ESP_LOGW(TAG, "Invalid data format received. Expected 5 fields, got %d.", field_count);
                    continue;
                }

                // 1. 发送MQTT消息
                cJSON *root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "operation", package.operation);
                cJSON_AddStringToObject(root, "name", package.name);
                cJSON_AddStringToObject(root, "express_number", package.express_number);
                cJSON_AddStringToObject(root, "location_code", package.location_code);
                cJSON_AddStringToObject(root, "phone_number", package.phone_number);
                
                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str) {
                    PublishMessage(MQTT_TOPIC_TX, json_str);
                    free(json_str);
                }
                cJSON_Delete(root);

                // 2. 发送串口消息

                std::stringstream ss;
                ss << "op:" << package.operation << ",lc:" << package.location_code << "\r\n";

                std::string uart_msg = ss.str();
                uart_write_bytes(UART_NUM_1, uart_msg.c_str(), uart_msg.length());
                ESP_LOGI(TAG, "Sent to UART: %s", uart_msg.c_str());
                
            }
        }

        if (sock != -1) {
            ESP_LOGI(TAG, "Shutting down TCP connection...");
            shutdown(sock, 0);
            close(sock);
            {
                std::lock_guard<std::mutex> lock(client_socket_mutex);
                if (client_socket == sock) {
                    client_socket = -1;
                }
            }
        }
    }
    // This part is unreachable in the current loop
    close(server_fd);
}