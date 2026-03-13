#include "printsphere/application.hpp"

#include <cassert>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint32_t kApplicationTaskStackBytes = 12288;
constexpr UBaseType_t kApplicationTaskPriority = 5;

void application_task(void*) {
  static printsphere::Application app;
  app.run();
  vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void) {
  const BaseType_t created =
      xTaskCreate(application_task, "printsphere_app", kApplicationTaskStackBytes / sizeof(StackType_t),
                  nullptr, kApplicationTaskPriority, nullptr);
  assert(created == pdPASS);
}
