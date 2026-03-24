#include <iostream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <ctime>
#include <cmath>

#pragma pack(push, 1)
struct AudioPacketHeader {
    uint32_t seq;
    uint64_t timestamp_us;
    uint16_t sample_rate;
    uint8_t  channels;
    uint8_t  encoding;
    uint16_t payload_len;
};
#pragma pack(pop)

uint64_t now_us() {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000ULL
         + (uint64_t)ts.tv_nsec / 1000ULL;
}

std::vector<uint8_t> pack_packet(
    uint32_t       seq,
    const uint8_t* audio_data,
    uint16_t       audio_len,
    uint16_t       sample_rate = 44100,
    uint8_t        channels    = 1,
    uint8_t        encoding    = 0
) {
    AudioPacketHeader header;
    header.seq          = seq;
    header.timestamp_us = now_us();
    header.sample_rate  = sample_rate;
    header.channels     = channels;
    header.encoding     = encoding;
    header.payload_len  = audio_len;

    std::vector<uint8_t> buffer(sizeof(AudioPacketHeader) + audio_len);
    std::memcpy(buffer.data(), &header, sizeof(AudioPacketHeader));
    std::memcpy(buffer.data() + sizeof(AudioPacketHeader),
                audio_data, audio_len);
    return buffer;
}

class JitterBuffer {
public:
    explicit JitterBuffer(size_t depth) : depth_(depth) {}

    void push(std::vector<uint8_t> frame) {
        buffer_.push_back(std::move(frame));
    }
    bool ready()  const { return buffer_.size() >= depth_; }
    size_t size() const { return buffer_.size(); }

    std::vector<uint8_t> pop() {
        auto f = buffer_.front();
        buffer_.erase(buffer_.begin());
        return f;
    }

private:
    size_t depth_;
    std::vector<std::vector<uint8_t>> buffer_;
};

class DelayMonitor {
public:
    void record(uint64_t capture_us) {
        double delay_ms = (double)(now_us() - capture_us) / 1000.0;
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
        double mean = mean_ms(), sq = 0;
        for (double d : delays_) sq += (d - mean) * (d - mean);
        return std::sqrt(sq / delays_.size());
    }

private:
    std::vector<double> delays_;
};

int main() {
    std::cout << "AKSYN Audio Packet Engine - C++ Demo\n";
    std::cout << "Header size: " << sizeof(AudioPacketHeader) << " bytes\n\n";

    JitterBuffer  jbuf(2);
    DelayMonitor  monitor;

    for (uint32_t seq = 0; seq < 10; seq++) {
        std::vector<uint8_t> fake_audio(1024 * 2, 0);
        auto packet = pack_packet(seq,
                                  fake_audio.data(),
                                  (uint16_t)fake_audio.size());

        AudioPacketHeader rx_header;
        std::memcpy(&rx_header, packet.data(), sizeof(AudioPacketHeader));

        monitor.record(rx_header.timestamp_us);

        std::vector<uint8_t> payload(
            packet.begin() + sizeof(AudioPacketHeader),
            packet.end()
        );
        jbuf.push(payload);

        std::cout << "  [SEQ " << seq << "]"
                  << "  packet_size: " << packet.size() << "B"
                  << "  jbuf_depth: "  << jbuf.size()
                  << "  ready: " << (jbuf.ready() ? "YES" : "NO")
                  << "\n";
    }

    std::cout << "\n--- Results ---\n";
    std::cout << "Mean delay : " << monitor.mean_ms()   << " ms\n";
    std::cout << "Jitter     : " << monitor.jitter_ms() << " ms\n";
    std::cout << "Jitter buf : " << jbuf.size() << " frames ready\n";
    return 0;
}