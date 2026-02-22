/*
  xdrv_93_esp32_wifi_csi.ino - ESP32 WiFi CSI driver for Tasmota (Simplified Single-Source)

  Copyright (C) 2025

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ---- Design notes ----

  CIR Pipeline (WifiCsiProcessPipeline):
    - Subcarriers are placed at their correct frequency positions (DC-centered) before IFFT,
      so the output delay bins have physical meaning (1 bin ≈ 1/BW seconds).
    - Energy normalization removes RSSI dependency.
    - Only the multipath "reflection" bins (1 .. max_delay_bin) are used for the
      activity metric; bin 0 is the direct path and is excluded.
    - Smoothing uses a rate-adaptive exponential filter with a fixed 200 ms time constant
      so the feel of the score is the same regardless of ping/packet rate.

  Noise floor learning:
    - A slow running minimum of both activity_score and variance tracks the
      ambient "quiet" level of the environment. This is never zero in practice.
    - Snap-down: if the new value is lower than the tracked floor, accept it instantly.
    - Slow rise: otherwise, the floor drifts up at WIFI_CSI_NOISE_LEARN_RATE so it
      can track gradual environmental changes (temperature, furniture moved, etc.).
    - Updated only when NOT in motion so movement does not corrupt the floor estimate.
    - These noise floor values are exposed in status/JSON and can be used upstream
      (e.g. Berry) to auto-calibrate the threshold: threshold = noise_floor_activity * K.

  Hysteresis / motion debounce:
    - Motion is detected with a HIGH threshold (threshold_enter = activity_threshold).
    - Once triggered, a holdoff countdown (holdoff_sec, default 3 s) is started.
    - The countdown is refreshed as long as activity stays above a LOW threshold
      (threshold_hold = threshold_enter * HYSTERESIS_RATIO, default 0.6).
    - Only when the countdown expires without a refresh is State set back to "None".
    - This creates a natural "tail": even a brief freeze after movement keeps the
      Motion state alive for holdoff_sec seconds, preventing rapid toggling.
    - holdoff_sec is configurable via CsiActivity <threshold> <holdoff_sec>.
*/

// needs: custom_sdkconfig = CONFIG_ESP_WIFI_CSI_ENABLED=y

#ifdef ESP32
#ifdef CONFIG_ESP_WIFI_CSI_ENABLED
#define USE_WIFI_CSI
#ifdef USE_WIFI_CSI

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/portmacro.h"
#include "freertos/ringbuf.h"
#include "dsps_fft2r.h"
#include "dsps_view.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include <math.h>

#define XDRV_93 93

// ---- Tunable constants ----
#define WIFI_CSI_RINGBUF_SIZE         (4 * 1024)
#define WIFI_CSI_MAX_SUBCARRIERS      114
#define WIFI_CSI_MAX_PACKET_SIZE      (sizeof(wifi_csi_packet_header_t) + 512)
#define WIFI_CSI_FFT_SIZE             128

#define WIFI_CSI_HT20_SC              56
#define WIFI_CSI_HT40_SC              114

// Smoothing: fixed 200 ms time-constant regardless of packet rate
#define WIFI_CSI_SMOOTH_TAU_SEC       0.200f

// Hysteresis: hold-threshold = enter-threshold * ratio
#define WIFI_CSI_HYSTERESIS_RATIO     0.60f

// Default holdoff after motion activity drops below hold-threshold (seconds)
#define WIFI_CSI_DEFAULT_HOLDOFF_SEC  3.0f

// Noise floor EMA rise-rate (≈ 1 "forget" per 1000 quiet packets)
#define WIFI_CSI_NOISE_LEARN_RATE     0.001f

/*********************************************************************************************\
 * Per-target feature detection
\*********************************************************************************************/

#ifdef CONFIG_SOC_WIFI_HE_SUPPORT
  #define RX_CTRL_RSSI(info)        (info->rx_ctrl.rssi)
  #define RX_CTRL_MODE(info)        (info->rx_ctrl.cur_bb_format)
  #define RX_CTRL_NOISE_FLOOR(info) (info->rx_ctrl.noise_floor)
  #define CSI_DATA_TYPE             int16_t
  #define CSI_BYTES_PER_SC          (2 * sizeof(int16_t))
#else
  #define RX_CTRL_RSSI(info)        (info->rx_ctrl.rssi)
  #define RX_CTRL_MODE(info)        (info->rx_ctrl.sig_mode)
  #define RX_CTRL_NOISE_FLOOR(info) (info->rx_ctrl.noise_floor)
  #define CSI_DATA_TYPE             int8_t
  #define CSI_BYTES_PER_SC          (2 * sizeof(int8_t))
#endif

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
  #define CSI_HAS_FIRST_WORD_BUG 1
#else
  #define CSI_HAS_FIRST_WORD_BUG 0
#endif

typedef struct __attribute__((packed)) {
  uint32_t timestamp;
  uint8_t  mac[6];
  int8_t   rssi;
  int8_t   noise_floor;
  uint16_t data_len;
} wifi_csi_packet_header_t;

#define WIFI_CSI_HEADER_SIZE sizeof(wifi_csi_packet_header_t)

/*********************************************************************************************\
 * Command table
\*********************************************************************************************/

const char kCsiCommands[] PROGMEM = "Csi|Enable|Channel|Activity|Status|Ping";

void (* const CsiCommand[])(void) PROGMEM = {
  &CmndCsiEnable,
  &CmndCsiChannel,
  &CmndCsiActivity,
  &CmndCsiStatus,
  &CmndCsiPing
};

static portMUX_TYPE wifi_csi_mux = portMUX_INITIALIZER_UNLOCKED;

/*********************************************************************************************\
 * Driver state struct
\*********************************************************************************************/

struct WifiCsi {
  // ---- General ----
  bool    enabled;
  uint8_t channel;
  uint8_t router_mac[6];
  bool    router_mac_valid;
  int     subcarrier_count;

  volatile uint32_t packet_count;
  volatile uint32_t packets_dropped;

  RingbufHandle_t ringbuf;

  // ---- CIR pipeline ----
  float fft_input[WIFI_CSI_FFT_SIZE * 2];  // complex interleaved
  float cir_profile[WIFI_CSI_FFT_SIZE];
  float cir_baseline[WIFI_CSI_FFT_SIZE];
  bool  cir_baseline_valid;
  uint32_t baseline_packet_count;
  bool  fft_initialized;
  int   max_delay_bin;                      // BW-derived, set on first packet

  // ---- Smoothing ----
  uint32_t last_packet_ms;                  // for per-packet alpha calculation

  // ---- Warmup ----
  uint32_t warmup_counter;

  // ---- Activity metric ----
  float last_activity_score;
  int8_t last_rssi;
  int8_t last_noise_floor;
  float activity_threshold;                 // enter-motion threshold (user settable)

  // ---- Variance (std-dev of last 10 smoothed scores) ----
  float   activity_history[10];
  uint8_t history_index;
  uint8_t history_count;
  float   variance;

  // ---- Noise floor learning ----
  // Tracks the ambient "quiet" level; never 0 in real environments.
  // Updated only when NOT in motion.
  float noise_floor_activity;
  float noise_floor_variance;
  bool  noise_floor_valid;

  // ---- Hysteresis / motion debounce ----
  bool     motion_active;       // debounced motion state (this is what drives State output)
  uint32_t motion_holdoff_ms;   // remaining holdoff countdown
  float    holdoff_sec;         // user-settable holdoff duration
  uint32_t movement_count;      // total enter-motion events

  // ---- Ping ----
  esp_ping_handle_t ping_handle;
  ip_addr_t         ping_target;
  uint8_t           ping_hz;
} *WifiCsi = nullptr;

/*********************************************************************************************\
 * Helper: subcarrier count from data length
\*********************************************************************************************/

int WifiCsiDeriveSubcarrierCount(uint16_t data_len) {
  int num_sc = (int)data_len / (int)CSI_BYTES_PER_SC;
  if (num_sc > WIFI_CSI_MAX_SUBCARRIERS) num_sc = WIFI_CSI_MAX_SUBCARRIERS;
  if (num_sc < 0) num_sc = 0;
  return num_sc;
}

// Maximum multipath delay bin for indoor use (~500 ns propagation).
// HT20: 20 MHz → 50 ns/bin → 10 bins + 2 margin = 12
// HT40: 40 MHz → 25 ns/bin → 20 bins + 2 margin = 22
int WifiCsiMaxDelayBin(int num_sc) {
  return (num_sc >= WIFI_CSI_HT40_SC) ? 22 : 12;
}

/*********************************************************************************************\
 * Variance tracker (sliding window of last 10 smoothed activity scores)
\*********************************************************************************************/

void WifiCsiUpdateVariance(float new_score) {
  if (!WifiCsi) return;

  WifiCsi->activity_history[WifiCsi->history_index] = new_score;
  WifiCsi->history_index = (WifiCsi->history_index + 1) % 10;
  if (WifiCsi->history_count < 10) WifiCsi->history_count++;

  float sum = 0.0f;
  for (uint8_t i = 0; i < WifiCsi->history_count; i++) sum += WifiCsi->activity_history[i];
  float mean = sum / WifiCsi->history_count;

  float var_sum = 0.0f;
  for (uint8_t i = 0; i < WifiCsi->history_count; i++) {
    float d = WifiCsi->activity_history[i] - mean;
    var_sum += d * d;
  }
  WifiCsi->variance = sqrtf(var_sum / WifiCsi->history_count);
}

/*********************************************************************************************\
 * Noise floor learning
 *
 * Snap-down: accept any lower value immediately (new quiet environment).
 * Slow rise:  drift upward at WIFI_CSI_NOISE_LEARN_RATE so old minima are
 *             eventually forgotten as the environment changes.
 * Guard:      only called when NOT in motion, so movement can't corrupt the floor.
 *
 * Practical use: threshold = noise_floor_activity * K  (K ~ 3–10, tune empirically).
 * The noise floor will never be 0; it reflects thermal noise, AP beacon variations,
 * and any other static background variation in the channel.
\*********************************************************************************************/

void WifiCsiUpdateNoiseFloor(float activity, float variance) {
  if (!WifiCsi || WifiCsi->motion_active) return;

  if (!WifiCsi->noise_floor_valid) {
    WifiCsi->noise_floor_activity = activity;
    WifiCsi->noise_floor_variance = variance;
    WifiCsi->noise_floor_valid    = true;
    return;
  }

  if (activity < WifiCsi->noise_floor_activity) {
    WifiCsi->noise_floor_activity = activity;
  } else {
    WifiCsi->noise_floor_activity =
        (1.0f - WIFI_CSI_NOISE_LEARN_RATE) * WifiCsi->noise_floor_activity +
        WIFI_CSI_NOISE_LEARN_RATE * activity;
  }

  if (variance < WifiCsi->noise_floor_variance) {
    WifiCsi->noise_floor_variance = variance;
  } else {
    WifiCsi->noise_floor_variance =
        (1.0f - WIFI_CSI_NOISE_LEARN_RATE) * WifiCsi->noise_floor_variance +
        WIFI_CSI_NOISE_LEARN_RATE * variance;
  }
}

/*********************************************************************************************\
 * Hysteresis / motion debounce state machine
 *
 * IDLE  → MOTION  : smoothed_activity > threshold_enter
 * MOTION: holdoff timer refreshed while activity > threshold_hold
 * MOTION → IDLE   : holdoff timer expires (activity persistently below threshold_hold)
 *
 * The holdoff prevents rapid on/off toggling ("nervousness") when someone
 * briefly pauses. A 3-second default means motion stays latched for at least
 * 3 seconds after the last detectable movement.
\*********************************************************************************************/

void WifiCsiUpdateMotionState(float smoothed_activity, uint32_t elapsed_ms) {
  if (!WifiCsi) return;

  float threshold_enter = WifiCsi->activity_threshold;
  float threshold_hold  = threshold_enter * WIFI_CSI_HYSTERESIS_RATIO;

  if (!WifiCsi->motion_active) {
    if (smoothed_activity > threshold_enter) {
      WifiCsi->motion_active     = true;
      WifiCsi->motion_holdoff_ms = (uint32_t)(WifiCsi->holdoff_sec * 1000.0f);
      WifiCsi->movement_count++;
    }
  } else {
    if (smoothed_activity > threshold_hold) {
      // Refresh the holdoff — person is still moving
      WifiCsi->motion_holdoff_ms = (uint32_t)(WifiCsi->holdoff_sec * 1000.0f);
    } else {
      // Counting down toward idle
      if (elapsed_ms >= WifiCsi->motion_holdoff_ms) {
        WifiCsi->motion_holdoff_ms = 0;
        WifiCsi->motion_active     = false;
      } else {
        WifiCsi->motion_holdoff_ms -= elapsed_ms;
      }
    }
  }
}

/*********************************************************************************************\
 * CIR Processing Pipeline
 *
 * Fix 1 — Correct DC-centered subcarrier placement:
 *   802.11n CSI subcarriers span negative and positive frequencies around DC.
 *   The ESP IQ buffer delivers them as [negative half | positive half].
 *   We place the positive half at FFT bins 1..half and the negative half at
 *   bins (N-half)..(N-1) — the standard wrap-around for a DFT.
 *   This makes the IFFT output a physically correct delay-domain CIR where
 *   bin k corresponds to a propagation delay of k / bandwidth.
 *
 * Fix 2 — Rate-adaptive exponential smoothing:
 *   α = 1 − exp(−Δt / τ)  with τ = WIFI_CSI_SMOOTH_TAU_SEC (200 ms).
 *   This is independent of ping rate or packet rate.
 *
 * Fix 3 — Physically derived bin range:
 *   max_delay_bin is computed from the channel bandwidth (HT20/HT40) to cover
 *   propagation delays up to ~500 ns, which covers all indoor multipath.
\*********************************************************************************************/

void WifiCsiProcessPipeline(void* csi_data_ptr, int8_t rssi, int num_sc) {
  if (!WifiCsi || !WifiCsi->fft_initialized || num_sc < 4) return;

  WifiCsi->warmup_counter++;

  // ---- Rate-adaptive alpha ----
  uint32_t now_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  uint32_t elapsed = (WifiCsi->last_packet_ms == 0) ? 20 : (now_ms - WifiCsi->last_packet_ms);
  if (elapsed == 0)  elapsed = 1;
  if (elapsed > 500) elapsed = 500;
  WifiCsi->last_packet_ms = now_ms;

  float alpha = 1.0f - expf(-(float)elapsed / (WIFI_CSI_SMOOTH_TAU_SEC * 1000.0f));

  // ---- Zero FFT buffer ----
  memset(WifiCsi->fft_input, 0, WIFI_CSI_FFT_SIZE * 2 * sizeof(float));

#ifdef CONFIG_SOC_WIFI_HE_SUPPORT
  int16_t* iq_data = (int16_t*)csi_data_ptr;
#else
  int8_t*  iq_data = (int8_t*)csi_data_ptr;
#endif

  // ---- Energy normalisation ----
  float energy_sum = 0.0f;
  for (int i = 0; i < num_sc; i++) {
    float re = (float)iq_data[i * 2];
    float im = (float)iq_data[i * 2 + 1];
    energy_sum += re * re + im * im;
  }
  float scale = (energy_sum > 0.001f) ? (1.0f / sqrtf(energy_sum)) : 1.0f;

  // ---- DC-centered subcarrier placement ----
  // ESP layout: indices 0..half-1 → negative subcarriers
  //             indices half..num_sc-1 → positive subcarriers
  // FFT bin mapping (N = WIFI_CSI_FFT_SIZE = 128):
  //   positive sc k (0-based) → bin k+1          (bins 1 .. half)
  //   negative sc k (0-based) → bin N-half+k      (bins N-half .. N-1)
  //   DC bin 0 stays zero (no DC subcarrier in 802.11)
  int half = num_sc / 2;

  for (int i = 0; i < half; i++) {
    // Positive subcarriers
    int data_idx = half + i;
    int fft_bin  = i + 1;
    float w  = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (half - 1)));
    WifiCsi->fft_input[fft_bin * 2 + 0] =  (float)iq_data[data_idx * 2]     * scale * w;
    WifiCsi->fft_input[fft_bin * 2 + 1] = -(float)iq_data[data_idx * 2 + 1] * scale * w; // conjugate
  }

  for (int i = 0; i < half; i++) {
    // Negative subcarriers (wrap to upper FFT half)
    int data_idx = i;
    int fft_bin  = WIFI_CSI_FFT_SIZE - half + i;
    float w  = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (half - 1)));
    WifiCsi->fft_input[fft_bin * 2 + 0] =  (float)iq_data[data_idx * 2]     * scale * w;
    WifiCsi->fft_input[fft_bin * 2 + 1] = -(float)iq_data[data_idx * 2 + 1] * scale * w;
  }

  // ---- FFT → CIR magnitude ----
  dsps_fft2r_fc32(WifiCsi->fft_input, WIFI_CSI_FFT_SIZE);
  dsps_bit_rev_fc32(WifiCsi->fft_input, WIFI_CSI_FFT_SIZE);

  for (int i = 0; i < WIFI_CSI_FFT_SIZE; i++) {
    float re = WifiCsi->fft_input[i * 2 + 0];
    float im = WifiCsi->fft_input[i * 2 + 1];
    WifiCsi->cir_profile[i] = sqrtf(re * re + im * im);
  }

  // ---- Initialise baseline on first valid packet ----
  if (!WifiCsi->cir_baseline_valid) {
    memcpy(WifiCsi->cir_baseline, WifiCsi->cir_profile, sizeof(WifiCsi->cir_profile));
    WifiCsi->cir_baseline_valid    = true;
    WifiCsi->baseline_packet_count = 1;
    WifiCsi->last_rssi             = rssi;
    WifiCsi->max_delay_bin         = WifiCsiMaxDelayBin(num_sc);
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: Baseline set — max_delay_bin=%d (num_sc=%d)"),
           WifiCsi->max_delay_bin, num_sc);
    return;
  }

  // Keep max_delay_bin in sync in case bandwidth changes
  WifiCsi->max_delay_bin = WifiCsiMaxDelayBin(num_sc);

  // ---- Activity metric (multipath bins only) ----
  float cir_diff_energy = 0.0f;
  for (int i = 1; i <= WifiCsi->max_delay_bin; i++) {
    float diff = WifiCsi->cir_profile[i] - WifiCsi->cir_baseline[i];
    cir_diff_energy += diff * diff;
  }
  float activity_score = cir_diff_energy * 1000.0f;

  // ---- Rate-adaptive EMA smoothing ----
  WifiCsi->last_activity_score =
      (1.0f - alpha) * WifiCsi->last_activity_score + alpha * activity_score;

  // ---- Variance of recent smoothed scores ----
  WifiCsiUpdateVariance(WifiCsi->last_activity_score);

  // ---- Hysteresis / motion debounce ----
  WifiCsiUpdateMotionState(WifiCsi->last_activity_score, elapsed);

  // ---- Noise floor learning (quiet periods only) ----
  WifiCsiUpdateNoiseFloor(WifiCsi->last_activity_score, WifiCsi->variance);

  // ---- Adaptive baseline (slower during motion) ----
  float learn_rate = WifiCsi->motion_active ? 0.003f : 0.05f;
  for (int i = 0; i < WIFI_CSI_FFT_SIZE; i++) {
    WifiCsi->cir_baseline[i] =
        (1.0f - learn_rate) * WifiCsi->cir_baseline[i] +
        learn_rate * WifiCsi->cir_profile[i];
  }
  WifiCsi->baseline_packet_count++;

  // ---- Periodic debug log ----
  static uint32_t last_log = 0;
  if (millis() - last_log > 10000) {
    last_log = millis();
    AddLog(LOG_LEVEL_DEBUG_MORE,
           PSTR("CSI: score=%.2f var=%.2f nf_act=%.2f nf_var=%.2f motion=%d holdoff=%u alpha=%.3f"),
           WifiCsi->last_activity_score, WifiCsi->variance,
           WifiCsi->noise_floor_activity, WifiCsi->noise_floor_variance,
           WifiCsi->motion_active, WifiCsi->motion_holdoff_ms, alpha);
  }

  WifiCsi->last_rssi = rssi;
}

/*********************************************************************************************\
 * WiFi CSI packet callback
\*********************************************************************************************/

static void WifiCsiProcessPacket(void* ctx, wifi_csi_info_t* info) {
  if (!WifiCsi || !WifiCsi->enabled || !info || !info->buf) return;
  if (info->len < 4) {
    portENTER_CRITICAL(&wifi_csi_mux);
    WifiCsi->packets_dropped++;
    portEXIT_CRITICAL(&wifi_csi_mux);
    return;
  }

  if (WifiCsi->router_mac_valid) {
    for (int i = 0; i < 6; i++)
      if (info->mac[i] != WifiCsi->router_mac[i]) return;
  }

  int8_t*  csi_data = info->buf;
  uint16_t data_len = info->len;

#if CSI_HAS_FIRST_WORD_BUG
  if (info->first_word_invalid) {
    csi_data += 4;
    data_len -= 4;
    if (data_len < 4) {
      portENTER_CRITICAL(&wifi_csi_mux);
      WifiCsi->packets_dropped++;
      portEXIT_CRITICAL(&wifi_csi_mux);
      return;
    }
  }
#endif

  if (data_len > (WIFI_CSI_MAX_PACKET_SIZE - WIFI_CSI_HEADER_SIZE))
    data_len = WIFI_CSI_MAX_PACKET_SIZE - WIFI_CSI_HEADER_SIZE;

  portENTER_CRITICAL(&wifi_csi_mux);
  WifiCsi->packet_count++;
  portEXIT_CRITICAL(&wifi_csi_mux);

  uint8_t packet_buffer[WIFI_CSI_MAX_PACKET_SIZE];
  wifi_csi_packet_header_t* header = (wifi_csi_packet_header_t*)packet_buffer;
  header->timestamp   = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  memcpy(header->mac, info->mac, 6);
  header->rssi        = RX_CTRL_RSSI(info);
  header->noise_floor = RX_CTRL_NOISE_FLOOR(info);
  header->data_len    = data_len;
  memcpy(packet_buffer + WIFI_CSI_HEADER_SIZE, csi_data, data_len);

  BaseType_t sent = xRingbufferSend(WifiCsi->ringbuf, packet_buffer,
                                    WIFI_CSI_HEADER_SIZE + data_len, 0);
  if (sent != pdTRUE) {
    portENTER_CRITICAL(&wifi_csi_mux);
    WifiCsi->packets_dropped++;
    portEXIT_CRITICAL(&wifi_csi_mux);
  }
}

/*********************************************************************************************\
 * Ring buffer drain (one packet per FUNC_LOOP call)
\*********************************************************************************************/

void WifiCsiProcessBuffer() {
  if (!WifiCsi || !WifiCsi->enabled) return;

  size_t   item_size;
  uint8_t* packet = (uint8_t*)xRingbufferReceive(WifiCsi->ringbuf, &item_size, 0);
  if (!packet) return;

  if (item_size >= WIFI_CSI_HEADER_SIZE) {
    wifi_csi_packet_header_t* header = (wifi_csi_packet_header_t*)packet;
    void*    csi_data = (void*)(packet + WIFI_CSI_HEADER_SIZE);
    uint16_t data_len = header->data_len;
    if (data_len > item_size - WIFI_CSI_HEADER_SIZE)
      data_len = item_size - WIFI_CSI_HEADER_SIZE;

    WifiCsi->last_noise_floor = header->noise_floor;
    int num_sc = WifiCsiDeriveSubcarrierCount(data_len);
    if (num_sc > 0) WifiCsi->subcarrier_count = num_sc;
    WifiCsiProcessPipeline(csi_data, header->rssi, num_sc);
  }

  vRingbufferReturnItem(WifiCsi->ringbuf, packet);
}

/*********************************************************************************************\
 * Router MAC detection
\*********************************************************************************************/

bool WifiCsiGetRouterMac() {
  wifi_ap_record_t ap_info;
  esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
  if (err != ESP_OK) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Not connected to AP (0x%x)"), err);
    WifiCsi->router_mac_valid = false;
    return false;
  }

  memcpy(WifiCsi->router_mac, ap_info.bssid, 6);
  WifiCsi->router_mac_valid = true;

  char mac_str[18];
  ToHex_P(WifiCsi->router_mac, 6, mac_str, 18, ':');
  AddLog(LOG_LEVEL_INFO, PSTR("CSI: Router MAC: %s (RSSI: %d dBm)"), mac_str, ap_info.rssi);

  if (ap_info.second == WIFI_SECOND_CHAN_NONE) {
    WifiCsi->subcarrier_count = WIFI_CSI_HT20_SC;
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: HT20 — 56 subcarriers"));
  } else {
    WifiCsi->subcarrier_count = WIFI_CSI_HT40_SC;
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: HT40 — 114 subcarriers"));
  }
  return true;
}

/*********************************************************************************************\
 * Ping management
\*********************************************************************************************/

void WifiCsiStopPing() {
  if (!WifiCsi || !WifiCsi->ping_handle) return;
  esp_ping_stop(WifiCsi->ping_handle);
  esp_ping_delete_session(WifiCsi->ping_handle);
  WifiCsi->ping_handle = nullptr;
  WifiCsi->ping_hz     = 0;
  AddLog(LOG_LEVEL_INFO, PSTR("CSI: Ping stopped"));
}

bool WifiCsiStartPing(const char* target_ip, uint8_t hz) {
  if (!WifiCsi) return false;
  WifiCsiStopPing();
  if (hz == 0) return true;

  ip_addr_t addr;
  if (!ipaddr_aton(target_ip, &addr)) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Invalid ping IP: %s"), target_ip);
    return false;
  }
  if (hz > 100) hz = 100;
  uint32_t interval_ms = 1000 / hz;

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr     = addr;
  cfg.count           = 0;
  cfg.interval_ms     = interval_ms;
  cfg.timeout_ms      = 1000;
  cfg.task_stack_size = 2048;
  cfg.task_prio       = 2;

  esp_ping_callbacks_t cbs = {};
  esp_err_t err = esp_ping_new_session(&cfg, &cbs, &WifiCsi->ping_handle);
  if (err != ESP_OK) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Ping session failed: 0x%x"), err);
    WifiCsi->ping_handle = nullptr;
    return false;
  }

  err = esp_ping_start(WifiCsi->ping_handle);
  if (err != ESP_OK) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Ping start failed: 0x%x"), err);
    esp_ping_delete_session(WifiCsi->ping_handle);
    WifiCsi->ping_handle = nullptr;
    return false;
  }

  WifiCsi->ping_hz = hz;
  memcpy(&WifiCsi->ping_target, &addr, sizeof(ip_addr_t));

  char ip_str[40];
  ipaddr_ntoa_r(&addr, ip_str, sizeof(ip_str));
  AddLog(LOG_LEVEL_INFO, PSTR("CSI: Ping %s at %d Hz (%d ms)"), ip_str, hz, interval_ms);
  return true;
}

/*********************************************************************************************\
 * Module init
\*********************************************************************************************/

void WifiCsiModuleInit(void) {
  WifiCsi = (struct WifiCsi*)calloc(1, sizeof(struct WifiCsi));
  if (!WifiCsi) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Failed to allocate driver state"));
    return;
  }

  WifiCsi->enabled          = false;
  WifiCsi->channel          = 6;
  WifiCsi->router_mac_valid = false;
  WifiCsi->subcarrier_count = WIFI_CSI_HT20_SC;
  WifiCsi->fft_initialized  = false;
  WifiCsi->cir_baseline_valid    = false;
  WifiCsi->baseline_packet_count = 0;
  WifiCsi->warmup_counter        = 0;
  WifiCsi->last_packet_ms        = 0;
  WifiCsi->last_rssi             = 0;
  WifiCsi->last_activity_score   = 0.0f;
  WifiCsi->max_delay_bin         = WifiCsiMaxDelayBin(WIFI_CSI_HT20_SC);

  WifiCsi->history_index = 0;
  WifiCsi->history_count = 0;
  WifiCsi->variance      = 0.0f;

  WifiCsi->noise_floor_activity = 0.0f;
  WifiCsi->noise_floor_variance = 0.0f;
  WifiCsi->noise_floor_valid    = false;

  WifiCsi->motion_active     = false;
  WifiCsi->motion_holdoff_ms = 0;
  WifiCsi->holdoff_sec       = WIFI_CSI_DEFAULT_HOLDOFF_SEC;
  WifiCsi->movement_count    = 0;
  WifiCsi->activity_threshold = 1000.0f;

  WifiCsi->ping_handle = nullptr;
  WifiCsi->ping_hz     = 0;

  WifiCsi->ringbuf = xRingbufferCreate(WIFI_CSI_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
  if (!WifiCsi->ringbuf) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Failed to create ring buffer"));
    free(WifiCsi);
    WifiCsi = nullptr;
    return;
  }

  esp_err_t ret = dsps_fft2r_init_fc32(NULL, WIFI_CSI_FFT_SIZE);
  if (ret != ESP_OK) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: FFT init failed!"));
  } else {
    WifiCsi->fft_initialized = true;
  }

  AddLog(LOG_LEVEL_INFO, PSTR("CSI: Driver ready — threshold=%.1f holdoff=%.1fs"),
         WifiCsi->activity_threshold, WifiCsi->holdoff_sec);
}

/*********************************************************************************************\
 * Enable / Disable
\*********************************************************************************************/

static void WifiCsiResetRuntime() {
  memset(WifiCsi->fft_input,    0, sizeof(WifiCsi->fft_input));
  memset(WifiCsi->cir_profile,  0, sizeof(WifiCsi->cir_profile));
  memset(WifiCsi->cir_baseline, 0, sizeof(WifiCsi->cir_baseline));
  WifiCsi->cir_baseline_valid    = false;
  WifiCsi->baseline_packet_count = 0;
  WifiCsi->warmup_counter        = 0;
  WifiCsi->last_packet_ms        = 0;
  WifiCsi->last_activity_score   = 0.0f;
  WifiCsi->last_rssi             = 0;
  memset(WifiCsi->activity_history, 0, sizeof(WifiCsi->activity_history));
  WifiCsi->history_index = 0;
  WifiCsi->history_count = 0;
  WifiCsi->variance      = 0.0f;
  WifiCsi->noise_floor_activity = 0.0f;
  WifiCsi->noise_floor_variance = 0.0f;
  WifiCsi->noise_floor_valid    = false;
  WifiCsi->motion_active        = false;
  WifiCsi->motion_holdoff_ms    = 0;
  WifiCsi->movement_count       = 0;
  WifiCsi->packet_count         = 0;
  WifiCsi->packets_dropped      = 0;
}

void WifiCsiEnable(bool enable) {
  if (!WifiCsi) return;

  if (enable && !WifiCsi->enabled) {
    if (!WifiCsiGetRouterMac()) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Cannot enable — not connected to AP"));
      return;
    }

    wifi_csi_config_t csi_config;
    memset(&csi_config, 0, sizeof(csi_config));
#ifdef CONFIG_SOC_WIFI_HE_SUPPORT
    csi_config.enable             = 1;
    csi_config.acquire_csi_legacy = 1;
    csi_config.acquire_csi_ht20  = 1;
    csi_config.acquire_csi_ht40  = 1;
    csi_config.val_scale_cfg      = 0;
#else
    csi_config.lltf_en            = true;
    csi_config.htltf_en           = true;
    csi_config.stbc_htltf2_en     = true;
    csi_config.ltf_merge_en       = true;
    csi_config.channel_filter_en  = true;
    csi_config.manu_scale         = false;
    csi_config.shift              = 0;
#endif

    esp_err_t err = esp_wifi_set_csi_config(&csi_config);
    if (err != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Config failed 0x%x"), err); return; }

    err = esp_wifi_set_csi_rx_cb(&WifiCsiProcessPacket, NULL);
    if (err != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Callback failed 0x%x"), err); return; }

    err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Enable failed 0x%x"), err);
      esp_wifi_set_csi_rx_cb(NULL, NULL);
      return;
    }

    WifiCsi->enabled = true;
    WifiCsiResetRuntime();
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: Enabled"));

  } else if (!enable && WifiCsi->enabled) {
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    WifiCsi->enabled = false;
    WifiCsiStopPing();
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: Disabled"));
  }
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

void CmndCsiEnable(void) {
  if (XdrvMailbox.payload >= 0 && XdrvMailbox.payload <= 1)
    WifiCsiEnable(XdrvMailbox.payload);
  ResponseCmndNumber(WifiCsi->enabled);
}

void CmndCsiChannel(void) {
  if (XdrvMailbox.payload >= 1 && XdrvMailbox.payload <= 14)
    WifiCsi->channel = XdrvMailbox.payload;
  ResponseCmndNumber(WifiCsi->channel);
}

/*
 * CsiActivity [<threshold> [<holdoff_sec>]]
 *
 * With no arguments:  report current settings.
 * With one argument:  set enter-motion threshold.
 * With two arguments: set threshold AND holdoff in seconds.
 *
 * Examples:
 *   CsiActivity               → {"Threshold":1000.0,"HoldoffSec":3.0}
 *   CsiActivity 800           → set threshold=800
 *   CsiActivity 800 5.0       → set threshold=800, holdoff=5 s
 *
 * Suggestion for auto-calibration from Berry:
 *   threshold = noise_floor_activity * 5   (start around K=5, tune to room)
 */
void CmndCsiActivity(void) {
  if (XdrvMailbox.data_len > 0) {
    char buf[64];
    strlcpy(buf, XdrvMailbox.data, sizeof(buf));
    char* saveptr;
    char* tok = strtok_r(buf, " ", &saveptr);
    if (tok) {
      float thr = CharToFloat(tok);
      if (thr >= 1.0f) {
        WifiCsi->activity_threshold = thr;
        AddLog(LOG_LEVEL_INFO, PSTR("CSI: threshold=%.1f"), thr);
      }
    }
    tok = strtok_r(nullptr, " ", &saveptr);
    if (tok) {
      float hld = CharToFloat(tok);
      if (hld >= 0.5f && hld <= 3600.0f) {
        WifiCsi->holdoff_sec = hld;
        AddLog(LOG_LEVEL_INFO, PSTR("CSI: holdoff=%.1f s"), hld);
      }
    }
  }
  Response_P(PSTR("{\"Threshold\":%*_f,\"HoldoffSec\":%*_f}"),
             1, &WifiCsi->activity_threshold,
             1, &WifiCsi->holdoff_sec);
}

void CmndCsiStatus(void) {
  if (!WifiCsi) { ResponseCmndChar(PSTR("Not initialized")); return; }

  char mac_str[18] = "Unknown";
  if (WifiCsi->router_mac_valid)
    ToHex_P(WifiCsi->router_mac, 6, mac_str, 18, ':');

  char ping_ip[40] = "None";
  if (WifiCsi->ping_handle)
    ipaddr_ntoa_r(&WifiCsi->ping_target, ping_ip, sizeof(ping_ip));

  Response_P(
    PSTR("{\"Enabled\":%d,\"RouterMAC\":\"%s\",\"Packets\":%u,\"Dropped\":%u,"
         "\"RSSI\":%d,\"Activity\":%*_f,\"StdDev\":%*_f,\"Movements\":%u,"
         "\"Subcarriers\":%d,\"MaxDelayBin\":%d,"
         "\"Threshold\":%*_f,\"HoldoffSec\":%*_f,"
         "\"MotionActive\":%d,\"HoldoffMs\":%u,"
         "\"NoiseFloorActivity\":%*_f,\"NoiseFloorVariance\":%*_f,\"NoiseFloorValid\":%d,"
         "\"BaselineValid\":%d,\"BaselinePackets\":%u,\"Warmup\":%u,"
         "\"PingHz\":%d,\"PingTarget\":\"%s\"}"),
    WifiCsi->enabled, mac_str,
    WifiCsi->packet_count, WifiCsi->packets_dropped,
    WifiCsi->last_rssi,
    2, &WifiCsi->last_activity_score,
    2, &WifiCsi->variance,
    WifiCsi->movement_count,
    WifiCsi->subcarrier_count, WifiCsi->max_delay_bin,
    1, &WifiCsi->activity_threshold,
    1, &WifiCsi->holdoff_sec,
    WifiCsi->motion_active, WifiCsi->motion_holdoff_ms,
    2, &WifiCsi->noise_floor_activity,
    2, &WifiCsi->noise_floor_variance,
    WifiCsi->noise_floor_valid,
    WifiCsi->cir_baseline_valid, WifiCsi->baseline_packet_count, WifiCsi->warmup_counter,
    WifiCsi->ping_hz, ping_ip);

  ResponseJsonEnd();
}

/*
 * CsiPing<Hz> <IP>
 *   CsiPing0              → stop
 *   CsiPing10 192.168.1.1 → ping at 10 Hz
 *   CsiPing               → query
 */
void CmndCsiPing(void) {
  if (!WifiCsi) { ResponseCmndChar(PSTR("Not initialized")); return; }

  if (XdrvMailbox.data_len == 0 && XdrvMailbox.index == 0) {
    char ip[40] = "None";
    if (WifiCsi->ping_handle) ipaddr_ntoa_r(&WifiCsi->ping_target, ip, sizeof(ip));
    Response_P(PSTR("{\"CsiPing\":{\"Hz\":%d,\"Target\":\"%s\"}}"), WifiCsi->ping_hz, ip);
    return;
  }

  uint8_t hz = (uint8_t)XdrvMailbox.index;
  if (hz == 0 || XdrvMailbox.data_len == 0) {
    WifiCsiStopPing();
    Response_P(PSTR("{\"CsiPing\":\"Stopped\"}"));
    return;
  }

  char* ip = XdrvMailbox.data;
  while (*ip == ' ') ip++;

  if (WifiCsiStartPing(ip, hz)) {
    char ip_str[40];
    ipaddr_ntoa_r(&WifiCsi->ping_target, ip_str, sizeof(ip_str));
    Response_P(PSTR("{\"CsiPing\":{\"Hz\":%d,\"Target\":\"%s\",\"IntervalMs\":%d}}"),
               WifiCsi->ping_hz, ip_str, 1000 / WifiCsi->ping_hz);
  } else {
    Response_P(PSTR("{\"CsiPing\":\"Error\"}"));
  }
}

/*********************************************************************************************\
 * Sensor output
\*********************************************************************************************/

const char* WifiCsiGetState(void) {
  if (!WifiCsi->enabled)                                              return PSTR("Disabled");
  if (!WifiCsi->cir_baseline_valid || WifiCsi->warmup_counter < 100) return PSTR("Warmup");
  if (WifiCsi->motion_active)                                         return PSTR("Motion");
  return PSTR("None");
}

void WifiCsiShow(bool json) {
  if (!WifiCsi) return;

  const char* state = WifiCsiGetState();

  if (json) {
    ResponseAppend_P(
      PSTR(",\"CSI\":{\"State\":\"%s\",\"Activity\":%*_f,\"StdDev\":%*_f,"
           "\"NoiseAct\":%*_f,\"NoiseVar\":%*_f,\"RSSI\":%d}"),
      state,
      2, &WifiCsi->last_activity_score,
      2, &WifiCsi->variance,
      2, &WifiCsi->noise_floor_activity,
      2, &WifiCsi->noise_floor_variance,
      WifiCsi->last_rssi);

#ifdef USE_WEBSERVER
  } else {
    WSContentSend_PD(PSTR("{s}CSI State{m}%s{e}"), state);

    if (WifiCsi->cir_baseline_valid && WifiCsi->warmup_counter >= 100) {
      WSContentSend_PD(PSTR("{s}CSI Activity{m}%*_f{e}"),    2, &WifiCsi->last_activity_score);
      WSContentSend_PD(PSTR("{s}CSI Std Dev{m}%*_f{e}"),     2, &WifiCsi->variance);
      WSContentSend_PD(PSTR("{s}Noise Floor Act{m}%*_f{e}"), 2, &WifiCsi->noise_floor_activity);
      WSContentSend_PD(PSTR("{s}Noise Floor Var{m}%*_f{e}"), 2, &WifiCsi->noise_floor_variance);

      if (WifiCsi->motion_active) {
        uint32_t secs = WifiCsi->motion_holdoff_ms / 1000;
        WSContentSend_PD(PSTR("{s}CSI Holdoff{m}%u s{e}"), secs);
      }

      // CIR Histogram (bins 1 .. max_delay_bin)
      WSContentSend_PD(PSTR("{s}CIR Histogram{m}"));
      int top_bin = WifiCsi->max_delay_bin;
      if (top_bin >= WIFI_CSI_FFT_SIZE) top_bin = WIFI_CSI_FFT_SIZE - 1;

      float max_val = 0.001f;
      for (int i = 1; i <= top_bin; i++)
        if (WifiCsi->cir_profile[i] > max_val) max_val = WifiCsi->cir_profile[i];

      const char* blocks[] = {"", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
      for (int i = 1; i <= top_bin; i++) {
        int level = (int)((WifiCsi->cir_profile[i] / max_val) * 7.0f);
        if (level < 1) level = 1;
        if (level > 7) level = 7;
        WSContentSend_PD(PSTR("%s"), blocks[level]);
      }
      WSContentSend_PD(PSTR("{e}"));

      if (WifiCsi->ping_handle) {
        char ip_str[40];
        ipaddr_ntoa_r(&WifiCsi->ping_target, ip_str, sizeof(ip_str));
        WSContentSend_PD(PSTR("{s}CSI Ping{m}%s @ %d Hz{e}"), ip_str, WifiCsi->ping_hz);
      }
    }
#endif
  }
}

/*********************************************************************************************\
 * Driver interface
\*********************************************************************************************/

bool Xdrv93(uint32_t function) {
  bool result = false;

  if (FUNC_INIT == function) {
    WifiCsiModuleInit();
  } else if (WifiCsi) {
    switch (function) {
      case FUNC_COMMAND:     result = DecodeCommand(kCsiCommands, CsiCommand); break;
      case FUNC_JSON_APPEND: WifiCsiShow(true);  break;
#ifdef USE_WEBSERVER
      case FUNC_WEB_SENSOR:  WifiCsiShow(false); break;
#endif
      case FUNC_ACTIVE:      result = true;       break;
      case FUNC_LOOP:        WifiCsiProcessBuffer(); break;
    }
  }
  return result;
}

#endif  // USE_WIFI_CSI
#endif  // CONFIG_ESP_WIFI_CSI_ENABLED
#endif  // ESP32
