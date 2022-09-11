/*
  xlgt_10_virtual_light.ino - virtual Berry light support for Tasmota

  Copyright (C) 2021  Theo Arends & Christian Baars

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

#ifdef USE_LIGHT
#define USE_BERRY_VIRTUAL_LIGHT
#ifdef USE_BERRY_VIRTUAL_LIGHT

/*********************************************************************************************\
 * Virtual light driver to be handled in Berry
\*********************************************************************************************/

#define XLGT_10                   10

extern "C" {
void * berryColorCB = nullptr;
uint8_t * berryColorBuffer = nullptr;

  void BerryVirtualLightInit(){
      TasmotaGlobal.light_type = LT_RGB;
      TasmotaGlobal.light_driver = XLGT_10;
      AddLog(LOG_LEVEL_DEBUG, PSTR("DBG: Virtual light added, with type: %u"), TasmotaGlobal.light_type);
  }

  bool BerryVirtualLightSetChannels(void) {
    if(berryColorCB){
      uint32_t rgb;
      memcpy(berryColorBuffer,(uint8*)XdrvMailbox.command,12);
      void (*func_ptr)() = (void (*)())berryColorCB;
      func_ptr();
    }
    return true;
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xlgt10(uint8_t function)
{
  bool result = false;

  switch (function) {
    case FUNC_SET_CHANNELS:
      result = BerryVirtualLightSetChannels();
      break;
    case FUNC_MODULE_INIT:
      if(berryColorCB != nullptr){
        BerryVirtualLightInit();
      }
      break;
  }
  return result;
}

#endif  // USE_BERRY_VIRTUAL_LIGHT
#endif  // USE_LIGHT

