/*
  xdrv_52_3_berry_webcam.ino - Berry scripting language, native functions

  Copyright (C) 2024 Christian Baars & Stephan Hadinger, Berry language by Guan Wenliang https://github.com/Skiars/berry

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

#if defined(USE_WEBCAM)
#ifdef USE_WEBCAM_BERRY
#define USE_WEBCAM_SETUP_ONLY

#include "esp_camera.h"
#include "esp_jpg_decode.h"

const be_const_member_t be_webcam_format_constants[] = {
    { "GRAYSCALE",(int32_t) PIXFORMAT_GRAYSCALE},
    { "JPEG",(int32_t) PIXFORMAT_JPEG},
    { "RGB565", (int32_t) PIXFORMAT_RGB565 },
    { "RGB888", (int32_t) PIXFORMAT_RGB888 },
};

const size_t be_webcam_format_constants_size = sizeof(be_webcam_format_constants)/sizeof(be_webcam_format_constants[0]);

/*********************************************************************************************\
 * Native functions mapped to Berry functions
\*********************************************************************************************/
extern "C" {

  struct {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t mode = 0;
    uint8_t format = 0;
    uint8_t state = 0;
  } WcBerry;

    // virtual member
  int be_webcam_member(bvm *vm);
  int be_webcam_member(bvm *vm) {
    be_const_module_member_raise(vm, be_webcam_format_constants, be_webcam_format_constants_size);
    be_return(vm);
  }

  int be_webcam_init(struct bvm *vm);
  int be_webcam_init(struct bvm *vm) {
    WcInit();
    be_return(vm);
  }

  int be_webcam_setup(struct bvm *vm);
  int be_webcam_setup(struct bvm *vm) {
    
    int32_t argc = be_top(vm); // Get the number of arguments
    if (argc == 1) {
      WcBerry.mode = be_toint(vm, 1);
    }
    else{
      be_raise(vm, "webcam_error", "need mode");
      be_return_nil(vm);
    }
    
    int result = WcSetup(WcBerry.mode);
    if (result > 0) {
      camera_fb_t *wc_fb = esp_camera_fb_get();
      if(wc_fb){
        WcBerry.width = wc_fb->width;
        WcBerry.height = wc_fb->height;
      }
    }
    be_pushint(vm, result);
    be_pop(vm, 1);
    be_return(vm);
  }

  int be_webcam_get_image(struct bvm *vm);
  int be_webcam_get_image(struct bvm *vm) {

    camera_fb_t *wc_fb = esp_camera_fb_get();
    if (!wc_fb) {
      be_raise(vm, "webcam_error", "could not get frame");
      be_return_nil(vm);
    }
    uint8_t * buffer = wc_fb->buf;
    size_t length = wc_fb->len;
    WcBerry.width = wc_fb->width;
    WcBerry.height = wc_fb->height;
    uint8_t * temp_buffer;
    int error = 0;

    int32_t argc = be_top(vm); // Get the number of arguments
    if (argc == 1) {
      WcBerry.format = be_toint(vm, 1);
      uint32_t bmp_size = wc_fb->width * wc_fb->height;
      switch(WcBerry.format) {
        case PIXFORMAT_GRAYSCALE: // mono
          buffer = (uint8_t *)heap_caps_malloc((bmp_size)+4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if(buffer){
            temp_buffer = (uint8_t *)heap_caps_malloc((bmp_size * 3)+4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(temp_buffer){
              if (fmt2rgb888(wc_fb->buf, wc_fb->len, PIXFORMAT_JPEG , temp_buffer)) {
                for(int i = 0; i < bmp_size; i++){
                  const int j = i * 3;
                  buffer[i] = (temp_buffer[j] +  temp_buffer[j+1] + temp_buffer[j+2])/3;
                }
                length = bmp_size;
                free(temp_buffer);
              }
              else{
                error = 1;
                be_raise(vm, "webcam_error", "decode error for GRAYSCALE");
              }
            }
          }
          break;
        case PIXFORMAT_RGB565: // rgb565
          buffer = (uint8_t *)heap_caps_malloc((bmp_size * 2)+4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if(buffer){
            if (!jpg2rgb565(wc_fb->buf, wc_fb->len, buffer , JPG_SCALE_NONE)) {
              error = 1;
              be_raise(vm, "webcam_error", "decode error for RGB565");
            }
            else{
              length = bmp_size * 2;
            }
          }
          break;
        case PIXFORMAT_RGB888: // rgb888
          buffer = (uint8_t *)heap_caps_malloc((bmp_size * 3)+4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if(buffer){
            if (!fmt2rgb888(wc_fb->buf, wc_fb->len, PIXFORMAT_JPEG , buffer)) {
              error = 1;
              be_raise(vm, "webcam_error", "decode error for RGB888");
            }
            else{
              length = bmp_size * 3;
            }
          }
          break;
        case PIXFORMAT_JPEG:
          // AddLog(LOG_LEVEL_INFO, PSTR("BRY: no conversion") );
          break;
        default:
          error = 1;
          be_raise(vm, "webcam_error", "unknown format");
      }
    }
    if(error  == 0){
      be_pushbytes(vm, buffer, length);
      WcBerry.width = wc_fb->width;
      WcBerry.height = wc_fb->height;
    }
    esp_camera_fb_return(wc_fb);
    if(WcBerry.format != PIXFORMAT_JPEG) {
      free(buffer);
    }
    if(error  == 0){
      be_return(vm);
    }
    else {
      be_return_nil(vm);
    }
  }


  // webcam.info(void) -> map
  int be_webcam_info(struct bvm *vm);
  int be_webcam_info(struct bvm *vm) {
    be_newobject(vm, "map");
    be_map_insert_int(vm, "mode", WcBerry.mode);
    be_map_insert_int(vm, "format", WcBerry.format);
    be_map_insert_int(vm, "width", WcBerry.width);
    be_map_insert_int(vm, "height", WcBerry.height);

    be_pop(vm, 1);
    be_return(vm);
  }
} //extern "C"

#endif // USE_WEBCAM_BERRY
#endif // USE_WEBCAM

#endif  // USE_BERRY
