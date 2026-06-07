/*
  ot_crypto_bearssl.cpp - OpenThread crypto platform layer using BearSSL

  Copyright (C) 2025 Christian Baars

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifdef USE_MATTER_THREAD

#include <string.h>
#include <openthread/error.h>
#include <openthread/platform/crypto.h>
#include "esp_log.h"

extern "C" {
#include "t_bearssl_block.h"
#include "t_bearssl_hash.h"
#include "t_bearssl_hmac.h"
#include "t_bearssl_rand.h"
}

#include "esp_random.h"

/* ---- Internal: PRNG for BearSSL EC keygen ---- */

/* Simple PRNG class wrapping ESP32 hardware RNG for BearSSL */
typedef struct {
  const br_prng_class *vtable;
} ot_bearssl_rng_context;

static void ot_rng_init(const br_prng_class **ctx, const void *params,
                         const void *seed, size_t seed_len) {
  (void)ctx; (void)params; (void)seed; (void)seed_len;
}

static void ot_rng_generate(const br_prng_class **ctx, void *out, size_t len) {
  (void)ctx;
  esp_fill_random(out, len);
}

static void ot_rng_update(const br_prng_class **ctx, const void *seed, size_t seed_len) {
  (void)ctx; (void)seed; (void)seed_len;
}

static const br_prng_class ot_rng_vtable = {
  /* context_size */ sizeof(ot_bearssl_rng_context),
  /* init */         &ot_rng_init,
  /* generate */     &ot_rng_generate,
  /* update */       &ot_rng_update,
};

/* ---- Internal: HMAC context wrapper ----
 * OpenThread's HMAC API is: Init → Start(key) → Update(data) → Finish(mac)
 * BearSSL separates: key_init(key_ctx, key) then hmac_init(ctx, key_ctx) → update → out
 * We store both in our context.
 */
typedef struct {
  br_hmac_key_context key_ctx;
  br_hmac_context     hmac_ctx;
} ot_hmac_sha256_context;

/* ---- AES ECB (single-block encrypt only) ---- */
/* OpenThread only needs AES-128/256 ECB encrypt for MAC-layer key schedule.
 * We store BearSSL CBC-enc keys and use CBC on 1 block with zero IV = ECB. */

extern "C" otError otPlatCryptoAesInit(otCryptoContext *aContext) {
  if (!aContext || aContext->mContextSize < sizeof(br_aes_ct_cbcenc_keys))
    return OT_ERROR_INVALID_ARGS;
  memset(aContext->mContext, 0, sizeof(br_aes_ct_cbcenc_keys));
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoAesSetKey(otCryptoContext *aContext, const otCryptoKey *aKey) {
  if (!aContext || !aKey || !aKey->mKey)
    return OT_ERROR_INVALID_ARGS;
  if (aContext->mContextSize < sizeof(br_aes_ct_cbcenc_keys))
    return OT_ERROR_FAILED;
  br_aes_ct_cbcenc_keys *ctx = static_cast<br_aes_ct_cbcenc_keys *>(aContext->mContext);
  br_aes_ct_cbcenc_init(ctx, aKey->mKey, aKey->mKeyLength);
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoAesEncrypt(otCryptoContext *aContext, const uint8_t *aInput, uint8_t *aOutput) {
  if (!aContext || !aInput || !aOutput)
    return OT_ERROR_INVALID_ARGS;
  if (aContext->mContextSize < sizeof(br_aes_ct_cbcenc_keys))
    return OT_ERROR_FAILED;
  br_aes_ct_cbcenc_keys *ctx = static_cast<br_aes_ct_cbcenc_keys *>(aContext->mContext);
  /* ECB = CBC with zero IV on single 16-byte block */
  uint8_t iv[16] = {0};
  memcpy(aOutput, aInput, 16);
  br_aes_ct_cbcenc_run(ctx, iv, aOutput, 16);
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoAesFree(otCryptoContext *aContext) {
  if (!aContext)
    return OT_ERROR_INVALID_ARGS;
  memset(aContext->mContext, 0, aContext->mContextSize);
  return OT_ERROR_NONE;
}

/* ---- HMAC-SHA256 ---- */

extern "C" otError otPlatCryptoHmacSha256Init(otCryptoContext *aContext) {
  if (!aContext || aContext->mContextSize < sizeof(ot_hmac_sha256_context)) {
    ESP_LOGE("BT_CRYPTO", "HMAC init: need %u, have %u",
             (unsigned)sizeof(ot_hmac_sha256_context),
             aContext ? (unsigned)aContext->mContextSize : 0);
    return OT_ERROR_INVALID_ARGS;
  }
  memset(aContext->mContext, 0, sizeof(ot_hmac_sha256_context));
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Deinit(otCryptoContext *aContext) {
  if (!aContext)
    return OT_ERROR_INVALID_ARGS;
  memset(aContext->mContext, 0, aContext->mContextSize);
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Start(otCryptoContext *aContext, const otCryptoKey *aKey) {
  if (!aContext || !aKey || !aKey->mKey)
    return OT_ERROR_INVALID_ARGS;
  if (aContext->mContextSize < sizeof(ot_hmac_sha256_context))
    return OT_ERROR_FAILED;
  ot_hmac_sha256_context *ctx = static_cast<ot_hmac_sha256_context *>(aContext->mContext);
  br_hmac_key_init(&ctx->key_ctx, &br_sha256_vtable, aKey->mKey, aKey->mKeyLength);
  br_hmac_init(&ctx->hmac_ctx, &ctx->key_ctx, 0); /* 0 = full output length (32 bytes) */
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Update(otCryptoContext *aContext, const void *aBuf, uint16_t aBufLength) {
  if (!aContext)
    return OT_ERROR_INVALID_ARGS;
  if (aContext->mContextSize < sizeof(ot_hmac_sha256_context))
    return OT_ERROR_FAILED;
  ot_hmac_sha256_context *ctx = static_cast<ot_hmac_sha256_context *>(aContext->mContext);
  br_hmac_update(&ctx->hmac_ctx, aBuf, aBufLength);
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Finish(otCryptoContext *aContext, uint8_t *aBuf, size_t aBufLength) {
  (void)aBufLength;
  if (!aContext || !aBuf)
    return OT_ERROR_INVALID_ARGS;
  if (aContext->mContextSize < sizeof(ot_hmac_sha256_context))
    return OT_ERROR_FAILED;
  ot_hmac_sha256_context *ctx = static_cast<ot_hmac_sha256_context *>(aContext->mContext);
  br_hmac_out(&ctx->hmac_ctx, aBuf);
  return OT_ERROR_NONE;
}

/* ---- SHA-256 ---- */

extern "C" otError otPlatCryptoSha256Init(otCryptoContext *aContext) {
  if (!aContext || aContext->mContextSize < sizeof(br_sha256_context))
    return OT_ERROR_INVALID_ARGS;
  memset(aContext->mContext, 0, sizeof(br_sha256_context));
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoSha256Deinit(otCryptoContext *aContext) {
  if (!aContext)
    return OT_ERROR_INVALID_ARGS;
  memset(aContext->mContext, 0, aContext->mContextSize);
  aContext->mContext     = nullptr;
  aContext->mContextSize = 0;
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoSha256Start(otCryptoContext *aContext) {
  if (!aContext || aContext->mContextSize < sizeof(br_sha256_context))
    return OT_ERROR_FAILED;
  br_sha256_context *ctx = static_cast<br_sha256_context *>(aContext->mContext);
  br_sha256_init(ctx);
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoSha256Update(otCryptoContext *aContext, const void *aBuf, uint16_t aBufLength) {
  if (!aContext || aContext->mContextSize < sizeof(br_sha256_context))
    return OT_ERROR_FAILED;
  br_sha256_context *ctx = static_cast<br_sha256_context *>(aContext->mContext);
  br_sha256_update(ctx, aBuf, aBufLength);
  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoSha256Finish(otCryptoContext *aContext, uint8_t *aHash, uint16_t aHashSize) {
  (void)aHashSize;
  if (!aContext || !aHash || aContext->mContextSize < sizeof(br_sha256_context))
    return OT_ERROR_FAILED;
  br_sha256_context *ctx = static_cast<br_sha256_context *>(aContext->mContext);
  br_sha256_out(ctx, aHash);
  return OT_ERROR_NONE;
}

/* ---- ECDSA P-256 ----
 *
 * REMOVED. The SRP client (and its ECDSA keygen / sign / verify) is no
 * longer hosted in BearThread. The Berry-side Matter_SRP_Client does all
 * signing in pure Berry using crypto.EC_P256().ecdsa_sign_sha256() with
 * in-Berry low-S normalization. BearThread is now transport-only.
 */

/* ---- Platform Init ---- */

extern "C" void otPlatCryptoInit(void) {
  /* No global initialization needed for BearSSL */
}

extern "C" void otPlatCryptoRandomInit(void) {
  /* ESP32 hardware RNG needs no initialization */
}

extern "C" void otPlatCryptoRandomDeinit(void) {
  /* Nothing to deinitialize */
}

extern "C" otError otPlatCryptoRandomGet(uint8_t *aBuffer, uint16_t aSize) {
  esp_fill_random(aBuffer, aSize);
  return OT_ERROR_NONE;
}

#endif /* USE_MATTER_THREAD */
