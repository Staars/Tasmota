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

extern const char* be_OT_get_role(void);
BE_FUNC_CTYPE_DECLARE(be_OT_get_role, "s", "");

extern void be_OT_set_dataset(struct bvm *vm, uint8_t *buf, size_t size);
BE_FUNC_CTYPE_DECLARE(be_OT_set_dataset, "", "@(bytes)~");

extern uint8_t* be_OT_get_dataset(int32_t notused, size_t *size);
BE_FUNC_CTYPE_DECLARE(be_OT_get_dataset, "&", "[i]");

extern const char* be_OT_get_eui64(void);
BE_FUNC_CTYPE_DECLARE(be_OT_get_eui64, "s", "");

extern int be_OT_get_ipaddr(bvm *vm);

extern void be_OT_state_cb(void *function);
BE_FUNC_CTYPE_DECLARE(be_OT_state_cb, "", "c");

extern void be_OT_factory_reset(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_factory_reset, "", "@");

extern int be_OT_netdata_services(bvm *vm);
extern int be_OT_set_log_level(bvm *vm);
extern int be_OT_coex_prefer_thread(bvm *vm);
extern int be_OT_poll_state(bvm *vm);

extern int be_OT_udp_open(bvm *vm);
extern int be_OT_udp_send(bvm *vm);
extern int be_OT_udp_poll(bvm *vm);

extern void be_OT_udp_close(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_udp_close, "", "@");

extern int be_OT_udp_srp_open(bvm *vm);
extern int be_OT_udp_srp_send(bvm *vm);
extern int be_OT_udp_srp_poll(bvm *vm);

extern void be_OT_udp_srp_close(struct bvm *vm);
BE_FUNC_CTYPE_DECLARE(be_OT_udp_srp_close, "", "@");

extern int be_OT_coap_send_request(bvm *vm);
extern int be_OT_coap_poll_response(bvm *vm);

extern int be_OT_srp_set_hostname(bvm *vm);
extern int be_OT_srp_add_service(bvm *vm);
extern int be_OT_srp_remove_service(bvm *vm);
extern int be_OT_srp_start(bvm *vm);
extern int be_OT_srp_stop(bvm *vm);
extern int be_OT_srp_is_running(bvm *vm);
extern int be_OT_srp_get_host_state(bvm *vm);
extern int be_OT_srp_get_server(bvm *vm);
extern int be_OT_srp_disable_autostart(bvm *vm);
extern int be_OT_srp_set_lease_interval(bvm *vm);

#include "be_fixed_OT.h"

/* @const_object_info_begin
module OT (scope: global) {
  init,           func(be_OT_init)
  start,          ctype_func(be_OT_start)
  stop,           ctype_func(be_OT_stop)
  get_role,       ctype_func(be_OT_get_role)
  set_dataset,    ctype_func(be_OT_set_dataset)
  get_dataset,    ctype_func(be_OT_get_dataset)
  get_eui64,      ctype_func(be_OT_get_eui64)
  get_ipaddr,     func(be_OT_get_ipaddr)
  state_cb,       ctype_func(be_OT_state_cb)
  poll_state,     func(be_OT_poll_state)
  factory_reset,  ctype_func(be_OT_factory_reset)
  netdata_services, func(be_OT_netdata_services)
  set_log_level,  func(be_OT_set_log_level)
  coex_prefer_thread, func(be_OT_coex_prefer_thread)
  udp_open,       func(be_OT_udp_open)
  udp_send,       func(be_OT_udp_send)
  udp_poll,       func(be_OT_udp_poll)
  udp_close,      ctype_func(be_OT_udp_close)
  udp_srp_open,   func(be_OT_udp_srp_open)
  udp_srp_send,   func(be_OT_udp_srp_send)
  udp_srp_poll,   func(be_OT_udp_srp_poll)
  udp_srp_close,  ctype_func(be_OT_udp_srp_close)
  coap_send_request,   func(be_OT_coap_send_request)
  coap_poll_response,  func(be_OT_coap_poll_response)
  srp_set_hostname, func(be_OT_srp_set_hostname)
  srp_add_service,  func(be_OT_srp_add_service)
  srp_remove_service, func(be_OT_srp_remove_service)
  srp_start,        func(be_OT_srp_start)
  srp_stop,         func(be_OT_srp_stop)
  srp_is_running,   func(be_OT_srp_is_running)
  srp_get_host_state, func(be_OT_srp_get_host_state)
  srp_get_server,   func(be_OT_srp_get_server)
  srp_disable_autostart, func(be_OT_srp_disable_autostart)
  srp_set_lease_interval, func(be_OT_srp_set_lease_interval)
}
@const_object_info_end */

#endif // USE_MATTER_THREAD
