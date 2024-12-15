/*
  xdrv_52_3_berry_tamp.ino - Berry scripting language, Tamp compression library

  Copyright (C) 2024 Stephan Hadinger & Christian Baars, Berry language by Guan Wenliang https://github.com/Skiars/berry

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

// Mappgin from internal light and a generic `light_state` Berry class

#ifdef USE_BERRY
#ifdef USE_TAMP_COMPRESSION

#include "be_mapping.h"
#include <string.h>
#include <tamp_compressor.h>
#include <tamp_decompressor.h>

extern "C" {
  /*********************************************************************************************\
   * Native functions mapped to Berry functions
   * 
   * import tamp
   * 
  \*********************************************************************************************/

  struct{
    TampCompressor compressor;
    TampDecompressor decompressor;
    TampConf conf = {
      .window =  10,
      .literal = 8,
      .use_custom_dictionary = false
      };
    unsigned char * window;
    int last_error = 0;
  }TAMP;

  int be_tamp_comp_init(bvm *vm, uint16_t window, uint16_t literal, bool use_custom_dictionary){
    int32_t argc = be_top(vm);
    TAMP.conf.window = (argc > 0) ? window & 0xf : 10;
    #ifdef CONFIG_TAMP_ESP32
    TAMP.conf.literal = 8; // will crash otherwise
    #else
    TAMP.conf.literal = (argc > 1) ? literal & 0xf : 8;
    #endif
    TAMP.conf.use_custom_dictionary = (argc > 2) ? use_custom_dictionary : false;
    if(TAMP.window != nullptr){
      free(TAMP.window );
    }
    TAMP.window = (unsigned char *)malloc(1 << TAMP.conf.window);
    int error = tamp_compressor_init(&TAMP.compressor, &TAMP.conf, TAMP.window);
    AddLog(LOG_LEVEL_DEBUG, "TAM: init compressor(%u,%u,%u) with error: %d", TAMP.conf.window, TAMP.conf.literal, TAMP.conf.use_custom_dictionary, error);
    return error;
  }

  int be_tamp_comp_run(bvm *vm){
    int32_t argc = be_top(vm);
    if (argc == 1 && be_isbytes(vm, 1)) {
      size_t input_size;
      const void * input = be_tobytes(vm, 1, &input_size);
      size_t input_consumed_size, output_written_size;
      size_t output_size = input_size + input_size/2;
      uint8_t output[output_size];
      TAMP.last_error = tamp_compressor_compress_and_flush(
        &TAMP.compressor,
        (unsigned char*)output, output_size, &output_written_size,
        (unsigned char*)input, input_size, &input_consumed_size,
        false);
      be_pushbytes(vm, output, output_written_size);
      AddLog(LOG_LEVEL_DEBUG, "TAM: error %i input size '%u', bytes written '%u'", TAMP.last_error, input_size, output_written_size);
      be_return(vm);
    }
    be_raise(vm, "attribute_error", NULL);
  }

  int be_tamp_decomp_init(bvm *vm, int window){
    int32_t argc = be_top(vm);
    TampConf _conf;
    if(argc==1){
      TAMP.conf.window = window;
      _conf = TAMP.conf;
    } 
    if(TAMP.window != nullptr){
      free(TAMP.window);
    }
    TAMP.window = (unsigned char *)malloc(1 << TAMP.conf.window);
    return tamp_decompressor_init(&TAMP.decompressor, &_conf, TAMP.window);
  }

  int be_tamp_decomp_run(bvm *vm){
    int32_t argc = be_top(vm);
    if (argc == 1 && be_isbytes(vm, 1)) {
      size_t input_size;
      const void * input = be_tobytes(vm, 1, &input_size);
      size_t input_consumed_size, output_written_size;
      size_t output_size = input_size * 4;
      uint8_t output[output_size];
      TAMP.last_error = tamp_decompressor_decompress(
        &TAMP.decompressor,
        (unsigned char*)output, output_size, &output_written_size,
        (const unsigned char*)input, input_size, &input_consumed_size
      );
      AddLog(LOG_LEVEL_DEBUG, "TAM: error %i input size '%u', bytes written '%u'", TAMP.last_error, input_size, output_written_size);
      be_pushbytes(vm, output, output_written_size);
      be_return(vm);
    }
    be_raise(vm, "attribute_error", NULL);
  }

  int be_tamp_last_error(){
    return TAMP.last_error;
  }
}

#endif // USE_TAMP_COMPRESSION
#endif  // USE_BERRY


/*
import tamp
tamp.compressor(9,7,false)
i="The idf.py flash target does not erase the entire flash contents. However it is sometimes useful to set the device back to a totally erased state, particularly when making partition table changes or OTA app updates. To erase the entire flash, run idf.py erase-flash."
#print(i)
t = tasmota.millis()
c = tamp.compress(bytes()..i)
print("compression in ms", tasmota.millis() - t, "ratio:", (real(size(c))/size(i)),"error:", tamp.error())
tamp.decompressor()
t = tasmota.millis()
w = tamp.decompress(c).asstring()
print("decompression in ms",tasmota.millis() - t,"ratio:",  (real(size(c))/size(w)),"error:",  tamp.error())
print(w == i)

->
compression in ms 10 ratio: 0.654135 error: 0
decompression in ms 2 ratio: 0.654135 error: 2 - Why error 2 ???
true
*/