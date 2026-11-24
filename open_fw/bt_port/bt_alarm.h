/* bt_alarm: deadline service for Bluedroid alarms (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M2 "Alarms").
 *
 * Replaces FreeRTOS software timers for BT: set/cancel/free update the deadline list and the alarm generation
 * synchronously in the caller under one critical section, so no command queue can lag behind a cancel or re-arm.
 *   caller (any task)       bt_alarm_set / bt_alarm_cancel / bt_alarm_free
 *   bt_alarm task           bt_alarm_service(): due entries -> pending list (stamped with their generation),
 *                           post one allocation-free drain item to BTC; returns time to the next deadline
 *   BTC task                bt_alarm_drain(): from the drain item AND after every BTC message; delivers a pending
 *                           expiry only if its stamp equals the alarm's current generation
 * Callbacks run in the drain caller's context (BTC), outside the critical section; bt_alarm_delivery_gen() returns
 * the validated generation. A callback whose meaning depends on "which arming" must, under the same lock that guards
 * its own set/cancel calls, accept the expiry only if bt_alarm_delivery_gen() == bt_alarm_generation(alarm): a
 * higher-priority task may cancel or re-arm between the drain's validation and the callback taking that lock.
 *
 * Pure logic: all OS access goes through bt_alarm_platform_t, so host tests can drive every interleaving with a fake
 * clock. Not usable from ISRs.
 * SPDX-License-Identifier: MIT */
#ifndef BT_ALARM_H
#define BT_ALARM_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*bt_alarm_cb_t)(void *arg);

typedef struct bt_alarm {
    /* private */
    bt_alarm_cb_t cb;
    void *arg;
    uint64_t deadline_us;
    uint32_t period_us;        /* 0 = one-shot */
    uint32_t generation;       /* bumped by set / cancel / free; survives free */
    uint32_t stamp;            /* generation captured when the expiry moved to the pending list */
    uint32_t coalesced;        /* periodic expiries merged while one delivery was still pending */
    struct bt_alarm *next_dl;  /* deadline list */
    struct bt_alarm *next_pd;  /* pending list */
    uint8_t armed;
    uint8_t pending;
    uint8_t in_use;
} bt_alarm_t;

typedef struct {
    uint64_t (*now_us)(void);
    void (*enter)(void);       /* short critical section (taskENTER_CRITICAL on target) */
    void (*exit)(void);
    void (*wake)(void);        /* wake the bt_alarm task (xTaskNotifyGive); must not fail or block */
    int (*post_drain)(void);   /* non-blocking, allocation-free post of a drain item to BTC; 0 = posted */
} bt_alarm_platform_t;

typedef struct {
    uint32_t delivered;
    uint32_t dropped_stale;
    uint32_t drain_post_failures;
    uint32_t coalesced;
} bt_alarm_stats_t;

void bt_alarm_init(const bt_alarm_platform_t *platform);

/* Prepare an alarm object for use. The object must be zero-initialised before its first setup; afterwards the
 * generation is preserved across free/setup cycles so a stale pending stamp can never match again. */
void bt_alarm_setup(bt_alarm_t *a, bt_alarm_cb_t cb, void *arg);

/* Arm (or re-arm) relative to now. period_us = 0 for one-shot. Returns the new generation. */
uint32_t bt_alarm_set(bt_alarm_t *a, uint32_t timeout_us, uint32_t period_us);

/* Disarm; a pending (not yet delivered) expiry is discarded. Returns the new generation. */
uint32_t bt_alarm_cancel(bt_alarm_t *a);

/* Cancel and release; the object may be freed by the caller afterwards (never referenced again). */
void bt_alarm_free(bt_alarm_t *a);

bool bt_alarm_is_armed(const bt_alarm_t *a);
uint32_t bt_alarm_generation(const bt_alarm_t *a);

/* bt_alarm task: process due deadlines. Returns microseconds until the next deadline (UINT32_MAX if none). */
uint32_t bt_alarm_service(void);

/* BTC: deliver valid pending expiries. Safe to call when nothing is pending. */
void bt_alarm_drain(void);

/* Generation of the expiry currently being delivered (valid inside a callback; 0 otherwise). */
uint32_t bt_alarm_delivery_gen(void);

void bt_alarm_get_stats(bt_alarm_stats_t *out);

#endif /* BT_ALARM_H */
