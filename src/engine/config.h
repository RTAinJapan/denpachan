#pragma once
// 設定 (%APPDATA%\denpachan\config.json)。設計書 §10.1。
//
// - 保存は一時ファイルに書いてから差し替える (書き込み中に落ちても壊れない)。
// - SRT のパスフレーズは DPAPI で暗号化して保存する。IPC では返さない (設定済みかどうかだけ返す)。
// - RTMP のストリームキーと pull の URL も DPAPI で暗号化して保存する。キーは送出側 (Nginx) に
//   設定してもらう必要があるので IPC では返す (パイプは同じユーザーしか開けない)。
// - メトリクスの受信口 (WebSocket) のトークンも DPAPI で暗号化して保存する。IPC では返さない (設定済みかどうかだけ)。

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>

#include "denpachan/channels.h"

namespace denpachan::engine {

using denpachan::kMaxChannels;
inline constexpr int kDefaultChannelCount = 4;

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
    // 配信テスト用のチャンネル。メトリクスの受信口 (WebSocket) からの操作 (開始・停止・録画・latency) は、
    // この印のあるチャンネルにだけ許す (イベント中に bot が本番のチャンネルを触らないように)
    bool test_slot = false;
};

nlohmann::json default_record_json();

// メトリクスの受信口 (WebSocket)。配信テストの bot が状態を受け取り、配信テスト用のチャンネルを操作する。
// docs/stream-test-automation.md §3.2
struct MetricsConfig {
    bool enabled = false;
    std::string bind = "127.0.0.1";
    int port = 9100;
    std::string token;  // 空なら認証なし (loopback に bind するときだけ許す)
    bool allow_control = false;

    bool operator==(const MetricsConfig&) const = default;
};

// bind が loopback (127.0.0.0/8、::1、localhost) か
bool is_loopback_bind(const std::string& bind);

struct GeneralConfig {
    // 使うチャンネル数 (1〜kMaxChannels)。画面にはこの数だけ出す。これより後ろのチャンネルは止めておく
    // (設定は残す)。設定ファイルには常に kMaxChannels 個のチャンネルを書く
    int channel_count = kDefaultChannelCount;
    int stall_ms = 2000;
    int audio_margin_ms = 10;
    bool av_sync = true;
    // RTMP サーバ (rtmpd)。RTMP push のチャンネルが 1 つでも動いていれば起動する
    std::string rtmp_bind = "0.0.0.0";
    int rtmp_port = 1935;
    int rtmp_idle_timeout_ms = 5000;
    MetricsConfig metrics;
};

struct Config {
    GeneralConfig general;
    std::array<ChannelConfig, kMaxChannels> channels;
};

// 使うチャンネル数より後ろのチャンネルを止める (enabled = false)。止めたものがあれば true
bool stop_unused_channels(Config& c);

// 使うチャンネル数を old_count から増やしたとき、新しく使うチャンネルの待ち受けポートや仮想カメラが
// 前からのチャンネルとぶつかっていれば、空いているポート・カメラに変える (validate で拒否されないように)
void make_room_for_new_channels(Config& c, int old_count);

enum class JsonForm {
    File,      // 保存用 (パスフレーズは DPAPI で暗号化)
    Ipc,       // UI へ返す (パスフレーズは返さず、設定済みかどうかだけ)
    Internal,  // 内部での差分適用用 (平文)
};

Config default_config();
nlohmann::json to_json(const Config& c, JsonForm form);
nlohmann::json to_json(const ChannelConfig& c, JsonForm form);
nlohmann::json to_json(const GeneralConfig& g, JsonForm form);
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
