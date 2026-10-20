// audio_probe: 入力デバイスから録音して、音声出力の試験結果を判定する開発用ツール。
//
// VB-Audio Virtual Cable などのループバック用デバイスと組み合わせて使う
// (engine の出力先を "CABLE Input" にして、この probe で "CABLE Output" を録る)。
//
//   audio_probe --dev "CABLE Output" --seconds 20 [--start 1] [--wav out.wav]
//
// 判定するもの:
// - 有音 / 無音の区間 (無音への切り替わりがフェードかどうか)
// - 不連続 (クリック) の数と時刻: 2 階差分 |x[n] - 2x[n-1] + x[n-2]| がしきい値を超えた点。
//   1kHz のテスト音 (-18 dBFS) なら通常は 0.003 程度なので、0.02 を超えたら不連続とみなす。

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "../engine/audio_devices.h"
#include "denpachan/win_util.h"

using Microsoft::WRL::ComPtr;
using namespace denpachan;
using namespace denpachan::engine;

namespace {

constexpr double kGlitchThreshold = 0.02;
constexpr double kActiveDbfs = -50.0;

struct Options {
    std::wstring device = L"CABLE Output";
    double seconds = 10;
    int start = 1;  // 調べるチャンネル (L)。R = start + 1
    std::wstring wav;
};

void write_wav(const std::wstring& path, const std::vector<float>& samples, int channels, int rate) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    const uint32_t data_bytes = uint32_t(samples.size() * sizeof(float));
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3);  // IEEE float
    u16(uint16_t(channels));
    u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 4));
    u16(uint16_t(channels * 4));
    u16(32);
    fwrite("data", 1, 4, f);
    u32(data_bytes);
    fwrite(samples.data(), sizeof(float), samples.size(), f);
    fclose(f);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring a = argv[i];
        const std::wstring v = argv[i + 1];
        if (a == L"--dev") o.device = v;
        else if (a == L"--seconds") o.seconds = std::stod(v);
        else if (a == L"--start") o.start = std::stoi(v);
        else if (a == L"--wav") o.wav = v;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const auto info = find_audio_device(eCapture, o.device);
    if (!info) {
        std::fprintf(stderr, "capture device not found: %s\n", to_utf8(o.device).c_str());
        return 1;
    }
    auto dev = open_audio_device(info->id);
    ComPtr<IAudioClient> client;
    WAVEFORMATEX* mix = nullptr;
    if (!dev || FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) || FAILED(client->GetMixFormat(&mix)) ||
        FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, mix, nullptr))) {
        std::fprintf(stderr, "cannot open capture device\n");
        return 1;
    }
    const int channels = mix->nChannels;
    const int rate = int(mix->nSamplesPerSec);
    const bool is_float = mix->wBitsPerSample == 32;
    ComPtr<IAudioCaptureClient> capture;
    client->GetService(IID_PPV_ARGS(&capture));
    client->Start();

    std::vector<float> samples;
    samples.reserve(size_t(o.seconds * rate * channels) + 4096);
    int discontinuities = 0;
    const int64_t end = qpc_now() + qpc_from_us(int64_t(o.seconds * 1e6));
    while (qpc_now() < end) {
        Sleep(5);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
            for (UINT32 i = 0; i < frames * UINT32(channels); ++i) {
                float v = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    v = is_float ? reinterpret_cast<float*>(data)[i] : reinterpret_cast<int16_t*>(data)[i] / 32768.0f;
                }
                samples.push_back(v);
            }
            capture->ReleaseBuffer(frames);
        }
    }
    client->Stop();
    CoTaskMemFree(mix);

    const size_t total = samples.size() / size_t(channels);
    const int cl = std::min(o.start - 1, channels - 1);
    const int cr = std::min(o.start, channels - 1);
    auto at = [&](size_t frame, int ch) { return samples[frame * size_t(channels) + size_t(ch)]; };

    std::printf("device: %s (%d ch, %d Hz), captured %.2f s, capture discontinuities %d\n", to_utf8(info->name).c_str(), channels, rate,
                double(total) / rate, discontinuities);

    // 有音 / 無音の区間 (10ms 単位)
    const size_t block = size_t(rate / 100);
    bool active = false;
    double seg_start = 0, seg_sum = 0;
    size_t seg_n = 0;
    std::printf("segments (ch %d):\n", o.start);
    for (size_t b = 0; b * block + block <= total; ++b) {
        double sum = 0;
        for (size_t i = b * block; i < (b + 1) * block; ++i) sum += double(at(i, cl)) * at(i, cl);
        const double rms_db = 10.0 * std::log10(sum / block + 1e-20);
        const bool now_active = rms_db > kActiveDbfs;
        const double t = double(b * block) / rate;
        if (now_active != active) {
            if (b > 0) {
                std::printf("  %7.2f - %7.2f s  %s", seg_start, t, active ? "active" : "silent");
                if (active) std::printf("  (%.1f dBFS rms)", 10.0 * std::log10(seg_sum / double(seg_n) + 1e-20));
                std::printf("\n");
            }
            active = now_active;
            seg_start = t;
            seg_sum = 0;
            seg_n = 0;
        }
        seg_sum += sum;
        seg_n += block;
    }
    std::printf("  %7.2f - %7.2f s  %s\n", seg_start, double(total) / rate, active ? "active" : "silent");

    // 不連続 (クリック)
    std::vector<double> glitches;
    for (int ch : {cl, cr}) {
        for (size_t i = 2; i < total; ++i) {
            const double d2 = std::fabs(at(i, ch) - 2.0 * at(i - 1, ch) + at(i - 2, ch));
            if (d2 > kGlitchThreshold) {
                const double t = double(i) / rate;
                if (glitches.empty() || t - glitches.back() > 0.005) glitches.push_back(t);
            }
        }
    }
    std::printf("glitches: %zu", glitches.size());
    for (size_t i = 0; i < glitches.size() && i < 20; ++i) std::printf("%s%.3f", i ? ", " : "  at ", glitches[i]);
    std::printf("\n");

    // L/R が一致しているか (チャンネルの配置確認用)
    double peak = 0;
    for (size_t i = 0; i < total; ++i) peak = std::max(peak, double(std::fabs(at(i, cl))));
    std::printf("peak (ch %d): %.1f dBFS\n", o.start, peak > 0 ? 20.0 * std::log10(peak) : -100.0);

    if (!o.wav.empty()) write_wav(o.wav, samples, channels, rate);
    CoUninitialize();
    return glitches.empty() ? 0 : 3;
}
