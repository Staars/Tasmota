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
│    └─ CoAP send/receive (via BearThread)       │
│         │                                      │
│         ▼                                      │
│  OT.coap_send_request / OT.coap_poll_response  │
│         │                                      │
└─────────┼──────────────────────────────────────┘
          │
┌─────────▼────────────────────────────────────────┐
│              BearThread (C++)                    │
│                                                  │
│  bt_coap.cpp    ─ CoAP wrapper (OT types hidden) │
│  bt_crypto_*.cpp ─ AES/HMAC/SHA/RNG only         │
│  bearthread-core-config.h ─ SRP_CLIENT=0,ECDSA=0 │
└──────────────────────────────────────────────────┘
```

### Key design decisions

- **SRP key is separate from NOC** (RFC 9665 §3.2.5.1). Apple mDNSResponder does not cross-check against NOC.
- **Low-S normalization is pure Berry** (byte-comparison of big-endian integers, post-processing ECDSA output).
- **SRP key is a compile-time constant** in `Matter_Thread_Device.be:kSrpPriv/kSrpPub` (secp256r1 `bytes("hex")`), passed via `set_key()` — NOT generated at runtime, NOT persisted.
- **Hostname**: derived from Tasmota EUI64 (deterministic, not random).
- **OT types are hidden from the .ino file** — only BearThread C++ files include `<openthread/*>` headers.
- **All SRP atomic in Berry** — no hybrid C/Berry crypto.

## Related Files

### Core Berry implementation
| File | Role |
|------|------|
| `lib/libesp32/berry_matter/src/embedded/Matter_SRP_Client.be` | Full SRP client: set_key, signing, DNS UPDATE builder, SIG(0), state machine, server discovery |
| `Matter_Thread_Device.be` | Orchestrator — creates `self.srp = matter.SRP_Client()` in `init()` |
| `lib/libesp32/berry_tasmota/src/be_OT_lib.c` | OT module registration (20 entries including CoAP, netdata_services, coex_prefer_thread) |
| `lib/libesp32/berry/generate/be_fixed_OT.h` | Auto-generated from be_OT_lib.c — do not hand-edit |

### BearThread (C++) — transport only
| File | Role |
|------|------|
| `lib/libesp32/BearThread/include/bearthread-core-config.h` | `SRP_CLIENT_ENABLE=0`, `ECDSA_ENABLE=0` |
| `lib/libesp32/BearThread/include/bt_platform.h` | CoAP wrapper API (`bt_coap_send_request`, `bt_coap_poll_response`, etc.) |
| `lib/libesp32/BearThread/src/bt_coap.cpp` | CoAP request/response wrapper (~230 lines), OT-internal queue |
| `lib/libesp32/BearThread/src/bt_crypto_bearssl.cpp` | AES/HMAC/SHA/RNG only (ECDSA PAL deleted) |

### Driver / bindings
| File | Role |
|------|------|
| `tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino` | 797 lines: OT bindings (CoAP send/poll, netdata_services, coex_prefer_thread, set_log_level, standard OT). No SRP. |

## Berry Syntax Rules

| Don't | Do |
|-------|----|
| `a if cond else b` | `cond ? a : b` |
| `s[a:b]` | `s[a..b]` (inclusive both ends) |
| `None` / `is` | `nil` / `==` |
| `ClassName.method()` from inside class | `self.method()` |
| `static def` (not callable via self) | `def` |

## State Machine

```
Stopped → ToAdd → Adding → Registered → ToRefresh → Refreshing → Registered
                  ↓                              ↓
               ToAdd (failure)              ToRefresh (failure)
ToRemove → Removing → Removed
```

## Build
- Environment: `tasmota32c6-mi32`, board `esp32c6`, defines `USE_BERRY` + `USE_MATTER_THREAD=1` + `USE_MATTER_DEVICE`
- Clean build required after config changes: `rm -rf .pio/build/tasmota32c6-mi32 && pio run`
