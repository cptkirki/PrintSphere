#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <thread>
using png_int_32 = int32_t;
extern std::atomic<unsigned> host_png_decodes, host_png_nonowner_decodes;
extern std::thread::id host_ui_owner;
struct png_image { uint32_t version=0, width=0, height=0, format=0; char message[64]{}; uint8_t fill=0; };
#define PNG_IMAGE_VERSION 1
#define PNG_FORMAT_BGRA 0
#define PNG_IMAGE_SIZE(i) (static_cast<size_t>((i).width) * (i).height * 4)
// Deliberately fake codec: exercise the real UI's buffer handoff/lifetime,
// without claiming to test libpng or JPEG decoding on the host.
inline bool png_image_begin_read_from_memory(png_image* image, const void* data, size_t bytes) {
  if (bytes != 1) return false;
  image->width=16; image->height=16; image->fill=*static_cast<const uint8_t*>(data);
  ++host_png_decodes;
  if (std::this_thread::get_id() != host_ui_owner) ++host_png_nonowner_decodes;
  return true;
}
inline bool png_image_finish_read(png_image* image, void*, void* buffer, png_int_32, void*) {
  auto* bytes=static_cast<uint8_t*>(buffer);
  for (size_t i=0; i<PNG_IMAGE_SIZE(*image); i+=4) {
    bytes[i]=bytes[i+1]=bytes[i+2]=image->fill; bytes[i+3]=255;
  }
  return true;
}
inline void png_image_free(png_image*) {}
