#pragma once
#include "mqtt_data.h"
#include <string>

// 前向声明 MQTTData 类
class MQTTData;

class AppCar {
public:

    static void Init(MQTTData* mqtt_data); 
    static void ProcessSTTText(const std::string& text);
    static void MoveForward();
    static void MoveBackward();
    static void TurnLeft();
    static void TurnRight();
    static void Stop();
private:
    // 私有成员和方法将在实现文件中定义
};