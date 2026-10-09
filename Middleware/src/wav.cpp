#include "wav.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace af {
namespace {

constexpr std::uint16_t kFormatPcm        = 1;
constexpr std::uint16_t kFormatFloat      = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT の頭 2 バイト（＝実際の形式番号）。
// 拡張形式は SubFormat の先頭 16bit に本来の形式番号が入る決まりなので、
// GUID を丸ごと比べる必要はありません。

struct Reader {
    std::FILE* f = nullptr;
    ~Reader() { if (f) std::fclose(f); }

    bool open(const std::wstring& p) { return ::_wfopen_s(&f, p.c_str(), L"rb") == 0 && f; }

    bool read(void* dst, std::size_t n) { return std::fread(dst, 1, n, f) == n; }

    bool u16(std::uint16_t* v) { return read(v, 2); }
    bool u32(std::uint32_t* v) { return read(v, 4); }
    bool tag(char out[4])      { return read(out, 4); }
    bool skip(long n)          { return std::fseek(f, n, SEEK_CUR) == 0; }
};

bool tagIs(const char t[4], const char* s) { return std::memcmp(t, s, 4) == 0; }

// 生のサンプル 1 個を float へ。
float toFloat(const unsigned char* p, int bits, bool isFloat) {
    if (isFloat) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    switch (bits) {
    case 8:   // 8bit だけ符号なし（0..255、中央 128）。ここを符号ありで読むと轟音になる。
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16: {
        std::int16_t v;
        std::memcpy(&v, p, 2);
        return v / 32768.0f;
    }
    case 24: {
        const std::int32_t v = (static_cast<std::int32_t>(p[2]) << 24 |
                                static_cast<std::int32_t>(p[1]) << 16 |
                                static_cast<std::int32_t>(p[0]) << 8) >> 8;   // 符号を伸ばす
        return v / 8388608.0f;
    }
    case 32: {
        std::int32_t v;
        std::memcpy(&v, p, 4);
        return static_cast<float>(v / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

// ヘッダを走査する。中身が要るときは dataPos / dataBytes を返す。
bool scan(const std::wstring& path, WavInfo* out, Reader* r,
          std::uint32_t* dataPos, std::uint32_t* dataBytes, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };

    if (!r->open(path)) return fail("ファイルを開けません");

    // 大きさを取っておく（一覧に出す）。
    std::fseek(r->f, 0, SEEK_END);
    const long size = std::ftell(r->f);
    std::fseek(r->f, 0, SEEK_SET);
    out->fileBytes = static_cast<std::uint64_t>(size < 0 ? 0 : size);

    char t[4];
    std::uint32_t riffSize = 0;
    if (!r->tag(t) || !tagIs(t, "RIFF")) return fail("RIFF ではありません");
    if (!r->u32(&riffSize)) return fail("壊れています");
    if (!r->tag(t) || !tagIs(t, "WAVE")) return fail("WAVE ではありません");

    bool haveFmt = false;
    *dataPos = 0;
    *dataBytes = 0;

    // ★塊を順に歩く。fmt と data の順序は決まっていないし、
    //   LIST や cue が間に挟まることも普通にある。決め打ちで読むと落ちる。
    while (true) {
        std::uint32_t len = 0;
        if (!r->tag(t) || !r->u32(&len)) break;

        if (tagIs(t, "fmt ")) {
            if (len < 16) return fail("fmt が短すぎます");
            std::uint16_t fmt = 0, ch = 0, bits = 0, blockAlign = 0;
            std::uint32_t rate = 0, bps = 0;
            if (!r->u16(&fmt) || !r->u16(&ch) || !r->u32(&rate) || !r->u32(&bps) ||
                !r->u16(&blockAlign) || !r->u16(&bits))
                return fail("fmt を読めません");

            std::uint32_t rest = len - 16;
            if (fmt == kFormatExtensible && rest >= 22) {
                std::uint16_t cbSize = 0, validBits = 0, sub = 0;
                std::uint32_t mask = 0;
                if (!r->u16(&cbSize) || !r->u16(&validBits) || !r->u32(&mask) || !r->u16(&sub))
                    return fail("拡張 fmt を読めません");
                fmt = sub;              // SubFormat の頭に本来の形式番号が入っている
                rest -= 10;
            }
            if (rest > 0 && !r->skip(static_cast<long>(rest))) return fail("fmt を飛ばせません");

            if (fmt != kFormatPcm && fmt != kFormatFloat) {
                char b[128];
                ::sprintf_s(b, "圧縮された WAV は取り込めません（形式 %u）", fmt);
                return fail(b);
            }
            if (ch < 1 || ch > 8)  return fail("チャンネル数が範囲外です");
            if (rate < 4000 || rate > 384000) return fail("周波数が範囲外です");
            if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32))
                return fail("ビット深度に対応していません（8/16/24/32 のみ）");
            if (fmt == kFormatFloat && bits != 32) return fail("float は 32bit のみです");

            out->sampleRate = static_cast<int>(rate);
            out->channels   = ch;
            out->bits       = bits;
            out->isFloat    = (fmt == kFormatFloat);
            haveFmt = true;
        } else if (tagIs(t, "data")) {
            *dataPos   = static_cast<std::uint32_t>(std::ftell(r->f));
            *dataBytes = len;
            // data のあとにも塊が続くことがあるが、必要なものは揃ったので抜ける。
            break;
        } else {
            // 奇数長の塊は 1 バイトの詰め物が入る決まり。
            const long adv = static_cast<long>(len) + (len & 1);
            if (!r->skip(adv)) break;
        }
    }

    if (!haveFmt)    return fail("fmt が見つかりません");
    if (*dataBytes == 0) return fail("data が見つかりません");

    const int bytesPerFrame = out->channels * (out->bits / 8);
    if (bytesPerFrame <= 0) return fail("形式が壊れています");
    out->frames = *dataBytes / static_cast<std::uint32_t>(bytesPerFrame);
    if (out->frames == 0) return fail("中身が空です");

    if (err) err->clear();
    return true;
}

}  // namespace

std::string WavInfo::formatLine() const {
    char b[64];
    ::sprintf_s(b, "%d bit %s", bits, isFloat ? "float" : "PCM");
    return b;
}

bool readWavInfo(const std::wstring& path, WavInfo* out, std::string* err) {
    Reader r;
    WavInfo tmp;
    std::uint32_t pos = 0, bytes = 0;
    if (!scan(path, &tmp, &r, &pos, &bytes, err)) return false;
    if (out) *out = tmp;
    return true;
}

bool readWav(const std::wstring& path, WavInfo* out, std::vector<float>* pcm, std::string* err) {
    Reader r;
    WavInfo info;
    std::uint32_t pos = 0, bytes = 0;
    if (!scan(path, &info, &r, &pos, &bytes, err)) return false;

    // ★上限を置く。試聴の道具なので、長い曲を丸ごとメモリに載せる必要はない。
    //   置かないと、間違って 1GB の素材を掴んだときに黙って固まる。
    constexpr std::uint64_t kMaxFrames = 60ull * 384000;   // 384kHz で 60 秒ぶん
    if (info.frames > kMaxFrames) {
        if (err) *err = "長すぎます（試聴は 60 秒まで）";
        return false;
    }

    const int bytesPerSample = info.bits / 8;
    const std::size_t total = static_cast<std::size_t>(info.frames) * info.channels;

    std::vector<unsigned char> raw(static_cast<std::size_t>(bytes));
    if (std::fseek(r.f, static_cast<long>(pos), SEEK_SET) != 0 ||
        std::fread(raw.data(), 1, raw.size(), r.f) != raw.size()) {
        if (err) *err = "中身を読めません";
        return false;
    }

    pcm->resize(total);
    for (std::size_t i = 0; i < total; ++i)
        (*pcm)[i] = toFloat(raw.data() + i * bytesPerSample, info.bits, info.isFloat);

    if (out) *out = info;
    if (err) err->clear();
    return true;
}

std::vector<float> conformToDevice(const std::vector<float>& src, const WavInfo& info,
                                   int deviceRate, int deviceChannels) {
    std::vector<float> out;
    if (src.empty() || info.channels <= 0 || info.sampleRate <= 0 ||
        deviceRate <= 0 || deviceChannels <= 0)
        return out;

    const double ratio = static_cast<double>(info.sampleRate) / deviceRate;
    const std::uint64_t srcFrames = info.frames;
    const std::size_t dstFrames =
        static_cast<std::size_t>(static_cast<double>(srcFrames) / ratio);
    if (dstFrames == 0) return out;

    out.resize(dstFrames * static_cast<std::size_t>(deviceChannels), 0.0f);

    for (std::size_t i = 0; i < dstFrames; ++i) {
        const double sp = i * ratio;
        const std::uint64_t i0 = static_cast<std::uint64_t>(sp);
        const std::uint64_t i1 = (i0 + 1 < srcFrames) ? i0 + 1 : i0;
        const float t = static_cast<float>(sp - static_cast<double>(i0));

        for (int c = 0; c < deviceChannels; ++c) {
            // 足りないチャンネルは最後のもので埋める（モノラル → 両耳に同じ音）。
            const int sc = (c < info.channels) ? c : info.channels - 1;
            const float a = src[static_cast<std::size_t>(i0) * info.channels + sc];
            const float b = src[static_cast<std::size_t>(i1) * info.channels + sc];
            out[i * deviceChannels + c] = a + (b - a) * t;
        }
    }
    return out;
}

}  // namespace af
