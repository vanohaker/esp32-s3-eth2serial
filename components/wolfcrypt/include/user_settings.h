#pragma once
#include "sdkconfig.h"
#include <stdlib.h>
// Keep the crypto library on IDF's standard thread-safe heap. No hardware ports.
#define XMALLOC_USER
#define XMALLOC knife_malloc
#define XFREE knife_free
#define XREALLOC knife_realloc
#define WOLFSSL_ESPIDF
#define WOLFSSL_ESP32
#define NO_ESP32_CRYPT
#define NO_WOLFSSL_ESP32_CRYPT_HASH
#define NO_WOLFSSL_ESP32_CRYPT_AES
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI
#define SINGLE_THREADED
#define WOLFCRYPT_ONLY
#define WOLFSSL_WOLFSSH
#define WOLFSSL_KEY_GEN
#define WOLFSSL_PUBLIC_MP
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define WOLFSSL_SP_256
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_HAVE_SP_RSA
#define WOLFSSL_SP_NO_3072
#define WOLFSSL_SP_NO_4096
#define HAVE_ECC
#define ECC_USER_CURVES
#define HAVE_ECC256
#define HAVE_CURVE25519
#define HAVE_ED25519
#define WOLFSSL_SHA512
#define WOLFSSL_SHA384
#define HAVE_HKDF
#define HAVE_AESGCM
#define WOLFSSL_AES_COUNTER
#define WOLFSSL_AES_DIRECT
#define WOLFSSL_BASE64_ENCODE
#define NO_DSA
#define NO_DH
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_DES3
#define NO_PSK
#define NO_PWDBASED
#define NO_FILESYSTEM
#define WOLFSSH_NO_CLIENT
#define WOLFSSH_NO_DH_GEX_SHA256
#define WOLFSSH_NO_DH_GROUP14_SHA1
#define WOLFSSH_NO_DH_GROUP14_SHA256
#define WOLFSSH_NO_DH_GROUP16_SHA512
#define WOLFSSH_NO_SHA1
#define WOLFSSH_NO_AES_CBC
#define WOLFSSH_TERM
#define DEFAULT_WINDOW_SZ 4096
#define DEFAULT_MAX_PACKET_SZ 2048
