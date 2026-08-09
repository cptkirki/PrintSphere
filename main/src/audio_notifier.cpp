#include "printsphere/audio_notifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#elif defined(PRINTSPHERE_HW_VARIANT_KNOMI_V2)
#include "bsp/btt_knomi_v2.h"
#else
#error "Unknown PrintSphere hardware variant"
#endif

namespace printsphere {

namespace {

constexpr const char* kTag = "printsphere.audio";

// Sample rate kept low to minimise CPU and DMA pressure — a square wave only
// needs to clear Nyquist for the highest fundamental we play (~1500 Hz).
constexpr int kSampleRate = 16000;
constexpr int kAttackReleaseSamples = 80;  // 5 ms fade — kills click artefacts
constexpr int kQueueDepth = 4;
constexpr size_t kMinCustomPcmSamples = kSampleRate / 50;  // 20 ms
constexpr int kMinCustomPcmPeak = 96;

struct Note {
  uint16_t frequency_hz;  // 0 = silence (rest)
  uint16_t duration_ms;
};

struct Melody {
  const Note* notes;
  uint8_t count;
};

// ----- Pre-built melodies ---------------------------------------------------

constexpr Note kPrintStartedNotes[] = {
    {523, 90},   // C5
    {659, 90},   // E5
    {784, 140},  // G5
};

constexpr Note kPrintFinishedNotes[] = {
    {523, 110}, {659, 110}, {784, 110}, {1047, 220}, {0, 80}, {784, 90}, {1047, 320},
};

constexpr Note kPrintErrorNotes[] = {
    {220, 220}, {0, 80}, {220, 220}, {0, 80}, {165, 360},
};

constexpr Note kHmsAlertNotes[] = {
    {1100, 120}, {0, 45}, {850, 160}, {0, 45}, {1100, 120},
};

constexpr Note kPrintPausedNotes[] = {
    {659, 100}, {0, 60}, {659, 100},
};

constexpr Note kFilamentChangeNotes[] = {
    {784, 110}, {988, 110}, {784, 110}, {988, 200},
};

constexpr Note kReconnectNotes[] = {
    {600, 60}, {0, 40}, {800, 90},
};

constexpr Note kClickNotes[] = {
    {1800, 40},
};

constexpr Melody melody_for(AudioNotifier::Event event) {
  switch (event) {
    case AudioNotifier::Event::kPrintStarted:
      return {kPrintStartedNotes,
              sizeof(kPrintStartedNotes) / sizeof(kPrintStartedNotes[0])};
    case AudioNotifier::Event::kPrintFinished:
      return {kPrintFinishedNotes,
              sizeof(kPrintFinishedNotes) / sizeof(kPrintFinishedNotes[0])};
    case AudioNotifier::Event::kPrintError:
      return {kPrintErrorNotes,
              sizeof(kPrintErrorNotes) / sizeof(kPrintErrorNotes[0])};
    case AudioNotifier::Event::kHmsAlert:
      return {kHmsAlertNotes,
              sizeof(kHmsAlertNotes) / sizeof(kHmsAlertNotes[0])};
    case AudioNotifier::Event::kPrintPaused:
      return {kPrintPausedNotes,
              sizeof(kPrintPausedNotes) / sizeof(kPrintPausedNotes[0])};
    case AudioNotifier::Event::kFilamentChange:
      return {kFilamentChangeNotes,
              sizeof(kFilamentChangeNotes) / sizeof(kFilamentChangeNotes[0])};
    case AudioNotifier::Event::kReconnect:
      return {kReconnectNotes,
              sizeof(kReconnectNotes) / sizeof(kReconnectNotes[0])};
    case AudioNotifier::Event::kClick:
      return {kClickNotes, sizeof(kClickNotes) / sizeof(kClickNotes[0])};
  }
  return {kHmsAlertNotes, sizeof(kHmsAlertNotes) / sizeof(kHmsAlertNotes[0])};
}

// Worker-owned state. Kept in a single anonymous namespace because there is
// only ever one notifier instance for the whole firmware (matches the rest of
// printsphere's "one-of-everything" design).

// Queue items are uint8_t. Bits [6:0] carry the Event index; bit 7 is the
// "force" flag that bypasses both the master enabled gate and the per-event
// gate — used exclusively by web-portal test buttons.
constexpr uint8_t kQueueForceBit = 0x80U;

esp_codec_dev_handle_t g_codec = nullptr;
QueueHandle_t g_queue = nullptr;
std::atomic<int>* g_volume_ptr = nullptr;
std::atomic<bool>* g_enabled_ptr = nullptr;
std::atomic<bool>* g_event_enabled_ptr = nullptr;
std::mutex* g_pcm_mutex_ptr = nullptr;
std::vector<int16_t>* g_custom_pcm_ptr = nullptr;

void render_square_tone(uint16_t freq_hz, uint16_t duration_ms, int volume_pct,
                        std::vector<int16_t>& buffer) {
  const int total_samples = std::max(1, kSampleRate * duration_ms / 1000);
  buffer.resize(static_cast<size_t>(total_samples));
  if (freq_hz == 0 || volume_pct <= 0) {
    std::fill(buffer.begin(), buffer.end(), int16_t{0});
    return;
  }
  // Cap to safe headroom. The ES8311 + on-board PA gets unpleasantly loud at
  // full scale, and square waves clip the smoothing filter even harder than
  // sine, so we never exceed ~50 % of int16 range.
  const int volume = std::clamp(volume_pct, 0, 100);
  const float amplitude = 0.50f * static_cast<float>(volume) / 100.0f;
  const int16_t peak = static_cast<int16_t>(amplitude * 32767.0f);
  const int half_period_samples = kSampleRate / (2 * std::max<int>(1, freq_hz));
  if (half_period_samples <= 0) {
    std::fill(buffer.begin(), buffer.end(), int16_t{0});
    return;
  }

  bool high = true;
  int phase = 0;
  for (int i = 0; i < total_samples; ++i) {
    int16_t value = high ? peak : static_cast<int16_t>(-peak);

    // Linear attack + release envelope to suppress audible clicks at the
    // start/end of every note.
    if (i < kAttackReleaseSamples) {
      value = static_cast<int16_t>(static_cast<int32_t>(value) * i /
                                    kAttackReleaseSamples);
    } else if (i > total_samples - kAttackReleaseSamples) {
      const int remaining = total_samples - i;
      value = static_cast<int16_t>(static_cast<int32_t>(value) * remaining /
                                    kAttackReleaseSamples);
    }
    buffer[i] = value;

    if (++phase >= half_period_samples) {
      phase = 0;
      high = !high;
    }
  }
}

bool custom_pcm_is_playable(const std::vector<int16_t>& samples) {
  if (samples.size() < kMinCustomPcmSamples) {
    return false;
  }
  int peak = 0;
  for (const int16_t sample : samples) {
    const int value = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
    if (value > peak) {
      peak = value;
    }
  }
  return peak >= kMinCustomPcmPeak;
}

void worker_task(void*) {
  std::vector<int16_t> render_buffer;
  render_buffer.reserve(kSampleRate / 2);  // up to 0.5 s notes

  uint8_t queue_item;
  while (true) {
    if (xQueueReceive(g_queue, &queue_item, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    const bool force = (queue_item & kQueueForceBit) != 0;
    const auto event = static_cast<AudioNotifier::Event>(queue_item & ~kQueueForceBit);
    const uint8_t event_idx = static_cast<uint8_t>(event);

    if (!force) {
      if (g_enabled_ptr == nullptr || !g_enabled_ptr->load(std::memory_order_relaxed)) {
        continue;
      }
      // Per-event gate: skip silently if this event has been disabled.
      if (g_event_enabled_ptr != nullptr && event_idx < AudioNotifier::kEventCount &&
          !g_event_enabled_ptr[event_idx].load(std::memory_order_relaxed)) {
        continue;
      }
    }
    if (g_codec == nullptr) {
      continue;
    }

    // Snapshot the custom PCM for this event (lock-free copy outside the
    // codec session so we don't hold the mutex while doing I2S DMA writes).
    std::vector<int16_t> custom_pcm;
    if (g_pcm_mutex_ptr != nullptr && g_custom_pcm_ptr != nullptr &&
        event_idx < AudioNotifier::kEventCount) {
      std::lock_guard<std::mutex> lock(*g_pcm_mutex_ptr);
      custom_pcm = g_custom_pcm_ptr[event_idx];  // copy (usually empty)
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.channel_mask = 0;
    fs.sample_rate = kSampleRate;
    if (esp_codec_dev_open(g_codec, &fs) != ESP_OK) {
      ESP_LOGW(kTag, "codec_dev_open failed");
      continue;
    }
    esp_codec_dev_set_out_vol(g_codec, 75.0f);

    const int volume = g_volume_ptr != nullptr
                            ? g_volume_ptr->load(std::memory_order_relaxed)
                            : 60;

    // Prime the I2S DMA with a silent pre-roll before the actual audio.
    // The codec hardware needs a few milliseconds of DMA clock before the
    // DAC output is stable. Without this, very short sounds such as Click
    // are fully or partially swallowed because the I2S channel isn't active
    // yet when the first real samples arrive.
    // 320 samples @ 16 kHz = 20 ms — enough for the PA to settle.
    {
      static const int16_t kPreRoll[320] = {};
      esp_codec_dev_write(g_codec, const_cast<int16_t*>(kPreRoll), sizeof(kPreRoll));
    }

    bool did_write = false;

    if (custom_pcm_is_playable(custom_pcm)) {
      // Play the user-supplied PCM blob, scaled by the current volume setting.
      // Apply the same 50 % headroom cap used by render_square_tone so the
      // on-board PA isn't overdriven by a hot WAV file at max volume.
      const float scale = 0.50f * static_cast<float>(std::clamp(volume, 0, 100)) / 100.0f;
      for (int16_t& s : custom_pcm) {
        s = static_cast<int16_t>(static_cast<int32_t>(s) * scale);
      }
      const int bytes = static_cast<int>(custom_pcm.size() * sizeof(int16_t));
      if (bytes > 0) {
        esp_codec_dev_write(g_codec, custom_pcm.data(), bytes);
        did_write = true;
      }
    } else {
      // Fall back to the built-in square-wave melody.
      const Melody melody = melody_for(event);
      for (uint8_t i = 0; i < melody.count; ++i) {
        const Note& note = melody.notes[i];
        render_square_tone(note.frequency_hz, note.duration_ms, volume, render_buffer);
        const int bytes = static_cast<int>(render_buffer.size() * sizeof(int16_t));
        if (bytes > 0) {
          esp_codec_dev_write(g_codec, render_buffer.data(), bytes);
          did_write = true;
        }
      }
    }

    // Post-roll: write silence after the actual audio so the I2S DMA pipeline
    // fully drains before esp_codec_dev_close stops the hardware. Without this,
    // short sounds are physically still in
    // the DMA FIFO when close kills the clock, resulting in silence or severe
    // truncation. 1024 samples @ 16 kHz = 64 ms covers typical DMA buffer
    // depths. This also makes the did_write fallback below redundant (the
    // pre-roll already enabled the channel), but keep it for safety.
    {
      static const int16_t kPostRoll[1024] = {};
      esp_codec_dev_write(g_codec, const_cast<int16_t*>(kPostRoll), sizeof(kPostRoll));
    }

    // Safety guard: if somehow neither pre-roll nor audio was written
    // (shouldn't happen), write a tiny silent frame so the channel is enabled
    // before close, avoiding an E-log from i2s_channel_disable.
    if (!did_write) {
      static const int16_t kSilence[16] = {};
      esp_codec_dev_write(g_codec, const_cast<int16_t*>(kSilence), sizeof(kSilence));
    }

    esp_codec_dev_close(g_codec);
  }
}

}  // namespace

AudioNotifier::AudioNotifier() {
  // Defaults: core print/HMS alerts and Click feedback = on; less common
  // optional notifications default off.
  for (uint8_t i = 0; i < kEventCount; ++i) {
    event_enabled_[i].store(i < 5U || i == static_cast<uint8_t>(Event::kClick),
                            std::memory_order_relaxed);
  }
}

esp_err_t AudioNotifier::initialize() {
  if (initialized_.load()) {
    return ESP_OK;
  }

  g_codec = bsp_audio_codec_speaker_init();
  if (g_codec == nullptr) {
#if defined(PRINTSPHERE_HW_VARIANT_KNOMI_V2) || defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
    ESP_LOGW(kTag, "audio codec unavailable on this board; notifier disabled");
    initialized_.store(true);
    enabled_.store(false);
    return ESP_OK;
#else
    ESP_LOGE(kTag, "bsp_audio_codec_speaker_init failed");
    return ESP_FAIL;
#endif
  }

  g_queue = xQueueCreate(kQueueDepth, sizeof(uint8_t));
  if (g_queue == nullptr) {
    ESP_LOGE(kTag, "queue alloc failed");
    return ESP_ERR_NO_MEM;
  }

  g_volume_ptr = &volume_pct_;
  g_enabled_ptr = &enabled_;
  g_event_enabled_ptr = event_enabled_;
  g_pcm_mutex_ptr = &pcm_mutex_;
  g_custom_pcm_ptr = custom_pcm_;

  // Stack: square-wave rendering is light, but esp_codec_dev_write goes through
  // I2S DMA + codec driver — 4 kB is comfortable, 3 kB cuts close.
  TaskHandle_t handle = nullptr;
  const BaseType_t ok =
      xTaskCreatePinnedToCore(&worker_task, "audio_notif", 4096, nullptr,
                              tskIDLE_PRIORITY + 3, &handle, 0);
  if (ok != pdPASS) {
    ESP_LOGE(kTag, "task create failed");
    return ESP_ERR_NO_MEM;
  }

  initialized_.store(true);
  ESP_LOGI(kTag, "audio notifier ready (rate=%d Hz, vol=%d%%, enabled=%d)",
           kSampleRate, volume_pct_.load(), enabled_.load() ? 1 : 0);
  return ESP_OK;
}

void AudioNotifier::play(Event event) {
  if (!initialized_.load(std::memory_order_relaxed)) {
    return;
  }
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_queue == nullptr) {
    return;
  }
  // Non-blocking — drop the event if the worker is still rendering.
  const uint8_t item = static_cast<uint8_t>(event);  // no force bit
  xQueueSend(g_queue, &item, 0);
}

void AudioNotifier::play_test() {
  // play_test() is invoked from the web portal even when the master or a
  // per-event gate is off. Use the force bit so the worker bypasses ALL gates.
  if (!initialized_.load(std::memory_order_relaxed) || g_queue == nullptr) {
    return;
  }
  const uint8_t item = static_cast<uint8_t>(Event::kPrintStarted) | kQueueForceBit;
  xQueueSend(g_queue, &item, 0);
}

void AudioNotifier::set_enabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
}

void AudioNotifier::set_volume_percent(int volume) {
  volume_pct_.store(std::clamp(volume, 0, 100), std::memory_order_relaxed);
}

void AudioNotifier::play_test_event(Event event) {
  // Force bit bypasses all gates — no save/restore race condition.
  if (!initialized_.load(std::memory_order_relaxed) || g_queue == nullptr) {
    return;
  }
  const uint8_t item = static_cast<uint8_t>(event) | kQueueForceBit;
  xQueueSend(g_queue, &item, 0);
}

void AudioNotifier::set_event_enabled(Event event, bool enabled) {
  const uint8_t idx = static_cast<uint8_t>(event);
  if (idx < kEventCount) {
    event_enabled_[idx].store(enabled, std::memory_order_relaxed);
  }
}

bool AudioNotifier::event_enabled(Event event) const {
  const uint8_t idx = static_cast<uint8_t>(event);
  if (idx < kEventCount) {
    return event_enabled_[idx].load(std::memory_order_relaxed);
  }
  return false;
}

void AudioNotifier::set_event_pcm(Event event, std::vector<int16_t> samples) {
  const uint8_t idx = static_cast<uint8_t>(event);
  if (idx < kEventCount) {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    custom_pcm_[idx] = std::move(samples);
  }
}

void AudioNotifier::clear_event_pcm(Event event) {
  const uint8_t idx = static_cast<uint8_t>(event);
  if (idx < kEventCount) {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    custom_pcm_[idx].clear();
  }
}

bool AudioNotifier::has_event_pcm(Event event) const {
  const uint8_t idx = static_cast<uint8_t>(event);
  if (idx < kEventCount) {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(pcm_mutex_));
    return !custom_pcm_[idx].empty();
  }
  return false;
}

}  // namespace printsphere
