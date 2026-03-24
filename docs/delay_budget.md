## Delay Budget — Pre-Implementation Estimate

| Component | Estimate | Basis |
|---|---|---|
| Audio frame duration | 10ms | 480 samples @ 48kHz |
| PortAudio input callback latency | ~5ms | defaultLowInputLatency |
| UDP send (loopback) | ~0.1ms | loopback kernel bypass |
| UDP recv (loopback) | ~0.1ms | same |
| Jitter buffer pre-fill (3 frames) | ~30ms | 3 × 10ms |
| PortAudio output callback latency | ~5ms | defaultLowOutputLatency |
| OS scheduler wake latency | ~2ms | typical Windows |
| **Total expected** | **~52ms** | |

Threshold defined as 150ms (ITU-T echo perception boundary).