#pragma once
#include "printsphere/bounded_io.hpp"
#include "esp_tls.h"
#include "esp_timer.h"
#include "mbedtls/ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace printsphere {
inline bool tls_retryable(ssize_t result) {
 return result==MBEDTLS_ERR_SSL_WANT_READ || result==MBEDTLS_ERR_SSL_WANT_WRITE;
}
inline void tls_retry_pause() { vTaskDelay(pdMS_TO_TICKS(10)); }
inline ssize_t tls_read_until(esp_tls_t* tls, void* data, size_t length, int64_t deadline) {
 return read_until([&]{return esp_tls_conn_read(tls,data,length);},tls_retryable,
                   esp_timer_get_time,tls_retry_pause,deadline);
}
inline bool tls_write_until(esp_tls_t* tls,const void* data,size_t length,int64_t deadline) {
 return write_all_until(static_cast<const uint8_t*>(data),length,
   [&](const uint8_t* bytes,size_t size){return esp_tls_conn_write(tls,bytes,size);},
   tls_retryable,esp_timer_get_time,tls_retry_pause,deadline);
}
}
