#include "WasapiEndpointFormat.h"

#include "AudioTypes.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

namespace EchoRadar {

bool QueryWasapiEndpointFormat(const wchar_t* endpointId,
                               WasapiEndpointFormat& format) noexcept {
    format = {};
#ifdef _WIN32
    if (endpointId == nullptr || endpointId[0] == L'\0') return false;

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(apartment);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    bool success = false;

    if (SUCCEEDED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->GetDevice(endpointId, &device)) &&
        SUCCEEDED(device->Activate(
            __uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(&client))) &&
        SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat != nullptr) {
        format.sampleRate = mixFormat->nSamplesPerSec;
        format.channelCount = mixFormat->nChannels;
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mixFormat->cbSize >= 22u) {
            const auto* extensible =
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat);
            format.channelMask = extensible->dwChannelMask;
        } else if (format.channelCount == 2) {
            // Conventional two-channel WAVEFORMATEX has an unambiguous L/R
            // order. Multichannel layouts still require an explicit mask.
            format.channelMask = WindowsSpeaker::Stereo;
        } else if (format.channelCount == 1) {
            format.channelMask = WindowsSpeaker::FrontCenter;
        }
        success = format.sampleRate != 0 && format.channelCount != 0;
    }

    if (mixFormat != nullptr) CoTaskMemFree(mixFormat);
    if (client != nullptr) client->Release();
    if (device != nullptr) device->Release();
    if (enumerator != nullptr) enumerator->Release();
    if (uninitialize) CoUninitialize();
    return success;
#else
    (void)endpointId;
    return false;
#endif
}

} // namespace EchoRadar
