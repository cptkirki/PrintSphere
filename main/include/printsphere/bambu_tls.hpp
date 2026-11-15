#pragma once
#include <cstdint>
#include <string>

namespace printsphere {
inline const std::string& bambu_ca_bundle() {
  extern const uint8_t root[] asm("_binary_bambu_cert_start");
  extern const uint8_t p2s[] asm("_binary_bambu_p2s_250626_cert_start");
  extern const uint8_t h2c[] asm("_binary_bambu_h2c_251122_cert_start");
  extern const uint8_t x2[] asm("_binary_bambu_x2c_260425_cert_start");
  static const std::string bundle = std::string(reinterpret_cast<const char*>(root)) + "\n" +
      reinterpret_cast<const char*>(p2s) + "\n" + reinterpret_cast<const char*>(h2c) + "\n" +
      reinterpret_cast<const char*>(x2);
  return bundle;
}
}  // namespace printsphere
