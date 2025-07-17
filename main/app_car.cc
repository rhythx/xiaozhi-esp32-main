
#include "app_car.h"
#include "application.h"
#include "mqtt_data.h"
#include <esp_log.h>
#include <string>
#include <regex> // 添加regex头文件以支持正则表达式
#include <driver/uart.h>
#include <cJSON.h> // 添加cJSON头文件以构造JSON

static const char* TAG = "AppCar";

//保存 MQTTData 实例的静态指针，用于串口任务转发数据
static MQTTData* s_mqtt_data = nullptr;

//串口接收任务函数
static void uart_receive_task(void *arg) {
    uint8_t data[128];
    while (1) {
        // 读取串口数据
        int len = uart_read_bytes(UART_NUM_1, data, sizeof(data) - 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0'; // 确保字符串结束
            ESP_LOGI(TAG, "Received UART data: %s", (const char*)data);
            
            // 当接收到串口内容时，通过wifi向tcp客户端发送信息
            if (s_mqtt_data) {
                s_mqtt_data->SendMessage(std::string(reinterpret_cast<char*>(data)));
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void AppCar::Init(MQTTData* mqtt_data) {
    //保存 MQTTData 指针
    s_mqtt_data = mqtt_data;

    // 初始化串口通信
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_1, &uart_config);
    // 根据实际硬件连接设置UART引脚 (例如 GPIO10-TX, GPIO11-RX)
    uart_set_pin(UART_NUM_1, 10, 11, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_1, 256, 256, 0, NULL, 0);
    
    // 创建串口接收任务
    xTaskCreate(uart_receive_task, "uart_receive_task", 2048, NULL, 10, NULL);

    ESP_LOGI(TAG, "AppCar and UART initialized.");
    const char* cmd = "AppCar Init OK\n";
    uart_write_bytes(UART_NUM_1, cmd, strlen(cmd));
}

void AppCar::ProcessSTTText(const std::string& text) {
    ESP_LOGI(TAG, "Processing STT text: %s", text.c_str());

     // 当识别到“我要转圈啦”时，通过串口发送指令
    if (text.find("我要转圈啦") != std::string::npos) {
        const char* cmd = "turn around";
        uart_write_bytes(UART_NUM_1, cmd, strlen(cmd));
        ESP_LOGI(TAG, "Sent 'turn around' command to UART.");
        return; // 处理完指令后直接返回
    }

    // 当文本含有"尾号"和4位数字时，解析并发送MQTT请求
    std::regex pattern(R"(尾号(\d{4}))");
    std::smatch match;
    if (std::regex_search(text, match, pattern) && match.size() > 1) {
        std::string tailNumber = match[1].str();
        ESP_LOGI(TAG, "Parsed mobile tail number for retrieval: %s", tailNumber.c_str());
        
        // 构造JSON消息
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "operation", 1); // 操作数1为获取
        cJSON_AddStringToObject(root, "phone_tail", tailNumber.c_str());
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            // 通过Application单例获取MQTTData实例并发布消息
            Application::GetInstance().GetMqttData()->PublishMessage(MQTT_TOPIC_TX, json_str);
            free(json_str);
        }
        cJSON_Delete(root);
        return; // 处理完取货指令后直接返回
    }
}