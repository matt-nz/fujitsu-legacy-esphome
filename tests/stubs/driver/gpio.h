#pragma once
#include <stdint.h>
typedef enum { GPIO_NUM_0 = 0 } gpio_num_t_unused;
typedef int gpio_num_t;
typedef enum { GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
int gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
int gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
