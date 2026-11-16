#pragma once
#include <cstddef>
#include <cstdint>
namespace printsphere {
template<class Read, class Retryable, class Now, class Pause>
auto read_until(Read read, Retryable retryable, Now now, Pause pause, int64_t deadline) -> decltype(read()) {
  while(now()<deadline) {
    auto result=read();
    if (!retryable(result)) return result;
    pause();
  }
  return static_cast<decltype(read())>(-1);
}
template<class Write, class Retryable, class Now, class Pause>
bool write_all_until(const uint8_t* data, size_t length, Write write, Retryable retryable,
                     Now now, Pause pause, int64_t deadline) {
  size_t offset=0;
  while(offset<length && now()<deadline) {
    const auto result=write(data+offset,length-offset);
    if(retryable(result)) { pause(); continue; }
    if(result<=0 || static_cast<size_t>(result)>length-offset) return false;
    offset+=static_cast<size_t>(result);
  }
  return offset==length;
}
}
