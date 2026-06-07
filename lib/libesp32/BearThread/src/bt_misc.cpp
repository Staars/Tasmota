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

/* Tasmota filesystem helpers (LittleFS). Declared here with C++ linkage so
 * they can be called from the otPlatSettings* C-linkage functions below.
 * These resolve at link time against tasmota/tasmota_xdrv_driver/xdrv_50_filesystem.ino.
 */
extern bool TfsFileExists(const char *fname);
extern size_t TfsFileSize(const char *fname);
extern bool TfsLoadFile(const char *fname, uint8_t *buf, uint32_t len);
extern bool TfsSaveFile(const char *fname, const uint8_t *buf, uint32_t len);
extern bool TfsDeleteFile(const char *fname);
extern bool TfsRenameFile(const char *fname1, const char *fname2);

/* All OT platform symbols below must keep C linkage so the OpenThread
 * library (compiled as C) can find them at link time. */
extern "C" {

/* ====== Settings (RAM cache + LittleFS persistence) ======
 *
 * OpenThread persists several keys that MUST survive reboot for an SRP
 * client to be accepted by an Apple Border Router:
 *   - 0x0007 SLAAC_IID_SECRET_KEY  → stable OMR address across reboots
 *   - 0x000b SRP_ECDSA_KEY         → stable SIG(0) KEY across reboots
 *   - 0x0001 ACTIVE_DATASET, 0x0003 NETWORK_INFO, 0x0004 PARENT_INFO …
 *
 * The previous implementation kept all of this in RAM only, so on every
 * boot the SLAAC IID and ECDSA key were regenerated. Apple's mDNSResponder
 * SRP advertising proxy then sees the EUI64-derived hostname re-appearing
 * with a different KEY (FCFS conflict per IETF draft-ietf-dnssd-srp §2.3.3)
 * and silently drops the UPDATE.
 *
 * We persist the whole settings table to a single binary blob on LittleFS
 * via the Tfs* helpers. Writes are atomic (tmp + rename). Mutations are
 * rare (a handful per network change) so we just flush on every change —
 * no debounce needed.
 *
 * File format (little-endian):
 *   [0..3]  magic 'B','T','S','1'
 *   [4]     version (1)
 *   [5]     reserved (0)
 *   [6..7]  num_entries (u16)
 * Then per entry:
 *   [0..1]  key (u16)
 *   [2..3]  value_len (u16)
 *   [4..]   value bytes
 */

#define BT_SETTINGS_MAX_ENTRIES  32
#define BT_SETTINGS_MAX_VALUE    256

typedef struct {
    uint16_t key;
    uint8_t  value[BT_SETTINGS_MAX_VALUE];
    uint16_t value_len;
    bool     used;
} bt_settings_entry_t;

static bt_settings_entry_t s_settings[BT_SETTINGS_MAX_ENTRIES];

static const char     BT_SETTINGS_PATH[]   = "/ot_settings.bin";
static const char     BT_SETTINGS_TMP[]    = "/ot_settings.tmp";
static const uint8_t  BT_SETTINGS_MAGIC[4] = {'B','T','S','1'};
static const uint8_t  BT_SETTINGS_VERSION  = 1;
static const size_t   BT_SETTINGS_HDR_LEN  = 8;

/* Load persisted settings into s_settings[]. Quiet no-op if file is missing
 * or invalid. s_settings[] must already be zeroed by the caller. */
static void bt_settings_load_from_fs(void)
{
    if (!TfsFileExists(BT_SETTINGS_PATH)) {
        AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings: no persisted file");
        return;
    }
    size_t flen = TfsFileSize(BT_SETTINGS_PATH);
    if (flen < BT_SETTINGS_HDR_LEN) {
        AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings: file too small (%u), ignoring", (unsigned)flen);
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(flen);
    if (!buf) {
        AddLog(BT_LOG_LEVEL_ERROR, "BT  : ot_settings: oom loading %u bytes", (unsigned)flen);
        return;
    }
    if (!TfsLoadFile(BT_SETTINGS_PATH, buf, flen)) {
        free(buf);
        return;
    }
    if (memcmp(buf, BT_SETTINGS_MAGIC, 4) != 0 || buf[4] != BT_SETTINGS_VERSION) {
        AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings: bad header, ignoring");
        free(buf);
        return;
    }
    uint16_t count = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    size_t   off   = BT_SETTINGS_HDR_LEN;
    uint16_t loaded = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (off + 4 > flen) break;
        uint16_t key  = (uint16_t)buf[off]   | ((uint16_t)buf[off + 1] << 8);
        uint16_t vlen = (uint16_t)buf[off + 2] | ((uint16_t)buf[off + 3] << 8);
        off += 4;
        if (vlen > BT_SETTINGS_MAX_VALUE || off + vlen > flen) {
            AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings: entry %u truncated, stopping", i);
            break;
        }
        /* Find a free slot in the RAM table */
        int slot = -1;
        for (int j = 0; j < BT_SETTINGS_MAX_ENTRIES; j++) {
            if (!s_settings[j].used) { slot = j; break; }
        }
        if (slot < 0) {
            AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings: RAM table full at entry %u", i);
            break;
        }
        s_settings[slot].key       = key;
        s_settings[slot].value_len = vlen;
        memcpy(s_settings[slot].value, buf + off, vlen);
        s_settings[slot].used      = true;
        off += vlen;
        loaded++;
    }
    free(buf);
    AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings loaded %u entries", loaded);
}

/* Serialize s_settings[] and atomically write to BT_SETTINGS_PATH. */
static void bt_settings_persist_to_fs(void)
{
    uint16_t count  = 0;
    size_t   needed = BT_SETTINGS_HDR_LEN;
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (s_settings[i].used) {
            count++;
            needed += 4 + s_settings[i].value_len;
        }
    }
    uint8_t *buf = (uint8_t *)malloc(needed);
    if (!buf) {
        AddLog(BT_LOG_LEVEL_ERROR, "BT  : ot_settings persist: oom (%u)", (unsigned)needed);
        return;
    }
    memcpy(buf, BT_SETTINGS_MAGIC, 4);
    buf[4] = BT_SETTINGS_VERSION;
    buf[5] = 0;
    buf[6] = (uint8_t)(count & 0xFF);
    buf[7] = (uint8_t)((count >> 8) & 0xFF);
    size_t off = BT_SETTINGS_HDR_LEN;
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (!s_settings[i].used) continue;
        buf[off]     = (uint8_t)(s_settings[i].key & 0xFF);
        buf[off + 1] = (uint8_t)((s_settings[i].key >> 8) & 0xFF);
        buf[off + 2] = (uint8_t)(s_settings[i].value_len & 0xFF);
        buf[off + 3] = (uint8_t)((s_settings[i].value_len >> 8) & 0xFF);
        memcpy(buf + off + 4, s_settings[i].value, s_settings[i].value_len);
        off += 4 + s_settings[i].value_len;
    }
    /* Atomic write: tmp -> delete final -> rename tmp to final.
     * LittleFS rename does not overwrite existing files, hence the
     * pre-delete. If the rename fails the old file is preserved. */
    bool ok = TfsSaveFile(BT_SETTINGS_TMP, buf, (uint32_t)needed);
    free(buf);
    if (!ok) {
        AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings persist: tmp write failed");
        return;
    }
    if (TfsFileExists(BT_SETTINGS_PATH)) {
        TfsDeleteFile(BT_SETTINGS_PATH);
    }
    if (!TfsRenameFile(BT_SETTINGS_TMP, BT_SETTINGS_PATH)) {
        AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings persist: rename failed");
        TfsDeleteFile(BT_SETTINGS_TMP);
    }
}

void otPlatSettingsInit(otInstance *aInstance, const uint16_t *aSensitiveKeys, uint16_t aSensitiveKeysLength)
{
    (void)aSensitiveKeys;
    (void)aSensitiveKeysLength;
    memset(s_settings, 0, sizeof(s_settings));
    bt_settings_load_from_fs();
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
            bt_settings_persist_to_fs();
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
            bt_settings_persist_to_fs();
            return OT_ERROR_NONE;
        }
    }
    return OT_ERROR_NO_BUFS;
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    bool changed = false;
    if (aIndex == -1) {
        /* Delete all entries for this key */
        for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
            if (s_settings[i].used && s_settings[i].key == aKey) {
                s_settings[i].used = false;
                changed = true;
            }
        }
        if (changed) bt_settings_persist_to_fs();
        return OT_ERROR_NONE;
    }

    int cur = 0;
    for (int i = 0; i < BT_SETTINGS_MAX_ENTRIES; i++) {
        if (s_settings[i].used && s_settings[i].key == aKey) {
            if (cur == aIndex) {
                s_settings[i].used = false;
                bt_settings_persist_to_fs();
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
    /* Factory-reset must also forget the on-disk copy so the next boot
     * gets a fresh SRP ECDSA key and SLAAC IID. */
    if (TfsFileExists(BT_SETTINGS_PATH)) TfsDeleteFile(BT_SETTINGS_PATH);
    if (TfsFileExists(BT_SETTINGS_TMP))  TfsDeleteFile(BT_SETTINGS_TMP);
    AddLog(BT_LOG_LEVEL_INFO, "BT  : ot_settings wiped (RAM + disk)");
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

};

#endif /* USE_MATTER_THREAD */
