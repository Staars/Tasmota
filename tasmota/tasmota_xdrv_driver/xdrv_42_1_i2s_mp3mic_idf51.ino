/*
  xdrv_42_i2s_audio.ino - Audio dac support for Tasmota

  Copyright (C) 2021  Gerhard Mutz and Theo Arends

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

#if ESP_IDF_VERSION_MAJOR >= 5
#ifdef ESP32
#if (defined(USE_I2S_AUDIO) && defined(USE_I2S_MIC))

i2s_chan_handle_t rx_handle = nullptr;

uint32_t SpeakerMic(uint8_t spkr) {
  esp_err_t err = ESP_OK;
  if(rx_handle == nullptr){
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    switch (audio_i2s.Settings->rx.mode){
      case 1:
          {
          i2s_pdm_rx_config_t pdm_rx_cfg = {
          .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(audio_i2s.Settings->rx.sample_rate),
          /* The default mono slot is the left slot (whose 'select pin' of the PDM microphone is pulled down) */
          .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
          .gpio_cfg = {
              .clk = (gpio_num_t)Pin(GPIO_I2S_WS), //legacy setting
              .din = (gpio_num_t)Pin(GPIO_I2S_DIN),
              .invert_flags = {
                  .clk_inv = false,
              },
          },
        };
        pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
        err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);}
        break;
      default: // same as 0
          {        
          i2s_std_config_t rx_std_cfg = {
          .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(audio_i2s.Settings->rx.sample_rate),
          .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
          .gpio_cfg = {
              .mclk = (gpio_num_t)Pin(GPIO_I2S_MCLK),
              .bclk = (gpio_num_t)Pin(GPIO_I2S_BCLK),
              .ws   = (gpio_num_t)Pin(GPIO_I2S_WS),
              .dout = (gpio_num_t)Pin(GPIO_I2S_DOUT),
              .din  = (gpio_num_t)Pin(GPIO_I2S_DIN),
              .invert_flags = {
                  .mclk_inv = false,
                  .bclk_inv = false,
                  .ws_inv   = false,
                  },
              },
          };
          rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
          i2s_channel_init_std_mode(rx_handle, &rx_std_cfg);}
      break;
    }
    err = i2s_channel_enable(rx_handle);
  }
  audio_i2s.mode = spkr;
  return err;
}

#include <layer3.h>
#include <types.h>

// micro to mp3 file or stream
void mic_task(void *arg){
  int8_t error = 0;
  uint8_t *ucp;
  int written;
  shine_config_t  config;
  shine_t s = nullptr;
  uint16_t samples_per_pass;
  File mp3_out = (File)nullptr;
  int16_t *buffer = nullptr;
  uint16_t bytesize;
  uint16_t bwritten;
  uint32_t ctime;
  uint32_t gain = audio_i2s.Settings->rx.gain;

  if (!audio_i2s.use_stream) {
    mp3_out = ufsp->open(audio_i2s.mic_path, "w");
    if (!mp3_out) {
      error = 1;
      goto exit;
    }
  } else {
    if (!audio_i2s.stream_active) {
      error = 2;
      audio_i2s.use_stream = 0;
      goto exit;
    }
    audio_i2s.client.flush();
    audio_i2s.client.setTimeout(3);
    audio_i2s.client.print("HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/mpeg;\r\n\r\n");

   //  Webserver->send(200, "application/octet-stream", "");
    //"Content-Type: audio/mp3;\r\n\r\n");
  }

  shine_set_config_mpeg_defaults(&config.mpeg);

  if (audio_i2s.Settings->rx.slot_type == 0) {
    config.mpeg.mode = MONO;
  } else {
    config.mpeg.mode = STEREO;
  }
  config.mpeg.bitr = 128;
  config.wave.samplerate = audio_i2s.Settings->rx.sample_rate;
  config.wave.channels = (channels)(audio_i2s.Settings->rx.slot_type + 1);

  if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0) {
    error = 3;
    goto exit;
  }

  s = shine_initialise(&config);
  if (!s) {
    error = 4;
    goto exit;
  }

  samples_per_pass = shine_samples_per_pass(s);
  bytesize = samples_per_pass * 2 * (audio_i2s.Settings->rx.slot_type + 1);

  buffer = (int16_t*)malloc(bytesize);
  if (!buffer) {
    error = 5;
    goto exit;
  }

  ctime = TasmotaGlobal.uptime;


  while (!audio_i2s.mic_stop) {
      size_t bytes_read;
      i2s_channel_read(rx_handle, (void*)buffer, bytesize, &bytes_read, (100 / portTICK_PERIOD_MS));

      if (gain > 1) {
        // set gain
        for (uint32_t cnt = 0; cnt < bytes_read / 2; cnt++) {
          buffer[cnt] *= gain;
        }
      }
      ucp = shine_encode_buffer_interleaved(s, buffer, &written);

      if (!audio_i2s.use_stream) {
        bwritten = mp3_out.write(ucp, written);
        if (bwritten != written) {
          break;
        }
      } else {
        audio_i2s.client.write((const char*)ucp, written);

        if (!audio_i2s.client.connected()) {
          break;
        }
      }
      audio_i2s.recdur = TasmotaGlobal.uptime - ctime;
  }

  ucp = shine_flush(s, &written);

  if (!audio_i2s.use_stream) {
    mp3_out.write(ucp, written);
  } else {
    audio_i2s.client.write((const char*)ucp, written);
  }


exit:
  if (s) {
    shine_close(s);
  }
  if (mp3_out) {
    mp3_out.close();
    AddLog(LOG_LEVEL_INFO, PSTR("I2S: MP3 file closed"));
  }
  if (buffer) {
    free(buffer);
  }

  if (audio_i2s.use_stream) {
    audio_i2s.client.stop();
  }

  SpeakerMic(0);
  audio_i2s.mic_stop = 0;
  audio_i2s.mic_error = error;
  AddLog(LOG_LEVEL_INFO, PSTR("mp3task result code: %d"), error);
  audio_i2s.mic_task_h = 0;
  audio_i2s.recdur = 0;
  audio_i2s.stream_active = 0;
  vTaskDelete(NULL);

}

int32_t i2s_record_shine(char *path) {
esp_err_t err = ESP_OK;

  // if (audio_i2s.mic_port == 0) {
    if (audio_i2s.decoder || audio_i2s.mp3) return 0;
  // }

  // err = SpeakerMic(0);
  // if (err) {
  //   if (audio_i2s.mic_port == 0) {
  //     SpeakerMic(1);
  //   }
  //   AddLog(LOG_LEVEL_INFO, PSTR("mic init error: %d"), err);
  //   return err;
  // }

  strlcpy(audio_i2s.mic_path, path, sizeof(audio_i2s.mic_path));

  audio_i2s.mic_stop = 0;

  uint32_t stack = 4096;

  audio_i2s.use_stream = !strcmp(audio_i2s.mic_path, "stream.mp3");

  if (audio_i2s.use_stream) {
    stack = 8000;
  }

  err = xTaskCreatePinnedToCore(mic_task, "MIC", stack, NULL, 3, &audio_i2s.mic_task_h, 1);

  return err;
}

void Cmd_MicRec(void) {
if (audio_i2s.Settings->rx.mp3_encoder == 0) {
  if (XdrvMailbox.data_len > 0) {
    if (!strncmp(XdrvMailbox.data, "-?", 2)) {
      Response_P("{\"I2SREC-duration\":%d}", audio_i2s.recdur);
    } else {
      i2s_record_shine(XdrvMailbox.data);
      ResponseCmndChar(XdrvMailbox.data);
    }
  } else {
    if (audio_i2s.mic_task_h) {
      // stop task
      audio_i2s.mic_stop = 1;
      while (audio_i2s.mic_stop) {
        delay(1);
      }
      ResponseCmndChar_P(PSTR("Stopped"));
    }
  }
}
else{
  ResponseCmndChar_P(PSTR("I2S: need PSRAM for MP3 recording"));
}
}

// mic gain in factor not percent
void Cmd_MicGain(void) {
  if ((XdrvMailbox.payload >= 0) && (XdrvMailbox.payload <= 256)) {
      audio_i2s.Settings->rx.gain = XdrvMailbox.payload;
  }
  ResponseCmndNumber(audio_i2s.Settings->rx.gain);
}

#endif // USE_I2S_AUDIO
#endif // ESP32
#endif // ESP_IDF_VERSION_MAJOR >= 5