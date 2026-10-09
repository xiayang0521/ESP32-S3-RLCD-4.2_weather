// 集中声明 BOOT 硬件引脚、单键手势和输入任务轮询参数。
// SEL（GPIO2）/BACK（GPIO15）物理键也已损坏，全部软件操作合并到 BOOT（GPIO0）：
// 单击移动、双击确认、长按返回；PWR 是纯硬件电源键，固件无法读取。
#pragma once
#include "driver/gpio.h"
inline constexpr gpio_num_t kBootButtonGpio = GPIO_NUM_0;
inline constexpr int kButtonIdlePollMs = 250;
inline constexpr int kButtonLowRefreshIdlePollMs = 500;
inline constexpr int kButtonActivePollMs = 50;
inline constexpr int kButtonPressedPollMs = 20;
inline constexpr int kButtonDoubleClickGapMs = 350;
