/*
 * bt_misc.c - BearThread miscellaneous platform functions
 *
 * Implements: settings (RAM-only), logging, memory, reset, assert
 *
 * Copyright (C) 2025 Christian Baars
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef USE_MATTER_THREAD

#include "bt_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"

#include "openthread/platform/logging.h"
#include "openthread/platform/memory.h"
#include "openthread/platform/misc.h"
#include "openthread/platform/settings.h"

/* Tasmota AddLog — route OT-internal logs into the Tasmota log stream so
 * they show up in the web console and over MQTT, instead of being
 * suppressed by ESP-IDF's LOG_LOCAL_LEVEL gate. AddLog is defined in
 * Tasmota's C++ code without `extern "C"`, so this file must be compiled
 * as C++ (renamed from bt_misc.c → bt_misc.cpp).
 *
 * Tasmota log levels: 1=ERROR, 2=INFO, 3=DEBUG, 4=DEBUG_MORE.
 */
extern void AddLog(uint32_t loglevel, const char *formatP, ...);
#define BT_LOG_LEVEL_ERROR  1
#define BT_LOG_LEVEL_INFO   2
#define BT_LOG_LEVEL_DEBUG  3

/* All OT platform symbols below must keep C linkage so the OpenThread
 * library (compiled as C) can find them at link time. */
extern "C" {

/* ====== Settings (RAM-only) ====== */

#define BT_SETTINGS_MAX_ENTRIES  32
#define BT_SETTINGS_MAX_VALUE    256

typedef struct {
    uint16_t key;
    uint8_t  value[BT_SETTINGS_MAX_VALUE];
    uint16_t value_len;
    bool     used;
} bt_settings_entry_t;

static bt_settings_entry_t s_settings[BT_SETTINGS_MAX_ENTRIES];

void otPlatSettingsInit(otInstance *aInstance, const uint16_t *aSensitiveKeys, uint16_t aSensitiveKeysLength)
{
    memset(s_settings, 0, sizeof(s_settings));
    ESP_LOGI(BT_LOG_TAG, "Settings: RAM-only store initialized (%d slots)", BT_SETTINGS_MAX_ENTRIES);
}

void otPlatSettingsDeinit(otInstance *aInstance)
{
    /* nothing to close */
}

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    int cur = 0;
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (s_settings[i].used && s_settings[i].key == aKey) {
            if (cur == aIndex) {
                if (aValue && aValueLength) {
                    uint16_t copy_len = (*aValueLength < s_settings[i].value_len) ? *aValueLength : s_settings[i].value_len;
                    memcpy(aValue, s_settings[i].value, copy_len);
                    *aValueLength = s_settings[i].value_len;
                } else if (aValueLength) {
                    *aValueLength = s_settings[i].value_len;
                }
                return OT_ERROR_NONE;
            }
            cur++;
        }
    }
    return OT_ERROR_NOT_FOUND;
}

otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    if (aValueLength > BT_SETTINGS_MAX_VALUE) return OT_ERROR_NO_BUFS;

    /* Delete all existing entries for this key first (Set = replace) */
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (s_settings[i].used && s_settings[i].key == aKey) {
            s_settings[i].used = false;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (!s_settings[i].used) {
            s_settings[i].key = aKey;
            s_settings[i].value_len = aValueLength;
            memcpy(s_settings[i].value, aValue, aValueLength);
            s_settings[i].used = true;
            return OT_ERROR_NONE;
        }
    }
    return OT_ERROR_NO_BUFS;
}

otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    if (aValueLength > BT_SETTINGS_MAX_VALUE) return OT_ERROR_NO_BUFS;

    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (!s_settings[i].used) {
            s_settings[i].key = aKey;
            s_settings[i].value_len = aValueLength;
            memcpy(s_settings[i].value, aValue, aValueLength);
            s_settings[i].used = true;
            return OT_ERROR_NONE;
        }
    }
    return OT_ERROR_NO_BUFS;
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    if (aIndex == -1) {
        /* Delete all entries for this key */
        for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
            if (s_settings[i].used && s_settings[i].key == aKey) {
                s_settings[i].used = false;
            }
        }
        return OT_ERROR_NONE;
    }

    int cur = 0;
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (s_settings[i].used && s_settings[i].key == aKey) {
            if (cur == aIndex) {
                s_settings[i].used = false;
                return OT_ERROR_NONE;
            }
            cur++;
        }
    }
    return OT_ERROR_NOT_FOUND;
}

void otPlatSettingsWipe(otInstance *aInstance)
{
    memset(s_settings, 0, sizeof(s_settings));
}

/* ====== Logging ====== */

#if (OPENTHREAD_CONFIG_LOG_OUTPUT == OPENTHREAD_CONFIG_LOG_OUTPUT_PLATFORM_DEFINED)
/* Route OT-internal logs through Tasmota's AddLog so they appear in the
 * normal Tasmota log stream (web console, serial, MQTT). The OT log
 * level decides which Tasmota loglevel we use; INFO/NOTE → INFO,
 * CRIT/WARN → INFO (so they're never suppressed below INFO), DEBG →
 * DEBUG. We deliberately don't gate on Tasmota's runtime log level here
 * — AddLog itself filters by loglevel — so all messages BearThread
 * emitted at compile-time are forwarded.
 */
void otPlatLog(otLogLevel log_level, otLogRegion log_region, const char *format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    /* Trim trailing CR/LF so AddLog's own newline doesn't double up. */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;

    uint32_t tasmota_level;
    const char *tag = "OT?";
    switch (log_level) {
    case OT_LOG_LEVEL_CRIT: tasmota_level = BT_LOG_LEVEL_ERROR; tag = "OT!"; break;
    case OT_LOG_LEVEL_WARN: tasmota_level = BT_LOG_LEVEL_INFO;  tag = "OTw"; break;
    case OT_LOG_LEVEL_NOTE: tasmota_level = BT_LOG_LEVEL_INFO;  tag = "OTn"; break;
    case OT_LOG_LEVEL_INFO: tasmota_level = BT_LOG_LEVEL_INFO;  tag = "OTi"; break;
    default:                tasmota_level = BT_LOG_LEVEL_DEBUG; tag = "OTd"; break;
    }
    AddLog(tasmota_level, "%s : %s", tag, buf);
}
#endif

/* ====== Memory ====== */

void *otPlatCAlloc(size_t num, size_t size)
{
    return calloc(num, size);
}

void otPlatFree(void *ptr)
{
    free(ptr);
}

/* ====== Misc (reset, assert, MCU power) ====== */

static otPlatMcuPowerState s_mcu_power_state = OT_PLAT_MCU_POWER_STATE_ON;

void otPlatReset(otInstance *aInstance)
{
    esp_restart();
}

otPlatResetReason otPlatGetResetReason(otInstance *aInstance)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return OT_PLAT_RESET_REASON_POWER_ON;
    case ESP_RST_SW:       return OT_PLAT_RESET_REASON_SOFTWARE;
    case ESP_RST_PANIC:    return OT_PLAT_RESET_REASON_FAULT;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return OT_PLAT_RESET_REASON_WATCHDOG;
    case ESP_RST_EXT:      return OT_PLAT_RESET_REASON_EXTERNAL;
    default:               return OT_PLAT_RESET_REASON_UNKNOWN;
    }
}

void otPlatWakeHost(void) {}

otError otPlatSetMcuPowerState(otInstance *aInstance, otPlatMcuPowerState aState)
{
    if (aState == OT_PLAT_MCU_POWER_STATE_ON || aState == OT_PLAT_MCU_POWER_STATE_LOW_POWER) {
        s_mcu_power_state = aState;
        return OT_ERROR_NONE;
    }
    return OT_ERROR_FAILED;
}

otPlatMcuPowerState otPlatGetMcuPowerState(otInstance *aInstance)
{
    return s_mcu_power_state;
}

void otPlatAssertFail(const char *filename, int line)
{
    ESP_LOGE(BT_LOG_TAG, "OT assert failed at %s:%d", filename, line);
    /* Use esp_system_abort so the panic message includes the OT source location */
    char buf[128];
    snprintf(buf, sizeof(buf), "OT assert at %s:%d", filename, line);
    esp_system_abort(buf);
}

#endif /* USE_MATTER_THREAD */
