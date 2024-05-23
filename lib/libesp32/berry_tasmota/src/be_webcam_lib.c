/********************************************************************
 * Tasmota lib
 * 
 * To use: `import webcam`
 * 
 *******************************************************************/

#ifdef USE_WEBCAM_BERRY

#include "be_constobj.h"
#include "be_mapping.h"

extern int be_webcam_member(bvm *vm); 
extern int be_webcam_init(bvm *vm);
extern int be_webcam_setup(bvm *vm);
extern int be_webcam_get_image(bvm *vm);
extern int be_webcam_info(bvm *vm);

#include "be_fixed_webcam.h"

/* @const_object_info_begin

module webcam (scope: global) {
    member, func(be_webcam_member)

    init, func(be_webcam_init)
    setup, func(be_webcam_setup)
    get_image, func(be_webcam_get_image)
    info, func(be_webcam_info)
}
@const_object_info_end */

#endif // USE_WEBCAM_BERRY
