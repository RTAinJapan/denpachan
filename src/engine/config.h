#pragma once
// 設定 (%APPDATA%\denpachan\config.json)。設計書 §10.1。
//
// - 保存は一時ファイルに書いてから差し替える (書き込み中に落ちても壊れない)。
// - SRT のパスフレーズは DPAPI で暗号化して保存する。IPC では返さない (設定済みかどうかだけ返す)。
// - RTMP のストリームキーと pull の URL も DPAPI で暗号化して保存する。キーは送出側 (Nginx) に
//   設定してもらう必要があるので IPC では返す (パイプは同じユーザーしか開けない)。

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>

namespace denpachan::engine {

inline constexpr int kChannelCount = 4;

struct SourceConfig {
    std::string protocol = "srt";   // srt / rtmp
    std::string mode = "listener";  // srt: listener / caller、rtmp: push / pull
    // SRT
    std::string host = "0.0.0.0";
    int port = 0;
    int latency_ms = 120;
    std::string passphrase;
    int pbkeylen = 0;
    std::string streamid;
    // RTMP push (rtmpd が受ける): rtmp://<この PC>:<rtmpPort>/<app>/<key>
    std::string app = "live";
    std::string key;
    // RTMP pull (こちらから取りに行く)
    std::string url;
};

struct AudioRouteConfig {
    std::string device;  // デバイス ID か名前の一部。空 = 出さない
    int start_channel = 1;
    std::string mode = "shared";  // shared / exclusive (デバイス単位)
    int offset_ms = 0;
    bool mute = false;
    bool fade = true;
};

struct ChannelConfig {
    int id = 1;
    std::string name;
    bool enabled = false;  // 待ち受け中か (engine 起動時にこの状態を復元する)
    SourceConfig source;
    int playout_buffer_ms = 0;
    int vcam = 0;  // 0 = 出さない
    AudioRouteConfig audio;
    // 録画 (設計書 §8)。worker がそのまま読む JSON。足りない項目は default_record_json() で埋める。
    // コマンドラインではなく共有メモリで渡すので、変えても worker は再起動しない。
    nlohmann::json record;
    std::string debug_args;  // 試験用: worker のコマンドラインに足す引数 (UI には出さない)
};

nlohmann::json default_record_json();

struct GeneralConfig {
    int stall_ms = 2000;
    int audio_margin_ms = 10;
    bool av_sync = true;
    // CPU で再エンコードして録画するときの外部 ffmpeg.exe。空なら denpachan と同じフォルダ、次に PATH から探す
    std::string ffmpeg_path;
    // RTMP サーバ (rtmpd)。RTMP push のチャンネルが 1 つでも動いていれば起動する
    std::string rtmp_bind = "0.0.0.0";
    int rtmp_port = 1935;
    int rtmp_idle_timeout_ms = 5000;
};

struct Config {
    GeneralConfig general;
    std::array<ChannelConfig, kChannelCount> channels;
};

enum class JsonForm {
    File,      // 保存用 (パスフレーズは DPAPI で暗号化)
    Ipc,       // UI へ返す (パスフレーズは返さず、設定済みかどうかだけ)
    Internal,  // 内部での差分適用用 (平文)
};

Config default_config();
nlohmann::json to_json(const Config& c, JsonForm form);
nlohmann::json to_json(const ChannelConfig& c, JsonForm form);
nlohmann::json to_json(const GeneralConfig& g);
// 足りない項目は既定値で埋める。型が違えば例外 (nlohmann::json::exception)。
ChannelConfig channel_from_json(const nlohmann::json& j, int id);
GeneralConfig general_from_json(const nlohmann::json& j);
Config config_from_json(const nlohmann::json& j);

// 問題があればその説明を返す
std::optional<std::string> validate(const Config& c);

std::wstring default_config_path();
bool load_config(const std::wstring& path, Config& out, std::string& error);
bool save_config(const std::wstring& path, const Config& c, std::string& error);

}  // namespace denpachan::engine
