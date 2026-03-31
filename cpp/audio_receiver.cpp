#include "aksyn_audio_common.h"

#include <portaudio.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <signal.h>
#endif

namespace {

struct ReceiverConfig {
  std::string bind_ip = "0.0.0.0";
  uint16_t bind_port = 9901;

  // Jitter buffer tuning (trade-off: glitch resistance vs delay).
  size_t prebuffer_frames = 3; // start playback after N frames (30ms @ 10ms frames)
  size_t max_buffer_frames = 200;

  std::string wav_out = "recordings/receiver_recording.wav";
  std::string csv_out = "results/delay_log_cpp.csv";
};

struct UdpSocket {
  int fd = -1;
};

bool udp_bind(UdpSocket& s, const ReceiverConfig& cfg) {
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
  s.fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s.fd < 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg.bind_port);
  if (inet_pton(AF_INET, cfg.bind_ip.c_str(), &addr.sin_addr) != 1) return false;

  int yes = 1;
  setsockopt(s.fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

  if (bind(s.fd, (sockaddr*)&addr, (int)sizeof(addr)) != 0) return false;
  return true;
}

void udp_close(UdpSocket& s) {
  if (s.fd >= 0) {
#if defined(_WIN32)
    closesocket((SOCKET)s.fd);
    WSACleanup();
#else
    close(s.fd);
#endif
    s.fd = -1;
  }
}

int udp_recv(UdpSocket& s, uint8_t* buf, int cap) {
  return recvfrom(s.fd, (char*)buf, cap, 0, nullptr, nullptr);
}

ReceiverConfig parse_args(int argc, char** argv) {
  ReceiverConfig c{};
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--ip") c.bind_ip = need("--ip");
    else if (a == "--port") c.bind_port = (uint16_t)std::stoi(need("--port"));
    else if (a == "--prebuffer") c.prebuffer_frames = (size_t)std::stoull(need("--prebuffer"));
    else if (a == "--maxbuf") c.max_buffer_frames = (size_t)std::stoull(need("--maxbuf"));
    else if (a == "--wav") c.wav_out = need("--wav");
    else if (a == "--csv") c.csv_out = need("--csv");
    else if (a == "--help" || a == "-h") {
      std::cout
        << "audio_receiver (UDP -> jitter buffer -> PortAudio playback + WAV)\n\n"
        << "Args:\n"
        << "  --ip <bind_ip>         (default 0.0.0.0)\n"
        << "  --port <bind_port>     (default 9901)\n"
        << "  --prebuffer <frames>   (default 3)\n"
        << "  --maxbuf <frames>      (default 200)\n"
        << "  --wav <path>           (default recordings/receiver_recording.wav)\n"
        << "  --csv <path>           (default results/delay_log_cpp.csv)\n";
      std::exit(0);
    }
  }
  return c;
}

struct SharedState {
  ReceiverConfig cfg;
  std::atomic<bool> running{true};

  std::atomic<uint32_t> last_seq{0};
  std::atomic<uint64_t> stream_nonce{0};

  // Negotiated/observed format (first packet sets the working format).
  std::atomic<uint32_t> sample_rate{0};
  std::atomic<uint16_t> channels{0};
  std::atomic<uint16_t> frame_samples{0};

  aksyn::JitterBuffer jbuf;

  SharedState(const ReceiverConfig& c) : cfg(c), jbuf(c.prebuffer_frames, c.max_buffer_frames) {}
  aksyn::RunningStats delay_net_ms; // receive_time - capture_time
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> plc_frames{0};

  std::mutex wav_mu;
  aksyn::WavWriter wav;

  std::mutex csv_mu;
  std::ofstream csv;
};

static int pa_output_cb(const void*, void* output, unsigned long frameCount,
                        const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* userData) {
  auto* st = (SharedState*)userData;
  auto* out = (int16_t*)output;

  uint16_t ch = st->channels.load(std::memory_order_relaxed);
  if (ch == 0) ch = 1;

  // Drain from jitter buffer; PLC if missing.
  auto f = st->jbuf.pop_or_plc((uint16_t)frameCount, ch);
  if (f.is_plc) st->plc_frames.fetch_add(1, std::memory_order_relaxed);

  size_t need = (size_t)frameCount * ch;
  if (f.pcm.size() != need) f.pcm.resize(need, 0);

  std::memcpy(out, f.pcm.data(), need * sizeof(int16_t));

  // Save to WAV (best-effort; mutex in callback isn't ideal, but acceptable for a prototype).
  {
    std::lock_guard<std::mutex> lk(st->wav_mu);
    st->wav.write_pcm16(out, need);
  }

  return paContinue;
}

#if defined(_WIN32)
static SharedState* g_state = nullptr;
static BOOL WINAPI on_ctrl(DWORD type) {
  if (!g_state) return FALSE;
  if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
    g_state->running.store(false, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}
#else
static SharedState* g_state = nullptr;
static void on_sigint(int) {
  if (g_state) g_state->running.store(false, std::memory_order_relaxed);
}
#endif

} // namespace

int main(int argc, char** argv) {
  ReceiverConfig cfg = parse_args(argc, argv);

  std::cout << "AKSYN audio_receiver\n";
  std::cout << "  Bind: " << cfg.bind_ip << ":" << cfg.bind_port << "\n";
  std::cout << "  Jitter buffer: prebuffer=" << cfg.prebuffer_frames
            << " max=" << cfg.max_buffer_frames << "\n";

  // Ensure output directories exist (cheap, cross-platform best-effort).
  std::system("mkdir recordings 2>nul");
  std::system("mkdir results 2>nul");

  UdpSocket udp{};
  if (!udp_bind(udp, cfg)) {
    std::cerr << "Failed to bind UDP socket\n";
    return 1;
  }

  SharedState st{cfg};
  g_state = &st;

#if defined(_WIN32)
  SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);
#endif

  st.csv.open(cfg.csv_out, std::ios::out | std::ios::trunc);
  st.csv << "seq,capture_mono_us,recv_mono_us,net_delay_ms,plc\n";

  PaError err = Pa_Initialize();
  if (err != paNoError) {
    std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
    udp_close(udp);
    return 1;
  }

  PaStreamParameters outParams{};
  outParams.device = Pa_GetDefaultOutputDevice();
  if (outParams.device == paNoDevice) {
    std::cerr << "No default output device\n";
    Pa_Terminate();
    udp_close(udp);
    return 1;
  }
  const PaDeviceInfo* outDev = Pa_GetDeviceInfo(outParams.device);
  outParams.channelCount = 1; // will update once format known; start mono.
  outParams.sampleFormat = paInt16;
  outParams.suggestedLatency = outDev ? outDev->defaultLowOutputLatency : 0.02;
  outParams.hostApiSpecificStreamInfo = nullptr;

  // Start network thread first: it will set sample rate/ch/frame size once first packet arrives.
  std::thread net([&]() {
    std::vector<uint8_t> buf(2048);
    bool format_set = false;
    uint32_t first_seq = 0;

    while (st.running.load(std::memory_order_relaxed)) {
      int n = udp_recv(udp, buf.data(), (int)buf.size());
      if (n <= 0) continue;

      aksyn::AudioPacketHeader h{};
      const uint8_t* payload = nullptr;
      if (!aksyn::parse_packet(buf.data(), (size_t)n, h, payload)) continue;
      if (h.encoding != (uint8_t)aksyn::AudioEncoding::PCM16LE) continue;

      uint64_t recv_us = aksyn::mono_time_us();
      double net_delay_ms = (double)(recv_us - h.capture_mono_us) / 1000.0;
      st.delay_net_ms.add(net_delay_ms);

      st.packets.fetch_add(1, std::memory_order_relaxed);
      st.last_seq.store(h.seq, std::memory_order_relaxed);
      st.stream_nonce.store(h.stream_nonce, std::memory_order_relaxed);

      if (!format_set) {
        format_set = true;
        first_seq = h.seq;
        st.sample_rate.store(h.sample_rate, std::memory_order_relaxed);
        st.channels.store(h.channels, std::memory_order_relaxed);
        st.frame_samples.store(h.frame_samples, std::memory_order_relaxed);
        st.jbuf.reset(first_seq);

        // Open WAV once format known.
        {
          std::lock_guard<std::mutex> lk(st.wav_mu);
          if (!st.wav.open(cfg.wav_out, h.sample_rate, h.channels)) {
            std::cerr << "Failed to open WAV: " << cfg.wav_out << "\n";
          }
        }

        std::cout << "  Detected stream:\n";
        std::cout << "    sample_rate=" << h.sample_rate << " channels=" << h.channels
                  << " frame_samples=" << h.frame_samples << "\n";
        std::cout << "    stream_nonce=" << h.stream_nonce << "\n";
      }

      // Payload -> PCM16
      size_t sample_count = h.payload_bytes / sizeof(int16_t);
      std::vector<int16_t> pcm(sample_count);
      std::memcpy(pcm.data(), payload, h.payload_bytes);
      st.jbuf.push(h.seq, h.capture_mono_us, std::move(pcm));

      {
        std::lock_guard<std::mutex> lk(st.csv_mu);
        st.csv << h.seq << "," << h.capture_mono_us << "," << recv_us << "," << net_delay_ms << ",0\n";
      }
    }
  });

  // Wait for format.
  std::cout << "Waiting for first packet...\n";
  while (st.running.load(std::memory_order_relaxed) && st.sample_rate.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  uint32_t sr = st.sample_rate.load();
  uint16_t ch = st.channels.load();
  uint16_t frame = st.frame_samples.load();
  if (sr == 0 || ch == 0 || frame == 0) {
    st.running.store(false);
    net.join();
    Pa_Terminate();
    udp_close(udp);
    return 1;
  }

  outParams.channelCount = ch;
  PaStream* stream = nullptr;
  err = Pa_OpenStream(
    &stream,
    nullptr,
    &outParams,
    (double)sr,
    frame,
    paNoFlag,
    &pa_output_cb,
    &st
  );
  if (err != paNoError) {
    std::cerr << "PortAudio open failed: " << Pa_GetErrorText(err) << "\n";
    st.running.store(false);
    net.join();
    Pa_Terminate();
    udp_close(udp);
    return 1;
  }

  err = Pa_StartStream(stream);
  if (err != paNoError) {
    std::cerr << "PortAudio start failed: " << Pa_GetErrorText(err) << "\n";
    Pa_CloseStream(stream);
    st.running.store(false);
    net.join();
    Pa_Terminate();
    udp_close(udp);
    return 1;
  }

  std::cout << "Receiving... press Ctrl+C to stop.\n";

  // Print live stats.
  while (st.running.load(std::memory_order_relaxed)) {
    Pa_Sleep(500);
    auto s = st.delay_net_ms.snapshot();
    if (s.n == 0) continue;
    std::cout << "  packets=" << s.n
              << " net_delay_ms(mean/p95/std)=" << s.mean_ms << "/" << s.p95_ms << "/" << s.stddev_ms
              << " jbuf=" << st.jbuf.size()
              << " plc=" << st.plc_frames.load()
              << "\n";
  }

  std::cout << "Stopping...\n";

  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  st.running.store(false);
  net.join();

  {
    std::lock_guard<std::mutex> lk(st.wav_mu);
    st.wav.close();
  }
  {
    std::lock_guard<std::mutex> lk(st.csv_mu);
    st.csv.close();
  }

  Pa_Terminate();
  udp_close(udp);
  return 0;
}

