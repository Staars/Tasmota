/*
  xdrv_52_3_berry_thread.ino - Berry scripting language, OpenThread native functions

  Copyright (C) 2025 Christian Baars & Stephan Hadinger, Berry language by Guan Wenliang https://github.com/Skiars/berry

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
#ifdef USE_MATTER_THREAD

#include <berry.h>

// 1. BearThread platform API (replaces ESP-IDF OpenThread wrappers)
#include "bt_platform.h"

// 2. OpenThread Native Includes (via BearThread's vendored headers)
#include <openthread/instance.h>
#include <openthread/dataset.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <openthread/srp_client.h>
#include <openthread/netdata.h>
#include <openthread/link.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/logging.h>

// 3. ESP-IDF base includes (still available from framework)
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


/*********************************************************************************************\
 * OpenThread native functions mapped to Berry module "OT"
 *
 * Provides: OT.init(), OT.start(), OT.stop(), OT.set_dataset(), OT.get_dataset(),
 *           OT.get_role(), OT.get_ipaddr(), OT.get_eui64(), OT.state_cb(),
 *           OT.srp_host(), OT.srp_service(), OT.srp_remove(), OT.factory_reset()
\*********************************************************************************************/

// ---- UDP receive queue ----
#define OT_UDP_RX_QUEUE_LEN  8
#define OT_UDP_RX_BUF_SIZE   1280  // IPv6 MTU

typedef struct {
  uint8_t  data[OT_UDP_RX_BUF_SIZE];
  uint16_t len;
  char     addr[OT_IP6_ADDRESS_STRING_SIZE];
  uint16_t port;
} ot_udp_rx_packet_t;

// ---- Internal state ----
typedef void (*ot_state_cb_t)(int32_t role, int32_t arg2, int32_t arg3, int32_t arg4);
static struct {
  bool          initialized = false;
  TaskHandle_t  task_handle = nullptr;
  bvm          *vm = nullptr;          // Berry VM reference for callbacks
  int           state_cb_ref = 0;       // C function pointer from cb.gen_cb (legacy, unused for invocation)
  // UDP socket state
  otUdpSocket   udp_socket;
  QueueHandle_t udp_rx_queue = nullptr;
  bool          udp_open = false;
  // SRP client state
  bool          srp_callback_set = false;
  char          srp_hostname[32] = {0};   // last registered SRP hostname (empty if none)
  // Thread state-change pending (latest role) - drained from Berry main task via OT.poll_state()
  // Cross-task callback into Berry VM is unsafe; we only stash the latest role here.
  volatile int32_t state_pending_role = -1;   // -1 means "no pending event"
} OT_State;

// ---- Forward declarations ----
// Use void* / uint32_t to avoid OT types in signatures (Arduino prototype generator
// places prototypes before includes, so otInstance/otChangedFlags are not yet defined).
static void ot_task(void *pvParameters);
static void ot_state_changed_callback(uint32_t aFlags, void *aContext);
static void ot_udp_receive_callback(void *aContext, void *aMessage, const void *aMessageInfo);
static void ot_srp_client_callback(otError aError, const otSrpClientHostInfo *aHostInfo,
                                   const otSrpClientService *aServices,
                                   const otSrpClientService *aRemovedServices, void *aContext);
static void ot_srp_autostart_callback(const otSockAddr *aServerSockAddr, void *aContext);

// ---- Helper: lock and get OT instance ----
// Returns OT instance as void* to avoid otInstance in function signature.
// Caller must call ot_unlock() after use.
static void* ot_lock_and_get(void) {
  if (!OT_State.initialized) return nullptr;
  bt_lock_acquire(portMAX_DELAY);
  return (void*)bt_get_instance();
}

static void ot_unlock(void) {
  bt_lock_release();
}

// ---- OT.init() ----
// Initialize OpenThread stack via BearThread, start OT task
// Called automatically on `import OT`
extern "C" int be_OT_init(bvm *vm) {
  if (OT_State.initialized) {
    be_return_nil(vm);  // already initialized
  }

  // Store VM reference for callbacks
  OT_State.vm = vm;

  // Initialize eventfd for OpenThread
  esp_vfs_eventfd_config_t eventfd_config = {
    .max_fds = 3,
  };
  ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

  // Initialize BearThread platform (lock, radio, OT instance)
  esp_err_t err = bt_platform_init();
  if (err != ESP_OK) {
    be_raisef(vm, "ot_error", "OT: bt_platform_init failed: %d", err);
    be_return_nil(vm);
  }

  // Register state change callback
  otInstance *instance = bt_get_instance();
  otSetStateChangedCallback(instance, (otStateChangedCallback)ot_state_changed_callback, nullptr);

  // Create OT main loop task
  xTaskCreate(ot_task, "ot_main", 6144, nullptr, 5, &OT_State.task_handle);

  OT_State.initialized = true;
  AddLog(LOG_LEVEL_INFO, PSTR("OT : OpenThread initialized (BearThread)"));

  be_return(vm);
}

// ---- OpenThread main loop task ----
static void ot_task(void *pvParameters) {
  bt_launch_mainloop();
  // Should not return
  vTaskDelete(nullptr);
}

// ---- State change callback (runs in OT task context) ----
// IMPORTANT: this fires on the OpenThread task. Calling back into the Berry VM
// directly from here is unsafe (the VM is single-threaded and bound to the
// Tasmota main task) and was causing instruction-access faults where PC ended
// up inside stack memory after the VM state was corrupted by concurrent access.
// Instead we just stash the latest role; Berry drains it from every_50ms via
// OT.poll_state().
static void ot_state_changed_callback(uint32_t aFlags, void *aContext) {
  if (aFlags & (OT_CHANGED_THREAD_ROLE | OT_CHANGED_THREAD_NETDATA |
                OT_CHANGED_IP6_ADDRESS_ADDED | OT_CHANGED_IP6_ADDRESS_REMOVED)) {
    otInstance *instance = bt_get_instance();
    otDeviceRole role = otThreadGetDeviceRole(instance);
    const char *role_str;
    switch (role) {
      case OT_DEVICE_ROLE_DISABLED: role_str = "disabled"; break;
      case OT_DEVICE_ROLE_DETACHED: role_str = "detached"; break;
      case OT_DEVICE_ROLE_CHILD:    role_str = "child"; break;
      case OT_DEVICE_ROLE_ROUTER:   role_str = "router"; break;
      case OT_DEVICE_ROLE_LEADER:   role_str = "leader"; break;
      default:                      role_str = "unknown"; break;
    }
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : role changed to '%s'"), role_str);

    // Stash latest role; Berry's every_50ms picks it up via OT.poll_state()
    OT_State.state_pending_role = (int32_t)role;
  }
}

// ---- OT.poll_state() -> int role or nil ----
// Drain a pending Thread role transition queued by ot_state_changed_callback.
// Returns the OT role enum (0..4) or nil if no pending event.
// Must be called from the Berry/Tasmota main task only.
extern "C" int be_OT_poll_state(bvm *vm) {
  int32_t role = OT_State.state_pending_role;
  if (role < 0) {
    be_return_nil(vm);
  }
  // Atomic-enough on this MCU: a single 32-bit store. Worst case we miss a
  // transient transition and the next callback re-stamps it.
  OT_State.state_pending_role = -1;
  be_pushint(vm, role);
  be_return(vm);
}

// ---- OT.start() ----
// Enable IPv6 interface and start Thread protocol
extern "C" void be_OT_start(struct bvm *vm) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  otIp6SetEnabled(instance, true);
  otThreadSetEnabled(instance, true);
  ot_unlock();

  AddLog(LOG_LEVEL_INFO, PSTR("OT : Thread started"));
}

// ---- OT.stop() ----
// Stop Thread protocol and disable IPv6
extern "C" void be_OT_stop(struct bvm *vm) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  otThreadSetEnabled(instance, false);
  otIp6SetEnabled(instance, false);
  ot_unlock();

  AddLog(LOG_LEVEL_INFO, PSTR("OT : Thread stopped"));
}

// ---- OT.get_role() -> string ----
extern "C" const char* be_OT_get_role(void) {
  if (!OT_State.initialized) return "uninitialized";

  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  otDeviceRole role = otThreadGetDeviceRole(instance);
  bt_lock_release();

  switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "unknown";
  }
}

// ---- OT.set_dataset(bytes) ----
// Set the Active Operational Dataset from TLV-encoded bytes
extern "C" void be_OT_set_dataset(struct bvm *vm, uint8_t *buf, size_t size) {
  if (!buf || size == 0 || size > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
    be_raisef(vm, "ot_error", "OT: invalid dataset size");
    return;
  }

  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  otOperationalDatasetTlvs dataset;
  dataset.mLength = size;
  memcpy(dataset.mTlvs, buf, size);

  otError error = otDatasetSetActiveTlvs(instance, &dataset);
  ot_unlock();

  if (error != OT_ERROR_NONE) {
    be_raisef(vm, "ot_error", "OT: set_dataset failed: %d", error);
  }
}

// ---- OT.get_dataset() -> bytes ----
// Get the Active Operational Dataset as TLV-encoded bytes
extern "C" uint8_t* be_OT_get_dataset(int32_t notused, size_t *size) {
  static otOperationalDatasetTlvs dataset;

  if (!OT_State.initialized) {
    *size = 0;
    return nullptr;
  }

  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  otError error = otDatasetGetActiveTlvs(instance, &dataset);
  bt_lock_release();

  if (error != OT_ERROR_NONE) {
    *size = 0;
    return nullptr;
  }

  *size = dataset.mLength;
  return dataset.mTlvs;
}

// ---- OT.get_eui64() -> string ----
// Return the factory EUI-64 of the 802.15.4 radio as hex string
extern "C" const char* be_OT_get_eui64(void) {
  static char eui64_str[17];  // 16 hex chars + null

  if (!OT_State.initialized) return "";

  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  otExtAddress eui64;
  otLinkGetFactoryAssignedIeeeEui64(instance, &eui64);
  bt_lock_release();

  snprintf(eui64_str, sizeof(eui64_str), "%02X%02X%02X%02X%02X%02X%02X%02X",
           eui64.m8[0], eui64.m8[1], eui64.m8[2], eui64.m8[3],
           eui64.m8[4], eui64.m8[5], eui64.m8[6], eui64.m8[7]);
  return eui64_str;
}

// ---- OT.get_ipaddr() -> list of IPv6 address strings (Berry func) ----
extern "C" int be_OT_get_ipaddr(bvm *vm) {
  if (!OT_State.initialized) {
    be_newobject(vm, "list");
    be_pop(vm, 1);
    be_return(vm);
  }

  be_newobject(vm, "list");

  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  const otNetifAddress *addr = otIp6GetUnicastAddresses(instance);
  char addr_str[OT_IP6_ADDRESS_STRING_SIZE];

  while (addr) {
    otIp6AddressToString(&addr->mAddress, addr_str, sizeof(addr_str));
    be_pushstring(vm, addr_str);
    be_data_push(vm, -2);
    be_pop(vm, 1);
    addr = addr->mNext;
  }
  bt_lock_release();

  be_pop(vm, 1);
  be_return(vm);
}

// ---- OT.state_cb(cb_ptr) ----
// Register a C function pointer (from cb.gen_cb) for Thread state changes
extern "C" void be_OT_state_cb(void *function) {
  OT_State.state_cb_ref = (int)(intptr_t)function;
  AddLog(LOG_LEVEL_DEBUG, PSTR("OT : state callback registered"));
}

// ---- OT.factory_reset() ----
// Erase all Thread persistent data and reset
extern "C" void be_OT_factory_reset(struct bvm *vm) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  otInstanceFactoryReset(instance);
  ot_unlock();

  AddLog(LOG_LEVEL_INFO, PSTR("OT : factory reset"));
}

// ---- SRP Client Functions ----

// ---- SRP client state callback (runs in OT task context) ----
// Logs SRP server registration progress so we can debug discovery issues.
static void ot_srp_client_callback(otError aError, const otSrpClientHostInfo *aHostInfo,
                                   const otSrpClientService *aServices,
                                   const otSrpClientService *aRemovedServices, void *aContext) {
  if (aError == OT_ERROR_NONE) {
    if (aHostInfo) {
      const char *st = "?";
      switch (aHostInfo->mState) {
        case OT_SRP_CLIENT_ITEM_STATE_TO_ADD:      st = "to-add"; break;
        case OT_SRP_CLIENT_ITEM_STATE_ADDING:      st = "adding"; break;
        case OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH:  st = "to-refresh"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REFRESHING:  st = "refreshing"; break;
        case OT_SRP_CLIENT_ITEM_STATE_TO_REMOVE:   st = "to-remove"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REMOVING:    st = "removing"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REGISTERED:  st = "registered"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REMOVED:     st = "removed"; break;
        default: break;
      }
      AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP host '%s' state=%s"),
             aHostInfo->mName ? aHostInfo->mName : "?", st);
    }
    const otSrpClientService *svc = aServices;
    while (svc) {
      const char *st = "?";
      switch (svc->mState) {
        case OT_SRP_CLIENT_ITEM_STATE_TO_ADD:      st = "to-add"; break;
        case OT_SRP_CLIENT_ITEM_STATE_ADDING:      st = "adding"; break;
        case OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH:  st = "to-refresh"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REFRESHING:  st = "refreshing"; break;
        case OT_SRP_CLIENT_ITEM_STATE_TO_REMOVE:   st = "to-remove"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REMOVING:    st = "removing"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REGISTERED:  st = "registered"; break;
        case OT_SRP_CLIENT_ITEM_STATE_REMOVED:     st = "removed"; break;
        default: break;
      }
      AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP svc '%s.%s' state=%s"),
             svc->mInstanceName, svc->mName, st);
      svc = svc->mNext;
    }
  } else {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP callback error=%d"), aError);
  }
}

// ---- SRP autostart callback (runs in OT task context) ----
// Fires when the SRP client autostarts after discovering a server, or stops.
static void ot_srp_autostart_callback(const otSockAddr *aServerSockAddr, void *aContext) {
  if (aServerSockAddr) {
    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&aServerSockAddr->mAddress, addr_str, sizeof(addr_str));
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP autostart, server [%s]:%d"),
           addr_str, aServerSockAddr->mPort);
  } else {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP autostart stopped (no server)"));
  }
}

// OT.srp_host(hostname, addrs) - set SRP client hostname and addresses
// Idempotent: if hostname is already registered with the same name, this is a no-op.
extern "C" int be_OT_srp_host(bvm *vm) {
  int argc = be_top(vm);
  if (argc < 1 || !be_isstring(vm, 1)) {
    be_raise(vm, kTypeError, nullptr);
  }

  const char *hostname = be_tostring(vm, 1);

  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    be_return_nil(vm);
  }

  // Idempotency: if we already registered this exact hostname, skip OT calls.
  // otSrpClientSetHostName returns OT_ERROR_INVALID_STATE once the host is in
  // a non-removable state, so calling it again would error out.
  if (OT_State.srp_hostname[0] != 0 && strcmp(OT_State.srp_hostname, hostname) == 0) {
    ot_unlock();
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : SRP hostname '%s' already set, skipping"), hostname);
    be_return_nil(vm);
  }

  // Register SRP client state callback once so we can observe registration progress.
  if (!OT_State.srp_callback_set) {
    otSrpClientSetCallback(instance, ot_srp_client_callback, nullptr);
    OT_State.srp_callback_set = true;
  }

  // Set hostname
  otError error = otSrpClientSetHostName(instance, hostname);
  if (error != OT_ERROR_NONE && error != OT_ERROR_ALREADY) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: srp_host set hostname failed: %d", error);
    be_return_nil(vm);
  }

  // Auto-set host addresses from Thread interface addresses
  error = otSrpClientEnableAutoHostAddress(instance);
  if (error != OT_ERROR_NONE && error != OT_ERROR_ALREADY) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : SRP auto host address failed: %d"), error);
  }

  // Enable auto-start (with autostart callback so we can see when SRP server is found)
  otSrpClientEnableAutoStartMode(instance, ot_srp_autostart_callback, nullptr);

  // Remember the hostname for idempotency
  strncpy(OT_State.srp_hostname, hostname, sizeof(OT_State.srp_hostname) - 1);
  OT_State.srp_hostname[sizeof(OT_State.srp_hostname) - 1] = 0;

  ot_unlock();

  AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP hostname set to '%s'"), hostname);
  be_return_nil(vm);
}

// OT.srp_running() - returns true if the SRP client is currently running
extern "C" int be_OT_srp_running(bvm *vm) {
  if (!OT_State.initialized) {
    be_pushbool(vm, false);
    be_return(vm);
  }
  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  bool running = otSrpClientIsRunning(instance);
  bt_lock_release();
  be_pushbool(vm, running);
  be_return(vm);
}

// OT.srp_server() - returns the SRP server address string the autostart picked,
// or empty string if no server has been discovered yet. Useful to debug
// OT_ERROR_RESPONSE_TIMEOUT (28) on otSrpClient updates.
extern "C" const char* be_OT_srp_server(void) {
  static char srv_str[OT_IP6_ADDRESS_STRING_SIZE + 8];
  srv_str[0] = 0;
  if (!OT_State.initialized) return "";
  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  const otSockAddr *srv = otSrpClientGetServerAddress(instance);
  if (srv && !otIp6IsAddressUnspecified(&srv->mAddress)) {
    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&srv->mAddress, addr_str, sizeof(addr_str));
    snprintf(srv_str, sizeof(srv_str), "[%s]:%d", addr_str, srv->mPort);
  }
  bt_lock_release();
  return srv_str;
}

// OT.set_log_level(level) - set OpenThread runtime log level.
// Levels per <openthread/logging.h>:
//   0 NONE, 1 CRIT, 2 WARN, 3 NOTE, 4 INFO, 5 DEBG.
// Returns true on success. Note: requires the OT library to be built with
// OPENTHREAD_CONFIG_LOG_LEVEL_DYNAMIC_ENABLE; otherwise the log level is
// fixed at compile time and this call returns false (OT_ERROR_DISABLED_FEATURE).
extern "C" int be_OT_set_log_level(bvm *vm) {
  int argc = be_top(vm);
  if (argc < 1 || !be_isint(vm, 1)) {
    be_raise(vm, kTypeError, nullptr);
  }
  int lvl = be_toint(vm, 1);
  if (lvl < 0) lvl = 0;
  if (lvl > 5) lvl = 5;

#if OPENTHREAD_CONFIG_LOG_LEVEL_DYNAMIC_ENABLE
  bt_lock_acquire(portMAX_DELAY);
  otError err = otLoggingSetLevel((otLogLevel)lvl);
  bt_lock_release();

  if (err == OT_ERROR_NONE) {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : log level set to %d"), lvl);
    be_pushbool(vm, true);
  } else {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : set_log_level failed: %d"), err);
    be_pushbool(vm, false);
  }
#else
  // OT compiled without OPENTHREAD_CONFIG_LOG_LEVEL_DYNAMIC_ENABLE,
  // so otLoggingSetLevel is not in the library. Static level is set
  // at compile time (see bearthread-core-config.h OPENTHREAD_CONFIG_LOG_LEVEL).
  (void)lvl;
  // AddLog(LOG_LEVEL_INFO, PSTR("OT : set_log_level unavailable (DYNAMIC_LOG disabled, static=%d)"),
  //        OPENTHREAD_CONFIG_LOG_LEVEL);
  be_pushbool(vm, false);
#endif
  be_return(vm);
}

// OT.netdata_services() - returns a Berry list of strings, one per service
// entry in Thread Network Data. Useful to debug SRP autostart picks: shows
// every service the leader has published, including anycast SRP (service
// data byte 0x5c) and unicast SRP (0x5d) entries with the actual RLOC16
// and IPv6 address+port the BR is advertising.
extern "C" int be_OT_netdata_services(bvm *vm) {
  be_newobject(vm, "list");
  if (!OT_State.initialized) {
    be_pop(vm, 1);
    be_return(vm);
  }
  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  otNetworkDataIterator iter = OT_NETWORK_DATA_ITERATOR_INIT;
  otServiceConfig svc;
  char line[160];
  while (otNetDataGetNextService(instance, &iter, &svc) == OT_ERROR_NONE) {
    // Build a hex view of service-data and server-data
    char sdhex[2 * 8 + 1] = {0};
    int n = svc.mServiceDataLength > 8 ? 8 : svc.mServiceDataLength;
    for (int i = 0; i < n; i++) {
      snprintf(sdhex + i * 2, 3, "%02X", svc.mServiceData[i]);
    }
    char svrhex[2 * 24 + 1] = {0};
    int m = svc.mServerConfig.mServerDataLength > 24 ? 24 : svc.mServerConfig.mServerDataLength;
    for (int i = 0; i < m; i++) {
      snprintf(svrhex + i * 2, 3, "%02X", svc.mServerConfig.mServerData[i]);
    }

    // For Thread enterprise (44970) decode SRP server entries
    const char *kind = "?";
    char extra[80] = {0};
    if (svc.mEnterpriseNumber == 44970 && svc.mServiceDataLength >= 1) {
      uint8_t tag = svc.mServiceData[0];
      if (tag == 0x5C) {
        kind = "SRP-anycast";
        if (svc.mServiceDataLength >= 2) {
          snprintf(extra, sizeof(extra), " seq=%u", svc.mServiceData[1]);
        }
      } else if (svc.mServiceDataLength >= 1 && tag == 0x5D) {
        kind = "SRP-unicast";
        // server-data: 16-byte IPv6 + 2-byte port (big endian)
        if (svc.mServerConfig.mServerDataLength >= 18) {
          otIp6Address a;
          memcpy(a.mFields.m8, svc.mServerConfig.mServerData, 16);
          uint16_t port =
              (uint16_t)svc.mServerConfig.mServerData[16] << 8 |
              (uint16_t)svc.mServerConfig.mServerData[17];
          char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
          otIp6AddressToString(&a, addr_str, sizeof(addr_str));
          snprintf(extra, sizeof(extra), " [%s]:%u", addr_str, port);
        }
      }
    }

    snprintf(line, sizeof(line),
             "id=%u ent=%u rloc=0x%04x stable=%d kind=%s sd=%s svr=%s%s",
             svc.mServiceId, (unsigned)svc.mEnterpriseNumber,
             svc.mServerConfig.mRloc16, svc.mServerConfig.mStable ? 1 : 0,
             kind, sdhex, svrhex, extra);

    be_pushstring(vm, line);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  bt_lock_release();
  be_pop(vm, 1);  // remove list internal pointer
  be_return(vm);
}

// OT.srp_use_unicast() - look for a unicast SRP server entry in Thread
// Network Data and force the SRP client to use it instead of whatever
// (typically anycast) the autostart logic picked. Returns true if a
// unicast entry was found and applied; false otherwise.
//
// Background: the OpenThread SRP client autostart mode is supposed to
// prefer unicast over anycast, but on real Apple Border Router meshes we
// observe it locking onto the anycast ALOC `:0:ff:fe00:fc1X` and getting
// OT_ERROR_RESPONSE_TIMEOUT (28) forever. When netdata also publishes a
// real unicast SRP server, switching to it usually unblocks registration.
extern "C" int be_OT_srp_use_unicast(bvm *vm) {
  if (!OT_State.initialized) {
    be_pushbool(vm, false);
    be_return(vm);
  }
  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();

  otNetworkDataIterator iter = OT_NETWORK_DATA_ITERATOR_INIT;
  otServiceConfig svc;
  bool found = false;
  otSockAddr target;
  memset(&target, 0, sizeof(target));
  while (otNetDataGetNextService(instance, &iter, &svc) == OT_ERROR_NONE) {
    if (svc.mEnterpriseNumber != 44970) continue;
    if (svc.mServiceDataLength < 1) continue;
    if (svc.mServiceData[0] != 0x5D) continue;          // not unicast SRP
    if (svc.mServerConfig.mServerDataLength < 18) continue;
    memcpy(target.mAddress.mFields.m8, svc.mServerConfig.mServerData, 16);
    target.mPort = (uint16_t)svc.mServerConfig.mServerData[16] << 8 |
                   (uint16_t)svc.mServerConfig.mServerData[17];
    found = true;
    break;
  }
  if (!found) {
    bt_lock_release();
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : no unicast SRP server in netdata"));
    be_pushbool(vm, false);
    be_return(vm);
  }

  // Already on this server? No-op.
  const otSockAddr *cur = otSrpClientGetServerAddress(instance);
  if (cur && otIp6IsAddressEqual(&cur->mAddress, &target.mAddress) &&
      cur->mPort == target.mPort && otSrpClientIsRunning(instance)) {
    bt_lock_release();
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : SRP already on unicast server"));
    be_pushbool(vm, true);
    be_return(vm);
  }

  // Switch: disable autostart, stop, restart with target
  otSrpClientDisableAutoStartMode(instance);
  if (otSrpClientIsRunning(instance)) {
    otSrpClientStop(instance);
  }
  otError err = otSrpClientStart(instance, &target);
  bt_lock_release();

  char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
  otIp6AddressToString(&target.mAddress, addr_str, sizeof(addr_str));
  if (err == OT_ERROR_NONE) {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP forced unicast [%s]:%u"),
           addr_str, target.mPort);
    be_pushbool(vm, true);
  } else {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP force unicast failed: %d ([%s]:%u)"),
           err, addr_str, target.mPort);
    be_pushbool(vm, false);
  }
  be_return(vm);
}

// OT.srp_state() - returns SRP host state as string
extern "C" const char* be_OT_srp_state(void) {
  if (!OT_State.initialized) return "uninitialized";
  bt_lock_acquire(portMAX_DELAY);
  otInstance *instance = bt_get_instance();
  if (!otSrpClientIsRunning(instance)) {
    bt_lock_release();
    return "stopped";
  }
  const otSrpClientHostInfo *info = otSrpClientGetHostInfo(instance);
  bt_lock_release();
  if (!info) return "running";
  switch (info->mState) {
    case OT_SRP_CLIENT_ITEM_STATE_TO_ADD:      return "to-add";
    case OT_SRP_CLIENT_ITEM_STATE_ADDING:      return "adding";
    case OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH:  return "to-refresh";
    case OT_SRP_CLIENT_ITEM_STATE_REFRESHING:  return "refreshing";
    case OT_SRP_CLIENT_ITEM_STATE_TO_REMOVE:   return "to-remove";
    case OT_SRP_CLIENT_ITEM_STATE_REMOVING:    return "removing";
    case OT_SRP_CLIENT_ITEM_STATE_REGISTERED:  return "registered";
    case OT_SRP_CLIENT_ITEM_STATE_REMOVED:     return "removed";
    default: return "unknown";
  }
}

// OT.srp_service(instance_name, service_type, port, subtypes_list, txt_map)
// e.g. OT.srp_service("ABCDEF123456", "_matterc._udp", 5540, ["_S3", "_L840"], {"D":"840","VP":"FFF1+8000"})
extern "C" int be_OT_srp_service(bvm *vm) {
  int argc = be_top(vm);
  if (argc < 3 || !be_isstring(vm, 1) || !be_isstring(vm, 2) || !be_isint(vm, 3)) {
    be_raise(vm, kTypeError, nullptr);
  }

  const char *inst_name = be_tostring(vm, 1);
  const char *svc_type  = be_tostring(vm, 2);
  uint16_t    port      = (uint16_t)be_toint(vm, 3);

  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    be_return_nil(vm);
  }

  // Allocate service struct (persistent, leaked intentionally - OT takes ownership)
  otSrpClientService *service = (otSrpClientService *)calloc(1, sizeof(otSrpClientService));
  if (!service) {
    ot_unlock();
    be_raise(vm, "memory_error", "OT: out of memory");
    be_return_nil(vm);
  }

  service->mInstanceName = strdup(inst_name);
  service->mName = strdup(svc_type);
  service->mPort = port;

  // Parse subtypes list (arg 4): Berry list of strings → NULL-terminated C array
  service->mSubTypeLabels = nullptr;
  if (argc >= 4 && be_islistinstance(vm, 4)) {
    // Get the internal list via .p member
    be_getmember(vm, 4, ".p");
    int count = be_data_size(vm, -1);
    if (count > 0) {
      const char **labels = (const char **)calloc(count + 1, sizeof(const char *));
      if (labels) {
        for (int i = 0; i < count; i++) {
          be_pushint(vm, i);          // push index onto stack
          be_getindex(vm, -2);        // reads key at -1, pushes value; list .p is at -2
          // stack: ... .p_list index value
          if (be_isstring(vm, -1)) {
            labels[i] = strdup(be_tostring(vm, -1));
          } else {
            labels[i] = strdup("");
          }
          be_pop(vm, 2);             // pop index and value
        }
        labels[count] = nullptr;      // NULL terminator
        service->mSubTypeLabels = labels;
      }
    }
    be_pop(vm, 1);                    // pop .p list
  }

  // Parse TXT entries (arg 5): Berry map {key: value} → otDnsTxtEntry array
  service->mNumTxtEntries = 0;
  service->mTxtEntries = nullptr;
  if (argc >= 5 && be_ismapinstance(vm, 5)) {
    // Get the internal map via .p member (same pattern as berry_mdns)
    be_getmember(vm, 5, ".p");
    int32_t map_len = be_data_size(vm, -1);
    if (map_len > 0) {
      otDnsTxtEntry *entries = (otDnsTxtEntry *)calloc(map_len, sizeof(otDnsTxtEntry));
      if (entries) {
        int idx = 0;
        be_pushiter(vm, -1);            // push iterator for the internal map
        while (be_iter_hasnext(vm, -2) && idx < map_len) {
          be_iter_next(vm, -2);
          // stack: ... map iter key value
          const char *key = be_isstring(vm, -2) ? be_tostring(vm, -2) : "";
          entries[idx].mKey = strdup(key);
          // Convert value to string for TXT record
          if (be_isstring(vm, -1)) {
            const char *val_str = be_tostring(vm, -1);
            size_t val_len = strlen(val_str);
            uint8_t *val_buf = (uint8_t *)malloc(val_len);
            if (val_buf) {
              memcpy(val_buf, val_str, val_len);
              entries[idx].mValue = val_buf;
              entries[idx].mValueLength = val_len;
            } else {
              entries[idx].mValue = nullptr;
              entries[idx].mValueLength = 0;
            }
          } else if (be_isint(vm, -1)) {
            // Integer values: convert to string representation
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), "%d", (int)be_toint(vm, -1));
            size_t val_len = strlen(num_buf);
            uint8_t *val_buf = (uint8_t *)malloc(val_len);
            if (val_buf) {
              memcpy(val_buf, num_buf, val_len);
              entries[idx].mValue = val_buf;
              entries[idx].mValueLength = val_len;
            } else {
              entries[idx].mValue = nullptr;
              entries[idx].mValueLength = 0;
            }
          } else {
            entries[idx].mValue = nullptr;
            entries[idx].mValueLength = 0;
          }
          idx++;
          be_pop(vm, 2);              // pop key and value
        }
        be_pop(vm, 1);                // pop iterator
        service->mNumTxtEntries = idx;
        service->mTxtEntries = entries;
      }
    }
    be_pop(vm, 1);                    // pop .p map
  }

  otError error = otSrpClientAddService(instance, service);
  ot_unlock();

  if (error != OT_ERROR_NONE && error != OT_ERROR_ALREADY) {
    // Free allocated memory on failure
    if (service->mSubTypeLabels) {
      for (int i = 0; service->mSubTypeLabels[i]; i++) free((void*)service->mSubTypeLabels[i]);
      free((void*)service->mSubTypeLabels);
    }
    if (service->mTxtEntries) {
      for (int i = 0; i < service->mNumTxtEntries; i++) {
        free((void*)service->mTxtEntries[i].mKey);
        free((void*)service->mTxtEntries[i].mValue);
      }
      free((void*)service->mTxtEntries);
    }
    free((void*)service->mInstanceName);
    free((void*)service->mName);
    free(service);
    be_raisef(vm, "ot_error", "OT: srp_service add failed: %d", error);
    be_return_nil(vm);
  }

  int sub_count = 0;
  if (service->mSubTypeLabels) {
    for (int i = 0; service->mSubTypeLabels[i]; i++) sub_count++;
  }
  AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP service '%s' type '%s' port %d txt=%d sub=%d"),
         inst_name, svc_type, port, service->mNumTxtEntries, sub_count);
  be_return_nil(vm);
}

// OT.srp_remove(instance_name, service_type) - remove an SRP service
extern "C" int be_OT_srp_remove(bvm *vm) {
  int argc = be_top(vm);
  if (argc < 2 || !be_isstring(vm, 1) || !be_isstring(vm, 2)) {
    be_raise(vm, kTypeError, nullptr);
  }

  const char *inst_name = be_tostring(vm, 1);
  const char *svc_type  = be_tostring(vm, 2);

  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    be_return_nil(vm);
  }

  // Find and remove the matching service
  const otSrpClientService *svc = otSrpClientGetServices(instance);
  while (svc) {
    if (strcmp(svc->mInstanceName, inst_name) == 0 && strcmp(svc->mName, svc_type) == 0) {
      otError error = otSrpClientRemoveService(instance, (otSrpClientService *)svc);
      ot_unlock();
      if (error != OT_ERROR_NONE) {
        be_raisef(vm, "ot_error", "OT: srp_remove failed: %d", error);
      }
      AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP service '%s' removed"), inst_name);
      be_return_nil(vm);
    }
    svc = svc->mNext;
  }

  ot_unlock();
  AddLog(LOG_LEVEL_DEBUG, PSTR("OT : SRP service '%s' not found"), inst_name);
  be_return_nil(vm);
}

// ---- UDP receive callback (runs in OT task context) ----
static void ot_udp_receive_callback(void *aContext, void *aMessage, const void *aMessageInfo) {
  otMessage *msg = (otMessage *)aMessage;
  const otMessageInfo *info = (const otMessageInfo *)aMessageInfo;

  uint16_t offset = otMessageGetOffset(msg);
  uint16_t length = otMessageGetLength(msg) - offset;
  if (length == 0 || length > OT_UDP_RX_BUF_SIZE) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : UDP rx dropped, len=%d"), length);
    return;
  }

  if (!OT_State.udp_rx_queue) return;

  static ot_udp_rx_packet_t pkt;
  pkt.len = otMessageRead(msg, offset, pkt.data, length);
  otIp6AddressToString(&info->mPeerAddr, pkt.addr, sizeof(pkt.addr));
  pkt.port = info->mPeerPort;

  if (xQueueSend(OT_State.udp_rx_queue, &pkt, 0) != pdTRUE) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : UDP rx queue full, dropped"));
  }
}

// ---- OT.udp_open(port) ----
extern "C" int be_OT_udp_open(bvm *vm) {
  int argc = be_top(vm);
  if (argc < 1 || !be_isint(vm, 1)) {
    be_raise(vm, kTypeError, nullptr);
  }
  uint16_t port = (uint16_t)be_toint(vm, 1);

  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    be_return_nil(vm);
  }

  if (OT_State.udp_open) {
    otUdpClose(instance, &OT_State.udp_socket);
    OT_State.udp_open = false;
  }

  memset(&OT_State.udp_socket, 0, sizeof(OT_State.udp_socket));
  otError error = otUdpOpen(instance, &OT_State.udp_socket,
                            (otUdpReceive)ot_udp_receive_callback, nullptr);
  if (error != OT_ERROR_NONE) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_open failed: %d", error);
    be_return_nil(vm);
  }

  otSockAddr sockaddr;
  memset(&sockaddr, 0, sizeof(sockaddr));
  sockaddr.mPort = port;
  error = otUdpBind(instance, &OT_State.udp_socket, &sockaddr, OT_NETIF_THREAD_INTERNAL);
  if (error != OT_ERROR_NONE) {
    otUdpClose(instance, &OT_State.udp_socket);
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_bind failed: %d", error);
    be_return_nil(vm);
  }

  if (!OT_State.udp_rx_queue) {
    OT_State.udp_rx_queue = xQueueCreate(OT_UDP_RX_QUEUE_LEN, sizeof(ot_udp_rx_packet_t));
  }

  OT_State.udp_open = true;
  ot_unlock();

  AddLog(LOG_LEVEL_INFO, PSTR("OT : UDP socket opened on port %d"), port);
  be_return_nil(vm);
}

// ---- OT.udp_send(addr, port, payload) ----
extern "C" int be_OT_udp_send(bvm *vm) {
  int argc = be_top(vm);
  if (argc < 3 || !be_isstring(vm, 1) || !be_isint(vm, 2) || !be_isbytes(vm, 3)) {
    be_raise(vm, kTypeError, nullptr);
  }

  const char *addr_str = be_tostring(vm, 1);
  uint16_t port = (uint16_t)be_toint(vm, 2);
  size_t payload_len;
  const uint8_t *payload = (const uint8_t *)be_tobytes(vm, 3, &payload_len);

  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    be_return_nil(vm);
  }

  if (!OT_State.udp_open) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: UDP socket not open");
    be_return_nil(vm);
  }

  otMessage *message = otUdpNewMessage(instance, nullptr);
  if (!message) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_send no message buffer");
    be_return_nil(vm);
  }

  otError error = otMessageAppend(message, payload, (uint16_t)payload_len);
  if (error != OT_ERROR_NONE) {
    otMessageFree(message);
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_send append failed: %d", error);
    be_return_nil(vm);
  }

  otMessageInfo messageInfo;
  memset(&messageInfo, 0, sizeof(messageInfo));
  otIp6AddressFromString(addr_str, &messageInfo.mPeerAddr);
  messageInfo.mPeerPort = port;

  error = otUdpSend(instance, &OT_State.udp_socket, message, &messageInfo);
  if (error != OT_ERROR_NONE) {
    otMessageFree(message);
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_send failed: %d", error);
    be_return_nil(vm);
  }
  ot_unlock();

  be_return_nil(vm);
}

// ---- OT.udp_poll() -> [bytes, addr_string, port] or nil ----
extern "C" int be_OT_udp_poll(bvm *vm) {
  if (!OT_State.udp_rx_queue) {
    be_return_nil(vm);
  }

  static ot_udp_rx_packet_t pkt;
  if (xQueueReceive(OT_State.udp_rx_queue, &pkt, 0) != pdTRUE) {
    be_return_nil(vm);
  }

  be_newobject(vm, "list");
  // [0] = bytes payload
  be_pushbytes(vm, pkt.data, pkt.len);
  be_data_push(vm, -2);
  be_pop(vm, 1);
  // [1] = addr string
  be_pushstring(vm, pkt.addr);
  be_data_push(vm, -2);
  be_pop(vm, 1);
  // [2] = port int
  be_pushint(vm, pkt.port);
  be_data_push(vm, -2);
  be_pop(vm, 1);

  be_pop(vm, 1);  // pop list internal
  be_return(vm);
}

// ---- OT.udp_close() ----
extern "C" void be_OT_udp_close(struct bvm *vm) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  if (OT_State.udp_open) {
    otUdpClose(instance, &OT_State.udp_socket);
    OT_State.udp_open = false;
    AddLog(LOG_LEVEL_INFO, PSTR("OT : UDP socket closed"));
  }
  ot_unlock();
}

#endif  // USE_MATTER_THREAD
#endif  // USE_BERRY
