# Matter Thread Implementation Plan

## Goal
Implement Matter over Thread on Tasmota (ESP32-C6): commissioning (PASE + CASE), operational discovery (SRP/mDNS), and data exchange (Interaction Model over UDP) using Berry as the application layer and BearThread as the Thread transport.

## Design Overview

```
┌────────────────────────────────────────────────┐
│                  Berry (Tasmota)               │
│                                                │
│  Matter_Thread_Device.be                       │
│    └─ self.srp = matter.SRP_Client()           │
│         │                                      │
│         ▼                                      │
│  Matter_SRP_Client.be                          │
│    └─ Key set via set_key() from host          │
│    └─ Server discovery (OT.netdata_services)   │
│    └─ DNS UPDATE builder + SIG(0)              │
│    └─ ECDSA signing + low-S normalization      │
│    └─ State machine + backoff + refresh        │
│    └─ UDP send/receive (OT.udp_send/poll)      │
│         │                                      │
│         ▼                                      │
│  OT.udp_send / OT.udp_poll                     │
│         │                                      │
└─────────┼──────────────────────────────────────┘
          │
┌─────────▼────────────────────────────────────────┐
│              BearThread (C++)                    │
│                                                  │
│  bearthread-core-config.h ─ SRP_CLIENT=0,ECDSA=0 │
└──────────────────────────────────────────────────┘
```

### Key design decisions

- **SRP key is separate from NOC** (RFC 9665 §3.2.5.1). Apple mDNSResponder does not cross-check against NOC.
- **Low-S normalization is pure Berry** (byte-comparison of big-endian integers, post-processing ECDSA output).
- **SRP key is a compile-time constant** in `Matter_Thread_Device.be:kSrpPriv/kSrpPub` (secp256r1 `bytes("hex")`), passed via `set_key()` — NOT generated at runtime, NOT persisted. (Hardcoded dev key until RNG timing is resolved.)
- **Hostname**: derived from Tasmota EUI64 (deterministic, not random).
- **OT types are hidden from the .ino file** — only BearThread C++ files include `<openthread/*>` headers.
- **All SRP atomic in Berry** — no hybrid C/Berry crypto.
- **IPv6 string→bytes parsing is done in C**, not Berry — `matter.get_ip_bytes(s)` (in `be_matter_misc.cpp`, uses Arduino `IPAddress::fromString`) returns 16 bytes for IPv6 / 4 bytes for IPv4. Replaced the former hand-rolled Berry `_ipv6_string_to_bytes()`.
- **SRP transport is raw UDP** (DNS UPDATE), not CoAP. The OT module exposes `udp_open/send/poll/close`.

## Related Files

### Core Berry implementation
| File | Role |
|------|------|
| `lib/libesp32/berry_matter/src/embedded/Matter_SRP_Client.be` | Full SRP client: set_key, signing, DNS UPDATE builder, SIG(0) (zero timestamps matching OpenThread, computed key-tag), state machine, server discovery, UDP transport |
| `Matter_Thread_Device.be` | Orchestrator — creates `self.srp = matter.SRP_Client()` + `set_key(kSrpPriv, kSrpPub)` in `init()` |
| `lib/libesp32/berry_matter/src/be_matter_misc.cpp` | `matter.get_ip_bytes()` C helper — IP string → raw bytes (IPv6=16, IPv4=4) |
| `lib/libesp32/berry_tasmota/src/be_OT_lib.c` | OT module registration (20 entries including UDP, CoAP, netdata_services, coex_prefer_thread) |
| `lib/libesp32/berry/generate/be_fixed_OT.h` | Auto-generated from be_OT_lib.c — do not hand-edit |

### BearThread (C++) — transport only
| File | Role |
|------|------|
| `lib/libesp32/BearThread/include/bearthread-core-config.h` | `SRP_CLIENT_ENABLE=0`, `ECDSA_ENABLE=0` |

### Driver / bindings
| File | Role |
|------|------|
| `tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino` | 797 lines: OT bindings (UDP open/send/poll/close, CoAP send/poll, netdata_services, coex_prefer_thread, set_log_level, standard OT). No SRP. |

## Berry Syntax Rules

| Don't | Do |
|-------|-----|
| `a if cond else b` | `cond ? a : b` |
| `s[a:b]` | `s[a..b]` (inclusive both ends) |
| `None` / `is` | `nil` / `==` |
| `ClassName.method()` from inside class | `self.method()` |
| `static def` (not callable via self) | `def` |
| `bytes(N)` (positive = capacity only) | `bytes(-N)` (fixed N-byte buffer) |
| `int(hex_str, 16)` (ignores base) | `int("0x" + hex_str)` |
| Hand-rolled IPv6 string parser in Berry | `matter.get_ip_bytes(s)` C helper |

## State Machine

```
Stopped → ToAdd → Adding → Registered → ToRefresh → Refreshing → Registered
                  ↓                              ↓
               ToAdd (failure)              ToRefresh (failure)
ToRemove → Removing → Removed
                                          Any state → Error (internal failure)
```

## Build
- Environment: `tasmota32c6-mi32`, board `esp32c6`, defines `USE_BERRY` + `USE_MATTER_THREAD=1` + `USE_MATTER_DEVICE`
- Clean build required after config changes: `rm -rf .pio/build/tasmota32c6-mi32 && pio run`
- `USE_SHA_ROM` is **disabled** (commented out) in `platformio_tasmota32.ini` for this work.
- `sdkconfig.defaults` was regenerated/expanded (large diff, not SRP-specific).

## SRP DNS-UPDATE / SIG(0) Fixes (current)

Applied to `Matter_SRP_Client.be`:

- **KEY record corrected**: `kProtocolDnsSec = 0` (was 3) and `kKeyFlagsLow = 0x00` (was `0x02`/ZNZ removed).
- **SIG(0) timestamps are real**: inception/expiration derived from RTC UTC (SetUTCTime now works, see fixes below).
- **Real SIG(0) key tag**: computed via `_calc_key_tag()` (RFC 4034 Appendix B, 16-bit one's-complement fold over KEY RDATA) (was 0).
- **OPT pseudo-RR RDLENGTH fixed**: proper 2-byte placeholder written for the Update-Lease OPT record.
- **Counts before signing (RFC 2931 §2.3)**: UPCOUNT set to real value before signing; ADCOUNT temporarily reduced to 1 (SIG excluded) then restored to 2 after.
- **Response parsing**: `_on_success` parses rcode from the response header and either marks registered (rcode=0) or calls `_on_failure` with the rcode string. No hex payload dump (log level kept minimal).
- **Fixed reversed to_sign order (reverted)**: The original `to_sign = sig_rdata + msg[0..sig_owner_off-1]` was correct per RFC 2931 §3.1 (`data = RDATA | request - SIG(0)`). An incorrect inversion to `msg[0..] + sig_rdata` was briefly applied then reverted.
- **Fixed DER→raw signature**: `_ecdsa_sig_der()` was removed — SIG(0) now writes raw 64-byte r||s per RFC 6605 §4 instead of ASN.1/DER (`0x30 0x44 0x02 0x20...`). `max_sig_size` reduced from 72 to 64.
- **Fixed int64 division in SetUTCTime**: `Matter_Thread_Device.be:207` — `(t - int64(rtc_utc())) / int64(1000000)` avoids `divzero_error` from Berry int64 library where `/` only accepts int64 operands; int32 silently truncates to 32-bit.

### Known issues / omissions

- **SetUTCTime RTC sync**: `Matter_Thread_Device.be:203-208` handles cluster `0x0038` (Time Synchronization), command `0x0000` (SetUTCTime) by logging the UTC and returning SUCCESS. The int64 crash is fixed (confirmed: RTC shows correct epoch `1781037231`), but the handler never calls `tasmota.cmd("RtcSetUTC ...")`. Wall-clock accuracy depends on the network-provided time.
- **UDP socket sharing**: Both Matter (data exchange on port 5540) and SRP (port 0) use the same `OT.udp_open()`/`OT.udp_poll()` mechanism. Both poll from the same FreeRTOS queue in `Matter_Thread_Device.every_50ms()`. A late-inbound DNS response could be consumed by the Matter message handler instead of `_drain_responses()`, though in practice the SRP server address differs so messages are demuxed by destination port.

## Current Status (per latest on-device log)

- **BLE commissioning succeeds**: PASE (Pake1/2/3) → CASE (AddNOC, fabric `Apple Home` added) all complete.
- **Thread attaches**: dataset installed, role → child, OMR address `fd1d:bc81:cb5e:0:830b:3f8:dc64:65ce` assigned, OT UDP up on 5540.
- **SRP server discovered**: SRP-unicast entry `[fd1d:bc81:cb5e:0:8dee:b696:b381:9bc4]:63218` picked from Network Data.
- **SRP UPDATE is sent and reaches the BR** (`SRP UDP sent 552/553 bytes`, MeshForwarder confirms 600-byte UDP to the server) **but never gets a response** — SRP stays `Adding running=false`, retransmits forever (msg id 0,1,2,…), and commissioning times out (`-Session (removed)`).

## Root Cause Analysis

Two independent bugs were identified in `SIG(0)` signing:

### Bug 1 (not a bug — original code was correct)

The `to_sign` order was briefly suspected to be reversed. RFC 2931 §3.1 gives:
```
data = RDATA | request - SIG(0)   # SIG RDATA first, then DNS message
```
The original code at `Matter_SRP_Client.be:1150` was correct:
```berry
var to_sign = sig_rdata + msg[0 .. sig_owner_off - 1]
```
An incorrect inversion was applied and then reverted.

### Bug 2 (confirmed root cause — DER-encoded signature)

RFC 6605 §4 mandates that ECDSA P-256 SHA-256 signatures in SIG(0) records use **raw 64-byte r||s** format:
> "The two integers, each of which is formatted as a simple octet string, are combined into a single longer octet string for DNSSEC as the concatenation 'r | s'. For P-256, each integer MUST be encoded as 32 octets."

The code was calling `_ecdsa_sig_der(sig_norm)` which wrapped the 64-byte raw signature in ASN.1/DER (`30 44 02 20...`), producing 70+ bytes. The hex dump of msg_id=0 confirmed:
```
RDLENGTH = 0x46 (70 bytes)
signature = 30 44 02 20 4C B4 EC 0C ... 02 20 50 D0 CF 0F ... DD
```
The BR receives DER-encoded bytes, can't parse them as 64-byte r||s, and silently drops the UPDATE.

**Fix applied**: `var sig_der = sig_norm` (skip DER encoding), `max_sig_size` reduced from 72 to 64.

### Additional fix: int64 division in SetUTCTime

`Matter_Thread_Device.be:207`: `(t - int64(rtc_utc())) / int64(1000000)` — Berry's int64 `/` operator only accepts int64 operands; plain `int(rtc_utc())` and `1000000` cause `arg_get_p` to return NULL, raising `divzero_error`. Fixed by promoting both to int64. Logs confirm correct epoch (`1781037231`) after fix.

## Next Steps

1. Build + deploy the raw-signature fix
2. Check whether the BR now responds to SRP UPDATEs
3. If still failing, investigate:
   - Whether `_normalize_low_s` produces incorrect values
   - Whether the BR needs a different algorithm or key format
   - Whether the SRP server port/address is correct

