/*
  xdrv_93_esp32_wifi_csi.ino - ESP32 WiFi CSI driver for Tasmota (Simplified Single-Source)

  Copyright (C) 2025

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
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
#include <math.h>

#define XDRV_93 93

// CSI Configuration
#define WIFI_CSI_RINGBUF_SIZE (4 * 1024)      // 4KB ring buffer (reduced)
#define WIFI_CSI_MAX_SUBCARRIERS 114          // Maximum for HT40
#define WIFI_CSI_MAX_PACKET_SIZE (sizeof(wifi_csi_packet_header_t) + 512)

/*********************************************************************************************\
 * Per-target feature detection
\*********************************************************************************************/

#ifdef CONFIG_SOC_WIFI_HE_SUPPORT
  // HE-capable chips (ESP32-C6, C5, C61)
  #define RX_CTRL_RSSI(info) (info->rx_ctrl.rssi)
  #define RX_CTRL_MODE(info) (info->rx_ctrl.cur_bb_format)
  #define RX_CTRL_NOISE_FLOOR(info) (info->rx_ctrl.noise_floor)
  #define CSI_DATA_TYPE int16_t
  #define CSI_BYTES_PER_SC (2 * sizeof(int16_t))
#else
  // Non-HE chips (ESP32, S2, S3, C3, C2)
  #define RX_CTRL_RSSI(info) (info->rx_ctrl.rssi)
  #define RX_CTRL_MODE(info) (info->rx_ctrl.sig_mode)
  #define RX_CTRL_NOISE_FLOOR(info) (info->rx_ctrl.noise_floor)
  #define CSI_DATA_TYPE int8_t
  #define CSI_BYTES_PER_SC (2 * sizeof(int8_t))
#endif

// first_word_invalid hardware bug only on ESP32/S2/S3
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
  #define CSI_HAS_FIRST_WORD_BUG 1
#else
  #define CSI_HAS_FIRST_WORD_BUG 0
#endif

// Binary packet header (20 bytes)
typedef struct __attribute__((packed)) {
  uint32_t timestamp;
  uint8_t  mac[6];
  int8_t   rssi;
  int8_t   noise_floor;
  uint16_t data_len;
} wifi_csi_packet_header_t;

#define WIFI_CSI_HEADER_SIZE sizeof(wifi_csi_packet_header_t)



const char kCsiCommands[] PROGMEM = "Csi|Enable|Channel|Activity|Status";

void (* const CsiCommand[])(void) PROGMEM = {
  &CmndCsiEnable,
  &CmndCsiChannel,
  &CmndCsiActivity,
  &CmndCsiStatus
};

static portMUX_TYPE wifi_csi_mux = portMUX_INITIALIZER_UNLOCKED;

struct WifiCsi {
  bool enabled;
  uint8_t channel;
  uint8_t router_mac[6];           // MAC of connected AP (auto-detected)
  bool router_mac_valid;
  
  volatile uint32_t packet_count;
  volatile uint32_t packets_dropped;
  
  RingbufHandle_t ringbuf;
  int subcarrier_count;
  
  // CIR (Channel Impulse Response) based motion detection
  float fft_input[256];            // 128 complex samples (Real, Imag interleaved)
  float cir_profile[128];          // Magnitude of Channel Impulse Response
  float cir_baseline[128];         // Adaptive baseline of CIR
  bool cir_baseline_valid;         // Whether CIR baseline is valid
  uint32_t baseline_packet_count;  // Number of packets processed for baseline
  bool fft_initialized;            // Whether FFT tables are initialized
  
  // Warmup period
  uint32_t warmup_counter;         // Count of packets during warmup period
  
  // Statistics
  float last_activity_score;
  int8_t last_rssi;
  int8_t last_noise_floor;
  uint32_t movement_count;
  
  float activity_threshold;
  
  // Standard deviation calculation (sliding window of last 10 samples)
  float activity_history[10];      // Circular buffer for last 10 activity scores
  uint8_t history_index;           // Current index in circular buffer
  uint8_t history_count;           // Number of valid entries in history
  float variance;                  // Current standard deviation value (sqrt of variance)
} *WifiCsi = nullptr;

/*********************************************************************************************\
 * CSI Data Processing
\*********************************************************************************************/

int WifiCsiDeriveSubcarrierCount(uint16_t data_len) {
  int num_sc = data_len / CSI_BYTES_PER_SC;
  if (num_sc > WIFI_CSI_MAX_SUBCARRIERS) num_sc = WIFI_CSI_MAX_SUBCARRIERS;
  if (num_sc < 0) num_sc = 0;
  return num_sc;
}



// Helper function to calculate standard deviation of activity scores
void WifiCsiUpdateVariance(float new_activity_score) {
  if (!WifiCsi) return;
  
  // Add new score to circular buffer
  WifiCsi->activity_history[WifiCsi->history_index] = new_activity_score;
  WifiCsi->history_index = (WifiCsi->history_index + 1) % 10;
  
  // Update count (max 10)
  if (WifiCsi->history_count < 10) {
    WifiCsi->history_count++;
  }
  
  // Calculate mean
  float sum = 0.0f;
  for (uint8_t i = 0; i < WifiCsi->history_count; i++) {
    sum += WifiCsi->activity_history[i];
  }
  float mean = sum / WifiCsi->history_count;
  
  // Calculate variance (mean of squared differences)
  float variance_sum = 0.0f;
  for (uint8_t i = 0; i < WifiCsi->history_count; i++) {
    float diff = WifiCsi->activity_history[i] - mean;
    variance_sum += diff * diff;
  }
  // Store standard deviation instead of variance (sqrt of variance)
  // This gives values in the same units as activity_score
  float variance = variance_sum / WifiCsi->history_count;
  WifiCsi->variance = sqrtf(variance);
}

// --- New Processing Pipeline using CIR (Channel Impulse Response) ---
void WifiCsiProcessPipeline(void* csi_data_ptr, int8_t rssi, int num_sc) {
  if (!WifiCsi || !WifiCsi->fft_initialized || num_sc <= 0) return;
  
  // Increment warmup counter
  WifiCsi->warmup_counter++;
  
  // --- 1. Prepare Data for IFFT (Pad to 128) ---
  // Clear buffer
  memset(WifiCsi->fft_input, 0, 128 * 2 * sizeof(float));
  
  // Copy subcarriers to FFT buffer (Apply Hanning Window to reduce sidelobes)
  // We map 56/114 subcarriers to the first N bins.
  // Ideally we should map to -N/2 .. +N/2, but simple packing works for motion detection.
  
#ifdef CONFIG_SOC_WIFI_HE_SUPPORT
  int16_t* iq_data = (int16_t*)csi_data_ptr;
#else
  int8_t* iq_data = (int8_t*)csi_data_ptr;
#endif
  
  float energy_sum = 0;
  
  for (int i = 0; i < num_sc; i++) {
    // Hanning Window
    float window = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (num_sc - 1)));
    
    float re = (float)iq_data[i * 2];
    float im = (float)iq_data[i * 2 + 1];
    
    // Fill FFT input (Conjugate for IFFT trick: IFFT(x) = conj(FFT(conj(x)))/N)
    // We actually just want Magnitude, so standard FFT is fine too,
    // but let's do Conjugate input to be physically correct for CIR.
    WifiCsi->fft_input[i * 2 + 0] = re * window;
    WifiCsi->fft_input[i * 2 + 1] = -im * window; // Conjugate imaginary part
    
    energy_sum += (re * re + im * im);
  }
  
  // Normalize Input Power (Crucial for RSSI independence!)
  // This removes global signal strength changes before FFT
  if (energy_sum > 0.001f) {
    float scale = 1.0f / sqrtf(energy_sum);
    for (int i = 0; i < 128 * 2; i++) {
      WifiCsi->fft_input[i] *= scale;
    }
  }
  
  // --- 2. Perform FFT (Inverse via Conjugate trick) ---
  dsps_fft2r_fc32(WifiCsi->fft_input, 128);
  dsps_bit_rev_fc32(WifiCsi->fft_input, 128); // Bit reverse
  
  // --- 3. Compute Magnitude of CIR (Time Domain Profile) ---
  for (int i = 0; i < 128; i++) {
    float re = WifiCsi->fft_input[i * 2 + 0];
    float im = WifiCsi->fft_input[i * 2 + 1];
    WifiCsi->cir_profile[i] = sqrtf(re * re + im * im);
  }
  
  // --- 4. Motion Metric: Variance of REFLECTED Paths ---
  // Bin 0: Direct Path (Line of Sight) - IGNORE IT
  // Bins 1-15: Reflected Paths (Multipath) - MONITOR THESE
  
  if (!WifiCsi->cir_baseline_valid) {
    for (int i = 0; i < 128; i++) {
      WifiCsi->cir_baseline[i] = WifiCsi->cir_profile[i];
    }
    WifiCsi->cir_baseline_valid = true;
    WifiCsi->baseline_packet_count = 1;
    WifiCsi->last_rssi = rssi;
    
    return; // Skip motion detection for first packet
  }
  
  float cir_diff_energy = 0;
  // Focus on Bins 1 to 16 (Reflections). Ignore Bin 0 (Direct Path).
  for (int i = 1; i < 16; i++) {
    float diff = WifiCsi->cir_profile[i] - WifiCsi->cir_baseline[i];
    cir_diff_energy += (diff * diff);
  }
  
  // Scale score up for usability
  float activity_score = cir_diff_energy * 1000.0f;
  
  // Exponential smoothing
  WifiCsi->last_activity_score = 0.7f * WifiCsi->last_activity_score + 0.3f * activity_score;
  
  // Update standard deviation calculation (sliding window of last 10 samples)
  WifiCsiUpdateVariance(WifiCsi->last_activity_score);
  
  // --- 5. Adaptive Baseline Update ---
  // Update slowly to handle environmental drift
  float learn_rate = 0.05f;
  
  if (activity_score > WifiCsi->activity_threshold) {
    learn_rate = 0.005f; // Slower during motion
  }
  
  for (int i = 0; i < 128; i++) {
    WifiCsi->cir_baseline[i] = (1.0f - learn_rate) * WifiCsi->cir_baseline[i] +
                               learn_rate * WifiCsi->cir_profile[i];
  }
  
  WifiCsi->baseline_packet_count++;
  
  // --- 6. Motion Trigger ---
  if (WifiCsi->last_activity_score > WifiCsi->activity_threshold) {
    WifiCsi->movement_count++;
  }
  
  // Debug Log
  static uint32_t last_log = 0;
  if (millis() - last_log > 10000) {
    last_log = millis();
    AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("CSI-CIR: Score=%.2f RSSI=%d Bins[1-3]=%.3f,%.3f,%.3f"),
           WifiCsi->last_activity_score, rssi,
           WifiCsi->cir_profile[1], WifiCsi->cir_profile[2], WifiCsi->cir_profile[3]);
  }
  
  // Update RSSI for status reporting
  WifiCsi->last_rssi = rssi;
}

/*********************************************************************************************\
 * WiFi CSI Callback
\*********************************************************************************************/

static void WifiCsiProcessPacket(void* ctx, wifi_csi_info_t* info) {
  if (!WifiCsi || !WifiCsi->enabled || !info || !info->buf) return;
  if (info->len < 4) {
    portENTER_CRITICAL(&wifi_csi_mux);
    WifiCsi->packets_dropped++;
    portEXIT_CRITICAL(&wifi_csi_mux);
    return;
  }
  
  // Filter by router MAC (if valid)
  if (WifiCsi->router_mac_valid) {
    bool mac_match = true;
    for (int i = 0; i < 6; i++) {
      if (info->mac[i] != WifiCsi->router_mac[i]) {
        mac_match = false;
        break;
      }
    }
    if (!mac_match) return;  // Not from our router
  }
  
  int8_t* csi_data = info->buf;
  uint16_t data_len = info->len;
  
#if CSI_HAS_FIRST_WORD_BUG
  if (info->first_word_invalid) {
    csi_data = csi_data + 4;
    data_len -= 4;
    if (data_len < 4) {
      portENTER_CRITICAL(&wifi_csi_mux);
      WifiCsi->packets_dropped++;
      portEXIT_CRITICAL(&wifi_csi_mux);
      return;
    }
  }
#endif
  
  if (data_len > (WIFI_CSI_MAX_PACKET_SIZE - WIFI_CSI_HEADER_SIZE)) {
    data_len = WIFI_CSI_MAX_PACKET_SIZE - WIFI_CSI_HEADER_SIZE;
  }
  
  portENTER_CRITICAL(&wifi_csi_mux);
  WifiCsi->packet_count++;
  portEXIT_CRITICAL(&wifi_csi_mux);
  
  size_t total_size = WIFI_CSI_HEADER_SIZE + data_len;
  uint8_t packet_buffer[WIFI_CSI_MAX_PACKET_SIZE];
  
  wifi_csi_packet_header_t* header = (wifi_csi_packet_header_t*)packet_buffer;
  header->timestamp = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  memcpy(header->mac, info->mac, 6);
  header->rssi = RX_CTRL_RSSI(info);
  header->noise_floor = RX_CTRL_NOISE_FLOOR(info);
  header->data_len = data_len;
  
  memcpy(packet_buffer + WIFI_CSI_HEADER_SIZE, csi_data, data_len);
  
  BaseType_t sent = xRingbufferSend(WifiCsi->ringbuf, packet_buffer, total_size, 0);
  if (sent != pdTRUE) {
    portENTER_CRITICAL(&wifi_csi_mux);
    WifiCsi->packets_dropped++;
    portEXIT_CRITICAL(&wifi_csi_mux);
  }
}

/*********************************************************************************************\
 * Process CSI buffer
\*********************************************************************************************/

void WifiCsiProcessBuffer() {
  if (!WifiCsi || !WifiCsi->enabled) return;
  
  size_t item_size;
  uint8_t* packet = (uint8_t*)xRingbufferReceive(WifiCsi->ringbuf, &item_size, 0);
  
  if (!packet) return;
  
  if (item_size < WIFI_CSI_HEADER_SIZE) {
    vRingbufferReturnItem(WifiCsi->ringbuf, packet);
    return;
  }
  
  wifi_csi_packet_header_t* header = (wifi_csi_packet_header_t*)packet;
  void* csi_data = (void*)(packet + WIFI_CSI_HEADER_SIZE);
  uint16_t data_len = header->data_len;
  
  if (data_len > (item_size - WIFI_CSI_HEADER_SIZE)) {
    data_len = item_size - WIFI_CSI_HEADER_SIZE;
  }
  
  WifiCsi->last_noise_floor = header->noise_floor;
  
  int num_sc = WifiCsiDeriveSubcarrierCount(data_len);
  if (num_sc > 0) {
    WifiCsi->subcarrier_count = num_sc;
  }
  
  // Pass raw CSI data to CIR-based processing pipeline
  WifiCsiProcessPipeline(csi_data, header->rssi, num_sc);
  
  vRingbufferReturnItem(WifiCsi->ringbuf, packet);
}

/*********************************************************************************************\
 * Get Router MAC Address
\*********************************************************************************************/

bool WifiCsiGetRouterMac() {
  wifi_ap_record_t ap_info;
  esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
  
  if (err == ESP_OK) {
    memcpy(WifiCsi->router_mac, ap_info.bssid, 6);
    WifiCsi->router_mac_valid = true;
    
    char mac_str[18];
    ToHex_P(WifiCsi->router_mac, 6, mac_str, 18, ':');
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: Router MAC: %s (RSSI: %d dBm)"), 
           mac_str, ap_info.rssi);
    
    // Detect bandwidth
    if (ap_info.second == WIFI_SECOND_CHAN_NONE) {
      WifiCsi->subcarrier_count = 56;
      AddLog(LOG_LEVEL_INFO, PSTR("CSI: HT20 mode - 56 subcarriers"));
    } else {
      WifiCsi->subcarrier_count = 114;
      AddLog(LOG_LEVEL_INFO, PSTR("CSI: HT40 mode - 114 subcarriers"));
    }
    
    return true;
  } else {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Not connected to AP (error 0x%x)"), err);
    WifiCsi->router_mac_valid = false;
    return false;
  }
}

/*********************************************************************************************\
 * Module Init
\*********************************************************************************************/

void WifiCsiModuleInit(void) {
  WifiCsi = (struct WifiCsi*)calloc(1, sizeof(struct WifiCsi));
  if (!WifiCsi) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Failed to allocate driver state"));
    return;
  }
  
  WifiCsi->enabled = false;
  WifiCsi->channel = 6;
  WifiCsi->router_mac_valid = false;
  WifiCsi->subcarrier_count = 56;
  
  // Initialize CIR (Channel Impulse Response) fields
  memset(WifiCsi->fft_input, 0, sizeof(WifiCsi->fft_input));
  memset(WifiCsi->cir_profile, 0, sizeof(WifiCsi->cir_profile));
  memset(WifiCsi->cir_baseline, 0, sizeof(WifiCsi->cir_baseline));
  WifiCsi->cir_baseline_valid = false;
  WifiCsi->baseline_packet_count = 0;
  WifiCsi->fft_initialized = false;
  WifiCsi->warmup_counter = 0;
  WifiCsi->last_rssi = 0;
  WifiCsi->last_activity_score = 0;
  
  // Initialize standard deviation calculation fields
  memset(WifiCsi->activity_history, 0, sizeof(WifiCsi->activity_history));
  WifiCsi->history_index = 0;
  WifiCsi->history_count = 0;
  WifiCsi->variance = 0.0f;
  
  WifiCsi->ringbuf = xRingbufferCreate(WIFI_CSI_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
  if (!WifiCsi->ringbuf) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Failed to create ring buffer"));
    free(WifiCsi);
    WifiCsi = nullptr;
    return;
  }
  
  // Initialize FFT table for 128 points
  esp_err_t ret = dsps_fft2r_init_fc32(NULL, 128);
  if (ret != ESP_OK) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CSI: FFT Init failed!"));
  } else {
    WifiCsi->fft_initialized = true;
  }
  
  WifiCsi->activity_threshold = 1000.0f;  // Default threshold
  
  AddLog(LOG_LEVEL_INFO, PSTR("CSI: WiFi CSI driver initialized (CIR-based motion detection)"));
  AddLog(LOG_LEVEL_INFO, PSTR("CSI: Activity threshold: %.1f"), WifiCsi->activity_threshold);
}

/*********************************************************************************************\
 * Enable/Disable
\*********************************************************************************************/

void WifiCsiEnable(bool enable) {
  if (!WifiCsi) return;
  
  if (enable && !WifiCsi->enabled) {
    // Get router MAC before enabling
    if (!WifiCsiGetRouterMac()) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Cannot enable - not connected to AP"));
      return;
    }
    
    wifi_csi_config_t csi_config;
    memset(&csi_config, 0, sizeof(csi_config));
    
#ifdef CONFIG_SOC_WIFI_HE_SUPPORT
    csi_config.enable = 1;
    csi_config.acquire_csi_legacy = 1;
    csi_config.acquire_csi_ht20 = 1;
    csi_config.acquire_csi_ht40 = 1;
    csi_config.val_scale_cfg = 0;
#else
    csi_config.lltf_en = true;
    csi_config.htltf_en = true;
    csi_config.stbc_htltf2_en = true;
    csi_config.ltf_merge_en = true;
    csi_config.channel_filter_en = true;
    csi_config.manu_scale = false;
    csi_config.shift = 0;
#endif
    
    esp_err_t err = esp_wifi_set_csi_config(&csi_config);
    if (err != ESP_OK) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Config failed: 0x%x"), err);
      return;
    }
    
    err = esp_wifi_set_csi_rx_cb(&WifiCsiProcessPacket, NULL);
    if (err != ESP_OK) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Callback failed: 0x%x"), err);
      return;
    }
    
    err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CSI: Enable failed: 0x%x"), err);
      esp_wifi_set_csi_rx_cb(NULL, NULL);
      return;
    }
    
    WifiCsi->enabled = true;
    WifiCsi->packet_count = 0;
    WifiCsi->packets_dropped = 0;
    
    // Reset CIR (Channel Impulse Response) fields
    memset(WifiCsi->fft_input, 0, sizeof(WifiCsi->fft_input));
    memset(WifiCsi->cir_profile, 0, sizeof(WifiCsi->cir_profile));
    memset(WifiCsi->cir_baseline, 0, sizeof(WifiCsi->cir_baseline));
    WifiCsi->cir_baseline_valid = false;
    WifiCsi->baseline_packet_count = 0;
    WifiCsi->warmup_counter = 0;
    WifiCsi->last_activity_score = 0;
    WifiCsi->last_rssi = 0;
    
    // Reset standard deviation calculation fields
    memset(WifiCsi->activity_history, 0, sizeof(WifiCsi->activity_history));
    WifiCsi->history_index = 0;
    WifiCsi->history_count = 0;
    WifiCsi->variance = 0.0f;
    
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: Enabled (CIR-based motion detection)"));
  } else if (!enable && WifiCsi->enabled) {
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    WifiCsi->enabled = false;
    AddLog(LOG_LEVEL_INFO, PSTR("CSI: Disabled"));
  }
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

void CmndCsiEnable(void) {
  if (XdrvMailbox.payload >= 0 && XdrvMailbox.payload <= 1) {
    WifiCsiEnable(XdrvMailbox.payload);
  }
  ResponseCmndNumber(WifiCsi->enabled);
}

void CmndCsiChannel(void) {
  if (XdrvMailbox.payload >= 1 && XdrvMailbox.payload <= 14) {
    WifiCsi->channel = XdrvMailbox.payload;
  }
  ResponseCmndNumber(WifiCsi->channel);
}

void CmndCsiActivity(void) {
  if (XdrvMailbox.data_len > 0) {
    float new_value = CharToFloat(XdrvMailbox.data);
    if (new_value >= 0.5f && new_value <= 100.0f) {
      WifiCsi->activity_threshold = new_value;
      AddLog(LOG_LEVEL_INFO, PSTR("CSI: Activity threshold: %.1f"), WifiCsi->activity_threshold);
    }
  }
  ResponseCmndFloat(WifiCsi->activity_threshold, 1);
}

void CmndCsiStatus(void) {
  if (!WifiCsi) {
    ResponseCmndChar(PSTR("Not initialized"));
    return;
  }
  
  char mac_str[18] = "Unknown";
  if (WifiCsi->router_mac_valid) {
    ToHex_P(WifiCsi->router_mac, 6, mac_str, 18, ':');
  }
  
  Response_P(PSTR("{\"Enabled\":%d,\"RouterMAC\":\"%s\",\"Packets\":%u,\"Dropped\":%u,"
                  "\"RSSI\":%d,\"Activity\":%*_f,\"StdDev\":%*_f,\"Movements\":%u,\"Subcarriers\":%d,"
                  "\"Threshold\":%*_f,\"BaselineValid\":%d,\"BaselinePackets\":%u,\"Warmup\":%u}"),
             WifiCsi->enabled,
             mac_str,
             WifiCsi->packet_count,
             WifiCsi->packets_dropped,
             WifiCsi->last_rssi,
             2, &WifiCsi->last_activity_score,
             2, &WifiCsi->variance,
             WifiCsi->movement_count,
             WifiCsi->subcarrier_count,
             1, &WifiCsi->activity_threshold,
             WifiCsi->cir_baseline_valid,
             WifiCsi->baseline_packet_count,
             WifiCsi->warmup_counter);
  
  ResponseJsonEnd();
}

/*********************************************************************************************\
 * Sensor output
\*********************************************************************************************/

const char* WifiCsiGetState(void) {
  if (!WifiCsi->enabled) return PSTR("Disabled");
  if (!WifiCsi->cir_baseline_valid || WifiCsi->warmup_counter < 100) return PSTR("Warmup");
  if (WifiCsi->last_activity_score > WifiCsi->activity_threshold) return PSTR("Motion");
  return PSTR("None");
}

void WifiCsiShow(bool json) {
  if (!WifiCsi) return;
  
  const char* state = WifiCsiGetState();
  
  if (json) {
    ResponseAppend_P(PSTR(",\"CSI\":{\"State\":\"%s\",\"Activity\":%*_f,\"StdDev\":%*_f,\"RSSI\":%d}"),
      state,
      2, &WifiCsi->last_activity_score,
      2, &WifiCsi->variance,
      WifiCsi->last_rssi);
#ifdef USE_WEBSERVER
  } else {
    WSContentSend_PD(PSTR("{s}CSI State{m}%s{e}"), state);
    
    // Only show activity, standard deviation and histogram after warmup
    if (WifiCsi->cir_baseline_valid && WifiCsi->warmup_counter >= 100) {
      WSContentSend_PD(PSTR("{s}CSI Activity{m}%*_f{e}"), 2, &WifiCsi->last_activity_score);
      WSContentSend_PD(PSTR("{s}CSI Std Dev{m}%*_f{e}"), 2, &WifiCsi->variance);
      
      // Display CIR Histogram (Bins 1-15) using Unicode rectangles
      WSContentSend_PD(PSTR("{s}CIR Histogram{m}"));
      
      // Find maximum value for normalization
      float max_val = 0.001f; // Avoid division by zero
      for (int i = 1; i <= 15; i++) {
        if (WifiCsi->cir_profile[i] > max_val) {
          max_val = WifiCsi->cir_profile[i];
        }
      }
      
      // Display histogram for bins 1-15
      for (int i = 1; i <= 15; i++) {
        // Normalize value to 0-7 range for 8 Unicode blocks
        int level = (int)((WifiCsi->cir_profile[i] / max_val) * 7.0f);
        if (level < 1) level = 1;
        if (level > 7) level = 7;
        
        // Unicode block characters from empty to full
        // U+2581 to U+2588: ▁▂▃▄▅▆▇█
        const char* blocks[] = {"", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        
        // Use appropriate block character
        WSContentSend_PD(PSTR("%s"), blocks[level]);
      }
      WSContentSend_PD(PSTR("{e}"));
    }
#endif
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv93(uint32_t function) {
  bool result = false;
  
  if (FUNC_INIT == function) {
    WifiCsiModuleInit();
  } else if (WifiCsi) {
    switch (function) {
      case FUNC_COMMAND:
        result = DecodeCommand(kCsiCommands, CsiCommand);
        break;
      case FUNC_JSON_APPEND:
        WifiCsiShow(true);
        break;
#ifdef USE_WEBSERVER
      case FUNC_WEB_SENSOR:
        WifiCsiShow(false);
        break;
#endif
      case FUNC_ACTIVE:
        result = true;
        break;
      case FUNC_LOOP:
        WifiCsiProcessBuffer();
        break;
    }
  }
  return result;
}

#endif  // USE_WIFI_CSI
#endif // CONFIG_ESP_WIFI_CSI_ENABLED
#endif // ESP32
