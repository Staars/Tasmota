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

## Current status

| Stage | Status |
|-------|--------|
| BLE PASE commissioning | ✓ |
| Thread attach (child) | ✓ |
| SRP registration (`_matter._tcp`) | ✓ |
| CASE session over Thread | **✗** — times out after 90s |
| Full Matter commissioning | **✗** |

SRP works for the first time ever. The remaining blocker is CASE: the
commissioner (phone) disconnects BLE after provisioning the Thread dataset
and expects to reach the device over Thread via the Apple TV border router.
The device is on the network (child, SRP registered, UDP port 5540 open,
`candidate fabric present`) but CASE fails within 90 s.

This is likely a **Matter‑layer / transport issue**, not crypto or radio.

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

Two independent tracks:

### A. Fix CASE commissioning (current)
Investigate why the commissioner can't establish a CASE session over Thread
after BLE drops. Possible causes:
- Phone app not switching to Thread/BR for IPv6
- Matter stack not responding to CASE messages on UDP port 5540
- Missing routing / BR forwarding

### B. Re‑enable BearSSL + Berry SRP (long‑term)
Goal: eliminate libs that matter‑device can't distribute.
- **BearSSL crypto** — restore `CRYPTO_LIB_PLATFORM` and `bt_crypto_bearssl.cpp`
  (currently `#if 0`). The MAC‑layer radio bug was the real problem; BearSSL
  crypto was a red herring.
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
