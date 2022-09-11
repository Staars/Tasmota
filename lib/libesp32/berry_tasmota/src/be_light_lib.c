/********************************************************************
 * Tasmota lib
 * 
 * To use: `import tasmota`
 *******************************************************************/
#include "be_constobj.h"
#include "be_mapping.h"

#ifdef USE_LIGHT
extern int l_getlight(bvm *vm);
extern int l_setlight(bvm *vm);

extern void l_berry_cb(void* function, uint8_t * buffer);
BE_FUNC_CTYPE_DECLARE(l_berry_cb, "", "cc");

extern int l_gamma8(bvm *vm);
extern int l_gamma10(bvm *vm);
extern int l_rev_gamma10(bvm *vm);

/* @const_object_info_begin
module light (scope: global) {
    get, func(l_getlight)
    set, func(l_setlight)
    set_cb, ctype_func(l_berry_cb)

    gamma8, func(l_gamma8)
    gamma10, func(l_gamma10)
    reverse_gamma10, func(l_rev_gamma10)
}
@const_object_info_end */
#include "be_fixed_light.h"

#endif // USE_LIGHT