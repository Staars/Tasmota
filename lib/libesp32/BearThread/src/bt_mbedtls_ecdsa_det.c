/*
  bt_mbedtls_ecdsa_det.c - provide mbedtls_ecdsa_sign_det_ext() for BearThread

  Copyright (C) 2025 Christian Baars

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ---------------------------------------------------------------------------
  Why this file exists
  ---------------------------------------------------------------------------
  The goal is to use the *exact same* mbedTLS ECDSA implementation that the
  espressif esp-matter "light" Thread example uses, so a possible bug in the
  custom BearSSL crypto layer can be ruled out for OpenThread SRP SIG(0)
  signing.

  esp-matter builds mbedTLS from source (ESP-IDF component) with
  CONFIG_MBEDTLS_ECDSA_DETERMINISTIC=y, so its libmbedcrypto contains
  mbedtls_ecdsa_sign_det_ext(). Tasmota's pioarduino build, however, links the
  *prebuilt* arduino-esp32 libmbedcrypto.a, and that prebuilt archive does NOT
  export mbedtls_ecdsa_sign_det_ext() (only mbedtls_ecdsa_sign() and the
  restartable variant are present). Setting CONFIG_MBEDTLS_ECDSA_DETERMINISTIC=y
  in custom_sdkconfig has no effect because mbedTLS is not recompiled from
  source - hence the link error:

      undefined reference to `mbedtls_ecdsa_sign_det_ext'

  This file re-supplies that single missing wrapper. It is a faithful copy of
  mbedTLS 3.6.x ecdsa.c (derive_mpi + the deterministic wrapper). The actual EC
  signing math still runs through the prebuilt mbedTLS mbedtls_ecdsa_sign(), and
  the deterministic nonce is derived with RFC 6979 / HMAC-DRBG exactly as
  mbedTLS does, so the produced (r,s) signature is byte-identical to what
  esp-matter produces for the same key and message.
*/

#ifdef USE_MATTER_THREAD

#if 0   // Disabled — BearSSL crypto provides deterministic ECDSA signing via bt_crypto_bearssl.cpp

#include <string.h>

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hmac_drbg.h>
#include <mbedtls/error.h>

#if defined(MBEDTLS_ECDSA_C) && defined(MBEDTLS_ECDSA_DETERMINISTIC)

#ifndef MBEDTLS_MPI_CHK
#define MBEDTLS_MPI_CHK(f)       \
    do {                         \
        if ((ret = (f)) != 0) {  \
            goto cleanup;        \
        }                        \
    } while (0)
#endif

/* Reduce a hash (msg digest) to an integer modulo the group order N.
 * Faithful copy of the static derive_mpi() helper from mbedTLS ecdsa.c. */
static int bt_derive_mpi(const mbedtls_ecp_group *grp, mbedtls_mpi *x,
                         const unsigned char *buf, size_t blen) {
  int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
  size_t n_size = (grp->nbits + 7) / 8;
  size_t use_size = blen > n_size ? n_size : blen;

  MBEDTLS_MPI_CHK(mbedtls_mpi_read_binary(x, buf, use_size));
  if (use_size * 8 > grp->nbits) {
    MBEDTLS_MPI_CHK(mbedtls_mpi_shift_r(x, use_size * 8 - grp->nbits));
  }

  /* While at it, reduce modulo N */
  if (mbedtls_mpi_cmp_mpi(x, &grp->N) >= 0) {
    MBEDTLS_MPI_CHK(mbedtls_mpi_sub_mpi(x, x, &grp->N));
  }

cleanup:
  return ret;
}

/*
 * Deterministic signature wrapper (RFC 6979), faithful to mbedTLS 3.6.x.
 *
 * Seeds an HMAC-DRBG from (private key || reduced message hash) and uses it as
 * the nonce/RNG source for the regular mbedTLS ECDSA signature routine. This is
 * exactly the construction mbedTLS uses internally for deterministic ECDSA.
 */
int mbedtls_ecdsa_sign_det_ext(mbedtls_ecp_group *grp, mbedtls_mpi *r,
                               mbedtls_mpi *s, const mbedtls_mpi *d,
                               const unsigned char *buf, size_t blen,
                               mbedtls_md_type_t md_alg,
                               int (*f_rng_blind)(void *, unsigned char *, size_t),
                               void *p_rng_blind) {
  int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
  mbedtls_hmac_drbg_context rng_ctx;
  unsigned char data[2 * MBEDTLS_ECP_MAX_BYTES];
  size_t grp_len = (grp->nbits + 7) / 8;
  const mbedtls_md_info_t *md_info;
  mbedtls_mpi h;

  (void)f_rng_blind;
  (void)p_rng_blind;

  if ((md_info = mbedtls_md_info_from_type(md_alg)) == NULL) {
    return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
  }

  mbedtls_mpi_init(&h);
  mbedtls_hmac_drbg_init(&rng_ctx);

  /* Use private key and message hash (reduced) to initialize HMAC_DRBG */
  MBEDTLS_MPI_CHK(mbedtls_mpi_write_binary(d, data, grp_len));
  MBEDTLS_MPI_CHK(bt_derive_mpi(grp, &h, buf, blen));
  MBEDTLS_MPI_CHK(mbedtls_mpi_write_binary(&h, data + grp_len, grp_len));
  MBEDTLS_MPI_CHK(mbedtls_hmac_drbg_seed_buf(&rng_ctx, md_info, data, 2 * grp_len));

  ret = mbedtls_ecdsa_sign(grp, r, s, d, buf, blen,
                           mbedtls_hmac_drbg_random, &rng_ctx);

cleanup:
  mbedtls_hmac_drbg_free(&rng_ctx);
  mbedtls_mpi_free(&h);

  return ret;
}

#endif /* MBEDTLS_ECDSA_C && MBEDTLS_ECDSA_DETERMINISTIC */

#endif  // #if 0
#endif /* USE_MATTER_THREAD */
