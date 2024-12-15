/********************************************************************
 * Tasmota lib
 * 
 * To use: import miniz`
 *******************************************************************/
#include "be_constobj.h"
#include "be_mapping.h"

#if defined(USE_MINIZ_COMPRESSION)

extern void be_miniz_version(void);
BE_FUNC_CTYPE_DECLARE(be_miniz_version, "s", "");


/* @const_object_info_begin
module miniz (scope: global) {
  run,          ctype_func(be_miniz_version)
}
@const_object_info_end */
#include "be_fixed_miniz.h"

#endif // USE_MINIZ_COMPRESSION
