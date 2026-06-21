/*
 * bt_platform.h - BearThread platform internal header
 *
 * Replaces esp_openthread_platform.h / esp_openthread_common_macro.h
 * Self-contained — no dependency on ESP-IDF's OpenThread component headers.
 */

#ifndef BT_PLATFORM_H_
#define BT_PLATFORM_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "openthread/instance.h"
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_LOG_TAG "BearThread"

#ifndef MS_PER_S
#define MS_PER_S 1000
#endif
#ifndef US_PER_MS
#define US_PER_MS 1000
#endif
#ifndef US_PER_S
#define US_PER_S (MS_PER_S * US_PER_MS)
#endif

/* ---- Platform init/deinit ---- */
esp_err_t bt_platform_init(void);
esp_err_t bt_platform_deinit(void);

/* ---- OT instance management ---- */
otInstance *bt_get_instance(void);

/* ---- Lock (replaces esp_openthread_lock) ---- */
esp_err_t bt_lock_init(void);
void      bt_lock_deinit(void);
bool      bt_lock_acquire(uint32_t block_ticks);
void      bt_lock_release(void);

/* ---- Main loop ---- */
esp_err_t bt_launch_mainloop(void);

/* ---- Radio platform ---- */
esp_err_t bt_radio_init(void);
void      bt_radio_deinit(void);
void      bt_radio_reclaim(void);
void      bt_radio_update(fd_set *read_fds, int *max_fd);
esp_err_t bt_radio_process(otInstance *instance, const fd_set *read_fds);

/* ---- Alarm platform ---- */
esp_err_t bt_alarm_init(void);
void      bt_alarm_deinit(void);
void      bt_alarm_update(struct timeval *timeout);
esp_err_t bt_alarm_process(otInstance *instance);
int       bt_alarm_get_event_fd(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_PLATFORM_H_ */
