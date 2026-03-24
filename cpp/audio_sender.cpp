#include "aksyn_audio_common.h"

#include <portaudio.h>

#include <atomic>
#include <chrono>
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

struct SenderConfig {
  std::string dest_ip = "127.0.0.1";
  uint16_t dest_port = 9901;
  uint32_t sample_rate = 48000;
  uint16_t channels = 1;
  uint16_t frame_samples = 480; // 10ms @ 48k
  int input_device = -1;        // default

  // Network variation injection (for validation).
  double drop_pct = 0.0;
  int extra_delay_ms = 0; // fixed extra delay per packet
};

struct UdpSocket {
  int fd = -1;
  sockaddr_in dst{};
};

bool udp_init(UdpSocket& s, const SenderConfig& cfg) {
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
  s.fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s.fd < 0) return false;
  std::memset(&s.dst, 0, sizeof(s.dst));
  s.dst.sin_family = AF_INET;
  s.dst.sin_port = htons(cfg.dest_port);
  if (inet_pton(AF_INET, cfg.dest_ip.c_str(), &s.dst.sin_addr) != 1) return false;
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

bool udp_send(UdpSocket& s, const uint8_t* data, size_t len) {
  int sent = sendto(s.fd, (const char*)data, (int)len, 0, (sockaddr*)&s.dst, (int)sizeof(s.dst));
  return sent == (int)len;
}

struct CaptureState {
  SenderConfig cfg;
  UdpSocket udp;

  std::atomic<uint32_t> seq{0};
  uint64_t stream_nonce = 0;
  std::atomic<bool> running{true};

  // For simulation knobs.
  std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> uni{0.0, 100.0};
};

static int pa_input_cb(const void* input, void*, unsigned long frameCount,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* userData) {
  auto* st = (CaptureState*)userData;
  if (!st->running.load(std::memory_order_relaxed)) return paComplete;
  if (!input) return paContinue;

  const auto* in_i16 = (const int16_t*)input;
  size_t samples = (size_t)frameCount * st->cfg.channels;
  size_t payload_bytes = samples * sizeof(int16_t);

  aksyn::AudioPacketHeader h{};
  h.magic = aksyn::kMagicAKSN;
  h.version = aksyn::kProtoVersion;
  h.header_bytes = (uint16_t)sizeof(aksyn::AudioPacketHeader);
  h.seq = st->seq.fetch_add(1, std::memory_order_relaxed);
  h.capture_mono_us = aksyn::mono_time_us();
  h.sample_rate = st->cfg.sample_rate;
  h.channels = st->cfg.channels;
  h.frame_samples = (uint16_t)frameCount;
  h.encoding = (uint8_t)aksyn::AudioEncoding::PCM16LE;
  h.flags = 0;
  h.payload_bytes = (uint16_t)payload_bytes;
  h.stream_nonce = st->stream_nonce;

  // Apply drop / delay simulation (validation requirement: realistic network variation).
  if (st->cfg.drop_pct > 0.0) {
    double r = st->uni(st->rng);
    if (r < st->cfg.drop_pct) return paContinue;
  }

  if (st->cfg.extra_delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(st->cfg.extra_delay_ms));
  }

  auto pkt = aksyn::build_packet(h, (const uint8_t*)in_i16);
  (void)udp_send(st->udp, pkt.data(), pkt.size());
  return paContinue;
}

#if defined(_WIN32)
static CaptureState* g_state = nullptr;
static BOOL WINAPI on_ctrl(DWORD type) {
  if (!g_state) return FALSE;
  if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
    g_state->running.store(false, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}
#else
static CaptureState* g_state = nullptr;
static void on_sigint(int) {
  if (g_state) g_state->running.store(false, std::memory_order_relaxed);
}
#endif

SenderConfig parse_args(int argc, char** argv) {
  SenderConfig c{};
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };

    if (a == "--ip") c.dest_ip = need("--ip");
    else if (a == "--port") c.dest_port = (uint16_t)std::stoi(need("--port"));
    else if (a == "--sr") c.sample_rate = (uint32_t)std::stoul(need("--sr"));
    else if (a == "--ch") c.channels = (uint16_t)std::stoul(need("--ch"));
    else if (a == "--frame") c.frame_samples = (uint16_t)std::stoul(need("--frame"));
    else if (a == "--in") c.input_device = std::stoi(need("--in"));
    else if (a == "--drop") c.drop_pct = std::stod(need("--drop"));
    else if (a == "--delay-ms") c.extra_delay_ms = std::stoi(need("--delay-ms"));
    else if (a == "--help" || a == "-h") {
      std::cout
        << "audio_sender (PortAudio mic -> UDP)\n\n"
        << "Args:\n"
        << "  --ip <dest_ip>         (default 127.0.0.1)\n"
        << "  --port <dest_port>     (default 9901)\n"
        << "  --sr <sample_rate>     (default 48000)\n"
        << "  --ch <channels>        (default 1)\n"
        << "  --frame <samples>      (default 480 = 10ms @ 48k)\n"
        << "  --in <device_index>    (default -1 = system default)\n"
        << "  --drop <pct>           (simulate packet loss, default 0)\n"
        << "  --delay-ms <ms>        (simulate extra network delay, default 0)\n";
      std::exit(0);
    }
  }
  return c;
}

} // namespace

int main(int argc, char** argv) {
  SenderConfig cfg = parse_args(argc, argv);

  std::cout << "AKSYN audio_sender\n";
  std::cout << "  Dest: " << cfg.dest_ip << ":" << cfg.dest_port << "\n";
  std::cout << "  Audio: " << cfg.sample_rate << " Hz, " << cfg.channels << " ch, "
            << cfg.frame_samples << " samples/frame\n";
  std::cout << "  Net sim: drop=" << cfg.drop_pct << "% extra_delay_ms=" << cfg.extra_delay_ms << "\n";

  CaptureState st{};
  st.cfg = cfg;
  st.stream_nonce = aksyn::random_u64();
  g_state = &st;

#if defined(_WIN32)
  SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);
#endif

  if (!udp_init(st.udp, cfg)) {
    std::cerr << "Failed to init UDP socket\n";
    return 1;
  }

  PaError err = Pa_Initialize();
  if (err != paNoError) {
    std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
    udp_close(st.udp);
    return 1;
  }

  PaStreamParameters inParams{};
  inParams.device = (cfg.input_device >= 0) ? cfg.input_device : Pa_GetDefaultInputDevice();
  if (inParams.device == paNoDevice) {
    std::cerr << "No default input device\n";
    Pa_Terminate();
    udp_close(st.udp);
    return 1;
  }
  const PaDeviceInfo* inDev = Pa_GetDeviceInfo(inParams.device);
  inParams.channelCount = cfg.channels;
  inParams.sampleFormat = paInt16;
  inParams.suggestedLatency = inDev ? inDev->defaultLowInputLatency : 0.02;
  inParams.hostApiSpecificStreamInfo = nullptr;

  PaStream* stream = nullptr;
  err = Pa_OpenStream(
    &stream,
    &inParams,
    nullptr,
    (double)cfg.sample_rate,
    cfg.frame_samples,
    paNoFlag,
    &pa_input_cb,
    &st
  );
  if (err != paNoError) {
    std::cerr << "PortAudio open failed: " << Pa_GetErrorText(err) << "\n";
    Pa_Terminate();
    udp_close(st.udp);
    return 1;
  }

  err = Pa_StartStream(stream);
  if (err != paNoError) {
    std::cerr << "PortAudio start failed: " << Pa_GetErrorText(err) << "\n";
    Pa_CloseStream(stream);
    Pa_Terminate();
    udp_close(st.udp);
    return 1;
  }

  std::cout << "  stream_nonce: " << st.stream_nonce << "\n";
  std::cout << "Streaming... press Ctrl+C to stop.\n";

  while (st.running.load(std::memory_order_relaxed) && Pa_IsStreamActive(stream) == 1) {
    Pa_Sleep(250);
  }

  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  udp_close(st.udp);
  return 0;
}

