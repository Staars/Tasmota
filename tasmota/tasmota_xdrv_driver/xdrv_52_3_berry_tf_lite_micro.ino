/*
  xdrv_52_3_tf_lite_micro.ino - Berry scripting language, High-Level Tensor Flow Lite for Microprocessors model deployer
  Now with CSI (Channel State Information) support

  Copyright (C) 2022 Christian Baars & Stephan Hadinger, Berry language by Guan Wenliang https://github.com/Skiars/berry

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_BERRY

#include <berry.h>

#ifdef USE_BERRY_TF_LITE

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/core/api/error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/c/c_api_types.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "freertos/ringbuf.h"
#include "tensorflow/lite/c/common.h"

#ifdef USE_I2S
#include "mfcc.h"
#endif //USE_I2S

#ifdef USE_TF_LITE_CSI
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/portmacro.h"
#include "ping/ping_sock.h"

// Check SOC support for CSI
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
  #define CSI_SUPPORTED 1
#elif CONFIG_IDF_TARGET_ESP32C2
  #define CSI_SUPPORTED 0
  #warning "ESP32-C2 does not support WiFi CSI"
#else
  #define CSI_SUPPORTED 0
  #warning "Unknown ESP32 variant - CSI support uncertain"
#endif

#endif // USE_TF_LITE_CSI

/*********************************************************************************************\
 * Internal helper classes and constants
\*********************************************************************************************/
#ifdef USE_I2S

#define kObservationWindow    1000    //milliseconds
#define kAudioSampleFrequency 16000   
#define kAudioSampleBits      16

struct TFL_mic_descriptor_t{
  uint8_t channel_fmt;      // UNUSED NOW !!!
  uint8_t preamp;           // UNUSED NOW !!!
  uint8_t slice_dur;        // milliseconds
  uint8_t slice_stride;     // milliseconds
  uint8_t num_filter;       // mfe bins
  uint8_t num_coeff;        // mfcc coefficients, if 0 -> compute MFE only
  uint8_t fft_bins;         // 2^fft_bins
  uint8_t max_invocations;  // max. invocations per second
  uint8_t db_floor;        // filter out noise below decibel threshold, treated as negative value
  uint8_t preemphasis;     //  as <float>preemphasis/100.0f , 0 - no preemphasis
};

struct TFL_mic_ctx_t{
  TaskHandle_t audio_capture_task = nullptr;
  SemaphoreHandle_t feature_buffer_mutex = nullptr;
  MFCC * mfcc = nullptr;
  int8_t* model_input_buffer = nullptr;

  union{
    struct {
      uint32_t is_audio_initialized:1;
      uint32_t is_first_time:1;
      // uint32_t new_feature_data:1;
      uint32_t continue_audio_capture:1;
      uint32_t stop_audio_capture:1;
      uint32_t audio_capture_ended:1;
      uint32_t use_mfcc:1;
      uint32_t use_gain_filter:1;
    } flag;
    uint32_t flags;
  };
  int feature_buffer_idx = 0;
  int8_t *feature_buffer;
  // user input
  // int32_t channel_fmt;  // UNUSED
  int32_t preamp;       // setup by I2S driver
  int32_t slice_dur;    // milliseconds
  int32_t slice_stride; // milliseconds
  uint8_t num_filter;   // mfe filter bins
  uint8_t num_coeff;    // mfcc coefficients, if 0 -> compute MFE only
  int32_t fft_bins;     // 2^fft_bins
  // calculated
  int32_t slice_size;   // bytes
  int32_t slice_count;
  uint16_t i2s_samples_to_get;
  int16_t db_floor;    // filter out noise below decibel threshold, this is now a negative value
  float preemphasis;
};

#endif //USE_I2S

#ifdef USE_TF_LITE_CSI
// --- Timestamp helpers ---
static inline uint32_t get_timestamp_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000); // ms since boot
}

// CSI Descriptor Structure
struct TFL_csi_descriptor_t {
    uint8_t sample_rate;       // Packets per second (1-100)
    uint8_t feature_mode;      // 0=RAW, 1=LIGHT
    uint8_t use_quantization;  // Quantize for TFLite
    uint8_t training_mode;     // Enable training data output
    uint8_t max_invocations;   // Max inferences per second
};

// CSI Context Structure
struct TFL_csi_ctx_t {
    volatile bool capturing;                    // volatile for thread-safety
    volatile uint32_t packets_received;         // atomic counter
    volatile uint32_t packets_dropped;          // atomic counter
    int8_t* feature_buffer;
    int feature_buffer_size;
    volatile int feature_buffer_idx;            // atomic index
    int feature_size;
    TFL_csi_descriptor_t config;
    SemaphoreHandle_t buffer_mutex;             // mutex for buffer access

    esp_ping_handle_t ping_handle;
    esp_ping_config_t ping_config;
    ip_addr_t target_addr;
};

#endif // USE_TF_LITE_CSI

struct TFL_stats_t{
  uint32_t model_size = 0;
  uint32_t used_arena_bytes = 0;
  uint32_t invocations = 0;
  uint32_t loop_task_free_stack_bytes = 0;
  uint32_t mic_task_free_stack_bytes = 0;
};

struct TFL_ctx_t{
const tflite::Model* model = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;
int8_t *berry_output_buf = nullptr;
size_t berry_output_bufsize;
int TensorArenaSize = 2000;
uint8_t max_invocations = 4;    // max. invocations per second

TaskHandle_t loop_task = nullptr;
// QueueHandle_t loop_queue = nullptr;
union{
    struct {
    uint32_t init_done:1;
    uint32_t delay_next_invocation:1;
    uint32_t running_invocation:1;
    uint32_t running_loop:1;
    // uint32_t stop_loop:1;
    uint32_t loop_ended:1;
    uint32_t unread_output:1;
    uint32_t new_input_data:1;
    uint32_t use_mic:1;
    uint32_t use_csi:1;
    } option;
    uint32_t options;
};
#ifdef USE_I2S
TFL_mic_ctx_t *mic = nullptr;
#endif // USE_I2S
#ifdef USE_TF_LITE_CSI
TFL_csi_ctx_t *csi = nullptr;
#endif // USE_TF_LITE_CSI
TFL_stats_t *stats = nullptr;
};

TFL_ctx_t *TFL = nullptr;
RingbufHandle_t TFL_log_buffer = nullptr;
#ifdef USE_TF_LITE_CSI
RingbufHandle_t TFL_training_buffer = nullptr;
static portMUX_TYPE csi_spinlock = portMUX_INITIALIZER_UNLOCKED; // spinlock for ISR
#endif

/*********************************************************************************************\
 * CSI-specific functions
\*********************************************************************************************/
#ifdef USE_TF_LITE_CSI

static inline void TFL_extract_features_raw(int8_t* csi_data, int data_len, TFL_csi_ctx_t* csi, wifi_csi_info_t* info);
static inline void TFL_extract_features_lightweight(int8_t* csi_data, int data_len, TFL_csi_ctx_t* csi, wifi_csi_info_t* info);
static inline void TFL_extract_features_mfcc_like(int8_t* csi_data, int data_len, TFL_csi_ctx_t* csi, wifi_csi_info_t* info);

/**
 * @brief Lightweight CSI callback - must be fast and ISR-safe
 */
static void IRAM_ATTR wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
    if (!TFL || !TFL->csi || !TFL->csi->capturing || !info || !info->buf) {
        return;
    }
    
    // Quick validation
    if (info->len < 4) {
        portENTER_CRITICAL_ISR(&csi_spinlock);
        TFL->csi->packets_dropped++;
        portEXIT_CRITICAL_ISR(&csi_spinlock);
        return;
    }
    
    TFL_csi_ctx_t* csi = TFL->csi;

    // Get buffer pointers
    int8_t* csi_data = info->buf;
    uint16_t data_len = info->len;
    
    // Skip first word if invalid (ESP32 hardware limitation)
    if (info->first_word_invalid) {
        csi_data += 4;
        data_len -= 4;
        if (data_len < 4) {
            portENTER_CRITICAL_ISR(&csi_spinlock);
            csi->packets_dropped++;
            portEXIT_CRITICAL_ISR(&csi_spinlock);
            return;
        }
    }
    
    // Get current buffer index atomically
    portENTER_CRITICAL_ISR(&csi_spinlock);
    int current_idx = csi->feature_buffer_idx;
    int next_idx = (current_idx + 1) % 10;
    portEXIT_CRITICAL_ISR(&csi_spinlock);
    
    // Calculate output location
    int8_t* output_buffer = csi->feature_buffer + (current_idx * csi->feature_size);
    
    // Copy only what fits
    int copy_size = (data_len < csi->feature_size) ? data_len : csi->feature_size;
    memcpy(output_buffer, csi_data, copy_size);
    
    // Zero-pad if needed
    if (copy_size < csi->feature_size) {
        memset(output_buffer + copy_size, 0, csi->feature_size - copy_size);
    }
    
    if (csi->config.training_mode) {
        switch (csi->config.feature_mode) {
            case 0: // Raw
                TFL_extract_features_raw(csi_data, copy_size, csi, info);
                break;
            case 1: // Lightweight  
                TFL_extract_features_lightweight(csi_data, copy_size, csi, info);
                break;
            case 2: // MFCC-like
                // TFL_extract_features_mfcc_like(csi_data, copy_size, csi, info);
                break;
        }
    }

    // Update index and counters atomically
    portENTER_CRITICAL_ISR(&csi_spinlock);
    csi->feature_buffer_idx = next_idx;
    csi->packets_received++;
    portEXIT_CRITICAL_ISR(&csi_spinlock);
    
    // Signal new data
    TFL->option.new_input_data = 1;
}

static inline void TFL_extract_features_raw(int8_t* csi_data, int data_len, TFL_csi_ctx_t* csi, wifi_csi_info_t* info) {
    if (!TFL_training_buffer) return;
    
    // Validate data length to prevent overflow
    if (data_len > 512) {  // Reasonable max for CSI data
        data_len = 512;
    }
    
    // Binary packet: [timestamp(4)][rssi(1)][noise_floor(1)][data_len(2)][csi_data(data_len)]
    size_t packet_size = 8 + data_len;
    
    // Static buffer to avoid malloc in ISR - reused across calls
    static uint8_t bin_packet[520];  // 8 header + 512 max data
    
    // Timestamp (4 bytes, little-endian for easier parsing)
    *(uint32_t *)&bin_packet[0] = get_timestamp_ms();

    
    // RSSI (as signed int8_t)
    bin_packet[4] = (uint8_t)(info->rx_ctrl.rssi);
    
    // Noise floor (as signed int8_t)
    bin_packet[5] = (uint8_t)(info->rx_ctrl.noise_floor);
    
    // Data length (2 bytes, little-endian)
    bin_packet[6] = data_len & 0xFF;
    bin_packet[7] = (data_len >> 8) & 0xFF;
    
    // Copy raw CSI data
    memcpy(bin_packet + 8, csi_data, data_len);
    
    // Send to ring buffer - uses internal copy
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t result = xRingbufferSendFromISR(TFL_training_buffer, bin_packet, packet_size, &xHigherPriorityTaskWoken);
    
    if (result != pdTRUE) {
        // Track dropped packets
        portENTER_CRITICAL_ISR(&csi_spinlock);
        csi->packets_dropped++;
        portEXIT_CRITICAL_ISR(&csi_spinlock);
    }
    
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static inline void TFL_extract_features_lightweight(int8_t* csi_data, int data_len, TFL_csi_ctx_t* csi, wifi_csi_info_t* info) {
    if (!TFL_training_buffer) return;
    
    // Validate input
    if (data_len <= 0 || data_len > 512) return;
    
    // Enhanced statistical features
    int32_t sum = 0;
    int64_t sum_sq = 0;
    int8_t min_val = 127;
    int8_t max_val = -128;
    int32_t abs_sum = 0;
    int zero_crossings = 0;
    int8_t prev_val = csi_data[0];
    
    // Single pass through data for basic statistics
    for (int i = 0; i < data_len; i++) {
        int8_t val = csi_data[i];
        int32_t val32 = val;
        int64_t val_sq = (int64_t)val32 * val32;
        
        sum += val32;
        sum_sq += val_sq;
        abs_sum += abs(val32);
        
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
        
        // Zero crossing detection with noise threshold
        if (i > 0) {
            if ((prev_val > 2 && val < -2) || (prev_val < -2 && val > 2)) {
                zero_crossings++;
            }
        }
        prev_val = val;
    }
    
    // Calculate ALL statistics in floating point
    float mean_f = (float)sum / (float)data_len;
    float variance_f = ((float)sum_sq - (float)data_len * mean_f * mean_f) / (float)(data_len - 1);
    if (variance_f < 0) variance_f = 0.0f;
    float std_dev_f = sqrtf(variance_f);
    
    // Calculate centered moments for skewness and kurtosis
    float sum_cube_centered = 0.0f;
    float sum_quad_centered = 0.0f;
    
    for (int i = 0; i < data_len; i++) {
        float centered = (float)csi_data[i] - mean_f;
        float squared = centered * centered;
        sum_cube_centered += squared * centered;
        sum_quad_centered += squared * squared;
    }
    
    // Calculate skewness and kurtosis
    float skewness_f = 0.0f;
    float kurtosis_f = 0.0f;
    
    if (std_dev_f > 1e-10f) {
        float n = (float)data_len;
        skewness_f = (sum_cube_centered / n) / (std_dev_f * std_dev_f * std_dev_f);
        kurtosis_f = (sum_quad_centered / n) / (variance_f * variance_f) - 3.0f;
    }
    
    // Scale for integer transmission (preserve 3 decimal places)
    int32_t mean_scaled = (int32_t)(mean_f * 1000.0f);
    int32_t std_dev_scaled = (int32_t)(std_dev_f * 1000.0f);
    int32_t skewness_scaled = (int32_t)(skewness_f * 1000.0f);
    int32_t kurtosis_scaled = (int32_t)(kurtosis_f * 1000.0f);
    
    // Energy - cap to prevent overflow
    int32_t energy = (sum_sq > 0x7FFFFFFF) ? 0x7FFFFFFF : (int32_t)sum_sq;
    int32_t mad = abs_sum / data_len;
    
    // Binary packet - LITTLE ENDIAN to match raw format
    uint8_t bin_packet[34];   // one less than before

    // Timestamp (little-endian, 4 bytes)
    *(uint32_t *)&bin_packet[0] = get_timestamp_ms();


    // RSSI and noise floor
    bin_packet[4] = (uint8_t)(info->rx_ctrl.rssi);
    bin_packet[5] = (uint8_t)(info->rx_ctrl.noise_floor);

    // Statistical features (little-endian, scaled by 1000)
    bin_packet[6]  = mean_scaled & 0xFF;
    bin_packet[7]  = (mean_scaled >> 8) & 0xFF;
    bin_packet[8]  = (mean_scaled >> 16) & 0xFF;
    bin_packet[9]  = (mean_scaled >> 24) & 0xFF;

    bin_packet[10] = std_dev_scaled & 0xFF;
    bin_packet[11] = (std_dev_scaled >> 8) & 0xFF;
    bin_packet[12] = (std_dev_scaled >> 16) & 0xFF;
    bin_packet[13] = (std_dev_scaled >> 24) & 0xFF;

    bin_packet[14] = (uint8_t)min_val;
    bin_packet[15] = (uint8_t)max_val;

    bin_packet[16] = energy & 0xFF;
    bin_packet[17] = (energy >> 8) & 0xFF;
    bin_packet[18] = (energy >> 16) & 0xFF;
    bin_packet[19] = (energy >> 24) & 0xFF;

    bin_packet[20] = mad & 0xFF;
    bin_packet[21] = (mad >> 8) & 0xFF;
    bin_packet[22] = (mad >> 16) & 0xFF;
    bin_packet[23] = (mad >> 24) & 0xFF;

    bin_packet[24] = zero_crossings & 0xFF;
    bin_packet[25] = (zero_crossings >> 8) & 0xFF;

    bin_packet[26] = skewness_scaled & 0xFF;
    bin_packet[27] = (skewness_scaled >> 8) & 0xFF;
    bin_packet[28] = (skewness_scaled >> 16) & 0xFF;
    bin_packet[29] = (skewness_scaled >> 24) & 0xFF;

    bin_packet[30] = kurtosis_scaled & 0xFF;
    bin_packet[31] = (kurtosis_scaled >> 8) & 0xFF;
    bin_packet[32] = (kurtosis_scaled >> 16) & 0xFF;
    bin_packet[33] = (kurtosis_scaled >> 24) & 0xFF;

    // Send binary packet with error tracking
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t result = xRingbufferSendFromISR(TFL_training_buffer, bin_packet, 34, &xHigherPriorityTaskWoken);
    
    if (result != pdTRUE) {
        portENTER_CRITICAL_ISR(&csi_spinlock);
        csi->packets_dropped++;
        portEXIT_CRITICAL_ISR(&csi_spinlock);
    }
    
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static inline void TFL_extract_features_mfcc_like(int8_t* csi_data, int data_len, TFL_csi_ctx_t* csi, wifi_csi_info_t* info){
  // implement later
}

/**
 * @brief Initialize CSI with proper error handling
 */
bool TFL_init_CSI(const uint8_t* descriptor) {
    AddLog(LOG_LEVEL_INFO, PSTR("TFL: Starting CSI initialization"));
    
#if !CSI_SUPPORTED
    AddLog(LOG_LEVEL_ERROR, PSTR("TFL: CSI not supported on this ESP32 variant"));
    return false;
#endif
    TFL_csi_descriptor_t* csi_desc = (TFL_csi_descriptor_t*)descriptor;
    esp_err_t err;
    int subcarrier_count = 56; // Default for 802.11n TODO: ESP32-S3 only!!!!!
    esp_netif_t* netif;
    
    // Allocate context
    TFL->csi = new TFL_csi_ctx_t{};
    if (!TFL->csi) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Failed to allocate CSI context"));
        return false;
    }
    TFL->csi->config = *csi_desc;

    // Check WiFi mode and connection
    wifi_mode_t mode;
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK || (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA)) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: WiFi not in STA mode"));
        goto error_cleanup;
    }
    
    // Get AP info
    wifi_ap_record_t ap_info;
    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Not connected to AP"));
        goto error_cleanup;
    }

    if (ap_info.bandwidth == WIFI_BW_HT40) {
        subcarrier_count = 114;
        AddLog(LOG_LEVEL_INFO, PSTR("TFL: HT40 mode detected - 114 subcarriers"));
    }
    AddLog(LOG_LEVEL_INFO, PSTR("TFL: Connected to %s, Channel %d, Bandwidth: %u"), ap_info.ssid, ap_info.primary, ap_info.bandwidth);
    
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            // Use gateway directly for ping target - convert esp_ip4_addr_t to ip_addr_t
            IP_ADDR4(&TFL->csi->target_addr, 
                    ip4_addr1(&ip_info.gw),
                    ip4_addr2(&ip_info.gw), 
                    ip4_addr3(&ip_info.gw), 
                    ip4_addr4(&ip_info.gw));
            
            AddLog(LOG_LEVEL_INFO, PSTR("TFL: STA IP: " IPSTR), IP2STR(&ip_info.ip));
            AddLog(LOG_LEVEL_INFO, PSTR("TFL: Gateway: " IPSTR), IP2STR(&ip_info.gw));
        } else {
            AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Failed to get IP info"));
            goto error_cleanup;
        }
    } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Failed to get network interface"));
        goto error_cleanup;
    }

    // Setup ping using Espressif ping_sock API
    TFL->csi->ping_config = ESP_PING_DEFAULT_CONFIG();
    TFL->csi->ping_config.target_addr = TFL->csi->target_addr;
    TFL->csi->ping_config.count = 0;  // infinite pings
    TFL->csi->ping_config.interval_ms = 1000 / csi_desc->sample_rate;
    TFL->csi->ping_config.timeout_ms = 1000;
    TFL->csi->ping_config.task_stack_size = 2048;
    TFL->csi->ping_config.task_prio = 2;

    esp_ping_callbacks_t cbs;
    cbs.on_ping_success = NULL;  // No action needed on ping success
    cbs.on_ping_timeout = NULL;  // No action needed on ping timeout  
    cbs.on_ping_end = NULL;      // No action needed on ping end
    cbs.cb_args = TFL->csi;

    err = esp_ping_new_session(&TFL->csi->ping_config, &cbs, &TFL->csi->ping_handle);
    if (err != ESP_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Ping session creation failed: 0x%x"), err);
        goto error_cleanup;
    }

    // Start pinging
    err = esp_ping_start(TFL->csi->ping_handle);

    // Create mutex
    TFL->csi->buffer_mutex = xSemaphoreCreateMutex();
    if (!TFL->csi->buffer_mutex) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Failed to create mutex"));
        delete TFL->csi;
        TFL->csi = nullptr;
        return false;
    }
    
    // Calculate feature size (I/Q pairs as int8_t)
    TFL->csi->feature_size = subcarrier_count * 2;
    
    // Allocate ring buffer (10 frames)
    TFL->csi->feature_buffer_size = TFL->csi->feature_size * 10;
    TFL->csi->feature_buffer = (int8_t*)heap_caps_malloc(
        TFL->csi->feature_buffer_size, 
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    
    if (!TFL->csi->feature_buffer) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Failed to allocate feature buffer"));
        goto error_cleanup;
    }
    
    memset(TFL->csi->feature_buffer, 0, TFL->csi->feature_buffer_size);
    
    // Clean slate
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Register callback
    err = esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL);
    if (err != ESP_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: CSI callback failed: 0x%x"), err);
        goto error_cleanup;
    }
    
    // Enable promiscuous mode
    // esp_wifi_set_promiscuous(true);
    // vTaskDelay(pdMS_TO_TICKS(50));
    
    // Configure CSI
    wifi_csi_config_t csi_config;
    memset(&csi_config, 0, sizeof(csi_config));
    csi_config.lltf_en = 1;
    
    err = esp_wifi_set_csi_config(&csi_config);
    if (err != ESP_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: CSI config failed: 0x%x"), err);
        goto error_cleanup;
    }
    
    // Enable CSI
    err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: CSI enable failed: 0x%x"), err);
        goto error_cleanup;
    }
    
    TFL->csi->capturing = true;
    TFL->max_invocations = csi_desc->max_invocations;
    TFL->stats = new TFL_stats_t;

    return true;

error_cleanup:
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    esp_wifi_set_promiscuous(false);
    TFL->stats = nullptr;
    
    if (TFL->csi) {
        if (TFL->csi->buffer_mutex) {
            vSemaphoreDelete(TFL->csi->buffer_mutex);
        }
        if (TFL->csi->feature_buffer) {
            free(TFL->csi->feature_buffer);
        }
        delete TFL->csi;
        TFL->csi = nullptr;
    }
    return false;
}

/**
 * @brief Stop CSI capture and cleanup
 */
void TFL_stop_csi_capture() {
    if (!TFL || !TFL->csi) {
        return;
    }
    
    AddLog(LOG_LEVEL_INFO, PSTR("TFL: Stopping CSI capture..."));
    
    // Stop capturing first
    TFL->csi->capturing = false;

    // Stop pinging
    if (TFL->csi->ping_handle) {
        esp_ping_stop(TFL->csi->ping_handle);
        esp_ping_delete_session(TFL->csi->ping_handle);
        TFL->csi->ping_handle = nullptr;
    }
    
    // Disable CSI
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    esp_wifi_set_promiscuous(false);
    
    // Small delay to ensure callback isn't running
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Clean up resources
    if (TFL->csi->buffer_mutex) {
        vSemaphoreDelete(TFL->csi->buffer_mutex);
        TFL->csi->buffer_mutex = nullptr;
    }
    
    if (TFL->csi->feature_buffer) {
        free(TFL->csi->feature_buffer);
        TFL->csi->feature_buffer = nullptr;
    }
    
    delete TFL->csi;
    TFL->csi = nullptr;
    
    AddLog(LOG_LEVEL_INFO, PSTR("TFL: CSI stopped and cleaned up"));
}

#endif // USE_TF_LITE_CSI

/*********************************************************************************************\
 * Internal driver functions
\*********************************************************************************************/

/**
 * @brief This function is called from Microprint() from the Tensorflow framework
 *        Used to log from Tensorflow and from this (Tasmota) driver
 * 
 * @param s - message as c-string
 */
void TFL_Log(char *s){
  size_t len = strlen(s);
  if(len<5) return; // we assume this is for the trash

  xRingbufferSend(TFL_log_buffer, s, len+1 , pdMS_TO_TICKS(3));
}

bool TFL_create_task(){
    if (TFL->option.running_loop) return bfalse;
    if(TFL->loop_task!=nullptr) vTaskDelete(TFL->loop_task);
    
    xTaskCreatePinnedToCore(
    TFL_task_loop,                /* Function to implement the task */
    "tfl_loop",                   /* Name of the task */
    8000 + (TFL->TensorArenaSize),/* Stack size in words */
    NULL,                         /* Task input parameter */
    1,                            /* Priority of the task */
    &TFL->loop_task,              /* Task handle. */
    1);                           /* Core where the task should run */

    return btrue;
}

#ifdef USE_I2S
/**
 * @brief Set up some buffers and tables for feature extraction of audio samples. Must run once before starting audio capturing.
 * 
 * @return int - not used ATM
 */
int TFL_InitializeFeatures() {
  uint32_t samples_to_process = (TFL->mic->i2s_samples_to_get * TFL->mic->slice_dur)/TFL->mic->slice_stride;
  TFL->mic->mfcc = new MFCC(TFL->mic->num_coeff, samples_to_process, TFL->mic->num_filter, kAudioSampleFrequency, 300, 8000);
  TFL->mic->mfcc->set_preamp(TFL->mic->preamp);
  TFL->mic->mfcc->set_preemphasis(TFL->mic->preemphasis);
  MicroPrintf( PSTR( "MFCC %u initialized for %u samples, preamp: %u, preemphasis: %f"),TFL->mic->num_coeff,samples_to_process, TFL->mic->preamp, TFL->mic->preemphasis);
  return kTfLiteOk;
}

/**
 * @brief Computes features from every audio slice immediately after capturing it.
 * 
 * @param input - audio buffer
 * @param input_size - length auf audio input in samples (16-bit)
 * @param output_size - length of feature buffer in bytes (we use int8_t quantization)
 * @param output - feature buffer for one slice of audio
 * @param num_samples_read - not used anymore, to be removed
 * @return int 
 */
int TFL_GenerateFeatures(const int16_t* input, int input_size,
                                   int output_size, int8_t* output,
                                   size_t* num_samples_read) {

  float out_buf[output_size];

  TFL->mic->mfcc->mfcc_compute(input, out_buf);
  if(TFL->mic->num_coeff == 0){ // mfe only
    TFL->mic->mfcc->log10_normalize(out_buf, output_size, TFL->mic->db_floor);
  }
   
  float scale = TFL->input->params.scale;
  int32_t zero_point =  TFL->input->params.zero_point;
  float min_f = 1;
  float max_f = 0;

  for (size_t i = 0; i < output_size; ++i) {
    int32_t value = ((out_buf[i]/ scale) + zero_point);
    if(value <  -128){
      value = -128;
    }
    else if(value > 127){
      value = 127;
    }
    output[i] = value;
    // if(min_f>out_buf[i]) min_f = out_buf[i];
    // if(max_f<out_buf[i]) max_f = out_buf[i];
  }
  // MicroPrintf("%f %f %f %f %f %f %f %f %f %f %f %f %f",out_buf[0],out_buf[1] ,out_buf[2] ,out_buf[3] ,out_buf[4] ,out_buf[5] ,out_buf[6] ,out_buf[7] ,out_buf[8] ,out_buf[9] ,out_buf[10] ,out_buf[11] ,out_buf[12]);
  // MicroPrintf("min: %f max: %f",min_f,max_f);

  return kTfLiteOk;
}

/**
 * @brief Init I2S microphone. Pins must be configured in the "usual" Tasmota way. Some properties are variables stored in the descriptor.
 * 
 * @param descriptor - byte array passed from Berry. Arbitrary format - might change in the future!!
 * @return true - success
 * @return false - failure
 */
bool TFL_init_MIC(const uint8_t* descriptor){
   if (audio_i2s.in) {
      if(audio_i2s.in->getRxRate() != kAudioSampleFrequency || audio_i2s.in->getRxBitsPerSample() != kAudioSampleBits){
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: please configure microphone to 16 bits per sample at 16000 Hz"));
        return bfalse;
      }
      audio_i2s.in->startRx();
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: init mic"));
   }
   else{
      AddLog(LOG_LEVEL_ERROR, PSTR("TFL: could not connect to I2S driver"));
      return bfalse;
   }
  
  TFL->mic = new TFL_mic_ctx_t;
  TFL->mic->flags = 0;
  TFL_set_mic_config(descriptor);

  TFL->mic->feature_buffer_mutex = xSemaphoreCreateMutex();

  AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: MIC ctx created"));
  return btrue;
}

/**
 * @brief Function spawned as a task for capturing audio. Used for recording or inference.
 * 
 * @param arg - not used
 */
void TFL_capture_samples(void* arg) {
  MicroPrintf( PSTR( "Capture task started"));
  const int i2s_bytes_to_read = TFL->mic->i2s_samples_to_get * 2;  // according to slice duration

  const int buffer_size = (i2s_bytes_to_read * TFL->mic->slice_dur)/TFL->mic->slice_stride; // in bytes, current slice duration plus (potential) history data

  size_t samples_to_read;
  size_t bytes_read;
  int tf_status = 0;

  int16_t i2s_sample_buffer[buffer_size/2] = {0}; // in shorts, add the size to hold history data
  uint8_t  *i2s_byte_buffer = (uint8_t*)i2s_sample_buffer;
  uint32_t *i2s_long_buffer = (uint32_t*)i2s_sample_buffer;
  uint8_t  *i2s_read_buffer = i2s_byte_buffer + (buffer_size - i2s_bytes_to_read); // behind the history data, if slice duration != slice stride

  TFL_InitializeFeatures(); // TODO: check or not for success

  TFL->mic->flag.continue_audio_capture = 1;
  MicroPrintf( PSTR( "Enter capture samples loop"));

  while (TFL->mic->flag.continue_audio_capture == 1) {
    TFL->stats->mic_task_free_stack_bytes = uxTaskGetStackHighWaterMark(NULL);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    /* read slice data at once from i2s */
    // i2s_read(I2S_NUM, i2s_read_buffer, i2s_bytes_to_read, &bytes_read, pdMS_TO_TICKS(TFL->mic->slice_stride));
    esp_err_t err = i2s_channel_read(audio_i2s.in->getRxHandle(), (void*)i2s_read_buffer, i2s_bytes_to_read, &bytes_read, pdMS_TO_TICKS(TFL->mic->slice_stride));

    if (bytes_read <= 0) {
      MicroPrintf( PSTR( "Error %d in I2S, did read  %d bytes"), err, bytes_read);
    }
    else {
      if (bytes_read < i2s_bytes_to_read) {
       MicroPrintf(PSTR("Partial I2S read: %d"), bytes_read);
      }

      xSemaphoreTake(TFL->mic->feature_buffer_mutex, pdMS_TO_TICKS(TFL->mic->slice_stride) );

      tf_status = TFL_GenerateFeatures((const int16_t*)i2s_sample_buffer, buffer_size/2 , TFL->mic->slice_size,
                                            TFL->mic->feature_buffer + (TFL->mic->feature_buffer_idx * TFL->mic->slice_size),
                                            &samples_to_read);

      for(int i=0;i<(buffer_size - i2s_bytes_to_read)/4;i++){
        i2s_long_buffer[i] = i2s_long_buffer[i + ((buffer_size - i2s_bytes_to_read)/4)]; //move history to the front
      }

      TFL->mic->feature_buffer_idx += 1;
      if(TFL->mic->feature_buffer_idx == TFL->mic->slice_count){
        TFL->mic->feature_buffer_idx = 0;
      }
      TFL->option.new_input_data = 1;
      xSemaphoreGive(TFL->mic->feature_buffer_mutex);

    }
    // MicroPrintf( PSTR("t: %u"),xTaskGetTickCount()-xLastWakeTime);

    if(TFL->mic->flag.continue_audio_capture == 1) vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(TFL->mic->slice_stride) );
  }

  audio_i2s.in->stopRx();
  if(TFL->mic->mfcc != nullptr){
    delete TFL->mic->mfcc;
    TFL->mic->mfcc = nullptr;
  }
  MicroPrintf( PSTR("end capture task"));
  TFL->mic->flag.audio_capture_ended = 1;
  vTaskDelete(NULL);
}

/**
 * @brief Pass descriptor variables from Berry to the MIC context. Will also calculate some vars.
 * 
 * @param descriptor_buffer - byte array from Berry
 */
void TFL_set_mic_config(const uint8_t *descriptor_buffer){
  TFL_mic_descriptor_t *mic_descriptor = (TFL_mic_descriptor_t*)descriptor_buffer;
  // TFL->mic->channel_fmt = mic_descriptor->channel_fmt; // UNUSED!! - setup by I2S driver
  // TFL->mic->preamp = mic_descriptor->preamp; // UNUSED !!
  TFL->mic->preamp = audio_i2s.Settings->rx.gain / 16;  // setup by I2S driver
  TFL->mic->slice_dur = mic_descriptor->slice_dur;
  TFL->mic->slice_stride = mic_descriptor->slice_stride;
  TFL->mic->num_filter =  mic_descriptor->num_filter;
  TFL->mic->num_coeff =  mic_descriptor->num_coeff;
  TFL->mic->fft_bins = mic_descriptor->fft_bins;
  TFL->max_invocations = mic_descriptor->max_invocations;
  // now calculate the other settings
  TFL->mic->slice_size = mic_descriptor->num_coeff == 0 ? mic_descriptor->num_filter : mic_descriptor->num_coeff;
  TFL->mic->slice_count = (int32_t)((kObservationWindow/(float)TFL->mic->slice_stride) - 0.01); // floor(x) to int
  TFL->mic->preemphasis = (float)(mic_descriptor-> preemphasis)/100.0f;
  TFL->mic->i2s_samples_to_get = (TFL->mic->slice_stride * (kAudioSampleFrequency / 1000));
  TFL->mic->db_floor =  mic_descriptor->db_floor * -1;
  AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: filter: %u, coefficients: %u"), TFL->mic->num_filter, TFL->mic->num_coeff);
  AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: slice stride: %u ms -> slice count: %u, samples to read: %u"), TFL->mic->slice_stride, TFL->mic->slice_count, TFL->mic->i2s_samples_to_get);
}

/**
 * @brief Updates the input tensor with the data from the feature buffer, which works as a ring buffer and is a shared resource.
 * 
 */
void TFL_mic_feature_buf_to_input(){
  xSemaphoreTake(TFL->mic->feature_buffer_mutex, pdMS_TO_TICKS(TFL->mic->slice_stride) );
  // Copy feature buffer to input tensor
  int idx = TFL->mic->feature_buffer_idx + 1;  //oldest slice right after the newest slice
  if(idx == TFL->mic->slice_count) idx = 0;
  int slices_upperstack = TFL->mic->slice_count - idx;
  int slices_lowerstack = TFL->mic->slice_count - slices_upperstack;
  memcpy(TFL->mic->model_input_buffer,TFL->mic->feature_buffer + (idx * TFL->mic->slice_size), TFL->mic->slice_size * slices_upperstack);
  memcpy(TFL->mic->model_input_buffer +  (TFL->mic->slice_size * slices_upperstack),TFL->mic->feature_buffer, TFL->mic->slice_size * slices_lowerstack);
  xSemaphoreGive(TFL->mic->feature_buffer_mutex);
  return;
}

/**
 * @brief Helper function to stop audio capture task
 * 
 */
void TFL_stop_audio_capture(){
    MicroPrintf( PSTR("shall stop_capture_task"));
    if(TFL->mic->flag.continue_audio_capture == 0) return;
    TFL->mic->flag.continue_audio_capture = 0;
    uint32_t timeout = 0;
    while(TFL->mic->flag.audio_capture_ended == 0){
      if(timeout>3) break;
      vTaskDelay(pdMS_TO_TICKS(TFL->mic->slice_stride) );
      timeout++;
    }
    vSemaphoreDelete(TFL->mic->feature_buffer_mutex);
    delete[] TFL->mic->feature_buffer;
}
#endif //USE_I2S

/**
 * @brief Helper function to stop all running tasks
 * 
 */
void TFL_delete_tasks(){
  if(TFL == nullptr) return;
  TFL->option.running_loop = 0;
  while(TFL->option.loop_ended == 0){
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: task loop did stop"));
#ifdef USE_I2S
  if(TFL->mic != nullptr) {delete TFL->mic;} 
#endif //USE_I2S
#ifdef USE_TF_LITE_CSI
  TFL_stop_csi_capture();
#endif
  delete TFL;
  TFL = nullptr;
}

/**
 * @brief Starts inference task and the run loop. Should be terminated by signal from helper function.
 * 
 * @param pvParameters - not used
 */
void TFL_task_loop(void *pvParameters){
  uint8_t tensor_arena[TFL->TensorArenaSize];
  TFL->stats = new TFL_stats_t;
  tflite::AllOpsResolver resolver; //TODO: infer needed Ops from model??
  tflite::MicroInterpreter interpreter(
      TFL->model, resolver, tensor_arena, TFL->TensorArenaSize);
  int allocate_status = interpreter.AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    MicroPrintf( PSTR("AllocateTensors() failed"));
    goto loop_task_exit;
  }
  else{
    TFL->stats->used_arena_bytes = interpreter.arena_used_bytes();
  }
  // Obtain pointers to the model's input/output, we can use it externally
  TFL->input = interpreter.input(0);
  TFL->output = interpreter.output(0);

#ifdef USE_I2S
  if(TFL->option.use_mic == 1){
    TFL->mic->feature_buffer = new int8_t[TFL->mic->slice_size * TFL->mic->slice_count]();
    xTaskCreatePinnedToCore(TFL_capture_samples, "tfl_mic", 1024 * 5, NULL, 15, &TFL->mic->audio_capture_task, 0);
    if(TFL->mic->audio_capture_task == nullptr){
      MicroPrintf( PSTR("Creating capture task failed"));
      goto loop_task_exit;
    }
     MicroPrintf( PSTR("Created capture task"));
    TFL->mic->model_input_buffer = TFL->input->data.int8;
    vTaskDelay(pdMS_TO_TICKS(2000)); // wait for at least the time of the the microphone warm up
  }
#endif

// CSI runs completely callback-based - NO task needed!

  TFL->option.running_loop = 1;

// loop section
 MicroPrintf(PSTR("Enter task loop"));
  while (TFL->option.running_loop == 1)
  {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    TFL->stats->loop_task_free_stack_bytes = uxTaskGetStackHighWaterMark(NULL);

    while(TFL->option.delay_next_invocation == 1 && TFL->option.running_loop == 1){
      MicroPrintf(PSTR("delay_next_invocation"));
      vTaskDelay(10/ portTICK_PERIOD_MS);
    }
    TFL->option.delay_next_invocation = 1;

  #ifdef USE_I2S
    if(TFL->option.use_mic == 1){
      TFL->option.delay_next_invocation = 0; // Clean up later
      if(TFL->mic->flag.continue_audio_capture == 1){
        TFL_mic_feature_buf_to_input();
      }
    }
  #endif

  #ifdef USE_TF_LITE_CSI
    if(TFL->option.use_csi == 1 && TFL->csi && TFL->csi->capturing) {
      // Thread-safe buffer copy using mutex
      if (xSemaphoreTake(TFL->csi->buffer_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        
        // Read current index atomically
        portENTER_CRITICAL(&csi_spinlock);
        int current_idx = TFL->csi->feature_buffer_idx;
        portEXIT_CRITICAL(&csi_spinlock);
        
        // Get latest complete frame (one before current write position)
        int latest_idx = (current_idx - 1 + 10) % 10;
        
        int8_t* latest_features = TFL->csi->feature_buffer + (latest_idx * TFL->csi->feature_size);
        
        // Bounds checking for tensor input
        size_t copy_size = TFL->csi->feature_size;
        if (copy_size > TFL->input->bytes) {
          copy_size = TFL->input->bytes;
          AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: CSI feature truncated %d->%d"), TFL->csi->feature_size, copy_size);
        }
        
        // Copy to input tensor
        memcpy(TFL->input->data.int8, latest_features, copy_size);
        
        xSemaphoreGive(TFL->csi->buffer_mutex);
        
        TFL->option.new_input_data = 1;
        TFL->option.delay_next_invocation = 0;
      }
    }
  #endif

    if(TFL->option.new_input_data){
      TFL->option.running_invocation = 1;
      int invoke_status = interpreter.Invoke();
      if (invoke_status != kTfLiteOk) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TFL: Invoke failed"));
        TFL->option.running_loop = 0;
      }
      if(TFL->berry_output_buf != nullptr){
        memcpy(TFL->berry_output_buf,(int8_t*)TFL->output->data.data,TFL->berry_output_bufsize);
      }
      TFL->stats->invocations++;
      TFL->option.unread_output = 1;
      TFL->option.running_invocation = 0;
      TFL->option.new_input_data = 0;
    }
    if(TFL->option.running_loop == 1) vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000 / TFL->max_invocations));
  }

// end of loop section
loop_task_exit:
  delete TFL->stats;
#ifdef USE_I2S
  if(TFL->option.use_mic == 1) {TFL_stop_audio_capture();}
#endif //USE_I2S
#ifdef USE_TF_LITE_CSI
  TFL_stop_csi_capture();
#endif
  MicroPrintf(PSTR("end loop task"));
  TFL->option.loop_ended = 1;
  vTaskDelete( NULL );
}

// New function for full hex conversion without truncation
String CSI_HexToString(uint8_t* data, uint32_t length) {
  if (!data || !length) { return ""; }
  
  // Allocate string with exact size needed (2 chars per byte)
  String result;
  result.reserve(length * 2 + 1);
  
  char hex_char[3];
  for (uint32_t i = 0; i < length; i++) {
    snprintf(hex_char, sizeof(hex_char), "%02X", data[i]);
    result += hex_char;
  }
  
  return result;
}

/*********************************************************************************************\
 * Native functions mapped to Berry functions
\*********************************************************************************************/
extern "C" {

/**
 * @brief Create a context for a tensor flow session, that will later run in a task
 * 
 * @param vm 
 * @param type        BUF - generic byte buffer, MIC - microphone input, CSI - WiFi CSI input
 * @return btrue 
 * @return bfalse 
 */
  bbool be_TFL_begin(struct bvm *vm, const char* type, const uint8_t *descriptor, size_t len) {
    if (TFL_log_buffer == nullptr){
      TFL_log_buffer = xRingbufferCreate(1028, RINGBUF_TYPE_NOSPLIT);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: init log buffer"));
    }
#ifdef USE_TF_LITE_CSI
    if (TFL_training_buffer == nullptr) {
      TFL_training_buffer = xRingbufferCreate(2048, RINGBUF_TYPE_NOSPLIT);
    }
#endif

    TFL_delete_tasks();
    if(strlen(type) == 0){
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: context deleted"));
      return btrue;
    }
    TFL = new TFL_ctx_t{};
    TFL->options = 0;

    if(*(uint32_t*)type == 0x00465542){ //BUF
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: mode generic buffer"));
    }
    else if(*(uint32_t*)type == 0x0043494D){ //MIC
#ifdef USE_I2S
      if(descriptor && len==sizeof(TFL_mic_descriptor_t)){
        if(TFL_init_MIC(descriptor)){
          TFL->option.use_mic = 1;
        }
        else{
          return bfalse;
        }
      }
      else{
        AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: expected descriptor of size: %u"), sizeof(TFL_mic_descriptor_t));
        return bfalse;
      }
#else
      AddLog(LOG_LEVEL_ERROR, PSTR("TFL: firmware with I2S audio required !!"));
      return bfalse;
#endif //USE_I2S
    }
    else if(*(uint32_t*)type == 0x00495343){ //CSI
#ifdef USE_TF_LITE_CSI
      if(descriptor && len==sizeof(TFL_csi_descriptor_t)){
        if(TFL_init_CSI(descriptor)){
          TFL->option.use_csi = 1;
          AddLog(LOG_LEVEL_INFO, PSTR("TFL: CSI initialization SUCCESS"));
        }
        else{
          AddLog(LOG_LEVEL_ERROR, PSTR("TFL: CSI initialization FAILED"));
          return bfalse;
        }
      }
      else{
        AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: expected CSI descriptor of size: %u"), sizeof(TFL_csi_descriptor_t));
        return bfalse;
      }
#else
      AddLog(LOG_LEVEL_ERROR, PSTR("TFL: firmware with CSI support required !!"));
      return bfalse;
#endif //USE_TF_LITE_CSI
    }
    else{
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: unknown mode"));
      return bfalse;
    }
    if(TFL!=nullptr){
      AddLog(LOG_LEVEL_INFO, PSTR("TFL: start TFL context with type: %s"), type);
      TFL->option.init_done = 1;
      return btrue;
    }
    return bfalse;
  }

/**
 * @brief Load tensor flow lite model and then start the tensor flow session in a task
 * 
 * @param vm 
 * @param buf     Model in a byte buffer
 * @param size    Size of buffer, must be 8-byte-aligned (auto-calculated by Berry)
 * @param arena   Size of the Tensor Arena in the stack of the TFL task
 * @return btrue 
 * @return bfalse 
 */
  bbool be_TFL_load(struct bvm *vm, const uint8_t *model_buf, size_t model_size, const uint8_t *output_buf, size_t output_size,int arena) {
    if(TFL){
      if(TFL->option.init_done){
        TFL->model = tflite::GetModel(model_buf);
        if ( TFL->model->version() != TFLITE_SCHEMA_VERSION) {
          AddLog(LOG_LEVEL_INFO, PSTR("TFL: Model schema version %d not supported "
                      "version %d."),  TFL->model->version(), TFLITE_SCHEMA_VERSION);
          return bfalse;
        }
        if(model_size%8 != 0){
          AddLog(LOG_LEVEL_INFO, PSTR("TFL: model not 8-byte aligned"));
          return bfalse;
        }
        if(arena){
          TFL->TensorArenaSize = arena;
        }

        TFL->berry_output_buf = (int8_t*)output_buf;
        TFL->berry_output_bufsize = output_size;
        TFL_create_task();
        AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: starting TFL task, model sz: %u, allocated arena sz: %u"),model_size,TFL->TensorArenaSize);
        AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: Berry output buffer of size: %u"),output_size);
        return btrue;
      }
    }
    return bfalse;
  }

  /**
   * @brief Send new input data to the tensor flow session
   * 
   * @param vm 
   * @param buf                   Arbitrary data in a byte buffer, must fit to the TF model
   * @param size                  Size of buffer (auto-calculated by Berry)
   * @param quantize_to_int8      Optional: convert bytes to quantized int8 values
   * @return btrue 
   * @return bfalse 
   */

  bbool be_TFL_input(struct bvm *vm, const uint8_t *buf, size_t size, bbool quantize_to_int8){
    if(!TFL) return bfalse;
    if(TFL->option.running_loop == 1){
      uint32_t timeout = 0;
      int8_t* tensor_input_buffer = tflite::GetTensorData<int8_t>(TFL->input);
      while(!TFL->option.delay_next_invocation == 1) {
        if(timeout>4) return bfalse;;
        delay(5);
        timeout++;
      }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: imput new data and invoke"));
      if(quantize_to_int8){
        int16_t temp_int;
        for(int i = 0; i < size; i++){
          temp_int = buf[i];
          tensor_input_buffer[i] = (int8_t)(temp_int - 128);
        }
      } else {
        memcpy(tensor_input_buffer, buf,size);
      }
      TFL->option.delay_next_invocation = 0;
      TFL->option.new_input_data = 1;
      return btrue;
    }
    return bfalse;
  }

  /**
   * @brief Get copy of the output sensor of the tensor flow session
   * 
   * @param vm 
   * @param buf       Arbitrary data in a byte buffer, must fit in size to the TF model
   * @param size      Size of buffer (auto-calculated by Berry)
   * @return btrue    - new data
   * @return bfalse   - old data
   */
  bbool be_TFL_output(struct bvm *vm, const uint8_t *buf, size_t size){
    if(!TFL) return bfalse;
    if(TFL->option.running_loop == 0) return bfalse;
    if(TFL->option.unread_output == 1){
      AddLog(LOG_LEVEL_DEBUG, PSTR("TFL: read output  data"));
      if(TFL->output != nullptr){
        TFL->option.unread_output = 0;
        return btrue; //new data
      }
    }
    return bfalse; // old data
  }

/**
 * @brief Read from the logging buffer from the TFL tasks
 * 
 * @param vm 
 * @return const char* 
 */
  const char * be_TFL_log(struct bvm *vm){
      // first check for training data (CSI)
#ifdef USE_TF_LITE_CSI
      if (TFL_training_buffer) {
        size_t size;
        uint8_t * training_item = (uint8_t *)xRingbufferReceive(TFL_training_buffer, &size, 0);
        if (training_item != NULL) {
          be_pushstring(vm, CSI_HexToString(training_item, size).c_str());
          vRingbufferReturnItem(TFL_training_buffer, (void *)training_item);
          return be_tostring(vm, -1);
        }
      }
#endif
      // fall back to regular logs
      if (TFL_log_buffer) {
        size_t size;
        char * item = (char *)xRingbufferReceive(TFL_log_buffer, &size, pdMS_TO_TICKS(5));
        if(item != NULL){
          be_pushstring(vm, item);
          vRingbufferReturnItem(TFL_log_buffer, (void *)item);
          return be_tostring(vm, -1);
        }
      }
      return NULL;
  }

/**
 * @brief Shows statistics about the model and the running TFL session
 * 
 * @param vm 
 * @return json string
 */
  const char * be_TFL_stats(struct bvm *vm){
      // Early validation - check if TFL system is properly initialized
      if(!TFL || !TFL->stats) {
          be_pushstring(vm, "{\"error\":\"tfl_not_initialized\"}");
          return be_tostring(vm, -1);
      }

      const size_t size = 2048;  // Increased buffer size
      char * s = (char*)calloc(size, 1);
      if (!s) {
          be_pushstring(vm, "{\"error\":\"memory_alloc_failed\"}");
          return be_tostring(vm, -1);
      }

      uint32_t pos = 0;
      uint32_t inc = 0;
      
      // Start JSON with comprehensive system info
      inc = snprintf_P(s + pos, size - pos, 
          PSTR("{\"system\":{\"initialized\":true,\"mode\":\""));
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Add mode information
      if (TFL->option.use_mic) {
          inc = snprintf_P(s + pos, size - pos, PSTR("MIC"));
      } else if (TFL->option.use_csi) {
          inc = snprintf_P(s + pos, size - pos, PSTR("CSI"));
      } else {
          inc = snprintf_P(s + pos, size - pos, PSTR("BUFFER"));
      }
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      inc = snprintf_P(s + pos, size - pos, PSTR("\",\"running\":%s,"), 
                      TFL->option.running_loop ? "true" : "false");
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      inc = snprintf_P(s + pos, size - pos, PSTR("\"max_invocations\":%u},"), TFL->max_invocations);
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Model information section
      inc = snprintf_P(s + pos, size - pos, PSTR("\"model\":{"));
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Input tensor details
      if (TFL->input && TFL->input->dims) {
          inc = snprintf_P(s + pos, size - pos, PSTR("\"input\":{\"shape\":["));
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
          
          uint32_t dims = TFL->input->dims->size;
          for(int i = 0; i < dims; i++) {
              inc = snprintf_P(s + pos, size - pos, PSTR("%u"), TFL->input->dims->data[i]);
              if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
              pos += inc;
              
              if (i != dims - 1) {
                  inc = snprintf_P(s + pos, size - pos, PSTR(","));
                  if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
                  pos += inc;
              }
          }
          
          // Add input tensor details
          inc = snprintf_P(s + pos, size - pos, PSTR("],\"type\":%u,\"bytes\":%u,\"quantization\":{"),
                          TFL->input->type, TFL->input->bytes);
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
          
          inc = snprintf_P(s + pos, size - pos, PSTR("\"scale\":%.6f,\"zero_point\":%d}},"),
                          TFL->input->params.scale, TFL->input->params.zero_point);
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
      } else {
          inc = snprintf_P(s + pos, size - pos, PSTR("\"input\":{\"shape\":[],\"type\":0,\"bytes\":0,\"quantization\":{\"scale\":0.0,\"zero_point\":0}},"));
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
      }

      // Output tensor details
      if (TFL->output && TFL->output->dims) {
          inc = snprintf_P(s + pos, size - pos, PSTR("\"output\":{\"shape\":["));
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
          
          uint32_t dims = TFL->output->dims->size;
          for(int i = 0; i < dims; i++) {
              inc = snprintf_P(s + pos, size - pos, PSTR("%u"), TFL->output->dims->data[i]);
              if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
              pos += inc;
              
              if (i != dims - 1) {
                  inc = snprintf_P(s + pos, size - pos, PSTR(","));
                  if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
                  pos += inc;
              }
          }
          
          // Add output tensor details
          inc = snprintf_P(s + pos, size - pos, PSTR("],\"type\":%u,\"bytes\":%u,\"quantization\":{"),
                          TFL->output->type, TFL->output->bytes);
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
          
          inc = snprintf_P(s + pos, size - pos, PSTR("\"scale\":%.6f,\"zero_point\":%d}},"),
                          TFL->output->params.scale, TFL->output->params.zero_point);
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
      } else {
          inc = snprintf_P(s + pos, size - pos, PSTR("\"output\":{\"shape\":[],\"type\":0,\"bytes\":0,\"quantization\":{\"scale\":0.0,\"zero_point\":0}},"));
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
      }

      // Arena information
      inc = snprintf_P(s + pos, size - pos, PSTR("\"arena\":{\"total_size\":%u,\"used_bytes\":%u,\"utilization\":%.1f}"),
                      TFL->TensorArenaSize, TFL->stats->used_arena_bytes,
                      TFL->TensorArenaSize > 0 ? (100.0 * TFL->stats->used_arena_bytes / TFL->TensorArenaSize) : 0.0);
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Close model section
      inc = snprintf_P(s + pos, size - pos, PSTR("},\"session\":{"));
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Runtime statistics
      inc = snprintf_P(s + pos, size - pos, PSTR("\"invocations\":%u,\"invocation_rate\":%.2f,"),
                      TFL->stats->invocations,
                      TFL->stats->invocations > 0 ? (TFL->stats->invocations / (millis() / 1000.0)) : 0.0);
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      inc = snprintf_P(s + pos, size - pos, PSTR("\"memory\":{\"loop_stack_free\":%u,"),
                      TFL->stats->loop_task_free_stack_bytes);
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Optional audio stats
      if(TFL->option.use_mic == 1 && TFL->stats->mic_task_free_stack_bytes > 0){
          inc = snprintf_P(s + pos, size - pos, PSTR("\"audio_stack_free\":%u,"), TFL->stats->mic_task_free_stack_bytes);
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
      }

  #ifdef USE_TF_LITE_CSI
      // Comprehensive CSI statistics
      if(TFL->option.use_csi == 1){
          inc = snprintf_P(s + pos, size - pos, PSTR("\"csi\":{"));
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
          
          if(TFL->csi){
              inc = snprintf_P(s + pos, size - pos, 
                  PSTR("\"packets_received\":%u,\"packets_dropped\":%u,\"drop_rate\":%.2f,"),
                  TFL->csi->packets_received, TFL->csi->packets_dropped,
                  TFL->csi->packets_received > 0 ? 
                  (100.0 * TFL->csi->packets_dropped / (TFL->csi->packets_received + TFL->csi->packets_dropped)) : 0.0);
              if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
              pos += inc;
              
              inc = snprintf_P(s + pos, size - pos, 
                  PSTR("\"feature_size\":%u,\"buffer_size\":%u,\"capturing\":%s"),
                  TFL->csi->feature_size, TFL->csi->feature_buffer_size,
                  TFL->csi->capturing ? "true" : "false");
              if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
              pos += inc;
          } else {
              inc = snprintf_P(s + pos, size - pos, PSTR("\"error\":\"csi_context_null\""));
              if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
              pos += inc;
          }
          
          inc = snprintf_P(s + pos, size - pos, PSTR("},"));
          if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
          pos += inc;
      }
  #endif

      // Performance metrics
      inc = snprintf_P(s + pos, size - pos, PSTR("\"performance\":{\"max_invocations_sec\":%u}}"), TFL->max_invocations);
      if (inc < 0 || (pos + inc) >= size) goto buffer_overflow;
      pos += inc;

      // Final safety check
      if (pos >= size) {
          goto buffer_overflow;
      }

      // Success - push the string
      be_pushstring(vm, s);
      free(s);
      return be_tostring(vm, -1);

  buffer_overflow:
    // Handle buffer overflow gracefully - create error message properly
    free(s);
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), 
             "{\"error\":\"buffer_overflow\",\"buffer_size\":2048,\"required_size\":%u}", 
             pos);
    
    be_pushstring(vm, error_msg);
    return be_tostring(vm, -1);
  }

} //extern "C"

#endif // USE_BERRY_TF_LITE
#endif  // USE_BERRY