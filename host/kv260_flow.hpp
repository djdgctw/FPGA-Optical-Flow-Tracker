#pragma once
#include <cstdint>

// The existing coefficient XO uses ap_fixed<40,8>: 32 fractional bits.
// HLS zero-pads its memory store to 64 bits. Sign-extend bit 39 explicitly.
inline int64_t kv260_signed_flow(uint64_t word) {
  constexpr uint64_t modulus=uint64_t{1}<<40;
  const uint64_t bits=word&(modulus-1);
  return (bits&(modulus>>1)) ? int64_t(bits)-int64_t(modulus) : int64_t(bits);
}
inline float kv260_flow_pixels(uint64_t word) {
  return float(kv260_signed_flow(word)/4294967296.0);
}
