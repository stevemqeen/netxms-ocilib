#ifndef _nxcrypto_h_
#define _nxcrypto_h_

#if defined(_WITH_ENCRYPTION) && !defined(ORA_PROC)

#include <openssl/crypto.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/opensslv.h>
#include <openssl/err.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#endif

#ifdef _WIN32
#include <wincrypt.h>
#endif

#ifdef NETXMS_NO_AES
#ifndef OPENSSL_NO_AES
#define OPENSSL_NO_AES
#endif
#endif

#ifdef NETXMS_NO_BF
#ifndef OPENSSL_NO_BF
#define OPENSSL_NO_BF
#endif
#endif

#ifdef NETXMS_NO_IDEA
#ifndef OPENSSL_NO_IDEA
#define OPENSSL_NO_IDEA
#endif
#endif

#ifdef NETXMS_NO_DES
#ifndef OPENSSL_NO_DES
#define OPENSSL_NO_DES
#endif
#endif

/**** Crypto helper functions ****/
#ifdef _WITH_ENCRYPTION
struct MD_STATE
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
   EVP_MD_CTX *context;
#else
   EVP_MD_CTX context;
#endif
};

typedef struct MD_STATE MD5_STATE;
#else
typedef unsigned char MD5_STATE[128];
#endif

void LIBNETXMS_EXPORTABLE MD5Init(MD5_STATE *state);
void LIBNETXMS_EXPORTABLE MD5Update(MD5_STATE *state, const void *data, size_t size);
void LIBNETXMS_EXPORTABLE MD5Final(MD5_STATE *state, BYTE *hash);

#ifdef _WITH_ENCRYPTION
typedef struct MD_STATE SHA1_STATE;
#else
typedef unsigned char SHA1_STATE[128];
#endif

void LIBNETXMS_EXPORTABLE SHA1Init(SHA1_STATE *state);
void LIBNETXMS_EXPORTABLE SHA1Update(SHA1_STATE *state, const void *data, size_t size);
void LIBNETXMS_EXPORTABLE SHA1Final(SHA1_STATE *state, BYTE *hash);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
typedef EVP_PKEY* RSA_KEY;
static inline EVP_PKEY *EVP_PKEY_from_RSA_KEY(RSA_KEY k)
{
   return k;
}
#else
typedef RSA* RSA_KEY;
static inline EVP_PKEY *EVP_PKEY_from_RSA_KEY(RSA_KEY k)
{
   EVP_PKEY *ek = EVP_PKEY_new();
   if (ek == NULL)
      return NULL;
   EVP_PKEY_assign_RSA(ek, k);
   return ek;
}
#endif

/**
 * Encrypt data block with RSA public key
 */
static inline ssize_t RSAPublicEncrypt(const void *in, size_t inlen, BYTE *out, size_t outlen, RSA_KEY rsa, int padding)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(rsa, NULL);
   if (EVP_PKEY_encrypt_init(ctx) <= 0)
   {
      EVP_PKEY_CTX_free(ctx);
      return -1;
   }
   EVP_PKEY_CTX_set_rsa_padding(ctx, padding);
   size_t bytes = outlen;
   int rc = EVP_PKEY_encrypt(ctx, out, &bytes, (const BYTE*)in, inlen);
   EVP_PKEY_CTX_free(ctx);
   return (rc > 0) ? bytes : -1;
#else
   return RSA_public_encrypt((int)inlen, (const BYTE*)in, out, rsa, padding);
#endif
}

/**
 * Decrypt data block with RSA private key
 */
static inline ssize_t RSAPrivateDecrypt(const BYTE *in, size_t inlen, void *out, size_t outlen, RSA_KEY rsa, int padding)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(rsa, NULL);
   EVP_PKEY_decrypt_init(ctx);
   EVP_PKEY_CTX_set_rsa_padding(ctx, padding);
   size_t bytes = outlen;
   int rc = EVP_PKEY_decrypt(ctx, (BYTE*)out, &bytes, in, inlen);
   EVP_PKEY_CTX_free(ctx);
   return (rc > 0) ? bytes : -1;
#else
   return RSA_private_decrypt((int)inlen, in, (BYTE*)out, rsa, padding);
#endif
}

/**
 * Decrypt data block with RSA public key
 */
static inline ssize_t RSAPublicDecrypt(const BYTE *in, size_t inlen, void *out, size_t outlen, RSA_KEY rsa, int padding)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(rsa, NULL);
   EVP_PKEY_verify_recover_init(ctx);
   EVP_PKEY_CTX_set_rsa_padding(ctx, padding);
   size_t bytes = outlen;
   int rc = EVP_PKEY_verify_recover(ctx, (BYTE*)out, &bytes, in, inlen);
   EVP_PKEY_CTX_free(ctx);
   return (rc > 0) ? bytes : -1;
#else
   return RSA_public_decrypt((int)inlen, in, (BYTE*)out, rsa, padding);
#endif
}

/**
 * Get maximum buffer size required for encryption/decryption operations with given buffer
 */
static inline size_t RSASize(RSA_KEY rsa)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
   return EVP_PKEY_get_size(rsa);
#else
   return RSA_size(rsa);
#endif
}

#endif

#endif
