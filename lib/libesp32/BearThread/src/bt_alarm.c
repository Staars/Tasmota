/*
 * bt_alarm.c - BearThread alarm/timer platform layer
 *
 * Implements OpenThread's alarm platform API.
 * Based on ESP-IDF's esp_openthread_alarm.c but self-contained.
 *
 * Copyright (C) 2025 Christian Baars
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef USE_MATTER_THREAD

#include "bt_platform.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "openthread/platform/alarm-micro.h"
#include "openthread/platform/alarm-milli.h"
#include "openthread/platform/logging.h"
#include "openthread/platform/time.h"

static uint32_t s_alarm_ms = 0;
static bool s_is_ms_running = false;
static uint32_t s_alarm_us = 0;
static bool s_is_us_running = false;

/* ESP one-shot timer + eventfd to wake select() when alarm fires.
 * This ensures alarms fire on time even when the main loop is
 * blocked in Berry GC or other processing. Without this, the
 * select()-based polling in bt_launch_mainloop() may use a stale
 * timeout computed before the alarm was set (e.g. from Berry FFI). */
static esp_timer_handle_t s_ms_timer = NULL;
static int s_alarm_event_fd = -1;

static void bt_alarm_timer_callback(void *arg)
{
    uint64_t val = 1;
    (void)arg;
    if (s_alarm_event_fd >= 0) {
        write(s_alarm_event_fd, &val, sizeof(val));
    }
}

static inline bool is_expired(uint32_t target, uint32_t now)
{
    return (((now - target) & (1 << 31)) == 0);
}

static inline uint32_t calculate_duration(uint32_t target, uint32_t now)
{
    return is_expired(target, now) ? 0 : target - now;
}

/* ---- BearThread internal API (init/deinit/get_fd) ---- */

esp_err_t bt_alarm_init(void)
{
    if (s_alarm_event_fd >= 0) return ESP_OK;

    /* eventfd is provided by ESP-IDF VFS (esp_vfs_eventfd_register must be
     * called before this).  We create it blocking then fcntl to non-blocking
     * since EFD_NONBLOCK may not be defined in the C6 newlib headers. */
    s_alarm_event_fd = eventfd(0, 0);
    if (s_alarm_event_fd < 0) return ESP_FAIL;
    {
        int fl = fcntl(s_alarm_event_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(s_alarm_event_fd, F_SETFL, fl | O_NONBLOCK);
    }

    esp_timer_create_args_t args = {
        .callback = bt_alarm_timer_callback,
        .name = "bt_alarm_ms",
    };
    return esp_timer_create(&args, &s_ms_timer);
}

void bt_alarm_deinit(void)
{
    if (s_ms_timer) {
        esp_timer_stop(s_ms_timer);
        esp_timer_delete(s_ms_timer);
        s_ms_timer = NULL;
    }
    if (s_alarm_event_fd >= 0) {
        close(s_alarm_event_fd);
        s_alarm_event_fd = -1;
    }
}

int bt_alarm_get_event_fd(void)
{
    return s_alarm_event_fd;
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
    // otPlatLog(OT_LOG_LEVEL_INFO, OT_LOG_REGION_CORE, "ALARM: ms start t0=%lu dt=%lu target=%lu",
    //           (unsigned long)aT0, (unsigned long)aDt, (unsigned long)s_alarm_ms);

    /* Arm the ESP one-shot timer to wake select() at the target time.
     * This avoids the race where the alarm is set (e.g. from Berry FFI)
     * after the OT task has already computed its select() timeout. */
    if (s_ms_timer) {
        esp_timer_stop(s_ms_timer);
        int64_t delay_us = (int64_t)aDt * (int64_t)US_PER_MS;
        if (delay_us < 1000) delay_us = 1000;
        esp_timer_start_once(s_ms_timer, delay_us);
    }
}

void otPlatAlarmMilliStop(otInstance *aInstance)
{
    if (s_is_ms_running) {
        // otPlatLog(OT_LOG_LEVEL_INFO, OT_LOG_REGION_CORE, "ALARM: ms stop target=%lu",
        //           (unsigned long)s_alarm_ms);
    }
    s_is_ms_running = false;
    if (s_ms_timer) {
        esp_timer_stop(s_ms_timer);
    }
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

/* ---- BearThread internal API (update + process) ---- */

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
        uint32_t now = otPlatAlarmMilliGetNow();
        // otPlatLog(OT_LOG_LEVEL_INFO, OT_LOG_REGION_CORE, "ALARM: ms FIRED target=%lu now=%lu drift=%ld",
        //           (unsigned long)s_alarm_ms, (unsigned long)now, (long)(now - s_alarm_ms));
        s_is_ms_running = false;
        otPlatAlarmMilliFired(instance);
    } else if (s_is_ms_running) {
        uint32_t now = otPlatAlarmMilliGetNow();
        uint32_t remain = calculate_duration(s_alarm_ms, now);
        if (remain < 500) {
            // otPlatLog(OT_LOG_LEVEL_INFO, OT_LOG_REGION_CORE, "ALARM: ms pending target=%lu now=%lu remain=%lu",
            //           (unsigned long)s_alarm_ms, (unsigned long)now, (unsigned long)remain);
        }
    }
    if (s_is_us_running && is_expired(s_alarm_us, otPlatAlarmMicroGetNow())) {
        s_is_us_running = false;
        otPlatAlarmMicroFired(instance);
    }
    return ESP_OK;
}

#endif /* USE_MATTER_THREAD */
