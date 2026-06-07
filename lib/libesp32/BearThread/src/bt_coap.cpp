/*
 * bt_coap.cpp - BearThread CoAP transport wrapper
 *
 * Provides a simple request/response wrapper around the OpenThread CoAP
 * client, so the Berry-side SRP client can send DNS-UPDATE messages
 * without depending on the OpenThread header surface.
 *
 * Copyright (C) 2025 Christian Baars
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef USE_MATTER_THREAD

#include "bt_platform.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/instance.h"
#include "openthread/error.h"

typedef struct {
    uint32_t    userdata;
    int32_t     err;          /* OT_ERROR_* (0 == none) */
    uint16_t    code;         /* CoAP response code byte */
    char        addr[BT_COAP_MAX_ADDR_LEN];
    uint16_t    port;
    uint8_t    *payload;      /* heap copy; freed by poll consumer or deinit */
    uint16_t    payload_len;
} bt_coap_queue_item_t;

static QueueHandle_t     s_bt_coap_rx_queue    = NULL;
static bool              s_bt_coap_initialized = false;

esp_err_t bt_coap_init(void)
{
    if (s_bt_coap_initialized) return ESP_OK;

    s_bt_coap_rx_queue = xQueueCreate(BT_COAP_RX_QUEUE_LEN,
                                      sizeof(bt_coap_queue_item_t));
    if (!s_bt_coap_rx_queue) {
        return ESP_ERR_NO_MEM;
    }
    s_bt_coap_initialized = true;
    return ESP_OK;
}

void bt_coap_deinit(void)
{
    if (!s_bt_coap_initialized) return;

    if (s_bt_coap_rx_queue) {
        bt_coap_queue_item_t item;
        while (xQueueReceive(s_bt_coap_rx_queue, &item, 0) == pdTRUE) {
            if (item.payload) {
                free(item.payload);
                item.payload = NULL;
            }
        }
        vQueueDelete(s_bt_coap_rx_queue);
        s_bt_coap_rx_queue = NULL;
    }
    s_bt_coap_initialized = false;
}

/* Runs in OT task context. We only stash a heap copy of the response
 * payload; the otMessage is owned by OT and freed after this callback
 * returns. */
static void bt_coap_response_handler(void               *aContext,
                                     otMessage          *aMessage,
                                     const otMessageInfo *aMessageInfo,
                                     otError             aResult)
{
    bt_coap_queue_item_t item;
    memset(&item, 0, sizeof(item));
    item.userdata = (uint32_t)(uintptr_t)aContext;
    item.err      = (int32_t)aResult;

    if (aResult == OT_ERROR_NONE && aMessage != NULL) {
        item.code = (uint16_t)otCoapMessageGetCode(aMessage);

        uint16_t offset = otMessageGetOffset(aMessage);
        uint16_t length = otMessageGetLength(aMessage);
        if (length > offset) {
            uint16_t plen = length - offset;
            if (plen > 1024) plen = 1024;  /* sanity cap */
            item.payload = (uint8_t *)malloc(plen);
            if (item.payload) {
                otMessageRead(aMessage, offset, item.payload, plen);
                item.payload_len = plen;
            }
        }
        if (aMessageInfo) {
            otIp6AddressToString(&aMessageInfo->mPeerAddr,
                                 item.addr, sizeof(item.addr));
            item.port = aMessageInfo->mPeerPort;
        }
    }

    if (s_bt_coap_rx_queue) {
        if (xQueueSend(s_bt_coap_rx_queue, &item, 0) != pdTRUE) {
            if (item.payload) {
                free(item.payload);
                item.payload = NULL;
            }
        }
    } else {
        if (item.payload) {
            free(item.payload);
            item.payload = NULL;
        }
    }
}

esp_err_t bt_coap_send_request(uint32_t    userdata,
                               const char *uri,
                               int         content_format,
                               const uint8_t *payload, size_t payload_len,
                               const char *addr_str, uint16_t port)
{
    esp_err_t rc = bt_coap_init();
    if (rc != ESP_OK) return rc;

    if (!addr_str || !*addr_str) return ESP_ERR_INVALID_ARG;

    bt_lock_acquire(portMAX_DELAY);
    otInstance *instance = bt_get_instance();
    if (!instance) {
        bt_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    otMessage *msg = otCoapNewMessage(instance, NULL);
    if (!msg) {
        bt_lock_release();
        return ESP_ERR_NO_MEM;
    }

    otCoapMessageInit(msg, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);

    otError err = OT_ERROR_NONE;

    if (uri && *uri) {
        err = otCoapMessageAppendUriPathOptions(msg, uri);
        if (err != OT_ERROR_NONE) {
            otMessageFree(msg);
            bt_lock_release();
            return ESP_FAIL;
        }
    }

    if (content_format > 0) {
        err = otCoapMessageAppendContentFormatOption(msg,
                                                    (otCoapOptionContentFormat)content_format);
        if (err != OT_ERROR_NONE) {
            otMessageFree(msg);
            bt_lock_release();
            return ESP_FAIL;
        }
    }

    if (payload && payload_len > 0) {
        if (payload_len > UINT16_MAX) {
            otMessageFree(msg);
            bt_lock_release();
            return ESP_ERR_INVALID_SIZE;
        }
        err = otCoapMessageSetPayloadMarker(msg);
        if (err != OT_ERROR_NONE) {
            otMessageFree(msg);
            bt_lock_release();
            return ESP_FAIL;
        }
        err = otMessageAppend(msg, payload, (uint16_t)payload_len);
        if (err != OT_ERROR_NONE) {
            otMessageFree(msg);
            bt_lock_release();
            return ESP_FAIL;
        }
    }

    otMessageInfo msgInfo;
    memset(&msgInfo, 0, sizeof(msgInfo));
    otIp6AddressFromString(addr_str, &msgInfo.mPeerAddr);
    msgInfo.mPeerPort = port;

    err = otCoapSendRequest(instance, msg, &msgInfo,
                            bt_coap_response_handler,
                            (void *)(uintptr_t)userdata);
    bt_lock_release();

    if (err != OT_ERROR_NONE) {
        /* On error, OT did not consume the message; free it ourselves. */
        otMessageFree(msg);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int bt_coap_poll_response(bt_coap_response_t *resp)
{
    if (!resp) return -1;
    if (!s_bt_coap_initialized || !s_bt_coap_rx_queue) return 0;

    bt_coap_queue_item_t item;
    if (xQueueReceive(s_bt_coap_rx_queue, &item, 0) != pdTRUE) {
        return 0;
    }

    resp->userdata    = (int32_t)item.userdata;
    resp->err         = item.err;
    resp->code        = item.code;
    resp->port        = item.port;
    resp->payload     = item.payload;
    resp->payload_len = item.payload_len;

    size_t copy_len = strnlen(item.addr, BT_COAP_MAX_ADDR_LEN - 1);
    memcpy(resp->addr, item.addr, copy_len);
    resp->addr[copy_len] = '\0';

    return 1;
}

#endif /* USE_MATTER_THREAD */
