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
    - Two activity metrics are computed from the CIR difference against an adaptive baseline:

        activity_score  — "narrow" bins 1 .. max_delay_bin
                          Covers indoor multipath up to ~500 ns / ~150 m path length.
                          HT20: bins 1–12  (20 MHz → 50 ns/bin)
                          HT40: bins 1–22  (40 MHz → 25 ns/bin)
                          Best for fast motion detection (walking, arm gestures).
                          Split into near/far sub-bands:
                            activity_near — bins 1..near_delay_bin (HT20: 1-4, HT40: 1-6)
                              Close proximity, short multipath (~0-20m one-way).
                              High near + low far = someone close to the sensor.
                            activity_far  — bins (near_delay_bin+1)..max_delay_bin
                              Distant/subtle motion, longer reflections.
                              Normalised per-bin so magnitude is comparable to near.
                              High far + low near = distant movement or next-room.

        activity_wide   — bins 1 .. (num_sc/2 - 1), the full usable CIR half
                          This is every delay bin that carries real signal energy.
                          HT20: bins 1–27,  HT40: bins 1–56
                          Slower-changing, captures long-lag multipath changes
                          caused by subtle presence (breathing, posture shifts).
                          Per-bin-normalised so it is comparable in magnitude to narrow.

    - Fast smoothing (200 ms τ) on both narrow and wide scores removes per-packet noise.
    - Slow smoothing (~1 s τ) on the narrow score yields a rolling average (activity_avg)
      suitable for presence likelihood estimation and noise floor learning.
    - Std-dev is computed over the fast narrow score and remains sensitive to macro motion.

  Noise floor learning:
    - Uses activity_avg (1 s rolling average), NOT the raw fast score, so transient
      spikes during brief movements don't corrupt the floor.
    - Snap-down: accept any lower value immediately.
    - Slow rise: drift up at WIFI_CSI_NOISE_LEARN_RATE (~1000 packet time constant).
    - Learns continuously, even during motion (10x slower rate) to adapt to AP power
      or configuration changes that alter the baseline signal characteristics.
    - RSSI-scaled: when RSSI drifts, the noise floor is proportionally corrected
      (RSSI drop → more noise in normalized signal → floor scales up, and vice versa).
      This keeps the threshold meaningful across AP power/beamforming changes.
    - threshold = noise_floor_activity * K  (K ~ 3–10, tune empirically).

  Adaptive baseline:
    - Aggressive learning rates: 0.15/pkt (quiet), 0.05/pkt (motion).
    - Absorbs stationary objects in ~7s — acceptable trade-off for fast recovery
      when AP dynamically changes power, beamforming, or channel parameters.
    - AP config changes settle in 1–2s instead of 20+.

  Hysteresis / motion debounce:
    - Enter motion : 1s average > threshold
    - Exit motion  : 5s average < threshold / 2

  CsiPing / MAC filtering:
    - In 802.11 infrastructure mode ALL unicast frames received by the STA have the
      AP/BSSID as the 802.11 transmitter address (info->mac in the CSI callback),
      regardless of which IP-layer device sent the packet.
    - Pinging any IP on the LAN elicits ICMP replies that are forwarded by the AP;
      the CSI frame always carries the router's BSSID.
    - Therefore CSI is always filtered to router_mac.  The ping IP only controls the
      cadence and the round-trip path — it does NOT affect MAC filtering.
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
#include "ping/ping_sock.h"
#include "lwip/inet.h"

#define XDRV_93 93

// ---- Tunable constants ----
#define WIFI_CSI_RINGBUF_SIZE         (4 * 1024)
#define WIFI_CSI_MAX_SUBCARRIERS      114
#define WIFI_CSI_MAX_PACKET_SIZE      (sizeof(wifi_csi_packet_header_t) + 512)
#define WIFI_CSI_FFT_SIZE             128

#define WIFI_CSI_HT20_SC              56
#define WIFI_CSI_HT40_SC              114

// Fast smoothing: 200 ms time-constant (per-packet noise removal)
#define WIFI_CSI_SMOOTH_TAU_SEC       0.200f

// Slow smoothing: ~1000 ms rolling average (presence likelihood / noise floor)
#define WIFI_CSI_AVG_TAU_SEC          1.000f

// Very slow smoothing: ~5000 ms rolling average (long-term presence tracking)
#define WIFI_CSI_AVG5_TAU_SEC         5.000f

// Noise floor EMA rise-rate (≈ 1 "forget" per 100 quiet packets)
#define WIFI_CSI_NOISE_LEARN_RATE     0.01f

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
  int   near_delay_bin;   // near/far split within narrow band
  int   max_delay_bin;    // narrow bin limit: ~500 ns indoor multipath
  int   wide_delay_bin;   // wide bin limit:   full usable CIR half (num_sc/2 - 1)

  // ---- Smoothing ----
  uint32_t last_packet_ms;

  // ---- Warmup ----
  uint32_t warmup_counter;

  // ---- Activity metrics ----
  float last_activity_score;  // narrow fast EMA (200ms τ) — motion trigger + variance
  float last_activity_near;   // near-band fast EMA (200ms τ) — close proximity
  float last_activity_far;    // far-band fast EMA (200ms τ) — distant/subtle
  float activity_avg;         // narrow slow EMA (1s τ)   — presence likelihood
  bool  activity_avg_valid;
  float activity_avg5;        // narrow very slow EMA (5s τ) — long-term presence tracking
  bool  activity_avg5_valid;
  float last_activity_wide;   // wide fast EMA  (200ms τ) — subtle presence
  int8_t last_rssi;
  int8_t last_noise_floor;
  float activity_threshold;   // enter-motion threshold (user settable)

  // ---- Statistics (sliding window of last 10 fast narrow scores) ----
  float   activity_history[10];
  uint8_t history_index;
  uint8_t history_count;
  float   variance;              // std-dev — magnitude of fluctuation
  float   skewness;              // >0 = spiking up, <0 = spiking down
  float   kurtosis;              // >0 = sharp rare spikes (real motion), <0 = uniform spread (drift/AP)

  // ---- RSSI tracking (slow EMA for noise floor scaling) ----
  float rssi_ema;
  bool  rssi_ema_valid;

  // ---- Noise floor learning (uses activity_avg, quiet periods only) ----
  float noise_floor_activity;
  float noise_floor_variance;
  bool  noise_floor_valid;

  // ---- Hysteresis / motion debounce ----
  bool     motion_active;
  uint32_t movement_count;

  // ---- Ping ----
  esp_ping_handle_t ping_handle;
  ip_addr_t         ping_target;
  uint8_t           ping_hz;

} *WifiCsi = nullptr;

/*********************************************************************************************\
 * Helpers
\*********************************************************************************************/

int WifiCsiDeriveSubcarrierCount(uint16_t data_len) {
  int num_sc = (int)data_len / (int)CSI_BYTES_PER_SC;
  if (num_sc > WIFI_CSI_MAX_SUBCARRIERS) num_sc = WIFI_CSI_MAX_SUBCARRIERS;
  if (num_sc < 0) num_sc = 0;
  return num_sc;
}

// Near/far split within narrow band:
// HT20: bins 1-4 = near (~0-60m round-trip), bins 5-12 = far
// HT40: bins 1-6 = near (~0-45m round-trip), bins 7-22 = far
int WifiCsiNearBin(int num_sc) {
  return (num_sc >= WIFI_CSI_HT40_SC) ? 6 : 4;
}

// Narrow bin limit: indoor multipath up to ~500 ns
// HT20: 20 MHz → 50 ns/bin → 10 bins + 2 guard = 12
// HT40: 40 MHz → 25 ns/bin → 20 bins + 2 guard = 22
int WifiCsiMaxDelayBin(int num_sc) {
  return (num_sc >= WIFI_CSI_HT40_SC) ? 22 : 12;
}

// Wide bin limit: every CIR bin that carries real signal energy.
// The symmetric IFFT of num_sc subcarriers has meaningful content in bins 1..(num_sc/2 - 1).
// Bins beyond that are zero-padded and contain only sidelobes / noise.
// Capped at FFT_SIZE/2 - 1 = 63.
int WifiCsiWideBin(int num_sc) {
  int wide = num_sc / 2 - 1;
  if (wide < 1) wide = 1;
  if (wide > WIFI_CSI_FFT_SIZE / 2 - 1) wide = WIFI_CSI_FFT_SIZE / 2 - 1;
  return wide;
}

/*********************************************************************************************\
 * Statistics tracker (sliding window of last 10 fast narrow scores)
 * Computes std-dev, skewness, and excess kurtosis in a single pass.
\*********************************************************************************************/

void WifiCsiUpdateStatistics(float new_score) {
  if (!WifiCsi) return;

  WifiCsi->activity_history[WifiCsi->history_index] = new_score;
  WifiCsi->history_index = (WifiCsi->history_index + 1) % 10;
  if (WifiCsi->history_count < 10) WifiCsi->history_count++;

  uint8_t n = WifiCsi->history_count;

  float sum = 0.0f;
  for (uint8_t i = 0; i < n; i++) sum += WifiCsi->activity_history[i];
  float mean = sum / n;

  float m2 = 0.0f, m3 = 0.0f, m4 = 0.0f;
  for (uint8_t i = 0; i < n; i++) {
    float d  = WifiCsi->activity_history[i] - mean;
    float d2 = d * d;
    m2 += d2;
    m3 += d2 * d;
    m4 += d2 * d2;
  }
  m2 /= n;
  m3 /= n;
  m4 /= n;

  WifiCsi->variance = sqrtf(m2);

  if (m2 > 0.0001f) {
    float sigma3 = m2 * WifiCsi->variance;   // m2^(3/2)
    WifiCsi->skewness = m3 / sigma3;
    WifiCsi->kurtosis = (m4 / (m2 * m2)) - 3.0f;  // excess kurtosis (normal = 0)
  } else {
    WifiCsi->skewness = 0.0f;
    WifiCsi->kurtosis = 0.0f;
  }
}

/*********************************************************************************************\
 * Noise floor learning (uses 1 s rolling average, learns continuously)
\*********************************************************************************************/

void WifiCsiUpdateNoiseFloor(float activity_avg, float variance, float rssi_correction) {
  if (!WifiCsi) return;
  if (!WifiCsi->activity_avg_valid) return;   // wait for slow EMA to warm up

  if (!WifiCsi->noise_floor_valid) {
    WifiCsi->noise_floor_activity = activity_avg;
    WifiCsi->noise_floor_variance = variance;
    WifiCsi->noise_floor_valid    = true;
    return;
  }

  // RSSI-scaled noise floor correction (#4):
  // When RSSI drifts, the normalized signal's noise level changes proportionally.
  // Scale existing noise floor before learning so threshold stays meaningful.
  // rssi_correction = 10^((old_rssi_ema - new_rssi_ema) / 20)
  //   RSSI drops → correction > 1 → noise floor rises (more noise in normalized signal)
  //   RSSI rises → correction < 1 → noise floor drops
  if (rssi_correction != 1.0f) {
    WifiCsi->noise_floor_activity *= rssi_correction;
    WifiCsi->noise_floor_variance *= rssi_correction;
  }

  // Use slower learning during motion to avoid chasing transients,
  // but still allow adaptation to environmental changes (AP power, config changes)
  float learn_rate = WifiCsi->motion_active ? (WIFI_CSI_NOISE_LEARN_RATE * 0.1f) : WIFI_CSI_NOISE_LEARN_RATE;

  // Snap-down / slow-rise
  if (activity_avg < WifiCsi->noise_floor_activity) {
    WifiCsi->noise_floor_activity = activity_avg;
  } else {
    WifiCsi->noise_floor_activity =
        (1.0f - learn_rate) * WifiCsi->noise_floor_activity +
        learn_rate * activity_avg;
  }

  if (variance < WifiCsi->noise_floor_variance) {
    WifiCsi->noise_floor_variance = variance;
  } else {
    WifiCsi->noise_floor_variance =
        (1.0f - learn_rate) * WifiCsi->noise_floor_variance +
        learn_rate * variance;
  }
}

/*********************************************************************************************\
 * Hysteresis / motion debounce state machine
 * Enter motion: 1s average > threshold
 * Exit motion:  5s average < threshold / 2
\*********************************************************************************************/

void WifiCsiUpdateMotionState(float activity_1s, float activity_5s) {
  if (!WifiCsi) return;
  if (!WifiCsi->activity_avg5_valid) return;  // wait for 5s average to be valid

  float threshold_enter = WifiCsi->activity_threshold;
  float threshold_exit  = threshold_enter / 2.0f;

  if (!WifiCsi->motion_active) {
    if (activity_1s > threshold_enter) {
      WifiCsi->motion_active = true;
      WifiCsi->movement_count++;
    }
  } else {
    if (activity_5s < threshold_exit) {
      WifiCsi->motion_active = false;
    }
  }
}

/*********************************************************************************************\
 * CIR Processing Pipeline
 *
 * Subcarrier placement (DC-centered):
 *   ESP layout: [0 .. half-1] = negative SCs, [half .. num_sc-1] = positive SCs
 *   FFT bin mapping (N=128):
 *     positive SC i → bin i+1            (bins 1 .. half)
 *     negative SC i → bin N-half+i       (bins N-half .. N-1)
 *     DC bin 0 = 0  (no DC subcarrier in 802.11)
 *
 * After IFFT:
 *   bin 0              = direct LOS path (excluded from both metrics)
 *   bins 1..max_delay  = near indoor multipath  → narrow activity
 *   bins 1..wide_delay = all usable multipath   → wide activity
 *   bins > wide_delay  = zero-padded, no real signal
 *
 * Rate-adaptive alpha:  α = 1 − exp(−Δt / τ)
 *   α_fast  τ = 200 ms   → last_activity_score, last_activity_wide, variance
 *   α_slow  τ = 1000 ms  → activity_avg (narrow only)
\*********************************************************************************************/

void WifiCsiProcessPipeline(void* csi_data_ptr, int8_t rssi, int num_sc) {
  if (!WifiCsi || !WifiCsi->fft_initialized || num_sc < 4) return;

  WifiCsi->warmup_counter++;

  // ---- Rate-adaptive alphas ----
  uint32_t now_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  uint32_t elapsed = (WifiCsi->last_packet_ms == 0) ? 20 : (now_ms - WifiCsi->last_packet_ms);
  if (elapsed == 0)  elapsed = 1;
  if (elapsed > 500) elapsed = 500;
  WifiCsi->last_packet_ms = now_ms;

  float dt         = (float)elapsed;
  float alpha_fast = 1.0f - expf(-dt / (WIFI_CSI_SMOOTH_TAU_SEC * 1000.0f));
  float alpha_slow = 1.0f - expf(-dt / (WIFI_CSI_AVG_TAU_SEC   * 1000.0f));
  float alpha_avg5 = 1.0f - expf(-dt / (WIFI_CSI_AVG5_TAU_SEC  * 1000.0f));

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
  int half = num_sc / 2;

  for (int i = 0; i < half; i++) {
    int data_idx = half + i;             // positive SCs
    int fft_bin  = i + 1;
    float w = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (half - 1)));
    WifiCsi->fft_input[fft_bin * 2 + 0] =  (float)iq_data[data_idx * 2]     * scale * w;
    WifiCsi->fft_input[fft_bin * 2 + 1] = -(float)iq_data[data_idx * 2 + 1] * scale * w;
  }

  for (int i = 0; i < half; i++) {
    int data_idx = i;                    // negative SCs → upper FFT half
    int fft_bin  = WIFI_CSI_FFT_SIZE - half + i;
    float w = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (half - 1)));
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
    WifiCsi->near_delay_bin        = WifiCsiNearBin(num_sc);
    WifiCsi->max_delay_bin         = WifiCsiMaxDelayBin(num_sc);
    WifiCsi->wide_delay_bin        = WifiCsiWideBin(num_sc);
    AddLog(LOG_LEVEL_INFO,
           PSTR("CSI: Baseline set — near=1..%d  far=%d..%d  wide=1..%d  (num_sc=%d)"),
           WifiCsi->near_delay_bin, WifiCsi->near_delay_bin + 1,
           WifiCsi->max_delay_bin, WifiCsi->wide_delay_bin, num_sc);
    return;
  }

  // Keep bin limits in sync in case bandwidth changes mid-session
  WifiCsi->near_delay_bin = WifiCsiNearBin(num_sc);
  WifiCsi->max_delay_bin  = WifiCsiMaxDelayBin(num_sc);
  WifiCsi->wide_delay_bin = WifiCsiWideBin(num_sc);

  // ---- Near/far activity split within narrow band ----
  // Near: bins 1..near_delay_bin  — close proximity (~0-20m one-way)
  // Far:  bins (near_delay_bin+1)..max_delay_bin — distant/subtle motion
  float near_energy = 0.0f;
  float far_energy  = 0.0f;
  for (int i = 1; i <= WifiCsi->max_delay_bin; i++) {
    float diff = WifiCsi->cir_profile[i] - WifiCsi->cir_baseline[i];
    float e = diff * diff;
    if (i <= WifiCsi->near_delay_bin)
      near_energy += e;
    else
      far_energy += e;
  }
  float narrow_energy = near_energy + far_energy;
  float narrow_score = narrow_energy * 1000.0f;
  float near_score   = near_energy * 1000.0f;
  int far_bins = WifiCsi->max_delay_bin - WifiCsi->near_delay_bin;
  float far_score = (far_bins > 0) ? (far_energy / (float)far_bins) * 1000.0f : 0.0f;

  // ---- Wide activity (bins 1..wide_delay_bin) — subtle presence ----
  // Normalised by bin count so its magnitude is comparable to narrow.
  float wide_energy = 0.0f;
  for (int i = 1; i <= WifiCsi->wide_delay_bin; i++) {
    float diff = WifiCsi->cir_profile[i] - WifiCsi->cir_baseline[i];
    wide_energy += diff * diff;
  }
  float wide_score = (wide_energy / (float)WifiCsi->wide_delay_bin) * 1000.0f;

  // ---- Fast EMA (200 ms τ) for all metrics ----
  WifiCsi->last_activity_score =
      (1.0f - alpha_fast) * WifiCsi->last_activity_score + alpha_fast * narrow_score;
  WifiCsi->last_activity_near =
      (1.0f - alpha_fast) * WifiCsi->last_activity_near  + alpha_fast * near_score;
  WifiCsi->last_activity_far =
      (1.0f - alpha_fast) * WifiCsi->last_activity_far   + alpha_fast * far_score;
  WifiCsi->last_activity_wide =
      (1.0f - alpha_fast) * WifiCsi->last_activity_wide  + alpha_fast * wide_score;

  // ---- Slow EMA (~1 s τ) — rolling average of narrow score ----
  if (!WifiCsi->activity_avg_valid) {
    WifiCsi->activity_avg       = WifiCsi->last_activity_score;
    WifiCsi->activity_avg_valid = true;
  } else {
    WifiCsi->activity_avg =
        (1.0f - alpha_slow) * WifiCsi->activity_avg + alpha_slow * WifiCsi->last_activity_score;
  }

  // ---- Very slow EMA (~5 s τ) — long-term presence tracking ----
  if (!WifiCsi->activity_avg5_valid) {
    WifiCsi->activity_avg5       = WifiCsi->last_activity_score;
    WifiCsi->activity_avg5_valid = true;
  } else {
    WifiCsi->activity_avg5 =
        (1.0f - alpha_avg5) * WifiCsi->activity_avg5 + alpha_avg5 * WifiCsi->last_activity_score;
  }

  // ---- Statistics of recent fast narrow scores (std-dev, skewness, kurtosis) ----
  WifiCsiUpdateStatistics(WifiCsi->last_activity_score);

  // ---- Hysteresis / motion debounce (1s avg to enter, 5s avg to exit) ----
  WifiCsiUpdateMotionState(WifiCsi->activity_avg, WifiCsi->activity_avg5);

  // ---- RSSI tracking (slow EMA, ~2s τ) and noise floor correction ----
  float rssi_correction = 1.0f;
  float rssi_f = (float)rssi;
  if (!WifiCsi->rssi_ema_valid) {
    WifiCsi->rssi_ema       = rssi_f;
    WifiCsi->rssi_ema_valid = true;
  } else {
    float alpha_rssi = 1.0f - expf(-dt / 2000.0f);  // ~2s τ
    float old_rssi_ema = WifiCsi->rssi_ema;
    WifiCsi->rssi_ema = (1.0f - alpha_rssi) * WifiCsi->rssi_ema + alpha_rssi * rssi_f;
    float delta_rssi = old_rssi_ema - WifiCsi->rssi_ema;
    if (fabsf(delta_rssi) > 0.01f) {
      rssi_correction = powf(10.0f, delta_rssi / 20.0f);
    }
  }

  // ---- Noise floor learning (1 s average, RSSI-corrected, learns continuously) ----
  WifiCsiUpdateNoiseFloor(WifiCsi->activity_avg, WifiCsi->variance, rssi_correction);

  // ---- Adaptive baseline — aggressive learning (#2) ----
  // 0.15 quiet / 0.05 motion: absorbs stationary objects in ~7s,
  // but AP parameter changes settle in 1-2s instead of 20+
  float learn_rate = WifiCsi->motion_active ? 0.05f : 0.15f;
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
           PSTR("CSI: narrow=%.2f near=%.2f far=%.2f avg=%.2f wide=%.2f var=%.2f skew=%.2f kurt=%.2f nf=%.2f motion=%d"),
           WifiCsi->last_activity_score, WifiCsi->last_activity_near, WifiCsi->last_activity_far,
           WifiCsi->activity_avg, WifiCsi->last_activity_wide,
           WifiCsi->variance, WifiCsi->skewness, WifiCsi->kurtosis,
           WifiCsi->noise_floor_activity, WifiCsi->motion_active);
  }

  WifiCsi->last_rssi = rssi;
}

/*********************************************************************************************\
 * WiFi CSI packet callback
 *
 * In 802.11 infrastructure mode info->mac is always the AP/BSSID regardless of the
 * IP-layer source.  We always filter to router_mac — see design notes above.
\*********************************************************************************************/

static void WifiCsiProcessPacket(void* ctx, wifi_csi_info_t* info) {
  if (!WifiCsi || !WifiCsi->enabled || !info || !info->buf) return;
  if (info->len < 4) {
    portENTER_CRITICAL(&wifi_csi_mux);
    WifiCsi->packets_dropped++;
    portEXIT_CRITICAL(&wifi_csi_mux);
    return;
  }

  // Always filter to router MAC (see design notes)
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
 * Ring buffer drain (up to 3 packets per FUNC_LOOP call)
\*********************************************************************************************/

void WifiCsiProcessBuffer() {
  if (!WifiCsi || !WifiCsi->enabled) return;

  for (int i = 0; i < 3; i++) {
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
  AddLog(LOG_LEVEL_INFO,
         PSTR("CSI: Ping %s at %d Hz (%d ms) — CSI filtered to router MAC"),
         ip_str, hz, interval_ms);
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
  WifiCsi->last_activity_near    = 0.0f;
  WifiCsi->last_activity_far     = 0.0f;
  WifiCsi->activity_avg          = 0.0f;
  WifiCsi->activity_avg_valid    = false;
  WifiCsi->last_activity_wide    = 0.0f;
  WifiCsi->near_delay_bin        = WifiCsiNearBin(WIFI_CSI_HT20_SC);
  WifiCsi->max_delay_bin         = WifiCsiMaxDelayBin(WIFI_CSI_HT20_SC);
  WifiCsi->wide_delay_bin        = WifiCsiWideBin(WIFI_CSI_HT20_SC);

  WifiCsi->history_index = 0;
  WifiCsi->history_count = 0;
  WifiCsi->variance      = 0.0f;
  WifiCsi->skewness      = 0.0f;
  WifiCsi->kurtosis      = 0.0f;

  WifiCsi->rssi_ema       = 0.0f;
  WifiCsi->rssi_ema_valid = false;
  WifiCsi->noise_floor_activity = 0.0f;
  WifiCsi->noise_floor_variance = 0.0f;
  WifiCsi->noise_floor_valid    = false;

  WifiCsi->motion_active     = false;
  WifiCsi->movement_count    = 0;
  WifiCsi->activity_threshold = 2000.0f;

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

  AddLog(LOG_LEVEL_INFO, PSTR("CSI: Driver ready — threshold=%.1f (exit at %.1f)"),
         WifiCsi->activity_threshold, WifiCsi->activity_threshold / 2.0f);
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
  WifiCsi->last_activity_near    = 0.0f;
  WifiCsi->last_activity_far     = 0.0f;
  WifiCsi->activity_avg          = 0.0f;
  WifiCsi->activity_avg_valid    = false;
  WifiCsi->activity_avg5         = 0.0f;
  WifiCsi->activity_avg5_valid   = false;
  WifiCsi->last_activity_wide    = 0.0f;
  WifiCsi->last_rssi             = 0;
  memset(WifiCsi->activity_history, 0, sizeof(WifiCsi->activity_history));
  WifiCsi->history_index = 0;
  WifiCsi->history_count = 0;
  WifiCsi->variance      = 0.0f;
  WifiCsi->skewness      = 0.0f;
  WifiCsi->kurtosis      = 0.0f;
  WifiCsi->rssi_ema             = 0.0f;
  WifiCsi->rssi_ema_valid       = false;
  WifiCsi->noise_floor_activity = 0.0f;
  WifiCsi->noise_floor_variance = 0.0f;
  WifiCsi->noise_floor_valid    = false;
  WifiCsi->motion_active        = false;
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
 * CsiActivity [<threshold>]
 *   CsiActivity               → {"Threshold":2000.0}
 *   CsiActivity 1500          → set enter-motion threshold
 *
 * Hysteresis: Enter when 1s avg > threshold, exit when 5s avg < threshold/2
 *
 * Auto-calibration hint (Berry):
 *   threshold = noise_floor_activity * K    (start K=5, tune to room)
 */
void CmndCsiActivity(void) {
  if (XdrvMailbox.data_len > 0) {
    float thr = CharToFloat(XdrvMailbox.data);
    if (thr >= 1.0f) {
      WifiCsi->activity_threshold = thr;
      AddLog(LOG_LEVEL_INFO, PSTR("CSI: threshold=%.1f (exit at %.1f)"), thr, thr / 2.0f);
    }
  }
  Response_P(PSTR("{\"Threshold\":%*_f}"),
             1, &WifiCsi->activity_threshold);
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
         "\"RSSI\":%d,\"NoiseFloor\":%d,"
         "\"Activity\":%*_f,\"ActivityNear\":%*_f,\"ActivityFar\":%*_f,"
         "\"ActivityAvg\":%*_f,\"ActivityAvg5\":%*_f,\"ActivityWide\":%*_f,"
         "\"StdDev\":%*_f,\"Skewness\":%*_f,\"Kurtosis\":%*_f,\"Movements\":%u,"
         "\"Subcarriers\":%d,\"NearBins\":%d,\"NarrowBins\":%d,\"WideBins\":%d,"
         "\"Threshold\":%*_f,"
         "\"MotionActive\":%d,"
         "\"NoiseFloorActivity\":%*_f,\"NoiseFloorVariance\":%*_f,\"NoiseFloorValid\":%d,"
         "\"BaselineValid\":%d,\"BaselinePackets\":%u,\"Warmup\":%u,"
         "\"PingHz\":%d,\"PingTarget\":\"%s\"}"),
    WifiCsi->enabled, mac_str,
    WifiCsi->packet_count, WifiCsi->packets_dropped,
    WifiCsi->last_rssi, WifiCsi->last_noise_floor,
    2, &WifiCsi->last_activity_score,
    2, &WifiCsi->last_activity_near,
    2, &WifiCsi->last_activity_far,
    2, &WifiCsi->activity_avg,
    2, &WifiCsi->activity_avg5,
    2, &WifiCsi->last_activity_wide,
    2, &WifiCsi->variance,
    2, &WifiCsi->skewness,
    2, &WifiCsi->kurtosis,
    WifiCsi->movement_count,
    WifiCsi->subcarrier_count, WifiCsi->near_delay_bin, WifiCsi->max_delay_bin, WifiCsi->wide_delay_bin,
    1, &WifiCsi->activity_threshold,
    WifiCsi->motion_active,
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
 *   CsiPing10 192.168.1.1 → ping router at 10 Hz to generate CSI frames
 *   CsiPing               → query current session
 *
 * Note: CSI is always filtered to the router MAC regardless of ping target IP.
 * Pinging the router (gateway IP) is the most common and reliable choice.
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
      PSTR(",\"CSI\":{\"State\":\"%s\","
           "\"Activity\":%*_f,\"Near\":%*_f,\"Far\":%*_f,"
           "\"ActivityAvg\":%*_f,\"ActivityAvg5\":%*_f,\"ActivityWide\":%*_f,"
           "\"StdDev\":%*_f,\"Skew\":%*_f,\"Kurt\":%*_f,"
           "\"NoiseAct\":%*_f,\"NoiseVar\":%*_f,\"RSSI\":%d,\"NoiseFloor\":%d,\"Events\":%u}"),
      state,
      2, &WifiCsi->last_activity_score,
      2, &WifiCsi->last_activity_near,
      2, &WifiCsi->last_activity_far,
      2, &WifiCsi->activity_avg,
      2, &WifiCsi->activity_avg5,
      2, &WifiCsi->last_activity_wide,
      2, &WifiCsi->variance,
      2, &WifiCsi->skewness,
      2, &WifiCsi->kurtosis,
      2, &WifiCsi->noise_floor_activity,
      2, &WifiCsi->noise_floor_variance,
      WifiCsi->last_rssi,
      WifiCsi->last_noise_floor,
      WifiCsi->movement_count);

#ifdef USE_WEBSERVER
  } else {
    WSContentSend_PD(PSTR("{s}CSI State{m}%s (events: %u){e}"), state, WifiCsi->movement_count);

    if (WifiCsi->cir_baseline_valid && WifiCsi->warmup_counter >= 100) {
      WSContentSend_PD(PSTR("{s}CSI RSSI{m}%d dBm{e}"), WifiCsi->last_rssi);
      WSContentSend_PD(PSTR("{s}CSI Noise Floor{m}%d dBm{e}"), WifiCsi->last_noise_floor);
      WSContentSend_PD(PSTR("{s}CSI Activity (narrow){m}%*_f{e}"), 2, &WifiCsi->last_activity_score);
      WSContentSend_PD(PSTR("{s}CSI Activity (near){m}%*_f{e}"),   2, &WifiCsi->last_activity_near);
      WSContentSend_PD(PSTR("{s}CSI Activity (far){m}%*_f{e}"),    2, &WifiCsi->last_activity_far);
      WSContentSend_PD(PSTR("{s}CSI Activity (1s avg){m}%*_f{e}"), 2, &WifiCsi->activity_avg);
      WSContentSend_PD(PSTR("{s}CSI Activity (5s avg){m}%*_f{e}"), 2, &WifiCsi->activity_avg5);
      WSContentSend_PD(PSTR("{s}CSI Activity (wide){m}%*_f{e}"),   2, &WifiCsi->last_activity_wide);
      WSContentSend_PD(PSTR("{s}CSI Std Dev{m}%*_f{e}"),           2, &WifiCsi->variance);
      WSContentSend_PD(PSTR("{s}CSI Skewness{m}%*_f{e}"),          2, &WifiCsi->skewness);
      WSContentSend_PD(PSTR("{s}CSI Kurtosis{m}%*_f{e}"),          2, &WifiCsi->kurtosis);
      WSContentSend_PD(PSTR("{s}Noise Floor Act{m}%*_f{e}"),       2, &WifiCsi->noise_floor_activity);
      WSContentSend_PD(PSTR("{s}Noise Floor Var{m}%*_f{e}"),       2, &WifiCsi->noise_floor_variance);

      // CIR Histogram: show left 16 bins (near-field multipath)
      WSContentSend_PD(PSTR("{s}CIR Histogram{m}"));
      int top_bin = 16;
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
