#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
namespace printsphere {
inline bool pcm_upload_is_playable(const std::vector<int16_t>& samples, size_t min_samples, int min_peak) {
  if (samples.size() < min_samples) {
    return false;
  }
  int peak = 0;
  for (const int16_t sample : samples) {
    const int value = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
    if (value > peak) {
      peak = value;
    }
  }
  return peak >= min_peak;
}

// Minimal RIFF/WAV parser. Accepts only mono 16-bit 16 kHz PCM WAV.
// Extracts the raw int16_t samples into `samples_out`.
inline bool parse_wav_pcm(const uint8_t* data, size_t len, std::vector<int16_t>& samples_out,
                   std::string& error_out, size_t min_samples, int min_peak) {
  samples_out.clear();
  error_out.clear();
  if (data == nullptr || len < 44) { error_out = "file too small"; return false; }
  if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
    error_out = "not a WAV file";
    return false;
  }

  uint32_t riff_size = 0;
  std::memcpy(&riff_size, data + 4, 4);
  if (riff_size < 36 || riff_size > len - 8) { error_out = "invalid RIFF length"; return false; }
  len = static_cast<size_t>(riff_size) + 8;
  uint16_t audio_format = 0, channels = 0, bits_per_sample = 0;
  uint32_t sample_rate = 0;
  const uint8_t* pcm_data = nullptr;
  size_t pcm_bytes = 0;

  bool format_seen=false;
  size_t pos = 12;
  while (pos <= len && len - pos >= 8) {
    uint32_t chunk_size = 0;
    std::memcpy(&chunk_size, data + pos + 4, 4);
    const size_t available = len - pos - 8;
    if (chunk_size > available) { error_out = "truncated WAV chunk"; return false; }
    if (std::memcmp(data + pos, "fmt ", 4) == 0) {
      if (format_seen) { error_out = "duplicate WAV format"; return false; }
      format_seen=true;
      if (chunk_size < 16) { error_out = "short WAV format"; return false; }
      std::memcpy(&audio_format, data + pos + 8, 2);
      std::memcpy(&channels, data + pos + 10, 2);
      std::memcpy(&sample_rate, data + pos + 12, 4);
      std::memcpy(&bits_per_sample, data + pos + 22, 2);
    } else if (std::memcmp(data + pos, "data", 4) == 0) {
      if (pcm_data) { error_out = "duplicate WAV data"; return false; }
      pcm_data = data + pos + 8;
      pcm_bytes = chunk_size;
    }
    const size_t padding = chunk_size & 1U;
    pos += 8 + chunk_size;
    if (padding && pos < len) ++pos;

  }

  if (pos != len) { error_out = "truncated WAV header"; return false; }
  if (pcm_data == nullptr) { error_out = "data chunk not found"; return false; }
  if (audio_format != 1)   { error_out = "only PCM WAV supported (format=1)"; return false; }
  if (channels != 1)       { error_out = "only mono WAV supported (channels=1)"; return false; }
  if (sample_rate != 16000) { error_out = "only 16000 Hz WAV supported"; return false; }
  if (bits_per_sample != 16) { error_out = "only 16-bit WAV supported"; return false; }

  if (pcm_bytes % 2) { error_out = "unaligned PCM samples"; return false; }
  const size_t sample_count = pcm_bytes / sizeof(int16_t);
  if (sample_count == 0) { error_out = "empty audio data"; return false; }

  samples_out.resize(sample_count);
  std::memcpy(samples_out.data(), pcm_data, sample_count * sizeof(int16_t));
  if (!pcm_upload_is_playable(samples_out, min_samples, min_peak)) {
    samples_out.clear();
    error_out = "audio too short or silent";
    return false;
  }
  return true;
}

}
