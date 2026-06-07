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
void      bt_radio_update(fd_set *read_fds, int *max_fd);
esp_err_t bt_radio_process(otInstance *instance, const fd_set *read_fds);

/* ---- Alarm platform ---- */
esp_err_t bt_alarm_init(void);
void      bt_alarm_deinit(void);
void      bt_alarm_update(struct timeval *timeout);
esp_err_t bt_alarm_process(otInstance *instance);
int       bt_alarm_get_event_fd(void);

/* ---- CoAP transport (for Berry-side SRP client) ----
 *
 * Provides a simple request/response wrapper around the OpenThread CoAP
 * client. Berry builds DNS-UPDATE payloads and asks BearThread to send them
 * as confirmable POSTs. Responses are queued internally and delivered
 * asynchronously when Berry calls bt_coap_poll_response().
 *
 * The OT-internal CoAP request can be sent only with the BT lock held;
 * callers do NOT need to lock — the wrapper handles that.
 */
#define BT_COAP_MAX_URI_LEN     64
#define BT_COAP_MAX_ADDR_LEN    64
#define BT_COAP_RX_QUEUE_LEN    8

typedef struct {
    int32_t  userdata;     /* opaque tag from request                       */
    int32_t  err;          /* 0=OK, OT_ERROR_* code on failure/timeout      */
    uint16_t code;         /* CoAP response code (0 on timeout)             */
    char     addr[BT_COAP_MAX_ADDR_LEN]; /* peer IPv6 string, NUL-terminated */
    uint16_t port;
    uint8_t *payload;      /* heap-allocated copy; caller frees after use   */
    uint16_t payload_len;
} bt_coap_response_t;

/* Lazy-initialised on first call. */
esp_err_t bt_coap_init(void);
void      bt_coap_deinit(void);

/* Send a CoAP confirmable POST. payload is copied internally; caller may
 * free after this returns. Response is delivered via the queue.
 * Returns ESP_OK if the request was queued for transmission. */
esp_err_t bt_coap_send_request(uint32_t userdata,
                               const char *uri,
                               int content_format,
                               const uint8_t *payload, size_t payload_len,
                               const char *addr_str, uint16_t port);

/* Poll one response from the queue. Returns 1 if received (caller frees
 * resp->payload), 0 if queue is empty, -1 on error. */
int bt_coap_poll_response(bt_coap_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* BT_PLATFORM_H_ */
