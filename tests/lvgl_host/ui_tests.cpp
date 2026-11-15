#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <cstdlib>
#include "printsphere/ui.hpp"
#include "esp_timer.h"
#include "png.h"

#undef assert
#define assert(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " #condition << " at " << __LINE__ << '\n'; std::exit(2); } } while (0)

lv_obj_t* find_text(lv_obj_t* object, const char* text) {
  if (lv_obj_check_type(object, &lv_label_class) && !std::strcmp(lv_label_get_text(object), text)) return object;
  for (uint32_t i=0; i<lv_obj_get_child_count(object); ++i)
    if (auto* found = find_text(lv_obj_get_child(object, i), text)) return found;
  return nullptr;
}
lv_obj_t* find_pager(lv_obj_t* object) {
  if (lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_dir(object) == LV_DIR_HOR) return object;
  for (uint32_t i=0; i<lv_obj_get_child_count(object); ++i)
    if (auto* found = find_pager(lv_obj_get_child(object, i))) return found;
  return nullptr;
}
const lv_image_dsc_t* find_preview(lv_obj_t* object) {
  if (lv_obj_check_type(object, &lv_image_class)) {
    const void* source=lv_image_get_src(object);
    if (source && lv_image_src_get_type(source)==LV_IMAGE_SRC_VARIABLE) {
      const auto* image=static_cast<const lv_image_dsc_t*>(source);
      if (image->header.w==16 && image->header.h==16) return image;
    }
  }
  for (uint32_t i=0; i<lv_obj_get_child_count(object); ++i)
    if (auto* image=find_preview(lv_obj_get_child(object, i))) return image;
  return nullptr;
}
void pump(unsigned ms) {
  for (unsigned i=0; i<ms; i+=5) { host_ticks += 5; lv_tick_inc(5); lv_timer_handler(); }
}
int main() {
  using namespace printsphere;
  lv_init();
  host_ui_owner=std::this_thread::get_id();
  Ui ui;
  assert(ui.initialize() == ESP_OK);
  auto* screen = lv_screen_active();
  auto* pager = find_pager(screen);
  assert(pager);
  PrinterSnapshot state;
  state.lifecycle = PrintLifecycleState::kPrinting;
  state.print_active = true;
  state.progress_percent = 25;
  state.preview_page_available = false;
  state.camera_page_available = false;
  ui.apply_snapshot(state);
  pump(100);
  assert(find_text(lv_layer_top(), "25%"));
  state.progress_percent = 55;
  std::thread producer([&] { ui.apply_snapshot(state); });
  producer.join();
  assert(find_text(lv_layer_top(), "25%")); // producer performs no widget mutation
  pump(30);
  assert(find_text(lv_layer_top(), "55%"));
  lv_obj_send_event(pager, LV_EVENT_SCROLL_BEGIN, nullptr);
  assert(ui.is_page_transition_active());
  for (int i=56; i<=99; ++i) { state.progress_percent = float(i); ui.apply_snapshot(state); }
  pump(60);
  assert(find_text(lv_layer_top(), "55%") && !find_text(lv_layer_top(), "99%"));
  lv_obj_send_event(pager, LV_EVENT_SCROLL_END, nullptr);
  pump(500);
  assert(!ui.is_page_transition_active() && find_text(lv_layer_top(), "99%"));
  // A nested vertical list/label bubbles scroll events. Those must never
  // suspend status delivery or snap the horizontal pager to another page.
  auto* nested_scroll=lv_obj_create(lv_obj_get_child(pager,0));
  lv_obj_add_flag(nested_scroll,LV_OBJ_FLAG_EVENT_BUBBLE);
  const int pager_x=lv_obj_get_scroll_x(pager);
  lv_obj_send_event(nested_scroll,LV_EVENT_SCROLL_BEGIN,nullptr);
  assert(!ui.is_page_transition_active());
  lv_obj_send_event(nested_scroll,LV_EVENT_SCROLL_END,nullptr);
  assert(lv_obj_get_scroll_x(pager)==pager_x && !ui.is_page_transition_active());
  lv_obj_delete(nested_scroll);
  pump(500); // settle layout invalidation caused by creating/deleting the fixture
  pump(50);
  unsigned redraws = 0;
  lv_display_add_event_cb(lv_display_get_default(), [](lv_event_t* event) {
    ++*static_cast<unsigned*>(lv_event_get_user_data(event));
  }, LV_EVENT_RENDER_START, &redraws);
  ui.apply_snapshot(state);
  pump(100);
  assert(redraws == 0); // identical status must not force another full-frame render
  std::vector<Ui::PrinterCardInfo> cards(2);
  cards[0].serial="one"; cards[0].name="Printer one";
  cards[1].index=1; cards[1].serial="two"; cards[1].name="Printer two";
  ui.update_printer_cards(cards);
  pump(60);
  auto* first_name = find_text(screen, "Printer one");
  assert(first_name);
  auto* first_card = lv_obj_get_parent(first_name);
  cards[0].connected=true; cards[0].active=true; cards[0].name="Renamed printer";
  ui.update_printer_cards(cards);
  pump(60);
  auto* renamed = find_text(screen, "Renamed printer");
  assert(renamed && lv_obj_get_parent(renamed) == first_card);
  state.preview_page_available=true;
  state.preview_blob=std::make_shared<std::vector<uint8_t>>(1,42);
  ui.apply_snapshot(state);
  pump(60);
  assert(host_png_decodes.load()==0); // offscreen preview is not decoded
  auto* preview_page=lv_obj_get_child(pager,Ui::kPageIdxPreview);
  lv_obj_update_layout(pager);
  lv_obj_scroll_to_x(pager,lv_obj_get_x(preview_page),LV_ANIM_OFF);
  pump(300);
  assert(ui.is_page2_active());
  std::thread image_producer([&]{ui.apply_snapshot(state);}); image_producer.join();
  pump(60);
  const auto* image=find_preview(screen);
  assert(image && image->data[0]==42 && host_png_nonowner_decodes.load()==1);
  state.preview_blob=std::make_shared<std::vector<uint8_t>>(1,99);
  std::thread replacement([&]{ui.apply_snapshot(state);}); replacement.join();
  assert(find_preview(screen)->data[0]==42); // old pixels live until owner commits replacement
  pump(60);
  assert(find_preview(screen)->data[0]==99 && host_png_decodes.load()==2);
  state.preview_blob.reset();
  ui.apply_snapshot(state); pump(60);
  assert(!find_preview(screen));
  std::atomic<bool> producer_done{false};
  std::thread concurrent_producer([&] {
    auto message=state;
    for (int i=0; i<1000; ++i) {
      message.progress_percent=float(i%100);
      ui.apply_snapshot(message);
    }
    message.progress_percent=77;
    ui.apply_snapshot(message);
    producer_done=true;
  });
  while (!producer_done.load()) { pump(5); std::this_thread::yield(); }
  concurrent_producer.join();
  pump(60);
  assert(find_text(lv_layer_top(),"77%") && !ui.has_pending_snapshot());
  pump(5100);
  auto performance = ui.performance_snapshot();
  assert(performance && performance->window_ms >= 5000);
  assert(performance->render_cycle.samples > 0 && performance->widget_update.samples > 0);
  std::cout << "Actual LVGL UI: owner handoff, scroll coalescing, no redundant redraw, stable cards, image lifetime and telemetry passed\n";
}
