/* esp_timer shim on bt_alarm: init + generation accessor for the patched osi/alarm.c. SPDX-License-Identifier: MIT */
#ifndef BT_ESP_TIMER_H
#define BT_ESP_TIMER_H
#include <stdint.h>
#include "esp_timer.h"

#define BT_ESP_TIMER_MAX 50u   /* = CONFIG_BT_ALARM_MAX_NUM */

typedef struct {
    void (*enter)(void);
    void (*exit)(void);
} bt_esp_timer_slot_lock_t;

/* Call once after bt_alarm_init(), before Bluedroid creates alarms. */
void bt_esp_timer_init(bt_esp_timer_slot_lock_t lock);

/* Current generation of the underlying bt_alarm (compare with bt_alarm_delivery_gen() under the caller's lock). */
uint32_t bt_esp_timer_generation(esp_timer_handle_t t);
#endif
