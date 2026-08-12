# Matter over Thread — SRP & Commissioning Status

## Root cause (found 2025-06-19)

The 6‑week‑old SRP failure was **not** crypto, SIG(0), server selection, or the
hand‑rolled Berry client. The root cause was a **MAC‑layer outbound security bug
in BearThread's radio driver** (`lib/libesp32/BearThread/src/bt_radio.c`):

```
otPlatRadioTransmit() — data‑TX path:

  Bug 1: inverted condition
    if (mIsSecurityProcessed)  →  should be  if (!mIsSecurityProcessed)

  Bug 2: missing call to esp_ieee802154_set_transmit_security()
    The hardware never encrypted the payload, set the key‑id, or wrote the MIC.
    Frame left the device with valid FCS but no MIC.
    The BR L2‑ACKed it, then dropped it silently on MAC‑security verify.

  Symptom: first ever "sec:yes" outbound data frame (SRP UPDATE) never got a
  response, error 28 (YXDOMAIN) / timeout → retransmit forever.

  The enh‑ack TX path (bt_radio.c:513) DID call the function correctly,
  so ACKs worked and the bug was invisible until a sec:yes data frame was sent.
```

**Fix:** replaced the 4‑line `#if OPENTHREAD_CONFIG_THREAD_VERSION_1_2` block
with the exact logic from the IDF reference
(`esp_openthread_radio.c:293‑309`): check `otMacFrameIsSecurityEnabled &&
!mIsSecurityProcessed`, set frame counter / key‑id for KeyIdMode1, copy the
current key, and call `esp_ieee802154_set_transmit_security()` before transmit.

**After the fix:** SRP registration now succeeds (`state=Registered`, first try).

---

## Current status (2025-06-20)

| Stage | Status |
|-------|--------|
| BLE PASE commissioning | ✓ |
| Thread attach (child) | ✓ |
| SRP registration (`_matter._tcp`) | ✓ |
| CASE session over Thread | ✓ |
| Full Matter commissioning | ✓ |

### Root cause of CASE failure — `coex_prefer_thread(true)` killed BLE

After SRP was fixed, CASE still failed because **no UDP packets ever arrived**
on port 5540. The commissioner never attempted CASE over Thread.

Timeline of the bug:

```
ConnectNetwork received over BLE
  → provision_thread_network() calls OT.start()
  → provision_thread_network() calls OT.coex_prefer_thread(true)   ← BUG
  → 802.15.4 radio set to HIGH priority → starves BLE
  → ~0.9 s later BLE disconnects
  → ConnectNetworkResponse deferred until Thread attaches (~5 s later)
  → By then BLE is dead → response silently dropped (ble_ready == false)
  → Commissioner never gets ConnectNetworkResponse
  → Never proceeds to operational discovery / CASE
  → Device waits forever, "OT UDP poll alive (no packets)"
```

**Fix (two parts in `Matter_Thread_Device.be`):**

1. **Removed `coex_prefer_thread(true)` from `provision_thread_network()`.**
   The coex bias to Thread was only needed once the device is on the network,
   not during network attachment. The `ot_state_changed()` handler already
   sets coex at role=child, which is after ConnectNetworkResponse is sent.

2. **Respond to ConnectNetwork immediately instead of deferring.**
   The old code deferred the response until Thread attached (5 s delay).
   The new code sends `{Success, "thread provisioning started"}` right away,
   over the still‑alive BLE link. The commissioner then finds the device
   via DNS‑SD / SRP and establishes CASE asynchronously.

Without these two changes, the ESP32‑C6's shared 2.4 GHz radio starves BLE
whenever 802.15.4 gets high coexistence priority — even before Thread has
attached — making the BLE transport unreliable for any operation that spans
the Thread‑startup window.

### What's working now

- Full commissioning flow over Thread (Apple TV BR, iPhone commissioner):
  1. BLE PASE (FFF6 service)
  2. AddOrUpdateThreadNetwork
  3. ConnectNetwork → immediate response over BLE
  4. Device joins Thread, attaches as child
  5. SRP registers `_matter._tcp` (native OT client, auto-address mode)
  6. Commissioner discovers via DNS‑SD / BR proxy
  7. CASE session established over Thread/UDP port 5540
  8. All subsequent Matter operation over Thread

---

## What changed to get SRP working

Along the way several other changes were made; they are **not needed** for
SRP but are now part of the current build:

| Change | Reason | Revert‑able? |
|--------|--------|-------------|
| `bt_radio.c` MAC security fix | **Root cause** | No — required |
| `CRYPTO_LIB = 0` (MBEDTLS) instead of `2` (PLATFORM/BearSSL) | Ruled out crypto; matches esp‑matter | **Yes** — can go back to BearSSL |
| `bt_crypto_bearssl.cpp` wrapped in `#if 0` | Elimination debug | **Yes** — restore BearSSL |

*Note: SRP SIG(0) uses ECDSA, not HMAC. HMAC is used by MLE, HKDF, etc.*

---

## Future direction

Now that full commissioning works over Thread, two tracks remain:

### A. Clean up debug diagnostics
The current build has diagnostic overhead added during the CASE investigation:
- `every_50ms()` heartbeat log every 5 s
- Unconditional UDP recv log at INFO level
- UDP RX packet counter + INFO log in C callback

These should be removed or demoted to DEBUG level once the fix is confirmed
stable across reboots / power cycles.

---

## Key files

| File | Role |
|------|------|
| `lib/libesp32/BearThread/src/bt_radio.c` | Radio driver; MAC‑security TX bug fixed at line 278‑292 |
| `lib/libesp32/BearThread/include/bearthread-core-config.h` | OT config; CRYPTO_LIB=0 now, was 2 |
| `lib/libesp32/BearThread/src/bt_crypto_bearssl.cpp` | BearSSL crypto — `#if 0`'d, restore for BearSSL path |
| `lib/libesp32/BearThread/src/bt_misc.cpp` | `otPlatSettings*` persistence |
| `tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino` | OT bindings: `srp_*`, callbacks |
| `Matter_Thread_Device.be` | Berry Matter orchestrator |
| Reference | `~/Developer/esp-matter/examples/light` + IDF `esp_openthread_radio.c` |

---
## Regression: WiFi ON blocks CASE over Thread on C6 (2025-06-21)

After removing `wifi 0` from the commissioned boot path, the device registers
SRP successfully (`services=2`) but stays **unreachable** from Apple Home.

### Root cause
The ESP32-C6 shares a single 2.4 GHz analog front-end between WiFi and 802.15.4.
With WiFi ON and actively transmitting (MQTT, web server), the coexistence
scheduler's default priority (`txrx=LOW`) causes 802.15.4 RX drops — including
CASE Sigma1 from the Border Router.

### Proof
Manual `wifi 0` after boot instantly makes the device reachable.

### Design options (pending)

| Option | Trade-off |
|--------|-----------|
| **A: WiFi OFF for commissioned** — revert to original `wifi 0` + `pending_radio_reclaim` | ✓ Proven to work, simple. ✗ No web/MQTT/OTA during Thread runtime. |
| **B: `coex_prefer_thread(true)` with WiFi ON** — restore coex bias now that radio reclaim is fixed | ✓ WiFi stays ON. ✗ Previously caused WiFi disconnects — may have been due to now-fixed reclaim spam, needs retest. |
| **C: WiFi OFF by default, on-demand toggle** — `wifi 0` on boot, command/topic for timed `wifi 1` (e.g. 5 min for OTA) | ✓ Thread reliable, WiFi available for maintenance. ✗ More complex. |
| **D: Per-traffic-class coex** — bias Thread only during CASE window | ✗ Over-engineered, fragile. |



```
pio run -e tasmota32c6   (or tasmota32c6-mi32)
```

---
---

# Matter Client (Initiator) Capabilities — Implementation Plan (2025-06-24)

## 1. Motivation

The concrete trigger is **time synchronization**: a Matter-over-Thread device
loses wall-clock time on reboot. The controller only pushes `SetUTCTime`
during commissioning, never after a reboot (confirmed on Apple Home). The
spec-correct, controller-independent fix is for the **device itself to pull
the time** from a peer node (`Time Synchronization` cluster `0x0038`,
attribute `UTCTime 0x0000`) — i.e. the device must act as a **Matter client /
initiator**, sending a Read Request to another node and processing the
Report Data response.

This same capability unlocks a broader class of features that Tasmota cannot
do today:

- Pull time from a `TrustedTimeSource` / the controller on boot (primary goal).
- Read attributes from other Matter nodes (sensor mirroring, bridging).
- Invoke commands on other nodes (e.g. a Tasmota button controlling a Matter
  bulb directly, "controller-lite").
- Subscribe to remote attributes.

This plan is intentionally **incremental**: it delivers the time-sync use case
first (Read client only), and leaves a clean path toward Invoke/Subscribe and
a minimal controller later.

## 2. Current architecture: server-only (confirmed)

Tasmota's Berry Matter stack is **strictly a Matter Server / responder**. It
parses incoming Read/Subscribe/Invoke/Write requests from a controller and
sends responses back **over exchanges the controller initiated**. It has no
code to discover a peer, open a CASE session to it, or send a request to it.

Evidence in the codebase:

| Concern | Status | Evidence |
|---------|--------|----------|
| Response opcodes (client-side) | **Stubbed → `return false`** | `Matter_IM.be:84-101` — Subscribe Response 0x04, Report Data 0x05, Write Response 0x07, Invoke Response 0x09 all `return false` "not implemented for Matter device"; handlers commented out at `Matter_IM.be:1064,1073,1189,1198` |
| CASE | **Responder only** | `Matter_Commissioning_Context.be` implements `parse_Sigma1/parse_Sigma3` (we receive Sigma1, we send Sigma2). No `build_Sigma1` / `parse_Sigma2` initiator path |
| Operational discovery | **Register only, no query** | `xdrv_52_3_berry_thread.ino` binds `otSrpClient*` (register our own `_matter._tcp`) and `otNetDataGetNextService` (read SRP server entries). **No `otDnsClient` browse/resolve binding** — confirmed by grep |
| Outgoing request builders | **None** | `Matter_IM_Data.be` has `ReadRequestMessage` / `InvokeRequestMessage` TLV classes, but they are used only to *parse incoming* requests or to build *fake/internal* reads (`send_subscribe_update`, `Matter_IM.be:1247`) |

## 3. What already exists and is reusable

The good news: most of the hard primitives already exist and are reusable. The
client path is mostly *new orchestration over existing building blocks*.

| Building block | Location | Reuse for client |
|----------------|----------|------------------|
| **Initiator frame builder** | `Matter_Message.be:315` `Frame.initiate_response()` — sets `x_flag_i=1`, allocates a fresh local `exchange_id` (`session._exchange_id += 1`, `\| 0x10000`), picks `counter_snd_next()` | Already used to push ReportData on controller-initiated subscriptions (`Matter_IM_Message.be:462,573`). It can send any opcode on an **already-established** session — extend it to drive new requests |
| **Frame encode / encrypt / decrypt** | `Matter_Message.be` `encode_frame()`, `encrypt()`, `decrypt()` | Symmetric — works for initiator or responder once keys exist |
| **Session object** | `Matter_Session.be` — `_ip`/`_port`, `counter_snd_next()`, `_exchange_id`, `get_i2r()`/`get_r2i()`, fabric accessors (`get_noc`, `get_ipk_epoch_key`, `get_fabric_compressed`, `get_pk` operational key) | A CASE-initiator session is the same object, just established from the other side |
| **MRP / retransmit / Ack matching** | `Matter_IM.be` `send_queue`, `send_enqueued()`, `process_incoming_ack()`, `find_sendqueue_by_exchangeid()`; `Matter_IM_Message` lifecycle (`ready`/`finishing`/`finished`, `ack_received`) | The same exchange/queue machinery tracks outgoing reliable client requests waiting for a response |
| **CASE crypto + TLV** | `Matter_Commissioning_Context.be` (HKDF labels `Sigma2`/`Sigma3`, AES-CCM, destination-id computation `find_fabric_by_destination_id`), `Matter_IM_Data.be` `Sigma1/Sigma2/Sigma3` TLV classes | Reused verbatim; we add the *opposite* role of each Sigma step |
| **Fabric / operational credentials** | `Matter_Fabric.be` — NOC, ICAC, IPK, root CA, operational node key | Everything CASE-initiator needs to authenticate to a peer in the same fabric |
| **SRP server entries from netdata** | `be_OT_netdata_services` (`xdrv_52_3_berry_thread.ino:308`) | Helps locate the SRP/BR server but is *not* a per-node resolver |

## 4. Gap analysis — the four missing layers

```diagram
╭──────────────────────────────────────────────────────────────╮
│  Application:  TimeSyncClient / generic ReadClient            │  ← NEW (Phase 4–5)
├──────────────────────────────────────────────────────────────┤
│  IM Initiator:  build ReadRequest/InvokeRequest,             │  ← NEW (Phase 3)
│                 handle ReportData/InvokeResponse/Status      │     (opcodes that
│                 (today: return false)                        │      today return false)
├──────────────────────────────────────────────────────────────┤
│  CASE Initiator:  build Sigma1, parse Sigma2, build Sigma3,  │  ← NEW (Phase 2)
│                   derive operational session keys           │
├──────────────────────────────────────────────────────────────┤
│  Operational discovery:  resolve                            │  ← NEW (Phase 1,
│   <compressed-fabric>-<node-id>._matter._tcp → IP:port      │     native binding)
├──────────────────────────────────────────────────────────────┤
│  EXISTING & REUSABLE                                          │
│  Frame.initiate_response · encode/encrypt · MRP send_queue · │
│  Session · Fabric · TLV · CASE crypto helpers                │
╰──────────────────────────────────────────────────────────────╯
```

1. **Operational discovery** — given a target `node_id` (and our fabric's
   compressed-fabric-id), resolve the operational DNS-SD instance
   `<compressed-fabric-id-hex>-<node-id-hex>._matter._tcp.local` to an
   IPv6 address + port (+ optional MRP/SAI/SII TXT params). Today there is
   **no DNS resolver binding** — only SRP register and a raw netdata reader.
2. **CASE initiator** — build `Sigma1`, parse `Sigma2`, build `Sigma3`, run
   the key schedule from the initiator's perspective, and create an
   operational `Matter_Session` with `i2r`/`r2i` keys. Today only the
   responder half exists.
3. **IM initiator** — build outgoing `ReadRequestMessage` / `InvokeRequestMessage`
   frames, enqueue them as reliable exchanges, and **implement the response
   handlers** (`process_report_data` 0x05, `process_invoke_response` 0x09,
   `process_write_response` 0x07, plus matching `Status Response` 0x01 to a
   pending request) that currently `return false`.
4. **Application clients** — a small `ReadClient` future/callback API, and the
   `TimeSyncClient` that uses it on boot.

## 5. Implementation phases

> **Build separation note:** every file and edit below is compiled **only** in
> the Thread/client build and adds **zero bytes** to the standard WiFi
> `USE_MATTER_DEVICE` firmware. The file names used here are conceptual; see
> **§6** for the gated `*_Thread.be` / `*_Client_Thread.be` names and the exact
> `#if USE_MATTER_THREAD` / `#if USE_MATTER_CLIENT` guards.

### Phase 0 — Scope decision (no code)

Decide the ceiling. Recommended: **"client-lite"** — initiate CASE + Read
(and later Invoke) to a peer **inside an existing fabric** using our own NOC.
Explicitly **out of scope**: acting as a commissioner (PASE, AddNOC,
certificate issuance, ACL administration of other nodes). That keeps the work
to ~the four layers above and avoids a full controller.

A key design question to settle up front: **which peer do we talk to and how
do we get its node_id?**
- For time sync, the natural target is the **administrator/controller node**
  for one of our fabrics, or a configured `TrustedTimeSource` node id. The
  fabric already stores `admin_subject` / `admin_vendor`
  (`Matter_Session.be:261-262`); `admin_subject` typically carries the
  controller's node id (CASE Authenticated Tag / node id). Verify this is
  populated and usable as the resolve target.

### Phase 1 — Operational discovery (native OT DNS resolver)

**New native binding** in `xdrv_52_3_berry_thread.ino` (C/C++):
- `OT.dns_resolve_service(instance_name, service_name)` →
  wraps `otDnsClientResolveService()` (and/or `otDnsClientBrowse`), with an
  async callback that returns `{host, port, addresses[], txt{}}` to Berry.
- Include `<openthread/dns_client.h>`; OpenThread's DNS client uses the BR as
  recursive resolver over Thread, which is already our data path.
- Mirror the existing async SRP callback pattern
  (`srp_client_callback`, `xdrv_52_3_berry_thread.ino:86`) — store pending
  request state, signal completion to Berry via the existing event/poll loop.

**New Berry helper** (small) — `Matter_Operational_Discovery.be`:
- Compute the operational instance name
  `format("%016X-%016X", compressed_fabric_id, node_id)` using
  `session.get_fabric_compressed()` and the target node id.
- Call `OT.dns_resolve_service(...)`, cache result (with TTL), expose a
  future/callback returning `(ip, port)`.

> Fallback if a DNS binding proves hard: the BR proxies SRP, and
> `otNetDataGetNextService` already lists registered hosts; a minimal resolver
> could be built from SRP/netdata for same-fabric peers. Prefer the proper
> `otDnsClient` path.

### Phase 2 — CASE initiator

**New file** `Matter_Commissioning_Initiator.be` (or extend
`Matter_Commisioning_Context`) implementing the initiator half:

| Step | Direction | Reuse |
|------|-----------|-------|
| Build & send `Sigma1` | we → peer | `Sigma1` TLV class (`Matter_IM_Data.be`); destination-id computation logic mirrored from `find_fabric_by_destination_id` (`Matter_Commissioning_Context.be:288`); ephemeral key via `crypto.EC_P256`; `Frame.initiate_response(..., opcode=0x30, PROTOCOL_ID_SECURE_CHANNEL)` on an **unsecured** session |
| Parse `Sigma2` | peer → we | `Sigma2` TLV class; AES-CCM + HKDF helpers already in responder |
| Build & send `Sigma3` | we → peer | `Sigma3` TLV class; sign TBSData3 with our operational key `session.get_pk()` / NOC (`Matter_Session.be:271`) |
| Receive `StatusReport` (SessionEstablished) | peer → we | `parse_StatusReport` (`Matter_Commissioning_Context.be:711`) |
| Derive operational keys | — | same key schedule as responder (`add_session`, `Matter_Commissioning_Context.be:46`), but `i2r`/`r2i` roles are from the **initiator** perspective |

Critical correctness points:
- **i2r vs r2i orientation**: as initiator we *encrypt with i2r, decrypt with
  r2i* (the responder does the opposite). The shared `Session.get_i2r/get_r2i`
  accessors stay the same; only which one we use for encrypt/decrypt flips. The
  existing responder code must not be disturbed — implement initiator
  encrypt/decrypt selection in the new client path.
- **Unsecured PROTOCOL_ID_SECURE_CHANNEL exchange** for Sigma1–3, then promote
  to the new operational session — Sigma1 needs an initiator exchange on a
  `local_session_id=0` frame; extend `initiate_response` (or add an
  `initiate_unsecure`) to allocate the unsecured exchange and our chosen
  `initiator_session_id`.
- Route incoming Sigma2/StatusReport: `Matter_MessageHandler.msg_received`
  currently sends unsecured secure-channel traffic to `commissioning`
  (responder). Add dispatch so frames belonging to a **locally-initiated**
  CASE exchange (our `exchange_id` with bit `0x10000`) go to the initiator.

### Phase 3 — IM initiator (requests + response handling)

In `Matter_IM.be`:
- **Replace the four `return false` stubs** (`Matter_IM.be:84-101`) with real
  handlers that look up the pending outgoing exchange via
  `find_sendqueue_by_exchangeid(msg.exchange_id)`:
  - `0x05 Report Data` → `process_report_data()` — parse `ReportDataMessage`,
    deliver attribute values to the waiting client, send Status/Ack.
  - `0x09 Invoke Response` → `process_invoke_response()`.
  - `0x07 Write Response` → `process_write_response()`.
  - `0x01 Status Response` already partially handled
    (`process_status_response`, `Matter_IM.be:214`) — ensure it resolves a
    pending *client* request, not just a server response.
- **New outgoing request classes** in `Matter_IM_Message.be` (subclass
  `Matter_IM_Message`, build with `Frame.initiate_response`, opcode `0x02`
  Read / `0x08` Invoke / `0x06` Write, `reliable=true`):
  - `Matter_IM_ReadRequest_Out`, `Matter_IM_InvokeRequest_Out`,
    `Matter_IM_WriteRequest_Out`.
  - Each carries a completion future/callback resolved when the matching
    response opcode arrives (correlated by `exchange_id`).
- Reuse `send_queue` + `send_enqueued` + MRP retransmit/timeout unchanged.

### Phase 4 — Application-facing client API

**New file** `Matter_Client.be` — thin orchestrator combining Phases 1–3:

```
matter.Client(device)
  .read_attribute(node_id, endpoint, cluster, attribute) -> future(value)
  .invoke_command(node_id, endpoint, cluster, command, args) -> future(status)
```

Flow per call: ensure operational session (Phase 1 resolve → Phase 2 CASE,
cached & reused) → build request (Phase 3) → resolve future on response.
Cache CASE sessions and honor session resumption to avoid a full handshake on
every call.

### Phase 5 — Time sync application (primary deliverable)

**New file / hook** `Matter_TimeSync_Client.be` (or method on the Root plugin):
- On boot, once Thread is attached and a fabric exists and
  `Rtc.utc_time < START_VALID_TIME (1451602800)`:
  1. pick target node id (controller `admin_subject` or configured
     `TrustedTimeSource`),
  2. `client.read_attribute(node, 0, 0x0038, 0x0000)` (UTCTime),
  3. on success, set RTC; on failure, retry with backoff and fall back to SNTP.
- Pairs with the already-shipped **Option 1a** honest-reporting change
  (UTCTime/Granularity report `null`/`0` when unsynced) from the prior thread.

## 6. Code separation & build gating (keep the standard WiFi build at zero added bytes)

This is a **hard requirement**: the time-sync / client capability has near-zero
value for a WiFi-connected Matter device (which has SNTP and where mDNS-based
operational discovery is a different mechanism). The standard `USE_MATTER_DEVICE`
build must not grow by a single byte.

### 6.1 How the existing Thread/standard split works

The codebase already has a clean, proven separation mechanism — reuse it
verbatim:

- **Build flag**: `USE_MATTER_THREAD` is defined **only** in the
  `tasmota32c6-mi32` env (`platformio_tasmota_cenv.ini:162`,
  `-DUSE_MATTER_THREAD=1`). The standard Matter build defines `USE_MATTER_DEVICE`
  but **never** `USE_MATTER_THREAD`.
- **Berry preprocessor**: `.be` files are run through a C-like preprocessor
  before solidification. Code inside `#if USE_MATTER_THREAD … #endif` is
  **excluded from solidification entirely** when the flag is off — it produces
  **no bytecode and no flash footprint** in the standard build. This works both
  at whole-class scope (e.g. `Matter_zz_Device_Thread.be`) **and** at
  method/branch scope inside a shared class (precedent: the
  `#if USE_MI_EXT_GUI` branch inside `Matter_Plugin_1_Root.be:1277-1302`).
- **Native (C) side**: `be_matter_module.c` wraps the `#include` of solidified
  Thread headers in `#if USE_MATTER_THREAD` (lines 284-288) and gates each class
  registration with a `USE_MATTER_THREAD` column in the registration table
  (lines 445, 461, 471). Absent the flag, the classes are not linked at all.

Existing Thread-only files following this convention:
`Matter_zz_Device_Thread.be`, `Matter_z_Commissioning_Thread.be`,
`Matter_Plugin_1_z_Root_Thread.be` — all wrap their class bodies in
`#if USE_MATTER_THREAD`.

### 6.2 What is genuinely transport-specific vs merely Thread-gated

| Layer | Transport-specific? | Notes |
|-------|--------------------|-------|
| **Operational discovery** | **Yes — truly Thread-specific** | Thread resolves the operational instance via **DNS-SD against the Border Router** (`otDnsClient`); WiFi uses **mDNS multicast** on the local LAN. Different code paths and different native bindings. Only the Thread (BR/`otDnsClient`) path is in scope here. |
| **UDP transport** | Yes | Thread uses OT UDP (`Matter_zz_Device_Thread`); WiFi uses `Matter_UDPServer`. Already abstracted behind the device's `msg_send`. |
| **CASE initiator** | No (transport-agnostic) | Same crypto/TLV over either UDP. But **gate it anyway** — it has no value in a WiFi standard build. |
| **IM initiator** (requests + response handlers) | No (transport-agnostic) | Same. Gate it anyway. |
| **Time-sync app** | No, but Thread-motivated | WiFi has SNTP; ~zero value off-Thread. Gate it. |

**Conclusion:** only discovery + transport are *technically* Thread-bound; the
CASE/IM initiator and time-sync are transport-agnostic but should still be
**compiled only when the feature is enabled**, to honor the zero-byte rule.

### 6.3 Separation strategy

1. **Default to gating everything new behind `USE_MATTER_THREAD`.** The standard
   build keeps the existing `return false` stubs (`Matter_IM.be:84-101`) and gains
   nothing. This is the simplest correct choice and matches the existing pattern.
2. **(Optional, recommended) introduce a dedicated `USE_MATTER_CLIENT` define**
   that *defaults to `USE_MATTER_THREAD`* — e.g. in `tasmota_configurations*.h`:
   `#if USE_MATTER_THREAD && !defined(USE_MATTER_CLIENT)` → `#define USE_MATTER_CLIENT 1`.
   This decouples "client capability" from "Thread transport" so a future WiFi
   build *could* opt in (mDNS discovery would still be needed), without touching
   any guards again. Use `#if USE_MATTER_CLIENT` on the transport-agnostic
   pieces and `#if USE_MATTER_THREAD` on the discovery/transport pieces.
3. **Put all new logic in new `*_Thread.be` / `*_Client.be` files** wrapped in
   the guard — never inline large bodies into shared files.
4. **Minimize shared-file edits; prefer hooks/overrides.** Where a shared file
   *must* change (the four `return false` stubs and the message-handler
   dispatch), keep the edit to a **single guarded branch** that delegates to the
   new client object, e.g.:
   ```berry
   elif opcode == 0x05   # Report Data
   #if USE_MATTER_CLIENT
     if self.client   return self.client.process_report_data(msg, val)   end
   #endif
     return false
   ```
   so the standard build still compiles to exactly `return false`.
   Even better, let the **Thread device subclass install the client handler** at
   runtime (it already subclasses `Matter_Device_BLE`), so the shared `Matter_IM`
   only needs a nil-check stub.

### 6.4 New & modified files (with gating)

| File | New / Mod | Guard | Purpose |
|------|-----------|-------|---------|
| `tasmota/.../xdrv_52_3_berry_thread.ino` | Mod | `#if USE_MATTER_THREAD` (file already fully Thread-gated) | Add `OT.dns_resolve_service` native binding (`otDnsClient`) |
| `Matter_Operational_Discovery_Thread.be` | New | `#if USE_MATTER_THREAD` | Operational instance name + DNS-SD resolve to IP:port (BR path) |
| `Matter_Commissioning_Initiator_Thread.be` | New | `#if USE_MATTER_CLIENT` | CASE initiator (Sigma1 build, Sigma2 parse, Sigma3 build, key schedule) |
| `Matter_Client_Thread.be` | New | `#if USE_MATTER_CLIENT` | App-facing read/invoke client API + `*_Out` request messages + response handlers |
| `Matter_TimeSync_Client_Thread.be` | New | `#if USE_MATTER_THREAD` | Boot-time UTCTime pull + SNTP fallback |
| `Matter_IM.be` | Mod (minimal) | `#if USE_MATTER_CLIENT` around single delegating branches | Route 0x05/0x07/0x09 + Status 0x01 to `self.client` when present; else unchanged `return false` |
| `Matter_Message.be` | Mod (minimal) | `#if USE_MATTER_CLIENT` | Initiator unsecured-exchange helper; i2r/r2i orientation for client |
| `Matter_MessageHandler.be` | Mod (minimal) | `#if USE_MATTER_CLIENT` | Dispatch responses for locally-initiated exchanges to the client/initiator |
| `Matter_zz_Device_Thread.be` | Mod | already `#if USE_MATTER_THREAD` | Instantiate + wire the client/discovery objects; trigger time-sync on attach |
| `be_matter_module.c` | Mod | `#if USE_MATTER_THREAD/_CLIENT` | `#include` solidified new headers + register new classes (mirror lines 284-288, 445-471) |
| `#@ solidify` tags + `solidify_all.be` | Mod | — | Solidify new classes (only built under the guard) |

**Net effect on the standard WiFi `USE_MATTER_DEVICE` build:** new files are not
solidified or linked; the only touched shared files (`Matter_IM`,
`Matter_Message`, `Matter_MessageHandler`) compile identically to today because
every added line sits inside an inactive `#if`. **Zero added bytes.**

## 7. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| **i2r/r2i inversion bugs** silently break either client or existing server | Keep initiator encrypt/decrypt in the new path; never change responder orientation; add round-trip self-test against a known controller |
| **No DNS client binding** is the largest unknown | Spike `otDnsClientResolveService` first (Phase 1); SRP/netdata fallback for same-fabric peers if needed |
| **CASE initiator crypto** subtle (destination id, TBSData/TBEData signing) | Reuse responder helpers verbatim; validate against `esp-matter`/chip-tool as reference initiator |
| **Coex / radio** (C6 shares 2.4 GHz) — extra outbound traffic during boot | Run time-sync pull *after* Thread attach is stable; keep it low-rate, one-shot with backoff (see WiFi/coex regression notes above) |
| **Flash / RAM** of new Berry classes | Phase it; only Phases 1–3 + 5 are needed for time sync; Invoke/Subscribe optional |
| **Session lifecycle** — peer may drop our initiated session | Use CASE resumption; re-establish on demand; cap concurrent initiated sessions |

## 8. Testing strategy

1. **Phase 1**: log resolved IP:port for the controller node; compare against
   `otNetDataGetNextService` output and chip-tool.
2. **Phase 2**: establish CASE to a chip-tool/`esp-matter` node; confirm
   `StatusReport = SessionEstablished` and a working operational session.
3. **Phase 3**: Read a known attribute (e.g. Basic Information `VendorName`)
   from a reference node and verify the decoded value.
4. **Phase 5**: cold-boot the C6 with RTC unset; confirm it pulls UTCTime and
   sets the clock without controller interaction; verify SNTP fallback path.

## 9. Effort estimate

- Phase 1 (DNS binding + helper): **M** — native OT work, one new binding.
- Phase 2 (CASE initiator): **L** — most intricate; crypto-correctness heavy.
- Phase 3 (IM initiator + response handlers): **M**.
- Phase 4 (client API): **S**.
- Phase 5 (time sync app): **S**.

Phases 1–3 + 5 are the minimum viable path for the time-sync goal. Phase 4
(generic Invoke/Subscribe) and a future commissioner are deliberately deferred.
This is "a large new capability, not a small patch" — but it is built almost
entirely by adding the *initiator* half of mechanisms whose *responder* half
already exists and is proven in production.
