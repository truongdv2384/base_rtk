#pragma once
// =============================================================================
//  OtaSha256 — wrapper streaming quanh mbedtls SHA-256 (IDF 5.x / mbedtls 3.x).
//  Dùng để verify TOÀN ẢNH firmware (uncompressed) trước khi commit OTA.
//  (Chưa dùng ở Phase 1; đưa vào sẵn cho Phase 2+.)
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include "mbedtls/sha256.h"

class OtaSha256 {
 public:
  OtaSha256();
  ~OtaSha256();

  void begin();
  void update(const uint8_t* data, size_t len);
  void finish(uint8_t out[32]);

  // So sánh hash 32-byte với chuỗi hex 64 ký tự (không phân biệt hoa/thường).
  // true nếu khớp và hex đúng 64 ký tự hợp lệ.
  static bool equalsHex(const uint8_t hash[32], const char* hex64);

 private:
  mbedtls_sha256_context ctx_;
};
