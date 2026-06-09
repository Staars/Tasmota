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
- **SIG(0) timestamps are zero**: inception=0, expiration=0. OpenThread's native SRP client on ESP32 uses seconds-since-boot (not epoch time), and the OT SRP server explicitly skips timestamp validation. Apple's srp-mdns-proxy accepts this. Real timestamps were tried (rtc_utc() -300 / +lease_sec) but reverted — the device has no reliable wall clock during commissioning (SetUTCTime never sets Rtc.utc_time, see note below).
- **Real SIG(0) key tag**: computed via `_calc_key_tag()` (RFC 4034 Appendix B, 16-bit one's-complement fold over KEY RDATA) (was 0).
- **OPT pseudo-RR RDLENGTH fixed**: proper 2-byte placeholder written for the Update-Lease OPT record.
- **Counts before signing (RFC 2931 §2.3)**: UPCOUNT set to real value before signing; ADCOUNT temporarily reduced to 1 (SIG excluded) then restored to 2 after.
- **Response parsing**: `_on_success` parses rcode from the response header and either marks registered (rcode=0) or calls `_on_failure` with the rcode string. No hex payload dump (log level kept minimal).

### Known issues / omissions

- **SetUTCTime never sets the RTC**: `Matter_Thread_Device.be:203-208` handles cluster `0x0038` (Time Synchronization), command `0x0000` (SetUTCTime) by merely logging the UTC value and returning SUCCESS. It never calls `tasmota.cmd("RtcSetUTC ...")` or otherwise updates `tasmota.rtc_utc()`. During commissioning, `rtc_utc()` returns 0. This is directly relevant to SIG(0) timestamps — with zero RTC, any non-zero timestamp would be incorrect.
- **UDP socket sharing**: Both Matter (data exchange on port 5540) and SRP (port 0) use the same `OT.udp_open()`/`OT.udp_poll()` mechanism. Both poll from the same FreeRTOS queue in `Matter_Thread_Device.every_50ms()`. A late-inbound DNS response could be consumed by the Matter message handler instead of `_drain_responses()`, though in practice the SRP server address differs so messages are demuxed by destination port.

## Current Status (per latest on-device log)

- **BLE commissioning succeeds**: PASE (Pake1/2/3) → CASE (AddNOC, fabric `Apple Home` added) all complete.
- **Thread attaches**: dataset installed, role → child, OMR address `fd1d:bc81:cb5e:0:830b:3f8:dc64:65ce` assigned, OT UDP up on 5540.
- **SRP server discovered**: SRP-unicast entry `[fd1d:bc81:cb5e:0:8dee:b696:b381:9bc4]:63218` picked from Network Data.
- **SRP UPDATE is sent and reaches the BR** (`SRP UDP sent 552/553 bytes`, MeshForwarder confirms 600-byte UDP to the server) **but never gets a response** — SRP stays `Adding running=false`, retransmits forever (msg id 0,1,2,…), and commissioning times out (`-Session (removed)`).

