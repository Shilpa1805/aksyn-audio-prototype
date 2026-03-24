#include "aksyn_audio_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace aksyn {

uint64_t mono_time_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t random_u64() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  return rng();
}

std::vector<uint8_t> build_packet(const AudioPacketHeader& h, const uint8_t* payload) {
  std::vector<uint8_t> out(sizeof(AudioPacketHeader) + h.payload_bytes);
  std::memcpy(out.data(), &h, sizeof(AudioPacketHeader));
  if (h.payload_bytes && payload) {
    std::memcpy(out.data() + sizeof(AudioPacketHeader), payload, h.payload_bytes);
  }
  return out;
}

bool parse_packet(const uint8_t* data, size_t len, AudioPacketHeader& out_h, const uint8_t*& out_payload) {
  if (!data || len < sizeof(AudioPacketHeader)) return false;
  std::memcpy(&out_h, data, sizeof(AudioPacketHeader));
  if (out_h.magic != kMagicAKSN) return false;
  if (out_h.version != kProtoVersion) return false;
  if (out_h.header_bytes != sizeof(AudioPacketHeader)) return false;
  if (sizeof(AudioPacketHeader) + out_h.payload_bytes > len) return false;
  out_payload = data + sizeof(AudioPacketHeader);
  return true;
}

void RunningStats::add(double x_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  samples_.push_back(x_ms);
}

DelayStats RunningStats::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  DelayStats s{};
  s.n = samples_.size();
  if (samples_.empty()) return s;

  std::vector<double> v = samples_;
  std::sort(v.begin(), v.end());

  auto percentile = [&](double p) -> double {
    if (v.empty()) return 0.0;
    double idx = p * (v.size() - 1);
    size_t i = static_cast<size_t>(idx);
    size_t j = std::min(i + 1, v.size() - 1);
    double t = idx - i;
    return v[i] * (1.0 - t) + v[j] * t;
  };

  s.min_ms = v.front();
  s.max_ms = v.back();
  s.p50_ms = percentile(0.50);
  s.p95_ms = percentile(0.95);

  double sum = 0.0;
  for (double x : v) sum += x;
  s.mean_ms = sum / v.size();

  double sq = 0.0;
  for (double x : v) sq += (x - s.mean_ms) * (x - s.mean_ms);
  s.stddev_ms = std::sqrt(sq / v.size());
  return s;
}

JitterBuffer::JitterBuffer(size_t prebuffer_frames, size_t max_frames)
  : prebuffer_(prebuffer_frames), max_(std::max(prebuffer_frames, max_frames)) {}

void JitterBuffer::reset(uint32_t next_seq) {
  std::lock_guard<std::mutex> lk(mu_);
  started_ = false;
  expected_ = next_seq;
  q_.clear();
  last_good_.reset();
}

void JitterBuffer::push(uint32_t seq, uint64_t capture_mono_us, std::vector<int16_t> pcm) {
  std::lock_guard<std::mutex> lk(mu_);

  // Drop very old packets.
  if (started_ && (int32_t)(seq - expected_) < -64) return;

  // Enforce bounded memory.
  if (q_.size() >= max_) {
    q_.pop_front();
  }

  // Keep queue roughly ordered by seq (small N).
  auto it = std::lower_bound(q_.begin(), q_.end(), seq,
    [](const Stored& a, uint32_t s) { return a.seq < s; });
  if (it != q_.end() && it->seq == seq) return; // duplicate
  q_.insert(it, Stored{seq, capture_mono_us, std::move(pcm)});
}

bool JitterBuffer::ready() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (started_) return true;
  return q_.size() >= prebuffer_;
}

size_t JitterBuffer::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return q_.size();
}

uint32_t JitterBuffer::expected_seq() const {
  std::lock_guard<std::mutex> lk(mu_);
  return expected_;
}

JitterBuffer::Frame JitterBuffer::pop_or_plc(uint16_t frame_samples, uint16_t channels) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!started_) {
    if (q_.size() >= prebuffer_) started_ = true;
    else {
      Frame f{};
      f.seq = expected_;
      f.is_plc = true;
      f.pcm.assign(static_cast<size_t>(frame_samples) * channels, 0);
      expected_++;
      return f;
    }
  }

  // Find expected_ in queue.
  for (auto it = q_.begin(); it != q_.end(); ++it) {
    if (it->seq == expected_) {
      Frame f{};
      f.seq = it->seq;
      f.capture_mono_us = it->capture_mono_us;
      f.pcm = std::move(it->pcm);
      f.is_plc = false;
      last_good_ = f.pcm;
      q_.erase(it);
      expected_++;
      return f;
    }
    if ((int32_t)(it->seq - expected_) > 0) break;
  }

  // Missing: PLC (repeat last frame if available, else silence).
  Frame f{};
  f.seq = expected_;
  f.is_plc = true;
  if (last_good_ && last_good_->size() == static_cast<size_t>(frame_samples) * channels) {
    f.pcm = *last_good_;
  } else {
    f.pcm.assign(static_cast<size_t>(frame_samples) * channels, 0);
  }
  expected_++;
  return f;
}

WavWriter::~WavWriter() {
  close();
}

bool WavWriter::open(const std::string& path, uint32_t sample_rate, uint16_t channels) {
  close();
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return false;
  sample_rate_ = sample_rate;
  channels_ = channels;
  data_bytes_ = 0;
  write_header_placeholder();
  return true;
}

void WavWriter::write_pcm16(const int16_t* samples, size_t sample_count) {
  if (!f_ || !samples || sample_count == 0) return;
  std::fwrite(samples, sizeof(int16_t), sample_count, f_);
  data_bytes_ += static_cast<uint32_t>(sample_count * sizeof(int16_t));
}

void WavWriter::close() {
  if (!f_) return;
  patch_header_sizes();
  std::fclose(f_);
  f_ = nullptr;
}

void WavWriter::write_header_placeholder() {
  // Minimal RIFF/WAVE PCM header (44 bytes).
  // We'll patch sizes on close.
  uint32_t riff = 0x46464952u; // 'RIFF'
  uint32_t wave = 0x45564157u; // 'WAVE'
  uint32_t fmt_ = 0x20746D66u; // 'fmt '
  uint32_t data = 0x61746164u; // 'data'

  uint32_t chunk_size = 36; // placeholder: 36 + data_bytes
  uint32_t subchunk1_size = 16;
  uint16_t audio_format = 1; // PCM
  uint16_t num_channels = channels_;
  uint32_t sample_rate = sample_rate_;
  uint16_t bits_per_sample = 16;
  uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
  uint16_t block_align = num_channels * (bits_per_sample / 8);
  uint32_t data_bytes = 0;

  std::fwrite(&riff, 4, 1, f_);
  std::fwrite(&chunk_size, 4, 1, f_);
  std::fwrite(&wave, 4, 1, f_);
  std::fwrite(&fmt_, 4, 1, f_);
  std::fwrite(&subchunk1_size, 4, 1, f_);
  std::fwrite(&audio_format, 2, 1, f_);
  std::fwrite(&num_channels, 2, 1, f_);
  std::fwrite(&sample_rate, 4, 1, f_);
  std::fwrite(&byte_rate, 4, 1, f_);
  std::fwrite(&block_align, 2, 1, f_);
  std::fwrite(&bits_per_sample, 2, 1, f_);
  std::fwrite(&data, 4, 1, f_);
  std::fwrite(&data_bytes, 4, 1, f_);
}

void WavWriter::patch_header_sizes() {
  if (!f_) return;
  // chunk_size at offset 4, data_bytes at offset 40.
  uint32_t chunk_size = 36 + data_bytes_;
  std::fseek(f_, 4, SEEK_SET);
  std::fwrite(&chunk_size, 4, 1, f_);
  std::fseek(f_, 40, SEEK_SET);
  std::fwrite(&data_bytes_, 4, 1, f_);
  std::fseek(f_, 0, SEEK_END);
}

} // namespace aksyn

