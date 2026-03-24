#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

// ── Encoding types ────────────────────────────────────────────
enum AudioEncoding : uint8_t {
    PCM_16 = 0,
    OPUS   = 1
};

// ── Packet header — matches Python struct "!IQHBBh" exactly ──
// Total header size: 4 + 8 + 2 + 1 + 1 + 2 = 18 bytes
#pragma pack(push, 1)
struct AudioPacketHeader {
    uint32_t seq;            // sequence number — detect drops/reorder
    uint64_t timestamp_us;   // capture time in microseconds since epoch
    uint16_t sample_rate;    // e.g. 44100
    uint8_t  channels;       // 1 = mono, 2 = stereo
    uint8_t  encoding;       // AudioEncoding enum
    uint16_t payload_len;    // number of audio bytes following this header
};
#pragma pack(pop)

// ── Pack a packet (header + audio payload) into a byte buffer ─
// This is what PortAudio's callback would call in production
std::vector<uint8_t> pack_packet(
    uint32_t        seq,
    const uint8_t*  audio_data,
    uint16_t        audio_len,
    uint16_t        sample_rate = 44100,
    uint8_t         channels    = 1,
    AudioEncoding   encoding    = PCM_16
) {
    // Get current time in microseconds
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL
                          + (uint64_t)ts.tv_nsec / 1000ULL;

    AudioPacketHeader header;
    header.seq         = seq;
    header.timestamp_us = timestamp_us;
    header.sample_rate = sample_rate;
    header.channels    = channels;
    header.encoding    = encoding;
    header.payload_len = audio_len;

    // Allocate buffer: header + payload
    std::vector<uint8_t> buffer(sizeof(AudioPacketHeader) + audio_len);

    // Copy header then payload into buffer
    std::memcpy(buffer.data(), &header, sizeof(AudioPacketHeader));
    std::memcpy(buffer.data() + sizeof(AudioPacketHeader),
                audio_data, audio_len);

    return buffer;
}

// ── Unpack a received packet ──────────────────────────────────
bool unpack_packet(
    const uint8_t*       raw,
    size_t               raw_len,
    AudioPacketHeader&   header_out,
    const uint8_t*&      payload_out
) {
    if (raw_len < sizeof(AudioPacketHeader)) return false;

    std::memcpy(&header_out, raw, sizeof(AudioPacketHeader));
    payload_out = raw + sizeof(AudioPacketHeader);
    return true;
}

// ── JitterBuffer — fixed depth ring buffer ────────────────────
// Holds N frames before playback starts, absorbs network variance
class JitterBuffer {
public:
    explicit JitterBuffer(size_t depth) : depth_(depth) {}

    void push(std::vector<uint8_t> frame) {
        buffer_.push_back(std::move(frame));
    }

    bool ready() const {
        return buffer_.size() >= depth_;
    }

    std::vector<uint8_t> pop() {
        auto frame = buffer_.front();
        buffer_.erase(buffer_.begin());
        return frame;
    }

    size_t size() const { return buffer_.size(); }

private:
    size_t                            depth_;
    std::vector<std::vector<uint8_t>> buffer_;
};

// ── DelayMonitor — computes mean + jitter ─────────────────────
class DelayMonitor {
public:
    void record(uint64_t capture_us) {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        uint64_t recv_us = (uint64_t)ts.tv_sec * 1000000ULL
                         + (uint64_t)ts.tv_nsec / 1000ULL;

        double delay_ms = (double)(recv_us - capture_us) / 1000.0;
        delays_.push_back(delay_ms);
    }

    double mean_ms() const {
        if (delays_.empty()) return 0.0;
        double sum = 0;
        for (double d : delays_) sum += d;
        return sum / delays_.size();
    }

    double jitter_ms() const {
        if (delays_.size() < 2) return 0.0;
        double mean = mean_ms();
        double sq_sum = 0;
        for (double d : delays_) sq_sum += (d - mean) * (d - mean);
        return std::sqrt(sq_sum / delays_.size());
    }

private:
    std::vector<double> delays_;
};