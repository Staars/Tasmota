# Matter over Thread — Plan v2: Replicate esp-matter `light`

## Why a new plan
6 weeks of debugging a hand-rolled Berry SRP/SIG(0)/ECDSA client failed. The
codebase has since pivoted to OpenThread's **native** SRP client, but
registration still fails: BLE+PASE+CASE succeed, Thread attaches, an SRP UPDATE
is sent to the Border Router (BR), but the BR **never responds** and the device
retransmits forever until commissioning times out.

New strategy: stop inventing, **replicate the known-good Espressif esp-matter
`light` example as closely as possible** (`~/Developer/esp-matter/examples/light`,
built `c6_thread`). Change one variable at a time against a frozen test setup.

## Reference vs. current implementation

| Aspect | esp-matter `light` (works) | Tasmota / BearThread (broken) |
|--------|----------------------------|-------------------------------|
| OpenThread source | Espressif IDF OpenThread component | Custom vendored `lib/libesp32/BearThread/openthread` |
| OT crypto backend | **mbedTLS** | **Custom BearSSL** (`bt_crypto_bearssl.cpp`, `CRYPTO_LIB_PLATFORM`) |
| SRP client | Native OT (`CONFIG_OPENTHREAD_SRP_CLIENT=y`) | Native OT (`SRP_CLIENT_ENABLE=1`) ✓ aligned |
| SRP startup | `otSrpClientEnableAutoStartMode()` (CHIP `GenericThreadStackManagerImpl_OpenThread.hpp:730`) | **Manual `otSrpClientStart(&sockAddr)`** with server parsed by hand from Network Data |
| DNS client | `CONFIG_OPENTHREAD_DNS_CLIENT=y` | `DNS_CLIENT_ENABLE=0` |
| Device type | FTD (MTD also valid) | MTD |
| ECDSA key | persisted via OT settings (`otPlatSettings`) | OT settings in `bt_misc.cpp` (LittleFS) — verify it loads before SRP |
| Hand-rolled SRP | none | **Dead code still present**: `Matter_SRP_Client.be` + `udp_srp_*` bindings |

### What's already aligned
- `Matter_Thread_Device.be` drives SRP purely via native `OT.srp_*` bindings
  (`srp_set_hostname`, `srp_add_service`, `srp_stop`, etc.). It does **not**
  instantiate `Matter_SRP_Client.be`.

## Root-cause priority (most → least likely)

1. **Manual `otSrpClientStart()` vs auto-start** — `Matter_Thread_Device.be:_start_srp_client()`
   calls `OT.srp_start(server)` after hand-parsing Network Data. esp-matter/CHIP
   uses `otSrpClientEnableAutoStartMode()`, letting OT pick the SRP server itself.
   Manual selection easily targets the wrong address/port (the old `63218`
   symptom). **Cheapest high-value fix.**
2. **BearSSL ECDSA SIG(0)** — only the crypto is custom now. A malformed/high-S
   signature or bad key DER makes the BR silently drop the UPDATE → exactly our
   symptom. Suspect after #1 is ruled out.
3. **Response not received / demux** — confirm the BR truly sends nothing on-air
   vs. the device dropping it (leftover dedicated SRP UDP socket, coex).
4. **Two SRP paths confusion** — `Matter_SRP_Client.be` + `udp_srp_*` are dead
   but should be removed to eliminate doubt.
5. **ECDSA key not persisted across boots** — product correctness; less likely to
   cause first-boot timeout.
6. **Custom OT version/wire bug, MTD vs FTD, DNS client disabled** — investigate
   last; expensive.

## Phased plan

### Phase 0 — Freeze the test matrix (establish ground truth)
- One BR (Apple/Google), one Thread dataset.
- **Build and flash esp-matter `light` (`c6_thread`) on the same C6 hardware** and
  confirm it commissions over Thread on this BR.
- Capture its successful SRP registration: OT logs (SRP/INFO) and ideally an
  802.15.4 sniffer trace. This is the **golden reference** for every comparison.

### Phase 1 — Single SRP path (remove dead hand-rolled code)
- Delete/neutralize `Matter_SRP_Client.be` and the `udp_srp_open/send/poll/close`
  bindings (driver + `be_OT_lib.c`) so only native OT SRP can run.
- Confirm logs show native path only (`BT_CRYPTO : ECDSA sign …`, OT SRP callback),
  no `OT : SRP UDP socket opened`.

### Phase 2 — Match CHIP's SRP startup (auto-start)
- Replace manual `_start_srp_client()` / `otSrpClientStart(&sockAddr)` with
  `otSrpClientEnableAutoStartMode(instance, cb, nullptr)` once during OT init,
  mirroring CHIP `GenericThreadStackManagerImpl_OpenThread.hpp:730`.
- Keep `otSrpClientSetCallback`, `otSrpClientSetHostName` +
  `otSrpClientEnableAutoHostAddress`, `otSrpClientAddService`,
  `otSrpClientSetLeaseInterval(3600)` + `otSrpClientSetKeyLeaseInterval(86400)`.
- Add a Berry wrapper `OT.srp_enable_autostart()`; keep `netdata_services()` as a
  diagnostic only.
- Success: host state → `Registered`; auto-start picks same destination as the
  esp-matter capture.

### Phase 3 — Fix / replace ECDSA (only if Phase 2 insufficient)
- First: add **low-S normalization** in `otPlatCryptoEcdsaSign` (`if s > n/2: s = n - s`)
  and a self-test that verifies the produced signature.
- Instrument: log SHA-256 digest, pubkey, raw 64-byte `r||s`; verify offline
  (OpenSSL/mbedTLS).
- If still failing: reimplement only `otPlatCryptoEcdsa{GenerateKey,GetPublicKey,
  Sign,Verify}` with **mbedTLS P-256** (Tasmota already links mbedTLS), keeping the
  same OT formats (keypair DER, raw 64-byte pubkey `X||Y`, raw 64-byte sig `r||s`).
  This matches esp-matter's crypto and removes the largest remaining unknown.

### Phase 4 — Verify persistence
- Confirm `bt_misc.cpp` `otPlatSettings*` loads SRP ECDSA key + SLAAC IID before
  the SRP client runs, and they're stable across reboot; factory reset wipes them.

### Phase 5 — Only if still failing: converge harder on esp-matter
- Enable `DNS_CLIENT_ENABLE`, try FTD, align BearThread OT config/version with the
  IDF OpenThread component — or replace BearThread with the IDF component (XL).

## Cheapest diagnostic (do early)
Capture one Tasmota SRP UPDATE and one esp-matter SRP UPDATE on the same BR and
diff: dest IPv6/port, DNS UPDATE opcode/zone, EDNS lease option, KEY alg=13,
SIG RDLENGTH, 64-byte raw signature, and whether the BR replies on-air.
- BR replies but device misses it → RX/demux/coex.
- Dest differs from esp-matter → server-selection (Phase 2).
- Dest same, SIG fails offline → crypto (Phase 3).
- Dest same, SIG ok, no reply → wire/policy/custom-OT (Phase 5).

## Key files
| File | Role |
|------|------|
| `Matter_Thread_Device.be` | Orchestrator; native `OT.srp_*`. Change manual start → autostart (Phase 2) |
| `tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino` | OT bindings: `srp_*`, `srp_client_callback`, `otSrpClientStart`; add autostart |
| `lib/libesp32/berry_tasmota/src/be_OT_lib.c` | OT module registration (+ regen `be_fixed_OT.h`) |
| `lib/libesp32/BearThread/include/bearthread-core-config.h` | OT config (SRP/ECDSA/DNS/crypto-lib) |
| `lib/libesp32/BearThread/src/bt_crypto_bearssl.cpp` | BearSSL `otPlatCryptoEcdsa*` — Phase 3 |
| `lib/libesp32/BearThread/src/bt_misc.cpp` | `otPlatSettings*` persistence — Phase 4 |
| `lib/libesp32/berry_matter/src/embedded/Matter_SRP_Client.be` | **Dead** hand-rolled SRP — remove (Phase 1) |
| Reference | `~/Developer/esp-matter/examples/light` + CHIP `GenericThreadStackManagerImpl_OpenThread.hpp` |

## Build
- Env `tasmota32c6-mi32`, board `esp32c6`; `USE_BERRY` + `USE_MATTER_DEVICE` + `USE_MATTER_THREAD=1`.
- Clean build after config changes: `rm -rf .pio/build/tasmota32c6-mi32 && pio run -e tasmota32c6-mi32`.
