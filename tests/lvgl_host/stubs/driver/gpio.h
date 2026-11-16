#pragma once
using gpio_num_t = int;
inline constexpr int GPIO_NUM_NC=-1, GPIO_NUM_4=4, GPIO_NUM_5=5, GPIO_NUM_6=6,
 GPIO_NUM_7=7, GPIO_NUM_11=11, GPIO_NUM_14=14, GPIO_NUM_15=15, GPIO_NUM_16=16,
 GPIO_NUM_38=38, GPIO_NUM_40=40, GPIO_NUM_41=41, GPIO_NUM_45=45, GPIO_NUM_47=47, GPIO_NUM_48=48;
inline int gpio_get_level(int) { return 1; }
