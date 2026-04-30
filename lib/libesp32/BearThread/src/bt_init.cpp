/*
 * bt_init.cpp - BearThread initialization, main loop, and lock
 *
 * Replaces esp_openthread.cpp, esp_openthread_lock.c, esp_openthread_platform.cpp
 * Self-contained — no dependency on ESP-IDF's OpenThread component.
 *
 * Copyright (C) 2025 Christian Baars
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef USE_MATTER_THREAD

#include "bt_platform.h"

#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "openthread/instance.h"
#include "openthread/tasklet.h"

/* ---- Lock ---- */
static SemaphoreHandle_t s_bt_mutex = NULL;

esp_err_t bt_lock_init(void)
{
    if (s_bt_mutex) return ESP_ERR_INVALID_STATE;
    s_bt_mutex = xSemaphoreCreateRecursiveMutex();
    return s_bt_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void bt_lock_deinit(void)
{
    if (s_bt_mutex) {
        vSemaphoreDelete(s_bt_mutex);
        s_bt_mutex = NULL;
    }
}

bool bt_lock_acquire(uint32_t block_ticks)
{
    if (!s_bt_mutex) return false;
    return xSemaphoreTakeRecursive(s_bt_mutex, block_ticks) == pdTRUE;
}

void bt_lock_release(void)
{
    if (s_bt_mutex) xSemaphoreGiveRecursive(s_bt_mutex);
}

/* ---- Instance ---- */

static otInstance *s_bt_instance = NULL;

otInstance *bt_get_instance(void)
{
    return s_bt_instance;
}

/* ---- Platform init ---- */

esp_err_t bt_platform_init(void)
{
    esp_err_t ret;

    ret = bt_lock_init();
    if (ret != ESP_OK) return ret;

    ret = bt_radio_init();
    if (ret != ESP_OK) {
        bt_lock_deinit();
        return ret;
    }

    /* Init OT instance */
    bt_lock_acquire(portMAX_DELAY);
    s_bt_instance = otInstanceInitSingle();
    bt_lock_release();

    if (!s_bt_instance) {
        bt_radio_deinit();
        bt_lock_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(BT_LOG_TAG, "Platform initialized");
    return ESP_OK;
}

esp_err_t bt_platform_deinit(void)
{
    otInstanceFinalize(bt_get_instance());
    bt_radio_deinit();
    bt_lock_deinit();
    return ESP_OK;
}

/* ---- Main loop ---- */

static volatile bool s_mainloop_running = false;

esp_err_t bt_launch_mainloop(void)
{
    otInstance *instance = bt_get_instance();
    s_mainloop_running = true;

    while (s_mainloop_running) {
        fd_set read_fds;
        struct timeval timeout;

        FD_ZERO(&read_fds);
        int max_fd = -1;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        bt_lock_acquire(portMAX_DELAY);

        /* Update: radio fds + alarm timeout */
        bt_radio_update(&read_fds, &max_fd);
        bt_alarm_update(&timeout);

        if (otTaskletsArePending(instance)) {
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
        }
        bt_lock_release();

        /* Wait for events */
        if (select(max_fd + 1, &read_fds, NULL, NULL, &timeout) >= 0) {
            bt_lock_acquire(portMAX_DELAY);

            /* Process radio events */
            bt_radio_process(instance, &read_fds);

            /* Process alarm events */
            bt_alarm_process(instance);

            /* Process OT tasklets */
            while (otTaskletsArePending(instance)) {
                otTaskletsProcess(instance);
            }

            bt_lock_release();
        } else {
            ESP_LOGE(BT_LOG_TAG, "select() failed");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

#endif /* USE_MATTER_THREAD */
