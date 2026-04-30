/*
 * bt_alarm.c - BearThread alarm/timer platform layer
 *
 * Implements OpenThread's alarm platform API.
 * Based on ESP-IDF's esp_openthread_alarm.c but self-contained.
 *
 * Copyright (C) 2025 Christian Baars
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


#include "bt_platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "openthread/platform/alarm-micro.h"
#include "openthread/platform/alarm-milli.h"
#include "openthread/platform/time.h"

static uint32_t s_alarm_ms = 0;
static bool s_is_ms_running = false;
static uint32_t s_alarm_us = 0;
static bool s_is_us_running = false;

static inline bool is_expired(uint32_t target, uint32_t now)
{
    return (((now - target) & (1 << 31)) == 0);
}

static inline uint32_t calculate_duration(uint32_t target, uint32_t now)
{
    return is_expired(target, now) ? 0 : target - now;
}

/* ---- OpenThread Platform API ---- */

uint64_t otPlatTimeGet(void)
{
    return (uint64_t)esp_timer_get_time();
}

void otPlatAlarmMilliStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    s_alarm_ms = aT0 + aDt;
    s_is_ms_running = true;
}

void otPlatAlarmMilliStop(otInstance *aInstance)
{
    s_is_ms_running = false;
}

uint32_t otPlatAlarmMilliGetNow(void)
{
    return otPlatTimeGet() / US_PER_MS;
}

void otPlatAlarmMicroStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    s_alarm_us = aT0 + aDt;
    s_is_us_running = true;
}

void otPlatAlarmMicroStop(otInstance *aInstance)
{
    s_is_us_running = false;
}

uint32_t otPlatAlarmMicroGetNow(void)
{
    return otPlatTimeGet();
}

/* ---- BearThread internal API ---- */

void bt_alarm_update(struct timeval *timeout)
{
    int64_t remain_min_time_us = INT64_MAX;
    int64_t remaining_us = 0;

    if (s_is_ms_running) {
        remaining_us = calculate_duration(s_alarm_ms, otPlatAlarmMilliGetNow()) * US_PER_MS;
        if (remain_min_time_us > remaining_us) {
            remain_min_time_us = remaining_us;
        }
    }
    if (s_is_us_running) {
        remaining_us = calculate_duration(s_alarm_us, otPlatAlarmMicroGetNow());
        if (remain_min_time_us > remaining_us) {
            remain_min_time_us = remaining_us;
        }
    }
    if (remain_min_time_us > 0 && remain_min_time_us < INT64_MAX) {
        if (remain_min_time_us < timeout->tv_sec * US_PER_S + timeout->tv_usec) {
            timeout->tv_sec = remain_min_time_us / US_PER_S;
            timeout->tv_usec = remain_min_time_us % US_PER_S;
        }
    } else if (remain_min_time_us <= 0) {
        timeout->tv_sec = 0;
        timeout->tv_usec = 0;
    }
}

esp_err_t bt_alarm_process(otInstance *instance)
{
    if (s_is_ms_running && is_expired(s_alarm_ms, otPlatAlarmMilliGetNow())) {
        s_is_ms_running = false;
        otPlatAlarmMilliFired(instance);
    }
    if (s_is_us_running && is_expired(s_alarm_us, otPlatAlarmMicroGetNow())) {
        s_is_us_running = false;
        otPlatAlarmMicroFired(instance);
    }
    return ESP_OK;
}
