// Mbed TLS configuration for VanillaDroid (MBEDTLS_CONFIG_FILE): only what SRP6 needs, big integers and
// SHA-1; phase 22 (distribution): SHA-256 for the packs' recipes and the downloads. No TLS, no PSA, no entropy
// sources (the client ephemeral comes from the OS random device).
// Phase 22 (release): named vd_mbedtls_config.h, not mbedtls_config.h: build_info.h includes MBEDTLS_CONFIG_FILE in
// quotes, which finds Mbed TLS's own include/mbedtls/mbedtls_config.h (its full default configuration) next to it
// first, so a file of ours with that name was never read.
#pragma once

#define VD_MBEDTLS_CONFIG 1 // core/ReleaseSignature.cpp checks that this file is the one read

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C

// Phase 22 (release): ECDSA verification over P-256 of the release manifest's signature (core/ReleaseSignature).
// The DER signature is read with the ASN.1 parser; ecdsa.c also requires the ASN.1 writer (for signing, which
// nothing calls: the linker drops it).
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
