#include "ota_sha256.h"

OtaSha256::OtaSha256()  { mbedtls_sha256_init(&ctx_); }
OtaSha256::~OtaSha256() { mbedtls_sha256_free(&ctx_); }

// is224 = 0 -> SHA-256. mbedtls 3.x: cac ham tra ve int (bo suffix _ret cu).
void OtaSha256::begin()                               { mbedtls_sha256_starts(&ctx_, 0); }
void OtaSha256::update(const uint8_t* d, size_t n)    { mbedtls_sha256_update(&ctx_, d, n); }
void OtaSha256::finish(uint8_t out[32])               { mbedtls_sha256_finish(&ctx_, out); }

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool OtaSha256::equalsHex(const uint8_t hash[32], const char* hex) {
  if (!hex) return false;
  for (int i = 0; i < 32; i++) {
    const int hi = hexNibble(hex[i * 2]);
    const int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    if ((uint8_t)((hi << 4) | lo) != hash[i]) return false;
  }
  return hex[64] == '\0';
}
