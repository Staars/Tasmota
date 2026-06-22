/*
  bt_hmac_mbedtls.cpp - HMAC-SHA256 using low-level mbedtls_sha256 (no MD layer)

  Provides otPlatCryptoHmacSha256*() strong overrides that bypass mbedTLS's
  MD abstraction layer. This is needed because arduino-esp32's prebuilt
  mbedtls library may not fully support MBEDTLS_MD_C for HMAC, while the
  underlying SHA-256 primitives are always available.
*/

#ifdef USE_MATTER_THREAD

#if 0   // Disabled — BearSSL crypto provides otPlatCryptoHmacSha256* via bt_crypto_bearssl.cpp

#include <string.h>
#include <stdlib.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <openthread/error.h>
#include <openthread/platform/crypto.h>

#define HMAC_BLOCK_SIZE 64

struct HmacCtx {
    mbedtls_sha256_context inner;
    mbedtls_sha256_context outer;
};

extern "C" otError otPlatCryptoHmacSha256Init(otCryptoContext *aContext)
{
    if (!aContext || aContext->mContextSize < sizeof(void *))
        return OT_ERROR_INVALID_ARGS;

    struct HmacCtx *ctx = (struct HmacCtx *)calloc(1, sizeof(struct HmacCtx));
    if (!ctx) return OT_ERROR_NO_BUFS;

    mbedtls_sha256_init(&ctx->inner);
    mbedtls_sha256_init(&ctx->outer);

    *(struct HmacCtx **)aContext->mContext = ctx;
    return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Deinit(otCryptoContext *aContext)
{
    if (!aContext) return OT_ERROR_INVALID_ARGS;

    struct HmacCtx *ctx = *(struct HmacCtx **)aContext->mContext;
    if (ctx) {
        mbedtls_sha256_free(&ctx->inner);
        mbedtls_sha256_free(&ctx->outer);
        free(ctx);
        *(struct HmacCtx **)aContext->mContext = NULL;
    }
    return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Start(otCryptoContext *aContext,
                                                const otCryptoKey *aKey)
{
    if (!aContext || !aKey || !aKey->mKey) return OT_ERROR_INVALID_ARGS;

    struct HmacCtx *ctx = *(struct HmacCtx **)aContext->mContext;
    if (!ctx) return OT_ERROR_INVALID_STATE;

    const uint8_t *key = aKey->mKey;
    size_t key_len = aKey->mKeyLength;
    uint8_t k[HMAC_BLOCK_SIZE];

    if (key_len > HMAC_BLOCK_SIZE) {
#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
        mbedtls_sha256_starts(&ctx->inner, 0);
        mbedtls_sha256_update(&ctx->inner, key, key_len);
        mbedtls_sha256_finish(&ctx->inner, k);
#else
        mbedtls_sha256_starts_ret(&ctx->inner, 0);
        mbedtls_sha256_update_ret(&ctx->inner, key, key_len);
        mbedtls_sha256_finish_ret(&ctx->inner, k);
#endif
        key = k;
        key_len = 32;
    }

    uint8_t k_ipad[HMAC_BLOCK_SIZE];
    uint8_t k_opad[HMAC_BLOCK_SIZE];

    memset(k_ipad, 0x36, HMAC_BLOCK_SIZE);
    memset(k_opad, 0x5c, HMAC_BLOCK_SIZE);

    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_sha256_starts(&ctx->inner, 0);
    mbedtls_sha256_update(&ctx->inner, k_ipad, HMAC_BLOCK_SIZE);

    mbedtls_sha256_starts(&ctx->outer, 0);
    mbedtls_sha256_update(&ctx->outer, k_opad, HMAC_BLOCK_SIZE);
#else
    mbedtls_sha256_starts_ret(&ctx->inner, 0);
    mbedtls_sha256_update_ret(&ctx->inner, k_ipad, HMAC_BLOCK_SIZE);

    mbedtls_sha256_starts_ret(&ctx->outer, 0);
    mbedtls_sha256_update_ret(&ctx->outer, k_opad, HMAC_BLOCK_SIZE);
#endif

    return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Update(otCryptoContext *aContext,
                                                 const void *aBuf,
                                                 uint16_t aBufLength)
{
    if (!aContext || !aBuf) return OT_ERROR_INVALID_ARGS;

    struct HmacCtx *ctx = *(struct HmacCtx **)aContext->mContext;
    if (!ctx) return OT_ERROR_INVALID_STATE;

#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_sha256_update(&ctx->inner, (const uint8_t *)aBuf, aBufLength);
#else
    mbedtls_sha256_update_ret(&ctx->inner, (const uint8_t *)aBuf, aBufLength);
#endif

    return OT_ERROR_NONE;
}

extern "C" otError otPlatCryptoHmacSha256Finish(otCryptoContext *aContext,
                                                 uint8_t *aBuf,
                                                 size_t aBufLength)
{
    (void)aBufLength;
    if (!aContext || !aBuf) return OT_ERROR_INVALID_ARGS;

    struct HmacCtx *ctx = *(struct HmacCtx **)aContext->mContext;
    if (!ctx) return OT_ERROR_INVALID_STATE;

    uint8_t inner_hash[32];

#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_sha256_finish(&ctx->inner, inner_hash);
    mbedtls_sha256_update(&ctx->outer, inner_hash, 32);
    mbedtls_sha256_finish(&ctx->outer, aBuf);
#else
    mbedtls_sha256_finish_ret(&ctx->inner, inner_hash);
    mbedtls_sha256_update_ret(&ctx->outer, inner_hash, 32);
    mbedtls_sha256_finish_ret(&ctx->outer, aBuf);
#endif

    return OT_ERROR_NONE;
}

#endif  // #if 0
#endif /* USE_MATTER_THREAD */
