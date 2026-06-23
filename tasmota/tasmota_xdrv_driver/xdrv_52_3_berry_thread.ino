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
#include <openthread/netdata.h>
#include <openthread/link.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/logging.h>
#include <openthread/srp_client.h>

// 3. ESP-IDF base includes (still available from framework)
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


/*********************************************************************************************\
 * OpenThread native functions mapped to Berry module "OT"
 *
 * Provides: OT.init(), OT.start(), OT.stop(), OT.set_dataset(),
 *           OT.get_ipaddr(), OT.get_eui64(),
 *           OT.netdata_services(), OT.udp_*(), OT.srp_*()
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
static struct {
  bool          initialized = false;
  TaskHandle_t  task_handle = nullptr;
  bvm          *vm = nullptr;          // Berry VM reference for callbacks
  // UDP socket state (Matter data)
  otUdpSocket   udp_socket;
  QueueHandle_t udp_rx_queue = nullptr;
  bool          udp_open = false;
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
extern "C" void srp_client_callback(otError aError, const otSrpClientHostInfo *aHostInfo,
                                    const otSrpClientService *aServices,
                                    const otSrpClientService *aRemovedServices, void *aContext);
static void srp_server_state_change(const otSockAddr *aServerSockAddr, void *aContext);

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

  // Set up native OT SRP client with auto-start (matches esp-matter flow).
  // The callback runs in the OT task context and logs the server address;
  // auto-start discovers the SRP server from Thread Network Data automatically.
  otSrpClientSetCallback(instance, srp_client_callback, nullptr);
  otSrpClientEnableAutoStartMode(instance, srp_server_state_change, nullptr);

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

extern "C" void be_OT_radio_reclaim(void) {
  bt_radio_reclaim();
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
extern "C" void be_OT_udp_open(struct bvm *vm, int32_t port) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
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
    return;
  }

  otSockAddr sockaddr;
  memset(&sockaddr, 0, sizeof(sockaddr));
  sockaddr.mPort = (uint16_t)port;
  error = otUdpBind(instance, &OT_State.udp_socket, &sockaddr, OT_NETIF_THREAD_INTERNAL);
  if (error != OT_ERROR_NONE) {
    otUdpClose(instance, &OT_State.udp_socket);
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_bind failed: %d", error);
    return;
  }

  if (!OT_State.udp_rx_queue) {
    OT_State.udp_rx_queue = xQueueCreate(OT_UDP_RX_QUEUE_LEN, sizeof(ot_udp_rx_packet_t));
  }

  OT_State.udp_open = true;
  ot_unlock();

  AddLog(LOG_LEVEL_INFO, PSTR("OT : UDP socket opened on port %d"), port);
}

// ---- OT.udp_send(addr, port, payload) ----
extern "C" void be_OT_udp_send(struct bvm *vm, const char *addr_str, int32_t port, const uint8_t *payload, size_t payload_len) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  if (!OT_State.udp_open) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: UDP socket not open");
    return;
  }

  otMessage *message = otUdpNewMessage(instance, nullptr);
  if (!message) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_send no message buffer");
    return;
  }

  otError error = otMessageAppend(message, payload, (uint16_t)payload_len);
  if (error != OT_ERROR_NONE) {
    otMessageFree(message);
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_send append failed: %d", error);
    return;
  }

  otMessageInfo messageInfo;
  memset(&messageInfo, 0, sizeof(messageInfo));
  otIp6AddressFromString(addr_str, &messageInfo.mPeerAddr);
  messageInfo.mPeerPort = (uint16_t)port;

  error = otUdpSend(instance, &OT_State.udp_socket, message, &messageInfo);
  if (error != OT_ERROR_NONE) {
    otMessageFree(message);
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: udp_send failed: %d", error);
    return;
  }
  ot_unlock();
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

// ---- SRP client native support ----

#define MAX_SRP_SERVICES 5
#define MAX_SUBTYPES 10
#define MAX_TXT_ENTRIES 15

struct SrpServiceStorage {
  bool in_use;
  otSrpClientService service;
  char name[32];
  char instance_name[64];
  
  char subtype_buffers[MAX_SUBTYPES][32];
  const char* subtype_ptrs[MAX_SUBTYPES + 1]; // NULL-terminated
  
  otDnsTxtEntry txt_entries[MAX_TXT_ENTRIES];
  char txt_keys[MAX_TXT_ENTRIES][16];
  char txt_values[MAX_TXT_ENTRIES][64];
};

static SrpServiceStorage s_srp_services[MAX_SRP_SERVICES];
static char s_srp_hostname[64];

static int split_string(const char *str, char delimiter, char out[][64], int max_entries) {
  int count = 0;
  const char *start = str;
  while (*start && count < max_entries) {
    const char *end = strchr(start, delimiter);
    int len = end ? (end - start) : strlen(start);
    if (len >= 64) len = 63;
    memcpy(out[count], start, len);
    out[count][len] = '\0';
    count++;
    if (!end) break;
    start = end + 1;
  }
  return count;
}

static bool parse_key_value(const char *entry, char *key, int max_key_len, char *value, int max_val_len) {
  const char *eq = strchr(entry, '=');
  if (!eq) return false;
  int key_len = eq - entry;
  if (key_len >= max_key_len) key_len = max_key_len - 1;
  memcpy(key, entry, key_len);
  key[key_len] = '\0';
  
  int val_len = strlen(eq + 1);
  if (val_len >= max_val_len) val_len = max_val_len - 1;
  memcpy(value, eq + 1, val_len);
  value[val_len] = '\0';
  
  return true;
}

extern "C" void be_OT_srp_set_hostname(struct bvm *vm, const char *hostname) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  strncpy(s_srp_hostname, hostname, sizeof(s_srp_hostname) - 1);
  s_srp_hostname[sizeof(s_srp_hostname) - 1] = '\0';

  otError err = otSrpClientSetHostName(instance, s_srp_hostname);
  if (err == OT_ERROR_NONE) {
    err = otSrpClientEnableAutoHostAddress(instance);
  }
  ot_unlock();

  if (err != OT_ERROR_NONE) {
    be_raisef(vm, "ot_error", "OT: srp_set_hostname failed: %d", err);
  }
}

static void log_hex(const char *label, const uint8_t *data, size_t len) {
  if (!data || len == 0) return;
  char buf[512];
  size_t pos = 0;
  for (size_t i = 0; i < len && pos < sizeof(buf) - 5; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", data[i]);
  }
  buf[pos] = '\0';
  AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP %s HEX (%zu): %s"), label, len, buf);
}

extern "C" void be_OT_srp_add_service(struct bvm *vm, const char *instance_name, const char *service_name, int32_t port, const char *subtypes_str, const char *txt_str) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  int slot = -1;
  for (int i = 0; i < MAX_SRP_SERVICES; i++) {
    if (s_srp_services[i].in_use && strcmp(s_srp_services[i].instance_name, instance_name) == 0) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    for (int i = 0; i < MAX_SRP_SERVICES; i++) {
      if (!s_srp_services[i].in_use) {
        slot = i;
        break;
      }
    }
  }

  if (slot == -1) {
    ot_unlock();
    be_raisef(vm, "ot_error", "OT: srp_add_service: no free slots");
  }

  SrpServiceStorage &storage = s_srp_services[slot];
  if (storage.in_use) {
    otSrpClientClearService(instance, &storage.service);
  }
  memset(&storage, 0, sizeof(storage));
  storage.in_use = true;

  strncpy(storage.name, service_name, sizeof(storage.name) - 1);
  strncpy(storage.instance_name, instance_name, sizeof(storage.instance_name) - 1);

  char subtype_tokens[MAX_SUBTYPES][64];
  int num_subtypes = 0;
  if (strlen(subtypes_str) > 0) {
    num_subtypes = split_string(subtypes_str, ',', subtype_tokens, MAX_SUBTYPES);
  }
  for (int i = 0; i < num_subtypes; i++) {
    strncpy(storage.subtype_buffers[i], subtype_tokens[i], sizeof(storage.subtype_buffers[i]) - 1);
    storage.subtype_ptrs[i] = storage.subtype_buffers[i];
  }
  storage.subtype_ptrs[num_subtypes] = nullptr;

  char txt_tokens[MAX_TXT_ENTRIES][64];
  int num_txt = 0;
  if (strlen(txt_str) > 0) {
    num_txt = split_string(txt_str, ',', txt_tokens, MAX_TXT_ENTRIES);
  }
  int valid_txt_count = 0;
  for (int i = 0; i < num_txt; i++) {
    if (parse_key_value(txt_tokens[i], storage.txt_keys[valid_txt_count], sizeof(storage.txt_keys[0]),
                        storage.txt_values[valid_txt_count], sizeof(storage.txt_values[0]))) {
      storage.txt_entries[valid_txt_count].mKey = storage.txt_keys[valid_txt_count];
      storage.txt_entries[valid_txt_count].mValue = (const uint8_t*)storage.txt_values[valid_txt_count];
      storage.txt_entries[valid_txt_count].mValueLength = strlen(storage.txt_values[valid_txt_count]);
      valid_txt_count++;
    }
  }

  storage.service.mName = storage.name;
  storage.service.mInstanceName = storage.instance_name;
  storage.service.mSubTypeLabels = (num_subtypes > 0) ? storage.subtype_ptrs : nullptr;
  storage.service.mTxtEntries = (valid_txt_count > 0) ? storage.txt_entries : nullptr;
  storage.service.mNumTxtEntries = valid_txt_count;
  storage.service.mPort = port;
  storage.service.mPriority = 0;
  storage.service.mWeight = 0;

  otError err = otSrpClientAddService(instance, &storage.service);
  ot_unlock();

  if (err != OT_ERROR_NONE) {
    be_raisef(vm, "ot_error", "OT: srp_add_service failed: %d", err);
  }
  // hex dump for debugging SRP message content
  log_hex("instance", (const uint8_t*)instance_name, strlen(instance_name));
  log_hex("service", (const uint8_t*)service_name, strlen(service_name));
  for (int i = 0; i < valid_txt_count; i++) {
    char kv[128];
    snprintf(kv, sizeof(kv), "%s=%s", storage.txt_keys[i], storage.txt_values[i]);
    log_hex("txt", (const uint8_t*)kv, strlen(kv));
  }
}

extern "C" void be_OT_srp_remove_service(struct bvm *vm, const char *instance_name, const char *service_name) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  otError err = OT_ERROR_NOT_FOUND;
  for (int i = 0; i < MAX_SRP_SERVICES; i++) {
    if (s_srp_services[i].in_use && strcmp(s_srp_services[i].instance_name, instance_name) == 0) {
      err = otSrpClientClearService(instance, &s_srp_services[i].service);
      s_srp_services[i].in_use = false;
      break;
    }
  }
  ot_unlock();

  if (err != OT_ERROR_NONE && err != OT_ERROR_NOT_FOUND) {
    be_raisef(vm, "ot_error", "OT: srp_remove_service failed: %d", err);
  }
}

// ---- SRP client callback ----
extern "C" void srp_client_callback(otError aError, const otSrpClientHostInfo *aHostInfo,
                                const otSrpClientService *aServices,
                                const otSrpClientService *aRemovedServices, void *aContext) {
  (void)aRemovedServices;
  (void)aContext;
  const char *state_str = "?";
  const char *hostname = "";
  if (aHostInfo) {
    switch (aHostInfo->mState) {
      case OT_SRP_CLIENT_ITEM_STATE_TO_ADD: state_str = "ToAdd"; break;
      case OT_SRP_CLIENT_ITEM_STATE_ADDING: state_str = "Adding"; break;
      case OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH: state_str = "ToRefresh"; break;
      case OT_SRP_CLIENT_ITEM_STATE_REFRESHING: state_str = "Refreshing"; break;
      case OT_SRP_CLIENT_ITEM_STATE_TO_REMOVE: state_str = "ToRemove"; break;
      case OT_SRP_CLIENT_ITEM_STATE_REMOVING: state_str = "Removing"; break;
      case OT_SRP_CLIENT_ITEM_STATE_REGISTERED: state_str = "Registered"; break;
      case OT_SRP_CLIENT_ITEM_STATE_REMOVED: state_str = "Removed"; break;
      default: break;
    }
    if (aHostInfo->mName) {
      hostname = aHostInfo->mName;
    }
  }
  int svc_count = 0;
  for (const otSrpClientService *s = aServices; s; s = s->mNext) {
    svc_count++;
  }
  if (aError == OT_ERROR_NONE) {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP cb OK host=%s state=%s services=%d"), hostname, state_str, svc_count);
  } else {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP cb error=%d host=%s state=%s services=%d"), aError, hostname, state_str, svc_count);
  }
}

// ---- SRP server state change callback (for auto-start mode) ----
// Fired by auto-start when the SRP server is detected or lost.
static void srp_server_state_change(const otSockAddr *aServerSockAddr, void *aContext) {
  (void)aContext;
  if (aServerSockAddr) {
    char ip_str[46];
    otIp6AddressToString(&aServerSockAddr->mAddress, ip_str, sizeof(ip_str));
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP server detected: [%s]:%d"), ip_str, aServerSockAddr->mPort);
  } else {
    AddLog(LOG_LEVEL_INFO, PSTR("OT : SRP server lost (netdata changed)"));
  }
}

extern "C" void be_OT_srp_start(struct bvm *vm) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }
  otSrpClientEnableAutoStartMode(instance, srp_server_state_change, nullptr);
  ot_unlock();
  AddLog(LOG_LEVEL_DEBUG, PSTR("OT : srp_start called (auto-start mode, manual address ignored)"));
}

extern "C" void be_OT_srp_set_lease_interval(struct bvm *vm, int32_t lease, int32_t key_lease) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }
  otSrpClientSetLeaseInterval(instance, (uint32_t)lease);
  otSrpClientSetKeyLeaseInterval(instance, (uint32_t)key_lease);
  ot_unlock();
}

extern "C" void be_OT_srp_stop(struct bvm *vm) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  if (!instance) {
    be_raisef(vm, "ot_error", "OT: not initialized");
    return;
  }

  otSrpClientClearHostAndServices(instance);
  for (int i = 0; i < MAX_SRP_SERVICES; i++) {
    s_srp_services[i].in_use = false;
  }
  ot_unlock();
}

extern "C" bbool be_OT_srp_is_running(void) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  bbool running = bfalse;
  if (instance) {
    running = otSrpClientIsRunning(instance) ? btrue : bfalse;
  }
  ot_unlock();
  return running;
}

extern "C" const char* be_OT_srp_get_host_state(void) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  const char *state_str = "unknown";
  if (instance) {
    const otSrpClientHostInfo *host_info = otSrpClientGetHostInfo(instance);
    if (host_info) {
      state_str = otSrpClientItemStateToString(host_info->mState);
    }
  }
  ot_unlock();
  return state_str;
}

extern "C" const char* be_OT_srp_get_server(void) {
  otInstance *instance = (otInstance*)ot_lock_and_get();
  const char *result = NULL;
  if (instance) {
    const otSockAddr *addr = otSrpClientGetServerAddress(instance);
    if (addr && addr->mPort > 0) {
      static char server_str[80];
      char ip_str[46];
      otIp6AddressToString(&addr->mAddress, ip_str, sizeof(ip_str));
      snprintf(server_str, sizeof(server_str), "[%s]:%d", ip_str, addr->mPort);
      result = server_str;
    }
  }
  ot_unlock();
  return result;
}
#endif  // USE_MATTER_THREAD
#endif  // USE_BERRY
