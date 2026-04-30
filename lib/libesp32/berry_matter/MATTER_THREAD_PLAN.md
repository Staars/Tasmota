# Matter over Thread - Implementation Plan for Tasmota

## 1. Overview

Implement Matter commissioning and operation over Thread for Tasmota on ESP32 SoCs with native IEEE 802.15.4 radio support.

### Supported SoCs
| SoC | 802.15.4 Radio | Wi-Fi | Notes |
|-----|:-:|:-:|-------|
| **ESP32-H2** | ✅ | ❌ | Thread-only, no Wi-Fi. Primary target. |
| **ESP32-C6** | ✅ | ✅ | Dual: Wi-Fi + Thread. Can be standalone Thread device. |
| **ESP32-C5** | ✅ | ✅ | Dual: Wi-Fi + Thread (newer silicon). |

### Key Difference from BLE
- **BLE** is a *commissioning-only* transport. After commissioning, the device operates over Wi-Fi/IP (UDP). BLE requires BTP (segmentation/reassembly) because BLE GATT has small MTU.
- **Thread** is both the *commissioning AND operational* transport. Thread provides a full IPv6 mesh network — Matter messages travel as standard UDP/IPv6 packets over Thread. **No custom transport protocol (no BTP equivalent) is needed.** The OpenThread stack provides a standard IP netif.

---

## 2. Architecture

### 2.1 What ESP-IDF Provides (OpenThread)

ESP-IDF includes a full **OpenThread** stack. Key APIs:

```
esp_openthread_init()           // Initialize OT stack
esp_openthread_launch_mainloop() // Run OT main loop (separate task)
esp_openthread_lock_acquire()   // Thread-safety lock
esp_openthread_lock_release()

// Network interface
esp_netif_new(ESP_NETIF_DEFAULT_OPENTHREAD)  // Create Thread netif
esp_openthread_netif_glue_init()
esp_netif_attach()

// Operational Dataset (Thread network credentials)
otDatasetSetActiveTlvs()        // Set Thread network credentials
otDatasetGetActiveTlvs()        // Get current credentials
otIp6SetEnabled()               // Enable IPv6
otThreadSetEnabled()            // Start/stop Thread
otThreadGetDeviceRole()         // Get current role (disabled/detached/child/router/leader)
```

**OpenThread modes on ESP:**
- **Standalone Node**: Full OT stack + app on same chip (ESP32-H2, C6, C5) → **This is our target**
- Radio Co-Processor (RCP): Chip acts as 802.15.4 radio for a host → Not relevant
- OpenThread Host: Wi-Fi chip connects to external RCP → Possible future Border Router mode

### 2.2 Component Architecture

```
┌─────────────────────────────────────────────────────┐
│                    Berry Layer                       │
│                                                     │
│  Matter_Thread_Device.be    (at repo root)          │
│  ├── extends matter.Device                          │
│  ├── overrides msg_send() → UDP over Thread netif   │
│  ├── handles Thread network provisioning            │
│  ├── NetworkCommissioning cluster (Thread variant)  │
│  └── SRP client for mDNS-SD on Thread               │
│                                                     │
├─────────────────────────────────────────────────────┤
│                    C++ Bridge                        │
│                                                     │
│  xdrv_52_3_berry_thread.ino                         │
│  ├── #ifdef USE_MATTER_THREAD                       │
│  ├── OpenThread init & main loop task               │
│  ├── Thread netif creation                          │
│  ├── Berry native module "OT" (OpenThread)          │
│  │   ├── OT.init()                                  │
│  │   ├── OT.start() / OT.stop()                    │
│  │   ├── OT.set_dataset(tlv_bytes)                  │
│  │   ├── OT.get_dataset() → bytes                   │
│  │   ├── OT.get_role() → string                     │
│  │   ├── OT.get_ipaddr() → list of IPv6             │
│  │   ├── OT.get_eui64() → string                    │
│  │   ├── OT.factory_reset()                         │
│  │   └── OT.state_cb(callback)                      │
│  └── SRP service registration helpers               │
│                                                     │
├─────────────────────────────────────────────────────┤
│                 ESP-IDF / OpenThread                  │
│                                                     │
│  esp_openthread (component)                         │
│  └── 802.15.4 radio driver                          │
└─────────────────────────────────────────────────────┘
```

### 2.3 How It Differs from the BLE Approach

| Aspect | BLE (Matter_BLE_Device.be) | Thread (Matter_Thread_Device.be) |
|--------|---------------------------|----------------------------------|
| Transport role | Commissioning only | Commissioning + Operational |
| Underlying protocol | BTP over GATT | UDP/IPv6 over 802.15.4 mesh |
| Custom protocol needed | Yes (BTP class) | No — standard UDP sockets |
| Post-commission transport | Switch to Wi-Fi UDP | Stay on Thread UDP |
| Network provisioning | WiFi SSID+password | Thread Operational Dataset (TLV) |
| Device discovery | BLE advertising | DNS-SD via SRP (Thread uses SRP client, not mDNS multicast) |
| NetworkCommissioning cluster | WiFi type (feature 0x01) | Thread type (feature 0x02) |
| msg_send() | BTP segmented send or UDP | UDP send (same as base Device) |
| SoC requirement | Any with BLE (ESP32-S3 etc.) | ESP32-H2, C6, C5 (802.15.4) |

---

## 3. Implementation Phases

### Phase 1: C++ Driver — `xdrv_52_3_berry_thread.ino`

**Goal:** Initialize OpenThread, create the Thread netif, expose Berry native module `OT`.

**Tasks:**
1. Create `xdrv_52_3_berry_thread.ino` in `tasmota/tasmota_xdrv_driver/`
2. Guard with `#ifdef USE_MATTER_THREAD`
3. Initialize OpenThread stack in a dedicated FreeRTOS task:
   - `esp_vfs_eventfd_register()`
   - `esp_openthread_init()` with platform config (radio = native, host = none, port = auto)
   - Create Thread netif: `esp_netif_new(ESP_NETIF_DEFAULT_OPENTHREAD)`
   - `esp_openthread_netif_glue_init()` + `esp_netif_attach()`
   - Launch `esp_openthread_launch_mainloop()` in the task
4. Implement Berry native module `OT` with functions:
   - `OT.init()` — start the OT task
   - `OT.set_dataset(bytes)` — set Active Operational Dataset from TLV bytes
   - `OT.get_dataset()` — return current dataset as TLV bytes
   - `OT.start()` — enable IPv6 + start Thread (`otIp6SetEnabled` + `otThreadSetEnabled`)
   - `OT.stop()` — stop Thread
   - `OT.get_role()` — return device role string ("disabled"/"detached"/"child"/"router"/"leader")
   - `OT.get_ipaddr()` — return list of IPv6 addresses
   - `OT.get_eui64()` — return EUI-64 of the 802.15.4 radio
   - `OT.state_cb(closure)` — register callback for state changes (role changes, connectivity)
   - `OT.srp_register(service_name, service_type, port, txt_entries)` — register SRP service for DNS-SD
   - `OT.srp_remove(service_name)` — remove SRP service
   - `OT.factory_reset()` — erase Thread persistent data
5. SRP client integration for service discovery (Thread devices register services via SRP to the Border Router, which makes them discoverable via DNS-SD)

**Key considerations:**
- OpenThread APIs are **not thread-safe**. Must use `esp_openthread_lock_acquire/release` when calling from Berry's task.
- OpenThread mainloop runs in its own FreeRTOS task.
- State change callback should use `tasmota.defer()` to avoid calling Berry from the OT task context.

### Phase 2: Berry Device — `Matter_Thread_Device.be`

**Goal:** Create Thread-specific Matter Device class that handles commissioning and operation over Thread.

**Tasks:**
1. Create `Matter_Thread_Device.be` at the Tasmota repo root
2. Class `MATTER_THREAD` extends `matter.Device`
3. Key overrides and additions:

```berry
class MATTER_THREAD : matter.Device
    var ot_started       # bool: OpenThread initialized
    var thread_connected # bool: Thread network attached

    def init()
        # Initialize OpenThread via OT.init()
        # Set up state callback
        # Initialize Matter stack (sessions, message_handler, etc.)
        # Same pattern as MATTER_BLE but no BTP needed
    end

    def start()
        # Start UDP server on Thread netif
        # Register SRP services for operational discovery
        self._start_udp(self.UDP_PORT)
    end

    def msg_send(msg)
        # Simply delegate to UDP server — Thread provides IP transport
        self.udp_server.send_UDP(msg)
    end

    # Thread network provisioning (called during commissioning)
    def provision_thread_network(dataset_tlv)
        OT.set_dataset(dataset_tlv)
        OT.start()
    end

    def every_second()
        super(self).every_second()
        # Monitor Thread connectivity
        # Check role changes
    end
end
```

4. **No BTP equivalent needed** — Thread provides standard UDP/IPv6, so `msg_send()` just uses the existing UDP server.

### Phase 3: NetworkCommissioning Cluster — Thread Variant

**Goal:** Handle Thread network credentials during Matter commissioning.

The Matter NetworkCommissioning cluster (0x0031) has a Thread-specific feature set:

**Current WiFi implementation** (in `Matter_Plugin_1_Root.be`):
- Feature map: `0x01` (WiFi)  
- Commands: `AddOrUpdateWiFiNetwork` (0x0002), `ConnectNetwork` (0x0006)

**Thread implementation needs:**
- Feature map: `0x02` (Thread)
- Commands:
  - `ScanNetworks` (0x0000) → Optional, can return empty for Thread
  - `AddOrUpdateThreadNetwork` (0x0003) → Receives Thread Operational Dataset TLV
  - `ConnectNetwork` (0x0006) → Apply dataset and join Thread network
  - `RemoveNetwork` (0x0004) → Remove stored Thread network

**Attributes for Thread:**
- `Networks` (0x0001) → List of configured Thread networks
- `ScanMaxTimeSeconds` (0x0002)
- `ConnectMaxTimeSeconds` (0x0003) → Time to join Thread network
- `InterfaceEnabled` (0x0004) → Is Thread interface up
- `LastConnectErrorValue` (0x0007)
- `SupportedThreadFeatures` (0x0009) → Thread-specific
- `ThreadVersion` (0x000A)

**Implementation approach:**
- Override the cluster in the Thread-specific Root Plugin or detect transport type at runtime
- The Thread Operational Dataset is a TLV blob containing: channel, PAN ID, extended PAN ID, network name, network key, mesh-local prefix, etc.
- On `AddOrUpdateThreadNetwork`: store the dataset
- On `ConnectNetwork`: call `OT.set_dataset()` + `OT.start()`, wait for role to become "child" or "router"

### Phase 4: Service Discovery — SRP Instead of mDNS

**Goal:** Thread devices use SRP (Service Registration Protocol) instead of multicast mDNS.

**How it works:**
- Thread networks don't support multicast well → mDNS doesn't work directly
- Thread Border Routers run an SRP server
- Thread end devices register services via SRP client to the Border Router
- The Border Router then advertises these on the Wi-Fi/Ethernet side via mDNS
- This makes the Thread device discoverable by Matter controllers

**Tasks:**
1. In the C++ driver, integrate with OpenThread's SRP client:
   - `otSrpClientSetCallback()` — state change notifications
   - `otSrpClientStart()` — auto-start when Thread attaches
   - `otSrpClientAddService()` — register _matter._tcp / _matterc._udp services
   - `otSrpClientSetHostName()` — set hostname for the device
   - `otSrpClientSetHostAddresses()` — set IPv6 addresses
2. In Berry, replace `mdns` calls with `OT.srp_register()` calls when running on Thread
3. Modify `Matter_Commissioning.be` or add overrides in `Matter_Thread_Device.be` to use SRP for:
   - Commissionable discovery (`_matterc._udp`)
   - Operational discovery (`_matter._tcp`)

### Phase 5: Commissioning Flow — End to End

**The complete Matter-over-Thread commissioning flow:**

```
Commissioner (phone)                    Thread Device (ESP32-H2)
        |                                        |
        |  1. Scan QR code / manual code          |
        |  2. Discover via BLE advertisement (*)  |
        |-----BLE PASE (secure channel)---------->|
        |  3. PASE established                    |
        |  4. Read device info, attestation       |
        |  5. Install NOC (fabric credentials)    |
        |  6. AddOrUpdateThreadNetwork            |
        |     (Thread Operational Dataset TLV) -->|--- OT.set_dataset()
        |  7. ConnectNetwork ------------------>  |--- OT.start()
        |     (device joins Thread network)       |--- Role: child/router
        |  8. Device registers via SRP            |--- OT.srp_register()
        |  9. Commissioner discovers via DNS-SD   |
        |----CASE (over Thread/IP)--------------->|
        | 10. CommissioningComplete               |
        |                                         |
```

**(*) Important:** For Thread devices, the initial commissioning usually happens over **BLE** — the controller discovers the device via BLE, does PASE over BLE, provisions Thread credentials, then switches to Thread for CASE. This means:
- **ESP32-H2**: Has BLE + 802.15.4. Can do BLE commissioning → Thread operation.
- **ESP32-C6**: Has BLE + Wi-Fi + 802.15.4. Can commission via BLE → Thread, or via Wi-Fi.

This means `Matter_Thread_Device.be` needs to handle the **BLE commissioning phase** (similar to `Matter_BLE_Device.be`) and then transition to Thread for operational communication. The BTP/GATT setup from BLE can be reused.

**Alternative:** An "on-network" commissioning where the device is already on a Thread network could skip BLE entirely.

---

## 4. File Map

| File | Location | Purpose |
|------|----------|---------|
| `xdrv_52_3_berry_thread.ino` | `tasmota/tasmota_xdrv_driver/` | C++ driver: OpenThread init, Berry `OT` module |
| `Matter_Thread_Device.be` | repo root | Berry: Thread-specific Matter Device class |
| Feature guard | build defines | `#define USE_MATTER_THREAD` |
| Root Plugin changes | `Matter_Plugin_1_Root.be` | Thread variant of NetworkCommissioning cluster (0x0031) |
| Plugin_0 changes | `Matter_Plugin_0.be` | Feature map: `0x02` for Thread instead of `0x01`/`0x05` for WiFi |

---

## 5. Open Questions & Decisions

1. **BLE+Thread hybrid commissioning:** The Matter spec says Thread devices MUST support BLE for commissioning. Should `Matter_Thread_Device.be` *include* the BLE/BTP commissioning code from `Matter_BLE_Device.be`, or should we create a combined class?

2. **ESP32-H2 vs C6 differences:** H2 has no Wi-Fi, so after commissioning the device stays entirely on Thread. C6 could potentially commission over BLE and then operate on either Wi-Fi or Thread. Should we support both paths on C6?

3. **Thread device type:** Sleepy End Device (SED), Minimal End Device (MED), or Full Thread Device (FTD)? For mains-powered Tasmota devices, FTD makes sense. For battery devices, SED.

4. **Border Router mode:** Should Tasmota on ESP32-C6 (which has both Wi-Fi and 802.15.4) also support acting as a Thread Border Router? This is a separate, larger effort but very valuable.

5. **DNS-SD / SRP timing:** The SRP registration must happen after the Thread network is joined and stable. Need to handle the timing carefully — retries if SRP server is not yet reachable.

6. **Coexistence with WiFi Matter:** On C6, could a device simultaneously serve Matter over WiFi and Thread? Probably not needed initially.

---

## 6. Dependencies

- **ESP-IDF OpenThread component**: Already included in ESP-IDF. Needs to be enabled in sdkconfig/platformio config for builds targeting H2/C6/C5.
- **Build system**: Need new build environment/variant for ESP32-H2 and Thread-enabled C6 builds with `USE_MATTER_THREAD` defined.
- **Flash/RAM budget**: OpenThread adds ~150-200KB flash. ESP32-H2 has 320KB SRAM + 4MB flash, should be sufficient.

---

## 7. Suggested Implementation Order

1. **Phase 1a**: Minimal C++ driver with OT init + Berry `OT` module (init, start, stop, get_role, set_dataset)
2. **Phase 1b**: SRP client integration in C++ driver
3. **Phase 2**: `Matter_Thread_Device.be` — basic class that starts OT, joins network, and opens UDP server
4. **Phase 3**: NetworkCommissioning cluster Thread variant (AddOrUpdateThreadNetwork + ConnectNetwork)
5. **Phase 4**: SRP-based service discovery (replace mDNS for Thread)
6. **Phase 5**: BLE commissioning integration (reuse BTP from `Matter_BLE_Device.be` for initial PASE)
7. **Phase 6**: End-to-end testing with chip-tool or phone commissioner
8. **Phase 7** (future): Border Router mode on ESP32-C6
