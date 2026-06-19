/*
  ot_crypto_bearssl.cpp - OpenThread crypto platform layer using BearSSL

  Copyright (C) 2025 Christian Baars

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifdef USE_MATTER_THREAD

#if 0   // BearSSL completely disabled — using CRYPTO_LIB_MBEDTLS with mbedTLS WEAK defaults

#include <string.h>
#include <openthread/error.h>
#include <openthread/platform/crypto.h>
#include "esp_log.h"

/* Tasmota AddLog — route logs into the Tasmota log stream. Declared in
 * tasmota code without extern "C", so this file must be compiled as C++.
 * Tasmota log levels: 1=ERROR, 2=INFO, 3=DEBUG, 4=DEBUG_MORE.
 */
extern void AddLog(uint32_t loglevel, const char *formatP, ...);
#define BT_CRYPTO_LOG_ERROR  1
#define BT_CRYPTO_LOG_INFO   2
#define BT_CRYPTO_LOG_DEBUG  3

extern "C" {
#include "t_bearssl_block.h"
#include "t_bearssl_hash.h"
#include "t_bearssl_hmac.h"
#include "t_bearssl_rand.h"
#include "t_bearssl_ec.h"
}

#include "esp_random.h"

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ecdsa.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>

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

/* ---- ECDSA P-256 ---- */

static const uint8_t kSecp256r1Oid[] = {0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};

/* Extract 32-byte raw private key from DER-encoded EC key pair */
static bool der_extract_privkey(const uint8_t *der, size_t der_len, uint8_t *privkey) {
  if (der_len < 2 || der[0] != 0x30) return false;
  size_t seq_len = der[1];
  if (seq_len > 127) {
    if (der[1] == 0x81 && der_len >= 3) {
      seq_len = der[2];
      der += 3;
      der_len -= 3;
    } else {
      return false;
    }
  } else {
    der += 2;
    der_len -= 2;
  }
  if (der_len < 3 || der[0] != 0x02 || der[1] != 0x01 || der[2] != 0x01) return false;
  der += 3; der_len -= 3;
  if (der_len < 34 || der[0] != 0x04 || der[1] != 0x20) return false;
  memcpy(privkey, der + 2, 32);
  return true;
}

/* Encode EC key pair to DER (RFC 5915) with private key + OID + public key */
static size_t der_encode_keypair(const uint8_t *privkey, const uint8_t *pubkey, uint8_t *der, size_t der_max) {
  size_t inner_len = 3 + 34 + 12 + 70;
  if (inner_len + 3 > der_max) return 0;

  size_t pos = 0;
  der[pos++] = 0x30;
  if (inner_len >= 128) {
    der[pos++] = 0x81;
    der[pos++] = (uint8_t)inner_len;
  } else {
    der[pos++] = (uint8_t)inner_len;
  }
  der[pos++] = 0x02; der[pos++] = 0x01; der[pos++] = 0x01;
  der[pos++] = 0x04; der[pos++] = 0x20;
  memcpy(der + pos, privkey, 32); pos += 32;
  der[pos++] = 0xa0; der[pos++] = 0x0a;
  memcpy(der + pos, kSecp256r1Oid, 10); pos += 10;
  der[pos++] = 0xa1; der[pos++] = 0x43;
  der[pos++] = 0x03; der[pos++] = 0x41; der[pos++] = 0x00;
  der[pos++] = 0x04;
  memcpy(der + pos, pubkey, 64); pos += 64;
  return pos;
}

extern "C" otError otPlatCryptoEcdsaGenerateKey(otPlatCryptoEcdsaKeyPair *aKeyPair) {
  if (!aKeyPair) return OT_ERROR_INVALID_ARGS;

  const br_ec_impl *ec = br_ec_get_default();
  ot_bearssl_rng_context rng_ctx;
  rng_ctx.vtable = &ot_rng_vtable;
  const br_prng_class **rng = &rng_ctx.vtable;

  br_ec_private_key sk;
  uint8_t kbuf_priv[BR_EC_KBUF_PRIV_MAX_SIZE];
  size_t priv_len = br_ec_keygen(rng, ec, &sk, kbuf_priv, BR_EC_secp256r1);
  if (priv_len == 0) return OT_ERROR_FAILED;

  br_ec_public_key pk;
  uint8_t kbuf_pub[BR_EC_KBUF_PUB_MAX_SIZE];
  size_t pub_len = br_ec_compute_pub(ec, &pk, kbuf_pub, &sk);
  if (pub_len == 0 || pub_len != 65) return OT_ERROR_FAILED;

  uint8_t privkey32[32];
  memset(privkey32, 0, 32);
  if (sk.xlen <= 32) {
    memcpy(privkey32 + (32 - sk.xlen), sk.x, sk.xlen);
  } else {
    return OT_ERROR_FAILED;
  }

  size_t der_len = der_encode_keypair(privkey32, kbuf_pub + 1, aKeyPair->mDerBytes, OT_CRYPTO_ECDSA_MAX_DER_SIZE);
  if (der_len == 0) return OT_ERROR_FAILED;
  aKeyPair->mDerLength = (uint8_t)der_len;

  AddLog(BT_CRYPTO_LOG_INFO, "BT_CRYPTO : ECDSA key generated der_len=%u", (unsigned)der_len);

  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoEcdsaGetPublicKey(const otPlatCryptoEcdsaKeyPair *aKeyPair,
                                                  otPlatCryptoEcdsaPublicKey     *aPublicKey) {
  if (!aKeyPair || !aPublicKey) return OT_ERROR_INVALID_ARGS;

  uint8_t privkey[32];
  if (!der_extract_privkey(aKeyPair->mDerBytes, aKeyPair->mDerLength, privkey))
    return OT_ERROR_PARSE;

  const br_ec_impl *ec = br_ec_get_default();
  br_ec_private_key sk;
  sk.curve = BR_EC_secp256r1;
  sk.x = privkey;
  sk.xlen = 32;

  br_ec_public_key pk;
  uint8_t kbuf_pub[BR_EC_KBUF_PUB_MAX_SIZE];
  size_t pub_len = br_ec_compute_pub(ec, &pk, kbuf_pub, &sk);
  if (pub_len != 65) return OT_ERROR_FAILED;

  memcpy(aPublicKey->m8, kbuf_pub + 1, 64);
  return OT_ERROR_NONE;
}

static int mbedtls_rng_wrapper(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

extern "C" otError otPlatCryptoEcdsaSign(const otPlatCryptoEcdsaKeyPair *aKeyPair,
                                          const otPlatCryptoSha256Hash   *aHash,
                                          otPlatCryptoEcdsaSignature     *aSignature) {
  if (!aKeyPair || !aHash || !aSignature) return OT_ERROR_INVALID_ARGS;

  mbedtls_pk_context pk;
  mbedtls_ecdsa_context ecdsa;
  mbedtls_mpi r, s;
  int ret;

  mbedtls_pk_init(&pk);
  mbedtls_ecdsa_init(&ecdsa);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);

  ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) {
    mbedtls_pk_free(&pk);
    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return OT_ERROR_FAILED;
  }

#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  ret = mbedtls_pk_parse_key(&pk, aKeyPair->mDerBytes, aKeyPair->mDerLength, nullptr, 0,
                              mbedtls_rng_wrapper, nullptr);
#else
  ret = mbedtls_pk_parse_key(&pk, aKeyPair->mDerBytes, aKeyPair->mDerLength, nullptr, 0);
#endif
  if (ret != 0) {
    mbedtls_pk_free(&pk);
    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return OT_ERROR_PARSE;
  }

  {
    mbedtls_ecp_keypair *keypair = mbedtls_pk_ec(pk);
    ret = mbedtls_ecdsa_from_keypair(&ecdsa, keypair);
  }
  if (ret != 0) {
    mbedtls_pk_free(&pk);
    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return OT_ERROR_FAILED;
  }

#if (MBEDTLS_VERSION_NUMBER >= 0x02130000)
  ret = mbedtls_ecdsa_sign_det_ext(&ecdsa.grp, &r, &s, &ecdsa.d, aHash->m8, 32, MBEDTLS_MD_SHA256, mbedtls_rng_wrapper, nullptr);
#else
  ret = mbedtls_ecdsa_sign_det(&ecdsa.grp, &r, &s, &ecdsa.d, aHash->m8, 32, MBEDTLS_MD_SHA256);
#endif
  if (ret != 0) {
    mbedtls_pk_free(&pk);
    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return OT_ERROR_FAILED;
  }

  memset(aSignature->m8, 0, OT_CRYPTO_ECDSA_SIGNATURE_SIZE);
  mbedtls_mpi_write_binary(&r, aSignature->m8, 32);
  mbedtls_mpi_write_binary(&s, aSignature->m8 + 32, 32);

  AddLog(BT_CRYPTO_LOG_INFO, "SIG_MTLS: r0=%02x s0=%02x",
         aSignature->m8[0], aSignature->m8[32]);

  mbedtls_pk_free(&pk);
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecdsa_free(&ecdsa);

  return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoEcdsaVerify(const otPlatCryptoEcdsaPublicKey *aPublicKey,
                                            const otPlatCryptoSha256Hash     *aHash,
                                            const otPlatCryptoEcdsaSignature *aSignature) {
  if (!aPublicKey || !aHash || !aSignature) return OT_ERROR_INVALID_ARGS;

  const br_ec_impl *ec = br_ec_get_default();

  uint8_t pub_point[65];
  pub_point[0] = 0x04;
  memcpy(pub_point + 1, aPublicKey->m8, 64);

  br_ec_public_key pk;
  pk.curve = BR_EC_secp256r1;
  pk.q = pub_point;
  pk.qlen = 65;

  uint32_t result = br_ecdsa_i15_vrfy_raw(ec, aHash->m8, 32, &pk, aSignature->m8, OT_CRYPTO_ECDSA_SIGNATURE_SIZE);
  return (result == 1) ? OT_ERROR_NONE : OT_ERROR_SECURITY;
}

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

#endif  // #if 0 — BearSSL disabled
#endif /* USE_MATTER_THREAD */
