/********************************************************************
 * Berry module `OT` (OpenThread)
 * 
 * To use: `import OT`
 * 
 * OpenThread support for Matter over Thread
 *******************************************************************/
#include "be_constobj.h"
#include "be_mapping.h"

#ifdef USE_MATTER_THREAD

extern int be_OT_init(bvm *vm);

extern void be_OT_start(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_start, "", "@");

extern void be_OT_stop(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_stop, "", "@");

extern void be_OT_set_dataset(struct bvm *vm, uint8_t *buf, size_t size);
BE_FUNC_CTYPE_DECLARE(be_OT_set_dataset, "", "@(bytes)~");

extern const char* be_OT_get_eui64(void);
BE_FUNC_CTYPE_DECLARE(be_OT_get_eui64, "s", "");

extern int be_OT_get_ipaddr(bvm *vm);

extern int be_OT_netdata_services(bvm *vm);
extern int be_OT_poll_state(bvm *vm);

extern void be_OT_udp_open(struct bvm *vm, int32_t port);
BE_FUNC_CTYPE_DECLARE(be_OT_udp_open, "", "@i");

extern void be_OT_udp_send(struct bvm *vm, const char *addr, int32_t port, const uint8_t *data, size_t size);
BE_FUNC_CTYPE_DECLARE(be_OT_udp_send, "", "@si(bytes)~");

extern int be_OT_udp_poll(bvm *vm);

extern void be_OT_udp_close(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_udp_close, "", "@");

extern void be_OT_srp_set_hostname(struct bvm *vm, const char *name);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_set_hostname, "", "@s");

extern void be_OT_srp_add_service(struct bvm *vm, const char *inst, const char *svc, int32_t port, const char *sub, const char *txt);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_add_service, "", "@ssiss");

extern void be_OT_srp_remove_service(struct bvm *vm, const char *inst, const char *svc);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_remove_service, "", "@ss");

extern void be_OT_srp_start(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_start, "", "@");

extern void be_OT_srp_stop(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_stop, "", "@");

extern bbool be_OT_srp_is_running(void);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_is_running, "b", "");

extern const char* be_OT_srp_get_host_state(void);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_get_host_state, "s", "");

extern const char* be_OT_srp_get_server(void);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_get_server, "s", "");

extern void be_OT_srp_set_lease_interval(struct bvm *vm, int32_t lease, int32_t key_lease);
BE_FUNC_CTYPE_DECLARE(be_OT_srp_set_lease_interval, "", "@ii");

#include "be_fixed_OT.h"

/* @const_object_info_begin
module OT (scope: global) {
  init,           func(be_OT_init)
  start,          ctype_func(be_OT_start)
  stop,           ctype_func(be_OT_stop)
  set_dataset,    ctype_func(be_OT_set_dataset)
  get_eui64,      ctype_func(be_OT_get_eui64)
  get_ipaddr,     func(be_OT_get_ipaddr)
  poll_state,     func(be_OT_poll_state)
  netdata_services, func(be_OT_netdata_services)
  udp_open,       ctype_func(be_OT_udp_open)
  udp_send,       ctype_func(be_OT_udp_send)
  udp_poll,       func(be_OT_udp_poll)
  udp_close,      ctype_func(be_OT_udp_close)
  srp_set_hostname, ctype_func(be_OT_srp_set_hostname)
  srp_add_service,  ctype_func(be_OT_srp_add_service)
  srp_remove_service, ctype_func(be_OT_srp_remove_service)
  srp_start,        ctype_func(be_OT_srp_start)
  srp_stop,         ctype_func(be_OT_srp_stop)
  srp_is_running,   ctype_func(be_OT_srp_is_running)
  srp_get_host_state, ctype_func(be_OT_srp_get_host_state)
  srp_get_server,   ctype_func(be_OT_srp_get_server)
  srp_set_lease_interval, ctype_func(be_OT_srp_set_lease_interval)
}
@const_object_info_end */

#endif // USE_MATTER_THREAD
