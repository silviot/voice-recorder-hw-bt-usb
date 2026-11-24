/* IDF task-priority constants Bluedroid expects. SPDX-License-Identifier: MIT */
#pragma once
#include <FreeRTOSConfig.h>
#define ESP_TASK_PRIO_MAX configMAX_PRIORITIES
#define ESP_TASK_PRIO_MIN 0
#define ESP_TASK_TIMER_PRIO (configMAX_PRIORITIES - 3)
#define BT_TASK_EXTRA_STACK_SIZE 0
