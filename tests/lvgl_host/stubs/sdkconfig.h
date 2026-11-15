#pragma once
#define CONFIG_SPIRAM 0
#define CONFIG_PRINTSPHERE_EXPERIMENTAL_PRINT_CONTROL 1
#if defined(__cplusplus) && defined(_WIN32)
#include <ctime>
inline std::tm* localtime_r(const std::time_t* value, std::tm* result) {
  return localtime_s(result, value) == 0 ? result : nullptr;
}
#endif
