#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aksyn {

// NOTE: This prototype focuses on PCM16LE frames transported over UDP.
enum class AudioEncoding : uint8_t {
  PCM16LE = 0,
};

#pragma pack(push, 1)
struct AudioPacketHeader {
  uint32_t magic;          // 'AKSN' little-endian for basic sanity checking
  uint16_t version;        // protocol version
  uint16_t header_bytes;   // sizeof(AudioPacketHeader)

  uint32_t seq;            // sequence number
  uint64_t capture_mono_us; // sender monotonic timestamp at capture (us)

  uint32_t sample_rate;    // e.g. 48000
  uint16_t channels;       // 1 or 2
  uint16_t frame_samples;  // samples per channel in this packet

  uint8_t encoding;        // AudioEncoding
  uint8_t flags;           // reserved
  uint16_t payload_bytes;  // number of audio bytes after header

  uint64_t stream_nonce;   // random per-run value (proof-of-liveness / run identity)
};
#pragma pack(pop)

static constexpr uint32_t kMagicAKSN = 0x4E534B41u; // 'AKSN' as little-endian int
static constexpr uint16_t kProtoVersion = 1;

uint64_t mono_time_us();
uint64_t random_u64();

// Serialize/parse (little-endian on wire for simplicity in a C++ prototype).
std::vector<uint8_t> build_packet(const AudioPacketHeader& h, const uint8_t* payload);
bool parse_packet(const uint8_t* data, size_t len, AudioPacketHeader& out_h, const uint8_t*& out_payload);

struct DelayStats {
  double mean_ms = 0.0;
  double stddev_ms = 0.0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double min_ms = 0.0;
  double max_ms = 0.0;
  size_t n = 0;
};

class RunningStats {
public:
  void add(double x_ms);
  DelayStats snapshot() const;
private:
  mutable std::mutex mu_;
  std::vector<double> samples_;
};

// Simple bounded jitter buffer for PCM frames.
// - push() accepts packets that may arrive with loss/reorder; it stores by seq.
// - pop() returns the next expected seq frame or a PLC frame when missing.
class JitterBuffer {
public:
  struct Frame {
    uint32_t seq = 0;
    uint64_t capture_mono_us = 0;
    std::vector<int16_t> pcm; // interleaved
    bool is_plc = false;
  };

  JitterBuffer(size_t prebuffer_frames, size_t max_frames);
  JitterBuffer(JitterBuffer&&) noexcept = default;
  JitterBuffer& operator=(JitterBuffer&&) noexcept = default;
  JitterBuffer(const JitterBuffer&) = delete;
  JitterBuffer& operator=(const JitterBuffer&) = delete;

  void reset(uint32_t next_seq);
  void push(uint32_t seq, uint64_t capture_mono_us, std::vector<int16_t> pcm);

  bool ready() const;
  size_t size() const;
  uint32_t expected_seq() const;

  // Returns a frame sized to frame_samples*channels.
  Frame pop_or_plc(uint16_t frame_samples, uint16_t channels);

private:
  mutable std::mutex mu_;
  size_t prebuffer_;
  size_t max_;
  bool started_ = false;
  uint32_t expected_ = 0;

  // For a prototype: map seq->pcm using deque of pairs (bounded).
  struct Stored {
    uint32_t seq;
    uint64_t capture_mono_us;
    std::vector<int16_t> pcm;
  };
  std::deque<Stored> q_;
  std::optional<std::vector<int16_t>> last_good_;
};

class WavWriter {
public:
  ~WavWriter();

  bool open(const std::string& path, uint32_t sample_rate, uint16_t channels);
  void write_pcm16(const int16_t* samples, size_t sample_count); // sample_count is interleaved count
  void close();

private:
  void write_header_placeholder();
  void patch_header_sizes();

  std::FILE* f_ = nullptr;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;
  uint32_t data_bytes_ = 0;
};

} // namespace aksyn

