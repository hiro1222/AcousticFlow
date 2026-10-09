// wav.h — WAV を読む。
//
// ★SoundLibrary は「音の在り処を持つだけ」なので、ここも**読むだけ**です。
//   素材を書き換える加工は持ちません（鳴らし方の調整は ClipSource が持ちます）。
//
// ⚠ ここで読めなかったものは**取り込ませません**。
//   「黙って変な音を出すより開かないほうを採る」を、デバイスと同じ規律で通します。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace af {

// 取り込んだ音の素性。**これは保存しません** ── ファイルを読めば分かることなので、
// 2 箇所に持つと必ず片方が古くなります。表示のたびにファイルから引きます。
struct WavInfo {
    int           sampleRate = 0;
    int           channels   = 0;
    int           bits       = 0;
    bool          isFloat    = false;
    std::uint64_t frames     = 0;
    std::uint64_t fileBytes  = 0;

    double seconds() const {
        return sampleRate > 0 ? static_cast<double>(frames) / sampleRate : 0.0;
    }
    // 「16bit PCM」のように 1 行で言う。
    std::string formatLine() const;
};

// ヘッダだけ読む。一覧に出すのはこちら（何十本あっても軽い）。
bool readWavInfo(const std::wstring& path, WavInfo* out, std::string* err);

// 中身まで読む。インターリーブした float（-1..1）で返す。
// 試聴のときだけ呼びます。
bool readWav(const std::wstring& path, WavInfo* out, std::vector<float>* pcm, std::string* err);

// 読んだ音を、デバイスの周波数とチャンネル数に合わせて並べ直す。
//
// ⚠ 変換は**線形補間**です。試聴に足りる品質という位置づけで、
//   ここを良くするのは「音が良くなる仕事」ではなく配管なので後回しにしています。
std::vector<float> conformToDevice(const std::vector<float>& src, const WavInfo& info,
                                   int deviceRate, int deviceChannels);

}  // namespace af
