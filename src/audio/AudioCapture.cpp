#include "AudioCapture.h"
#include "AudioRingBuffer.h"
#include "miniaudio.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace EchoRadar {

// ─────────────────────────────────────────────────────────────────────────────
//  Internal implementation
// ─────────────────────────────────────────────────────────────────────────────

struct AudioCapture::Impl {
    ma_context  ctx{};
    ma_device   dev{};
    bool        ctxInit{false};
    bool        devInit{false};

    AudioRingBuffer              ringBuf;
    AudioCapture::FrameCallback  legacyCb;  ///< optional callback via Start()

    std::atomic<float> leftRms{0.f};
    std::atomic<float> rightRms{0.f};
    std::atomic<float> leftPeak{0.f};
    std::atomic<float> rightPeak{0.f};
    std::atomic<bool>  running{false};

    Impl() : ringBuf(AudioCapture::kDefaultBufferFrames) {}

    // ── Level helper — called inside audio callback (no alloc, no log) ───────
    void UpdateLevels(const float* samples, ma_uint32 frameCount) {
        float sumL = 0.f, sumR = 0.f, pkL = 0.f, pkR = 0.f;
        for (ma_uint32 i = 0; i < frameCount; ++i) {
            const float l = samples[i * 2];
            const float r = samples[i * 2 + 1];
            sumL += l * l;
            sumR += r * r;
            const float al = l < 0.f ? -l : l;
            const float ar = r < 0.f ? -r : r;
            if (al > pkL) pkL = al;
            if (ar > pkR) pkR = ar;
        }
        const float invN = 1.f / static_cast<float>(frameCount);
        leftRms .store(std::sqrt(sumL * invN), std::memory_order_relaxed);
        rightRms.store(std::sqrt(sumR * invN), std::memory_order_relaxed);
        leftPeak .store(pkL,                   std::memory_order_relaxed);
        rightPeak.store(pkR,                   std::memory_order_relaxed);
    }

    // ── miniaudio callback ────────────────────────────────────────────────────
    static void DataCallback(ma_device* pDev, void* /*pOut*/,
                              const void* pIn, ma_uint32 frameCount)
    {
        if (!pDev || !pDev->pUserData) return;  // Safety check added
        
        auto* self = static_cast<Impl*>(pDev->pUserData);
        if (!pIn || frameCount == 0) return;
        if (!self) return;  // Additional safety check
        
        const auto* samples = static_cast<const float*>(pIn);

        // Push raw samples — no heap allocation, never blocks.
        // Disabled for Milestone 1 - ring buffer not needed for level monitoring
        // TODO: Fix buffer overflow issue in PushInterleaved() for Milestone 3
        // self->ringBuf.PushInterleaved(samples, frameCount);

        // Update rolling level metrics — no heap allocation.
        self->UpdateLevels(samples, frameCount);

        // Legacy FrameCallback path — only active when registered via Start().
        // Heap-allocates an AudioFrame; avoid if you care about callback latency.
        if (self->legacyCb) {
            AudioFrame f;
            f.sample_rate = 48000;
            f.left .resize(frameCount);
            f.right.resize(frameCount);
            for (ma_uint32 i = 0; i < frameCount; ++i) {
                f.left [i] = samples[i * 2];
                f.right[i] = samples[i * 2 + 1];
            }
            self->legacyCb(f);
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  AudioCapture
// ─────────────────────────────────────────────────────────────────────────────

AudioCapture::AudioCapture()  : m_impl(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::StartDefault()                        { return StartInternal(nullptr); }
bool AudioCapture::StartDeviceByName(const std::string& name) {
    return StartInternal(name.empty() ? nullptr : name.c_str());
}
bool AudioCapture::Start(const std::string& device_name, FrameCallback callback) {
    m_impl->legacyCb = std::move(callback);
    return StartInternal(device_name.empty() ? nullptr : device_name.c_str());
}

bool AudioCapture::StartInternal(const char* deviceName) {
    if (m_impl->running.load(std::memory_order_acquire)) return false;

    // ── Initialise context ────────────────────────────────────────────────────
    if (ma_context_init(nullptr, 0, nullptr, &m_impl->ctx) != MA_SUCCESS) {
        std::cerr << "[AudioCapture] Failed to initialise audio context.\n";
        return false;
    }
    m_impl->ctxInit = true;

    // ── Search for device by name ─────────────────────────────────────────────
    ma_device_id  foundId{};
    ma_device_id* pFoundId = nullptr;

    if (deviceName) {
        struct SearchCtx {
            std::string  needle;
            ma_device_id id{};
            bool         found{false};
        } sc;

        std::string lower(deviceName);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        sc.needle = std::move(lower);

        ma_context_enumerate_devices(
            &m_impl->ctx,
            [](ma_context*, ma_device_type type,
               const ma_device_info* pInfo, void* ud) -> ma_bool32
            {
                auto& s = *static_cast<SearchCtx*>(ud);
                if (type == ma_device_type_capture && !s.found) {
                    std::string name(pInfo->name);
                    std::transform(name.begin(), name.end(), name.begin(),
                                   [](unsigned char c){ return std::tolower(c); });
                    if (name.find(s.needle) != std::string::npos) {
                        s.id    = pInfo->id;
                        s.found = true;
                    }
                }
                return MA_TRUE;
            },
            &sc);

        if (sc.found) {
            foundId  = sc.id;
            pFoundId = &foundId;
        } else {
            std::cerr << "[AudioCapture] Device '" << deviceName
                      << "' not found. Falling back to default.\n";
        }
    }

    // ── Configure and open device ─────────────────────────────────────────────
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = 2;
    cfg.sampleRate       = 48000;
    cfg.dataCallback     = Impl::DataCallback;
    cfg.pUserData        = m_impl.get();

    if (pFoundId)
        cfg.capture.pDeviceID = pFoundId;

    if (ma_device_init(&m_impl->ctx, &cfg, &m_impl->dev) != MA_SUCCESS) {
        std::cerr << "[AudioCapture] Failed to open audio device.\n";
        ma_context_uninit(&m_impl->ctx);
        m_impl->ctxInit = false;
        return false;
    }
    m_impl->devInit = true;

    // Verify actual format matches what was requested.
    if (m_impl->dev.capture.channels != 2 || m_impl->dev.sampleRate != 48000) {
        std::cerr << "[AudioCapture] Warning: device opened with "
                  << m_impl->dev.capture.channels << " ch @ "
                  << m_impl->dev.sampleRate << " Hz (expected 2ch/48kHz). "
                  << "miniaudio will convert, but DSP accuracy may be reduced.\n";
    }

    if (ma_device_start(&m_impl->dev) != MA_SUCCESS) {
        std::cerr << "[AudioCapture] Failed to start audio device.\n";
        ma_device_uninit(&m_impl->dev);
        ma_context_uninit(&m_impl->ctx);
        m_impl->devInit = m_impl->ctxInit = false;
        return false;
    }

    m_impl->running.store(true, std::memory_order_release);
    std::cout << "[AudioCapture] Started: " << m_impl->dev.capture.name
              << "  ch=" << m_impl->dev.capture.channels
              << "  rate=" << m_impl->dev.sampleRate << " Hz\n";
    return true;
}

void AudioCapture::Stop() {
    if (!m_impl || !m_impl->running.load(std::memory_order_acquire)) return;
    m_impl->running.store(false, std::memory_order_release);
    
    // Give the audio callback time to see the running flag and exit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (m_impl->devInit) {
        ma_device_uninit(&m_impl->dev); // blocks until in-flight callback returns
        m_impl->devInit = false;
    }
    if (m_impl->ctxInit) {
        ma_context_uninit(&m_impl->ctx);
        m_impl->ctxInit = false;
    }
}

bool AudioCapture::IsRunning() const {
    return m_impl && m_impl->running.load(std::memory_order_acquire);
}

size_t AudioCapture::GetAvailableFrames() const {
    return m_impl ? m_impl->ringBuf.GetAvailableFrames() : 0;
}

size_t AudioCapture::ReadInterleaved(float* dst, size_t frameCount) {
    return m_impl ? m_impl->ringBuf.PopInterleaved(dst, frameCount) : 0;
}

AudioLevels AudioCapture::GetCurrentLevels() const {
    if (!m_impl) return {};
    return AudioLevels{
        m_impl->leftRms .load(std::memory_order_relaxed),
        m_impl->rightRms.load(std::memory_order_relaxed),
        m_impl->leftPeak .load(std::memory_order_relaxed),
        m_impl->rightPeak.load(std::memory_order_relaxed),
    };
}

AudioFrame AudioCapture::GetFrame(uint32_t timeout_ms) {
    constexpr size_t kBlock = 480; // 10 ms @ 48 kHz
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        if (m_impl && m_impl->ringBuf.GetAvailableFrames() >= kBlock) {
            std::vector<float> tmp(kBlock * 2);
            m_impl->ringBuf.PopInterleaved(tmp.data(), kBlock);
            AudioFrame f;
            f.sample_rate = 48000;
            f.left .resize(kBlock);
            f.right.resize(kBlock);
            for (size_t i = 0; i < kBlock; ++i) {
                f.left [i] = tmp[i * 2];
                f.right[i] = tmp[i * 2 + 1];
            }
            return f;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    AudioFrame silence;
    silence.sample_rate = 48000;
    silence.left .assign(kBlock, 0.f);
    silence.right.assign(kBlock, 0.f);
    return silence;
}

} // namespace EchoRadar
