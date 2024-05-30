/*
  xdrv_52_3_berry_img.ino - Berry scripting language, native functions

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

#ifdef USE_BERRY_IMAGE

#include "esp_camera.h"

#ifdef USE_BERRY_LVGL
#include "lvgl.h"
#endif // USE_BERRY_LVGL

/*********************************************************************************************\
 *
\*********************************************************************************************/

typedef struct {
    uint8_t * buf = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    pixformat_t format = PIXFORMAT_JPEG;
} image_t;

/*********************************************************************************************\
 * helper functions
\*********************************************************************************************/

struct be_img_util {
    static bool from_jpg(image_t *img, uint8_t* buffer, size_t len) {
      uint16_t width, height;
      if(get_jpeg_size(buffer, len, &width, &height) != true){
        return false;
      }
      pixformat_t format = PIXFORMAT_JPEG;
      return from_buffer(img, buffer, len, width, height, format);
    }

    static bool from_buffer(image_t *img, uint8_t* buffer, size_t len, uint16_t w, uint16_t h, pixformat_t f) {
      if(img->buf != nullptr) {
        free(img->buf);
      }
      img->buf = (uint8_t *)heap_caps_malloc((len)+4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if(img->buf) {
        memcpy(img->buf,buffer,len);
        img->len = len;
        img->format = f;
        img->width = w;
        img->height = h;
        return true;
      }
      return false;
    }
};


/*********************************************************************************************\
 * Native functions mapped to Berry functions
\*********************************************************************************************/


extern "C" {
  image_t* be_get_image_instance(struct bvm *vm);
  image_t* be_get_image_instance(struct bvm *vm) {
    be_getmember(vm, 1, ".p");
    image_t * img = (image_t *) be_tocomptr(vm, -1);
    be_pop(vm, 1);
    if(!img){
      be_raise(vm, "img_error", "no store instance");
    }
    return img;
  }

  int be_img_init(struct bvm *vm);
  int be_img_init(struct bvm *vm) {
    image_t *img = new image_t;
    be_pushcomptr(vm, (void*)img);
    be_setmember(vm, 1, ".p");
    be_return_nil(vm);
  }

  int be_img_from_jpg(struct bvm *vm);
  int be_img_from_jpg(struct bvm *vm) {
    int32_t argc = be_top(vm); // Get the number of arguments
    if (argc == 2 && be_isbytes(vm, 2)){
      size_t src_buf_len;
      uint8_t* src_buf = (uint8_t*) be_tobytes(vm, 2, &src_buf_len);
      be_pop(vm, 1);
      image_t * img = be_get_image_instance(vm);
      if(img){
        if(be_img_util::from_jpg(img,src_buf,src_buf_len) == false){
          be_raise(vm, "img_error", "could not store from jpg buffer");
        }
      }
    }
    else{
      be_raise(vm, "img_error", "wrong args for jpg buffer");
    }

    be_return(vm);
  }

  int be_img_from_buffer(struct bvm *vm); // (bytes(),width,height,format)
  int be_img_from_buffer(struct bvm *vm) {
    int32_t argc = be_top(vm); // Get the number of arguments
    if (argc == 5 && be_isbytes(vm, 2) && be_isint(vm, 3) && be_isint(vm, 4) && be_isint(vm, 5)){
      size_t src_buf_len;
      uint8_t* src_buf = (uint8_t*) be_tobytes(vm, 2, &src_buf_len);
      be_pop(vm, 2);
      image_t * img = be_get_image_instance(vm);
      if(img){
        if(img->buf != nullptr){
          free(img->buf);
        }
        uint16_t width = be_toint(vm, 3);
        uint16_t height = be_toint(vm, 4);
        int format = be_toint(vm, 5);
        if(be_img_util::from_buffer(img,src_buf,src_buf_len, width, height, (pixformat_t)format) == false){
          be_raise(vm, "img_error", "could not store from byte buffer");
        }
      }
    }
    be_return(vm);
  }

int be_img_lv_img_dsc(bvm *vm, void *image);
int be_img_lv_img_dsc(bvm *vm, void *image) {
#ifdef USE_BERRY_LVGL
    lv_obj_t *_i;
    image_t * img = be_get_image_instance(vm);
    if(!img){
      be_raise(vm, "img_error", "no image instance");
      be_return(vm);
    }
    int32_t argc = be_top(vm); // Get the number of arguments
    if (argc == 2 && be_isinstance(vm, 2)){
      be_getglobal(vm, "lv_image");
      be_getmember(vm, 2, "_p");
      _i = (lv_obj_t *)be_tocomptr(vm, -1);
      be_pop(vm, 1);  // remove _p attribute
    }
    if(!_i) {
      be_raise(vm, "img_error", "no LV image to set");
      be_return(vm);
    }
    if(img->format != PIXFORMAT_RGB565){
      be_raise(vm, "img_error", "only RGB565 supported");
      be_return(vm);
    }
    lv_image_dsc_t _img_dsc;
    _img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    _img_dsc.header.w = img->width;
    _img_dsc.header.h = img->height;
    _img_dsc.header.cf = LV_COLOR_FORMAT_NATIVE;
    _img_dsc.data_size = img->len;
    _img_dsc.data = (const uint8_t*)img->buf;
    lv_image_set_src(_i, &_img_dsc);
    be_return_nil(vm);
#endif // USE_BERRY_LVGL
  }

  int be_img_get_buffer(struct bvm *vm); //(roi)
  int be_img_get_buffer(struct bvm *vm) {
    image_t * img = be_get_image_instance(vm);
    if(img){
      be_pushbytes(vm, img->buf, img->len);
    }
    be_return(vm);
  }

  int be_img_convert_to(struct bvm *vm); // (pixformat)
  int be_img_convert_to(struct bvm *vm) {
    image_t * img = be_get_image_instance(vm);
    if(!img){
      be_raise(vm, "img_error", "no image instance");
      be_return(vm);
    }
    if(img->len == 0) {
      be_raise(vm, "img_error", "no image data");
      be_return(vm);
    }

    int32_t argc = be_top(vm);
    if (argc == 2 && be_isint(vm, 2)) {
      uint32_t format = be_toint(vm, 2);
      union{
        uint8_t* temp_buf = nullptr;
        uint16_t* temp_buf_16;
      };
      if(img->format == format){
        be_raise(vm, "img_error", "no format change");
        be_return(vm);
      }
      uint16_t bpp = 3; // most likely byte-per-pixel value
      size_t pixel_count = img->width * img->height ;
      size_t  temp_buf_len = pixel_count * bpp;
      temp_buf = (uint8_t *)heap_caps_malloc((temp_buf_len)+4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if(temp_buf == nullptr) {
        be_raise(vm, "img_error", "not enough heap");
        be_return_nil(vm);
      }
      // if(!jpg2rgb888(img->buf, img->len, temp_buf, jpg_scale_t(1))){
      if(!fmt2rgb888((const uint8_t *)img->buf, img->len, pixformat_t(format), temp_buf)){
        free(temp_buf);
        be_raise(vm, "img_error", "not enough heap");
        be_return_nil(vm);
      }
      switch(format){
        case PIXFORMAT_GRAYSCALE: // always from temporary RGB88
          {
            uint8_t r, g, b;
            for (uint32_t cnt=0; cnt<temp_buf_len; cnt+=3) {
              r = temp_buf[cnt];
              g = temp_buf[cnt+1];
              b = temp_buf[cnt+2];
              temp_buf[cnt/3] = (r + g + b) / 3;
            }
            temp_buf_len = img->width * img->height;
          }
          break;
        case PIXFORMAT_RGB565: // always from temporary RGB88
            {
              temp_buf_len = pixel_count * 2;
              uint8_t red, grn, blu;
              uint8_t r, g, b;
              int from = 0;
              for (uint32_t i=0; i<pixel_count; i+=2) {
                blu = temp_buf[from++];
                grn = temp_buf[from++];
                red = temp_buf[from++];
                b = (blu >> 3) & 0x1f;
                g = ((grn >> 2) & 0x3f) << 5;
                r = ((red >> 3) & 0x1f) << 11;
                temp_buf_16[i] = (uint16_t)(r | g | b);
              }
            }
          // jpg2rgb565(img->buf, img->len, temp_buf , JPG_SCALE_NONE);
          break;
        case PIXFORMAT_RGB888:
         // should already be there
          break;
        case PIXFORMAT_JPEG:
          fmt2jpg(img->buf, img->len, img->width, img->height, img->format, 20, &temp_buf, &temp_buf_len);
          break;
        default:
          be_raise(vm, "img_error", "format not supported");
          free(temp_buf);
          be_return_nil(vm);
         break;
      }
      free(img->buf);
      img->len = temp_buf_len;
      img->buf = (uint8_t*)heap_caps_realloc((void*)temp_buf, img->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // shrinking should never fail ...
      img->format = pixformat_t(format);
    }
    be_return(vm);
  }

  int be_img_info(struct bvm *vm);
  int be_img_info(struct bvm *vm) {
    image_t * img = be_get_image_instance(vm);
    if(!img){
      be_raise(vm, "img_error", "no image instance");
      be_return(vm);
    }
    be_newobject(vm, "map");
    be_map_insert_int(vm, "buf_addr", (uint32_t)img->buf);
    be_map_insert_int(vm, "size", img->len);
    be_map_insert_int(vm, "width", img->width);
    be_map_insert_int(vm, "height", img->height);
    be_map_insert_int(vm, "format", img->format);
    be_pop(vm, 1);
    be_return(vm);
  }

} //extern "C"

#endif // USE_BERRY_JPEG

#endif  // USE_BERRY
