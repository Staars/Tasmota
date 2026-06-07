/*
 * bt_radio.c - BearThread IEEE 802.15.4 radio platform layer
 *
 * Implements OpenThread's radio platform API using ESP32's IEEE 802.15.4 HAL.
 * Based on ESP-IDF's esp_openthread_radio.c but self-contained.
 *
 * Copyright (C) 2025 Christian Baars
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef USE_MATTER_THREAD

#include "bt_platform.h"

#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

#include "esp_ieee802154.h"
#include "esp_ieee802154_types.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_eventfd.h"
#include "esp_log.h"

#include "openthread/platform/radio.h"
#include "openthread/platform/diag.h"
#include "openthread/platform/time.h"
#include "utils/mac_frame.h"


#define BT_RX_BUFFER_SIZE 20

#define EVENT_TX_DONE          (1 << 0)
#define EVENT_TX_FAILED        (1 << 1)
#define EVENT_RX_DONE          (1 << 2)
#define EVENT_ENERGY_DETECT    (1 << 3)
#define EVENT_SLEEP            (1 << 4)

typedef struct {
    uint8_t length;
    uint8_t psdu[OT_RADIO_FRAME_MAX_SIZE];
} bt_radio_tx_psdu_t;

typedef struct {
    int head;
    int tail;
    atomic_int used;
} bt_circular_queue_t;

static otRadioFrame s_transmit_frame;
static bt_radio_tx_psdu_t s_transmit_psdu;
static otRadioFrame s_receive_frame[BT_RX_BUFFER_SIZE];
static otRadioFrame s_ack_frame;
static int s_ed_power;
static esp_ieee802154_tx_error_t s_tx_error;
static int s_radio_event_fd = -1;
static uint8_t s_txrx_events;
static bt_circular_queue_t s_recv_queue = {0, 0, 0};

#if OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT
static otRadioIeInfo s_transmit_ie_info;
#endif

#if OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2
static uint32_t s_mac_frame_counter;
static uint8_t s_key_id;
static struct otMacKeyMaterial s_pervious_key;
static struct otMacKeyMaterial s_current_key;
static struct otMacKeyMaterial s_next_key;
static bool s_with_security_enh_ack = false;
static uint32_t s_ack_frame_counter;
static uint8_t s_ack_key_id;
static uint8_t s_security_key[16];
static uint8_t s_security_addr[8];
static uint8_t *s_enhack;
#endif

static void set_event(uint8_t event)
{
    uint64_t event_write = event;
    s_txrx_events |= event;
    int ret = write(s_radio_event_fd, &event_write, sizeof(event_write));
    assert(ret == sizeof(event_write));
}

static inline void clr_event(uint8_t event) { s_txrx_events &= ~event; }
static inline bool get_event(uint8_t event) { return s_txrx_events & event; }

/* ---- Platform init/deinit ---- */

esp_err_t bt_radio_init(void)
{
    if (s_radio_event_fd != -1) return ESP_ERR_INVALID_STATE;

    s_radio_event_fd = eventfd(0, EFD_SUPPORT_ISR);
    if (s_radio_event_fd < 0) return ESP_FAIL;

    s_transmit_frame.mPsdu = s_transmit_psdu.psdu;

    for (uint8_t i = 0; i < BT_RX_BUFFER_SIZE; i++) {
        s_receive_frame[i].mPsdu = NULL;
    }
    s_ack_frame.mPsdu = NULL;
    memset(&s_recv_queue, 0, sizeof(s_recv_queue));

#if OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT
    s_transmit_frame.mInfo.mTxInfo.mIeInfo = &s_transmit_ie_info;
#endif

    esp_ieee802154_enable();
    esp_ieee802154_set_promiscuous(false);
    esp_ieee802154_set_rx_when_idle(true);

    return ESP_OK;
}

void bt_radio_deinit(void)
{
    if (s_radio_event_fd >= 0) {
        close(s_radio_event_fd);
        s_radio_event_fd = -1;
    }
    esp_ieee802154_disable();
}

void bt_radio_update(fd_set *read_fds, int *max_fd)
{
    FD_SET(s_radio_event_fd, read_fds);
    if (s_radio_event_fd > *max_fd) {
        *max_fd = s_radio_event_fd;
    }
}

esp_err_t bt_radio_process(otInstance *instance, const fd_set *read_fds)
{
    if (!FD_ISSET(s_radio_event_fd, read_fds)) return ESP_OK;

    uint64_t event_read;
    int ret = read(s_radio_event_fd, &event_read, sizeof(event_read));
    assert(ret == sizeof(event_read));

    if (get_event(EVENT_TX_DONE)) {
        clr_event(EVENT_TX_DONE);
        if (s_ack_frame.mPsdu == NULL) {
            otPlatRadioTxDone(instance, &s_transmit_frame, NULL, OT_ERROR_NONE);
        } else {
            otPlatRadioTxDone(instance, &s_transmit_frame, &s_ack_frame, OT_ERROR_NONE);
            esp_ieee802154_receive_handle_done(s_ack_frame.mPsdu - 1);
            s_ack_frame.mPsdu = NULL;
        }
    }

    if (get_event(EVENT_TX_FAILED)) {
        clr_event(EVENT_TX_FAILED);
        otError err = OT_ERROR_NONE;
        switch (s_tx_error) {
        case ESP_IEEE802154_TX_ERR_CCA_BUSY:
        case ESP_IEEE802154_TX_ERR_ABORT:
        case ESP_IEEE802154_TX_ERR_COEXIST:
            err = OT_ERROR_CHANNEL_ACCESS_FAILURE;
            break;
        case ESP_IEEE802154_TX_ERR_NO_ACK:
        case ESP_IEEE802154_TX_ERR_INVALID_ACK:
            err = OT_ERROR_NO_ACK;
            break;
        default:
            assert(false);
            break;
        }
        otPlatRadioTxDone(instance, &s_transmit_frame, NULL, err);
    }

    if (get_event(EVENT_ENERGY_DETECT)) {
        clr_event(EVENT_ENERGY_DETECT);
        otPlatRadioEnergyScanDone(instance, s_ed_power);
    }

    if (get_event(EVENT_RX_DONE)) {
        clr_event(EVENT_RX_DONE);
        while (atomic_load(&s_recv_queue.used)) {
            if (s_receive_frame[s_recv_queue.head].mPsdu != NULL) {
                otPlatRadioReceiveDone(instance, &s_receive_frame[s_recv_queue.head], OT_ERROR_NONE);
                esp_ieee802154_receive_handle_done(s_receive_frame[s_recv_queue.head].mPsdu - 1);
                s_receive_frame[s_recv_queue.head].mPsdu = NULL;
                s_recv_queue.head = (s_recv_queue.head + 1) % BT_RX_BUFFER_SIZE;
                atomic_fetch_sub(&s_recv_queue.used, 1);
            }
        }
    }

    if (get_event(EVENT_SLEEP)) {
        clr_event(EVENT_SLEEP);
        esp_ieee802154_sleep();
    }

    return ESP_OK;
}

/* ---- OpenThread Radio Platform API ---- */

void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    uint8_t eui64[8] = {0};
    esp_read_mac(eui64, ESP_MAC_IEEE802154);
    memcpy(aIeeeEui64, eui64, sizeof(eui64));
}

void otPlatRadioSetPanId(otInstance *aInstance, uint16_t panid)
{
    esp_ieee802154_set_panid(panid);
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance, const otExtAddress *aAddress)
{
    esp_ieee802154_set_extended_address(aAddress->m8);
}

void otPlatRadioSetShortAddress(otInstance *aInstance, uint16_t aAddress)
{
    esp_ieee802154_set_short_address(aAddress);
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    esp_ieee802154_set_promiscuous(aEnable);
}

otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    return (otRadioCaps)(OT_RADIO_CAPS_ENERGY_SCAN | OT_RADIO_CAPS_TRANSMIT_SEC | OT_RADIO_CAPS_RECEIVE_TIMING | OT_RADIO_CAPS_SLEEP_TO_TX);
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    return (esp_ieee802154_get_state() != ESP_IEEE802154_RADIO_DISABLE);
}

otError otPlatRadioEnable(otInstance *aInstance)
{
    /* Radio is already enabled by bt_radio_init(); do not re-enable here */
    return OT_ERROR_NONE;
}

otError otPlatRadioDisable(otInstance *aInstance)
{
    /* Radio will be disabled by bt_radio_deinit(); do not disable here */
    return OT_ERROR_NONE;
}

otError otPlatRadioSleep(otInstance *aInstance)
{
    esp_ieee802154_sleep();
    return OT_ERROR_NONE;
}

otError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    esp_ieee802154_set_channel(aChannel);
    esp_ieee802154_receive();
    return OT_ERROR_NONE;
}

otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    return &s_transmit_frame;
}

otError otPlatRadioTransmit(otInstance *aInstance, otRadioFrame *aFrame)
{
    aFrame->mPsdu[-1] = aFrame->mLength;

    esp_ieee802154_set_channel(aFrame->mChannel);

#if OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2
    if (aFrame->mInfo.mTxInfo.mIsSecurityProcessed) {
        otMacFrameSetFrameCounter(aFrame, s_mac_frame_counter++);
    }
#endif

    if (aFrame->mInfo.mTxInfo.mCsmaCaEnabled) {
        esp_ieee802154_transmit(&aFrame->mPsdu[-1], true);
    } else {
        esp_ieee802154_transmit(&aFrame->mPsdu[-1], false);
    }

    otPlatRadioTxStarted(aInstance, aFrame);
    return OT_ERROR_NONE;
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    return -120;
}

otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t *aThreshold)
{
    *aThreshold = -120;
    return OT_ERROR_NONE;
}

otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t aThreshold)
{
    return OT_ERROR_NONE;
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    return esp_ieee802154_get_promiscuous();
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    return esp_ieee802154_get_recent_rssi();
}

otError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    esp_ieee802154_set_channel(aScanChannel);
    esp_ieee802154_energy_detect(aScanDuration * 1000);  /* ms to us */
    return OT_ERROR_NONE;
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    esp_ieee802154_set_pending_mode(aEnable ? ESP_IEEE802154_AUTO_PENDING_ENHANCED : ESP_IEEE802154_AUTO_PENDING_DISABLE);
}

otError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, uint16_t aShortAddress)
{
    return (esp_ieee802154_add_pending_addr((const uint8_t *)&aShortAddress, true) == ESP_OK) ? OT_ERROR_NONE : OT_ERROR_NO_BUFS;
}

otError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    return (esp_ieee802154_add_pending_addr(aExtAddress->m8, false) == ESP_OK) ? OT_ERROR_NONE : OT_ERROR_NO_BUFS;
}

otError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, uint16_t aShortAddress)
{
    return (esp_ieee802154_clear_pending_addr((const uint8_t *)&aShortAddress, true) == ESP_OK) ? OT_ERROR_NONE : OT_ERROR_NO_BUFS;
}

otError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    return (esp_ieee802154_clear_pending_addr(aExtAddress->m8, false) == ESP_OK) ? OT_ERROR_NONE : OT_ERROR_NO_BUFS;
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    esp_ieee802154_reset_pending_table(false);
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    esp_ieee802154_reset_pending_table(true);
}

#if OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2
void otPlatRadioSetMacKey(otInstance *aInstance, uint8_t aKeyIdMode, uint8_t aKeyId,
                          const otMacKeyMaterial *aPrevKey, const otMacKeyMaterial *aCurrKey,
                          const otMacKeyMaterial *aNextKey, otRadioKeyType aKeyType)
{
    assert(aKeyType == OT_KEY_TYPE_LITERAL_KEY);
    assert(aPrevKey != NULL && aCurrKey != NULL && aNextKey != NULL);

    s_key_id = aKeyId;
    memcpy(&s_pervious_key, aPrevKey, sizeof(otMacKeyMaterial));
    memcpy(&s_current_key, aCurrKey, sizeof(otMacKeyMaterial));
    memcpy(&s_next_key, aNextKey, sizeof(otMacKeyMaterial));
}

void otPlatRadioSetMacFrameCounter(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    s_mac_frame_counter = aMacFrameCounter;
}

void otPlatRadioSetMacFrameCounterIfLarger(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    if (aMacFrameCounter > s_mac_frame_counter) {
        s_mac_frame_counter = aMacFrameCounter;
    }
}

uint64_t otPlatRadioGetNow(otInstance *aInstance)
{
    return (uint64_t)esp_timer_get_time();
}
#endif /* OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2 */

otError otPlatRadioSetChannelMaxTransmitPower(otInstance *aInstance, uint8_t aChannel, int8_t aMaxPower)
{
    return OT_ERROR_NONE;
}

void otPlatRadioSetRxOnWhenIdle(otInstance *aInstance, bool aEnable)
{
    esp_ieee802154_set_rx_when_idle(aEnable);
}

uint32_t otPlatRadioGetPreferredChannelMask(otInstance *aInstance)
{
    return 0x07FFF800;  /* channels 11-26 */
}

uint32_t otPlatRadioGetSupportedChannelMask(otInstance *aInstance)
{
    return 0x07FFF800;  /* channels 11-26 */
}

/* ---- ISR callbacks from IEEE 802.15.4 HAL ---- */

static void IRAM_ATTR convert_to_ot_frame(uint8_t *data, esp_ieee802154_frame_info_t *frame_info,
                                           otRadioFrame *radio_frame)
{
    radio_frame->mPsdu = data + 1;
    radio_frame->mLength = *data;
    radio_frame->mChannel = frame_info->channel;
    radio_frame->mInfo.mRxInfo.mRssi = frame_info->rssi;
    radio_frame->mInfo.mRxInfo.mLqi = frame_info->lqi;
    radio_frame->mInfo.mRxInfo.mAckedWithFramePending = frame_info->pending;
    radio_frame->mInfo.mRxInfo.mTimestamp = frame_info->timestamp;
}

void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                            esp_ieee802154_frame_info_t *ack_frame_info)
{
    assert(frame == (uint8_t *)&s_transmit_psdu);

    if (ack != NULL) {
        s_ack_frame.mLength = (uint16_t)(*ack);
        s_ack_frame.mPsdu = (uint8_t *)(ack + 1);
        s_ack_frame.mChannel = ack_frame_info->channel;
        s_ack_frame.mInfo.mRxInfo.mRssi = ack_frame_info->rssi;
        s_ack_frame.mInfo.mRxInfo.mLqi = ack_frame_info->lqi;
        s_ack_frame.mInfo.mRxInfo.mTimestamp = ack_frame_info->timestamp;
    }
    set_event(EVENT_TX_DONE);
}

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *data, esp_ieee802154_frame_info_t *frame_info)
{
    if (atomic_load(&s_recv_queue.used) == BT_RX_BUFFER_SIZE) {
        return;  /* buffer full, drop frame */
    }

    convert_to_ot_frame(data, frame_info, &(s_receive_frame[s_recv_queue.tail]));

#if OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2
    {
        otRadioFrame ot_frame;
        ot_frame.mPsdu = data + 1;
        if (otMacFrameIsAckRequested(&ot_frame) && otMacFrameIsVersion2015(&ot_frame)) {
            s_receive_frame[s_recv_queue.tail].mInfo.mRxInfo.mAckedWithSecEnhAck = s_with_security_enh_ack;
            s_receive_frame[s_recv_queue.tail].mInfo.mRxInfo.mAckFrameCounter = s_ack_frame_counter;
            s_receive_frame[s_recv_queue.tail].mInfo.mRxInfo.mAckKeyId = s_ack_key_id;
        } else {
            s_receive_frame[s_recv_queue.tail].mInfo.mRxInfo.mAckedWithSecEnhAck = false;
        }
        s_with_security_enh_ack = false;
    }
#endif

    s_recv_queue.tail = (s_recv_queue.tail + 1) % BT_RX_BUFFER_SIZE;
    atomic_fetch_add(&s_recv_queue.used, 1);
    set_event(EVENT_RX_DONE);
}

void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    assert(frame == (uint8_t *)&s_transmit_psdu);
    s_tx_error = error;
    set_event(EVENT_TX_FAILED);
}

void IRAM_ATTR esp_ieee802154_receive_sfd_done(void) {}
void IRAM_ATTR esp_ieee802154_transmit_sfd_done(uint8_t *frame) {}
void IRAM_ATTR esp_ieee802154_cca_done(bool channel_free) {}
void IRAM_ATTR esp_ieee802154_receive_at_done(void) { set_event(EVENT_SLEEP); }

void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power)
{
    s_ed_power = power;
    set_event(EVENT_ENERGY_DETECT);
}

#if OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2

static esp_err_t IRAM_ATTR enh_ack_set_security(otRadioFrame *ack_frame)
{
    struct otMacKeyMaterial *key = NULL;
    uint8_t key_id;

    assert(otMacFrameIsSecurityEnabled(ack_frame));
    key_id = otMacFrameGetKeyId(ack_frame);
    if (!(otMacFrameIsKeyIdMode1(ack_frame) && key_id != 0)) return ESP_FAIL;

    if (key_id == s_key_id)          key = &s_current_key;
    else if (key_id == s_key_id - 1) key = &s_pervious_key;
    else if (key_id == s_key_id + 1) key = &s_next_key;
    else return ESP_FAIL;

    s_ack_frame_counter = s_mac_frame_counter;
    s_ack_key_id = key_id;
    s_with_security_enh_ack = true;
    if (otMacFrameIsKeyIdMode1(ack_frame)) {
        esp_ieee802154_get_extended_address(s_security_addr);
        memcpy(s_security_key, (*key).mKeyMaterial.mKey.m8, OT_MAC_KEY_SIZE);
    }
    esp_ieee802154_set_transmit_security(&ack_frame->mPsdu[-1], s_security_key, s_security_addr);
    return ESP_OK;
}

esp_err_t IRAM_ATTR esp_ieee802154_enh_ack_generator(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info,
                                                      uint8_t *enhack_frame)
{
    otRadioFrame ack_frame;
    otRadioFrame ot_frame;
    uint8_t ack_ie_data[OT_ACK_IE_MAX_SIZE] = {0};
    uint8_t offset = 0;
    otError err;

    ack_frame.mPsdu = enhack_frame + 1;
    convert_to_ot_frame(frame, frame_info, &ot_frame);

    err = otMacFrameGenerateEnhAck(&ot_frame, frame_info->pending, ack_ie_data, offset, &ack_frame);
    if (err != OT_ERROR_NONE) return ESP_FAIL;

    enhack_frame[0] = ack_frame.mLength;
    s_enhack = enhack_frame;

    if (otMacFrameIsSecurityEnabled(&ack_frame) && !ack_frame.mInfo.mTxInfo.mIsSecurityProcessed) {
        otMacFrameSetFrameCounter(&ack_frame, s_mac_frame_counter++);
        if (enh_ack_set_security(&ack_frame) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}
#endif /* OPENTHREAD_CONFIG_THREAD_VERSION >= OT_THREAD_VERSION_1_2 */

/* ---- Entropy ---- */

otError otPlatEntropyGet(uint8_t *aOutput, uint16_t aOutputLength)
{
    esp_fill_random(aOutput, aOutputLength);
    return OT_ERROR_NONE;
}

#endif /* USE_MATTER_THREAD */
