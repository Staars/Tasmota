/********************************************************************
 * Tasmota lib
 * 
 * To use: import tamp`
 *******************************************************************/
#include "be_constobj.h"
#include "be_mapping.h"


#if defined(USE_TAMP_COMPRESSION)

extern int be_tamp_comp_init(struct bvm *vm, uint16_t window, uint16_t literal, bbool use_custom_dictionary);
BE_FUNC_CTYPE_DECLARE(be_tamp_comp_init, "i", "@[iib]");

extern int be_tamp_comp_run(struct bvm *vm);

extern int be_tamp_decomp_init(struct bvm *vm, int window);
BE_FUNC_CTYPE_DECLARE(be_tamp_decomp_init, "i", "@[i]");

extern int be_tamp_decomp_run(struct bvm *vm);

extern int be_tamp_last_error();
BE_FUNC_CTYPE_DECLARE(be_tamp_last_error, "i", "");


/* @const_object_info_begin
module tamp (scope: global) {
  compressor,          ctype_func(be_tamp_comp_init)
  decompressor,        ctype_func(be_tamp_decomp_init)
  compress,            func(be_tamp_comp_run)
  decompress,          func(be_tamp_decomp_run)
  error,               ctype_func(be_tamp_last_error)
}
@const_object_info_end */
#include "be_fixed_tamp.h"

#endif // USE_TAMP_COMPRESSION
