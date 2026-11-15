// Exercise the production staging implementation with asynchronous panel DMA.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"DMA check failed: %s, line %d\n",#c,__LINE__); std::exit(2); } } while (0)
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_NO_MEM=2;
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_DMA=2, MALLOC_CAP_8BIT=4;
constexpr int BSP_LCD_DMA_STAGING_ROWS=4;
struct bsp_rotation_panel_t {
  void* target=reinterpret_cast<void*>(1);
  uint16_t* rotation_buffer=nullptr;
  size_t rotation_buffer_pixels=0;
  bool te_sync_mode=true;
  uint8_t staging_slot=0;
};
void* io_handle=reinterpret_cast<void*>(2);
constexpr const char* TAG="test";
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
bool esp_ptr_external_ram(const void*) { return false; }
uint16_t* heap_caps_malloc(size_t bytes, int) { return static_cast<uint16_t*>(std::malloc(bytes)); }
const uint16_t* inflight=nullptr;
std::vector<uint16_t> inflight_copy;
unsigned submits=0, drains=0;
bool drain_failure=false;
void retire_dma() {
  if (inflight) {
    CHECK(std::equal(inflight_copy.begin(),inflight_copy.end(),inflight));
    inflight=nullptr;
  }
}
esp_err_t esp_lcd_panel_draw_bitmap(void*,int x1,int y1,int x2,int y2,const void* data) {
  // CO5300 CASET/RASET drain the previous transaction before queuing this one.
  retire_dma();
  CHECK(x2>x1 && y2>y1);
  inflight=static_cast<const uint16_t*>(data);
  inflight_copy.assign(inflight,inflight+size_t(x2-x1)*(y2-y1));
  ++submits;
  return ESP_OK; // asynchronous: the final DMA is deliberately still running
}
esp_err_t esp_lcd_panel_io_tx_param(void* io,int command,const void* param,size_t size) {
  CHECK(io==io_handle && command==-1 && param==nullptr && size==0);
  ++drains;
  if (drain_failure) return 17;
  retire_dma();
  return ESP_OK;
}
void checked_free(void* pointer) {
  // Freeing any staging allocation while the last DMA owns it is forbidden.
  CHECK(inflight==nullptr);
  std::free(pointer);
}
#define free checked_free
#include "../components/esp32_s3_touch_amoled_1_75/priv_include/bsp_dma_staging.h"
#undef free

int main() {
  bsp_rotation_panel_t panel;
  std::vector<uint16_t> pixels(466*24);
  for (size_t i=0;i<pixels.size();++i) pixels[i]=uint16_t(i);
  for (auto dimensions : {std::pair<int,int>{30,24},{466,24},{16,2},{466,1},{24,466}}) {
    const auto count=submits;
    const auto before=drains;
    CHECK(bsp_rotation_panel_draw_staged(&panel,0,0,dimensions.first,dimensions.second,pixels.data())==ESP_OK);
    CHECK(inflight==nullptr); // first chunk callback cannot complete the whole flush
    CHECK(submits-count==unsigned((dimensions.second+3)/4));
    CHECK(drains-before==2);
  }
  const auto* allocation=panel.rotation_buffer;
  // A failed initial drain must leave the DMA-owned allocation untouched.
  inflight=allocation;
  inflight_copy.assign(inflight,inflight+8);
  drain_failure=true;
  CHECK(bsp_rotation_panel_draw_staged(&panel,0,0,466,24,pixels.data())==17);
  CHECK(panel.rotation_buffer==allocation && inflight==allocation);
  drain_failure=false;
  retire_dma();
  CHECK(bsp_rotation_panel_draw_staged(&panel,0,0,0,24,pixels.data())==ESP_ERR_INVALID_ARG);
  checked_free(panel.rotation_buffer);
  std::puts("AMOLED production DMA staging: final completion, changing widths, small/rotated rectangles and drain failure passed");
}
