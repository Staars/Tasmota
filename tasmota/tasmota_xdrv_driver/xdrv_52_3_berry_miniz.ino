/*
  xdrv_52_3_berry_miniz.ino - Berry scripting language, MINIZ ROM functions

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

#ifdef USE_BERRY
#ifdef USE_MINIZ_COMPRESSION

#include "be_mapping.h"
#include <string.h>


extern "C" {
  /*********************************************************************************************\
   * Native functions mapped to Berry functions
   * 
   * import miniz
   * 
   * 
  \*********************************************************************************************/
  #include <miniz.h>

  const char * be_miniz_version(){
    return "0";
  }

}

#endif // USE_MINIZ_COMPRESSION
#endif  // USE_BERRY
