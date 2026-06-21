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
| `bt_hmac_mbedtls.cpp` — HMAC override via low‑level `mbedtls_sha256` | Worked around broken MD layer in prebuilt mbedtls | **Yes** — only needed for CRYPTO_LIB_MBEDTLS |
| `bt_mbedtls_ecdsa_det.c` — provides `mbedtls_ecdsa_sign_det_ext()` | Missing from arduino‑esp32 prebuilt lib | **Yes** — only needed for CRYPTO_LIB_MBEDTLS |

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

### B. Re‑enable BearSSL + Berry SRP (long‑term)
Goal: eliminate libs that matter‑device can't distribute.
- **BearSSL crypto** — restore `CRYPTO_LIB_PLATFORM` and `bt_crypto_bearssl.cpp`
  (currently `#if 0`). The MAC‑layer radio bug was the real problem; BearSSL
  crypto was a red herring. HMAC override (`bt_hmac_mbedtls.cpp`) and ECDSA
  shim (`bt_mbedtls_ecdsa_det.c`) become unnecessary.
- **Berry SRP** — the hand‑rolled `Matter_SRP_Client.be` is dead but the
  plan was always to go back to it after a working reference was established.
  The native OT path now serves as that reference.

---

## Key files

| File | Role |
|------|------|
| `lib/libesp32/BearThread/src/bt_radio.c` | Radio driver; MAC‑security TX bug fixed at line 278‑292 |
| `lib/libesp32/BearThread/include/bearthread-core-config.h` | OT config; CRYPTO_LIB=0 now, was 2 |
| `lib/libesp32/BearThread/src/bt_crypto_bearssl.cpp` | BearSSL crypto — `#if 0`'d, restore for BearSSL path |
| `lib/libesp32/BearThread/src/bt_hmac_mbedtls.cpp` | HMAC override — only for CRYPTO_LIB_MBEDTLS |
| `lib/libesp32/BearThread/src/bt_mbedtls_ecdsa_det.c` | Deterministic ECDSA wrapper — only for CRYPTO_LIB_MBEDTLS |
| `lib/libesp32/BearThread/src/bt_misc.cpp` | `otPlatSettings*` persistence |
| `tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino` | OT bindings: `srp_*`, callbacks |
| `Matter_Thread_Device.be` | Berry Matter orchestrator |
| Reference | `~/Developer/esp-matter/examples/light` + IDF `esp_openthread_radio.c` |

## Build

```
pio run -e tasmota32c6   (or tasmota32c6-mi32)
```
