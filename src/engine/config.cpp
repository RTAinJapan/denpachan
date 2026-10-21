#include "config.h"

#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include "audio_devices.h"

using nlohmann::json;

namespace denpachan::engine {

namespace {

// ---- DPAPI + Base64 ----

std::string base64_encode(const std::vector<BYTE>& data) {
    DWORD len = 0;
    CryptBinaryToStringA(data.data(), DWORD(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string out(len, '\0');
    CryptBinaryToStringA(data.data(), DWORD(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len);
    out.resize(len);
    return out;
}

std::vector<BYTE> base64_decode(const std::string& s) {
    DWORD len = 0;
    if (!CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return {};
    std::vector<BYTE> out(len);
    CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, out.data(), &len, nullptr, nullptr);
    out.resize(len);
    return out;
}

std::string protect(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"denpachan", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::vector<BYTE> bytes(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64_encode(bytes);
}

std::string unprotect(const std::string& encoded) {
    auto bytes = base64_decode(encoded);
    if (bytes.empty()) return {};
    DATA_BLOB in{DWORD(bytes.size()), bytes.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return plain;
}

template <typename T>
T get_or(const json& j, const char* key, const T& def) {
    if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return def;
    return j.at(key).get<T>();
}

std::optional<std::string> validate_record(const json& r) {
    try {
        auto in = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
        const std::string container = r.at("container");
        const std::string mode = r.at("mode");
        const std::string tmpl = r.at("template");
        const std::string repl = r.at("invalidCharReplacement");
        if (container != "mkv" && container != "ts") return "record.container must be mkv or ts";
        if (mode != "passthrough" && mode != "encode") return "record.mode must be passthrough or encode";
        if (tmpl.empty()) return "record.template is empty";
        if (repl.size() > 8 || repl.find_first_of(":*?\"<>|/\\") != std::string::npos) {
            return "record.invalidCharReplacement must not contain characters that are invalid in file names";
        }
        if (!in(r.at("gapTimeoutSec").get<int>(), 1, 3600)) return "record.gapTimeoutSec must be 1-3600";
        if (!in(r.at("maxSegmentMin").get<int>(), 0, 1440)) return "record.maxSegmentMin must be 0-1440";
        if (!in(r.at("minFreeGb").get<int>(), 0, 100000)) return "record.minFreeGb must be 0 or more";
        const auto& v = r.at("encode").at("video");
        const std::string codec = v.at("codec");
        const std::string rc = v.at("rc");
        const std::string encoder = v.at("encoder");
        if (encoder != "auto" && encoder != "nvenc" && encoder != "qsv" && encoder != "amf" && encoder != "cpu") {
            return "record.encode.video.encoder must be auto, nvenc, qsv, amf or cpu";
        }
        if (codec != "h264" && codec != "hevc" && codec != "av1") return "record.encode.video.codec must be h264, hevc or av1";
        if (rc != "cbr" && rc != "vbr" && rc != "cqp") return "record.encode.video.rc must be cbr, vbr or cqp";
        if (!in(v.at("bitrateKbps").get<int>(), 100, 500'000)) return "record.encode.video.bitrateKbps must be 100-500000";
        if (!in(v.at("qp").get<int>(), 0, 51)) return "record.encode.video.qp must be 0-51";
        if (!in(v.at("gopSec").get<int>(), 1, 20)) return "record.encode.video.gopSec must be 1-20";
        if (!in(r.at("encode").at("audio").at("bitrateKbps").get<int>(), 32, 512)) return "record.encode.audio.bitrateKbps must be 32-512";
        r.at("dir").get<std::string>();
        r.at("enabled").get<bool>();
        v.at("preset").get<std::string>();
    } catch (const json::exception& e) {
        return std::string("record: ") + e.what();
    }
    return std::nullopt;
}

}  // namespace

json default_record_json() {
    return {
        {"enabled", false},
        {"dir", ""},  // 空 = %USERPROFILE%\Videos\denpachan
        {"template", "{ch} - {YYYY}{MM}/{DD} {hh}-{mm}-{ss}"},
        {"container", "mkv"},
        {"mode", "passthrough"},
        {"gapTimeoutSec", 10},
        {"maxSegmentMin", 0},
        {"invalidCharReplacement", "-"},
        {"minFreeGb", 5},
        {"encode",
         {{"video",
           {{"codec", "h264"}, {"encoder", "auto"}, {"rc", "cbr"}, {"bitrateKbps", 12000}, {"qp", 23}, {"preset", "p4"}, {"gopSec", 2}}},
          {"audio", {{"bitrateKbps", 192}}}}},
    };
}

Config default_config() {
    Config c;
    for (int i = 0; i < kMaxChannels; ++i) {
        auto& ch = c.channels[size_t(i)];
        ch.id = i + 1;
        ch.name = "CH" + std::to_string(i + 1);
        ch.source.port = 9001 + i;
        ch.vcam = i + 1;
        ch.audio.start_channel = 1 + 2 * i;
        ch.record = default_record_json();
    }
    return c;
}

json to_json(const ChannelConfig& c, JsonForm form) {
    json source = {
        {"protocol", c.source.protocol},    {"mode", c.source.mode},         {"host", c.source.host},         {"port", c.source.port},
        {"latencyMs", c.source.latency_ms}, {"pbkeylen", c.source.pbkeylen}, {"streamId", c.source.streamid},
    };
    source["app"] = c.source.app;
    switch (form) {
    case JsonForm::File:
        source["passphraseProtected"] = protect(c.source.passphrase);
        source["keyProtected"] = protect(c.source.key);
        source["urlProtected"] = protect(c.source.url);
        break;
    case JsonForm::Ipc:
        source["hasPassphrase"] = !c.source.passphrase.empty();
        source["key"] = c.source.key;
        source["url"] = c.source.url;
        break;
    case JsonForm::Internal:
        source["passphrase"] = c.source.passphrase;
        source["key"] = c.source.key;
        source["url"] = c.source.url;
        break;
    }
    json j = {
        {"id", c.id},
        {"name", c.name},
        {"enabled", c.enabled},
        {"source", source},
        {"playoutBufferMs", c.playout_buffer_ms},
        {"vcam", c.vcam},
        {"audio",
         {{"device", c.audio.device},
          {"startChannel", c.audio.start_channel},
          {"mode", c.audio.mode},
          {"offsetMs", c.audio.offset_ms},
          {"mute", c.audio.mute},
          {"fade", c.audio.fade}}},
    };
    j["record"] = c.record;
    if (!c.debug_args.empty()) j["debugArgs"] = c.debug_args;
    return j;
}

json to_json(const GeneralConfig& g) {
    return {{"channelCount", g.channel_count},
            {"stallMs", g.stall_ms},
            {"audioMarginMs", g.audio_margin_ms},
            {"avSync", g.av_sync},
            {"rtmpServer", {{"bind", g.rtmp_bind}, {"port", g.rtmp_port}, {"idleTimeoutMs", g.rtmp_idle_timeout_ms}}}};
}

json to_json(const Config& c, JsonForm form) {
    json channels = json::array();
    for (const auto& ch : c.channels) channels.push_back(to_json(ch, form));
    return {{"version", 1}, {"general", to_json(c.general)}, {"channels", channels}};
}

ChannelConfig channel_from_json(const json& j, int id) {
    ChannelConfig def = default_config().channels[size_t(id - 1)];
    ChannelConfig c = def;
    c.id = id;
    c.name = get_or(j, "name", def.name);
    c.enabled = get_or(j, "enabled", def.enabled);
    c.playout_buffer_ms = get_or(j, "playoutBufferMs", def.playout_buffer_ms);
    c.vcam = get_or(j, "vcam", def.vcam);
    c.debug_args = get_or<std::string>(j, "debugArgs", "");
    c.record = default_record_json();
    if (j.contains("record") && j.at("record").is_object()) c.record.merge_patch(j.at("record"));
    if (j.contains("source")) {
        const auto& s = j.at("source");
        c.source.protocol = get_or(s, "protocol", def.source.protocol);
        c.source.mode = get_or(s, "mode", def.source.mode);
        c.source.host = get_or(s, "host", def.source.host);
        c.source.port = get_or(s, "port", def.source.port);
        c.source.latency_ms = get_or(s, "latencyMs", def.source.latency_ms);
        c.source.pbkeylen = get_or(s, "pbkeylen", def.source.pbkeylen);
        c.source.streamid = get_or(s, "streamId", def.source.streamid);
        c.source.app = get_or(s, "app", def.source.app);
        // 平文 (IPC / 内部) と暗号化 (保存ファイル) のどちらでも読む
        auto secret = [&](const char* plain, const char* protected_key) {
            if (s.contains(plain)) return get_or<std::string>(s, plain, "");
            if (s.contains(protected_key)) return unprotect(get_or<std::string>(s, protected_key, ""));
            return std::string();
        };
        c.source.passphrase = secret("passphrase", "passphraseProtected");
        c.source.key = secret("key", "keyProtected");
        c.source.url = secret("url", "urlProtected");
    }
    if (j.contains("audio")) {
        const auto& a = j.at("audio");
        c.audio.device = get_or(a, "device", def.audio.device);
        c.audio.start_channel = get_or(a, "startChannel", def.audio.start_channel);
        c.audio.mode = get_or(a, "mode", def.audio.mode);
        c.audio.offset_ms = get_or(a, "offsetMs", def.audio.offset_ms);
        c.audio.mute = get_or(a, "mute", def.audio.mute);
        c.audio.fade = get_or(a, "fade", def.audio.fade);
    }
    return c;
}

GeneralConfig general_from_json(const json& j) {
    GeneralConfig def;
    GeneralConfig g;
    g.channel_count = get_or(j, "channelCount", def.channel_count);  // 無い (0.1.x までの設定) なら 4
    g.stall_ms = get_or(j, "stallMs", def.stall_ms);
    g.audio_margin_ms = get_or(j, "audioMarginMs", def.audio_margin_ms);
    g.av_sync = get_or(j, "avSync", def.av_sync);
    // 以前あった "ffmpegPath" (CPU エンコードの ffmpeg.exe) は読まない。ffmpeg.exe は FFmpeg の DLL と同じフォルダから使う
    if (j.contains("rtmpServer")) {
        const auto& r = j.at("rtmpServer");
        g.rtmp_bind = get_or(r, "bind", def.rtmp_bind);
        g.rtmp_port = get_or(r, "port", def.rtmp_port);
        g.rtmp_idle_timeout_ms = get_or(r, "idleTimeoutMs", def.rtmp_idle_timeout_ms);
    }
    return g;
}

Config config_from_json(const json& j) {
    Config c = default_config();
    if (j.contains("general")) c.general = general_from_json(j.at("general"));
    if (j.contains("channels") && j.at("channels").is_array()) {
        for (const auto& ch : j.at("channels")) {
            const int id = get_or(ch, "id", 0);
            if (id >= 1 && id <= kMaxChannels) c.channels[size_t(id - 1)] = channel_from_json(ch, id);
        }
    }
    return c;
}

bool stop_unused_channels(Config& c) {
    bool stopped = false;
    for (auto& ch : c.channels) {
        if (ch.id > c.general.channel_count && ch.enabled) {
            ch.enabled = false;
            stopped = true;
        }
    }
    return stopped;
}

void make_room_for_new_channels(Config& c, int old_count) {
    const int count = c.general.channel_count;
    auto listen_key = [](const ChannelConfig& ch) { return std::pair{ch.source.host, ch.source.port}; };
    auto is_listener = [](const ChannelConfig& ch) { return ch.source.protocol == "srt" && ch.source.mode == "listener"; };
    for (int id = old_count + 1; id <= count; ++id) {
        auto& ch = c.channels[size_t(id - 1)];
        std::set<std::pair<std::string, int>> ports;
        std::set<int> vcams;
        for (const auto& other : c.channels) {
            if (other.id == id || other.id > count) continue;
            if (is_listener(other)) ports.insert(listen_key(other));
            if (other.vcam != 0) vcams.insert(other.vcam);
        }
        if (is_listener(ch) && ports.count(listen_key(ch))) {
            int port = 9001;
            while (ports.count({ch.source.host, port})) ++port;
            spdlog::info("ch{}: port {} is used by another channel; changed to {}", id, ch.source.port, port);
            ch.source.port = port;
        }
        if (ch.vcam != 0 && vcams.count(ch.vcam)) {
            // 空いている最小の番号にする (カメラはチャンネルと同じ数あるので、普通は空きがある)。無ければ「なし」
            int cam = 1;
            while (cam <= kMaxChannels && vcams.count(cam)) ++cam;
            if (cam > kMaxChannels) cam = 0;
            spdlog::info("ch{}: vcam {} is used by another channel; changed to {}", id, ch.vcam, cam);
            ch.vcam = cam;
        }
    }
}

std::optional<std::string> validate(const Config& c) {
    auto in = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
    if (!in(c.general.channel_count, 1, kMaxChannels)) return "channelCount must be 1-" + std::to_string(kMaxChannels);
    if (!in(c.general.stall_ms, 100, 60'000)) return "stallMs must be 100-60000";
    if (!in(c.general.audio_margin_ms, 0, 500)) return "audioMarginMs must be 0-500";

    if (!in(c.general.rtmp_port, 1, 65535)) return "rtmpServer.port must be 1-65535";
    if (!in(c.general.rtmp_idle_timeout_ms, 1000, 60'000)) return "rtmpServer.idleTimeoutMs must be 1000-60000";

    auto valid_name = [](const std::string& s, size_t max) {
        if (s.empty() || s.size() > max) return false;
        for (char ch : s) {
            if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')) return false;
        }
        return true;
    };

    std::set<int> vcams;
    std::set<std::pair<std::string, int>> listen_ports;
    std::set<std::pair<std::string, std::string>> rtmp_keys;
    for (const auto& ch : c.channels) {
        const std::string p = "ch" + std::to_string(ch.id) + ": ";
        const auto& s = ch.source;
        // 使うチャンネル数より後ろのチャンネルは、ポート・カメラ・キーの重複を調べない。画面から直せないうえ、
        // 0.1.x (4ch) の設定で ch1〜4 が 9005〜9008 を使っていると、既定値の ch5〜8 とぶつかって設定ごと捨てられるため。
        // 使うチャンネル数を増やしたときは make_room_for_new_channels が重複を外してから調べる
        const bool in_use = ch.id <= c.general.channel_count;
        if (!in(ch.vcam, 0, kMaxChannels)) return p + "vcam must be 0-" + std::to_string(kMaxChannels);
        if (in_use && ch.vcam != 0 && !vcams.insert(ch.vcam).second) return p + "vcam " + std::to_string(ch.vcam) + " is already used";
        if (!in(ch.playout_buffer_ms, 0, 2000)) return p + "playoutBufferMs must be 0-2000";
        if (!in(ch.audio.start_channel, 1, 64)) return p + "audio.startChannel must be 1-64";
        if (ch.audio.mode != "shared" && ch.audio.mode != "exclusive") return p + "audio.mode must be shared or exclusive";
        if (!in(ch.audio.offset_ms, -1000, 1000)) return p + "audio.offsetMs must be -1000-1000";
        if (auto err = validate_record(ch.record)) return p + *err;

        if (s.protocol == "rtmp") {
            if (s.mode == "push") {
                if (!valid_name(s.app, 63)) return p + "source.app must be 1-63 characters of [A-Za-z0-9-_.]";
                if (!valid_name(s.key, 127)) return p + "source.key must be 1-127 characters of [A-Za-z0-9-_.]";
                if (in_use && !rtmp_keys.insert({s.app, s.key}).second) return p + "the same app/key is used by another channel";
            } else if (s.mode == "pull") {
                if (s.url.rfind("rtmp://", 0) != 0 && s.url.rfind("rtmps://", 0) != 0) {
                    return p + "source.url must start with rtmp:// or rtmps://";
                }
            } else {
                return p + "source.mode must be push or pull for rtmp";
            }
            continue;
        }
        if (s.protocol != "srt") return p + "source.protocol must be srt or rtmp";
        if (s.mode != "listener" && s.mode != "caller") return p + "source.mode must be listener or caller";
        if (!in(s.port, 1, 65535)) return p + "source.port must be 1-65535";
        if (s.mode == "caller" && (s.host.empty() || s.host == "0.0.0.0")) return p + "source.host is required for caller";
        if (!in(s.latency_ms, 0, 10'000)) return p + "source.latencyMs must be 0-10000";
        if (!s.passphrase.empty() && (s.passphrase.size() < 10 || s.passphrase.size() > 79)) {
            return p + "source.passphrase must be 10-79 characters";
        }
        if (s.pbkeylen != 0 && s.pbkeylen != 16 && s.pbkeylen != 24 && s.pbkeylen != 32) {
            return p + "source.pbkeylen must be 0, 16, 24 or 32";
        }
        if (in_use && s.mode == "listener" && !listen_ports.insert({s.host, s.port}).second) {
            return p + "port " + std::to_string(s.port) + " is already used";
        }
    }
    return std::nullopt;
}

std::wstring default_config_path() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\denpachan";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

bool load_config(const std::wstring& path, Config& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "not found";
        return false;
    }
    try {
        out = config_from_json(json::parse(f));
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (auto err = validate(out)) {
        error = *err;
        return false;
    }
    return true;
}

bool save_config(const std::wstring& path, const Config& c, std::string& error) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write " + to_utf8(tmp);
            return false;
        }
        f << to_json(c, JsonForm::File).dump(2) << "\n";
        f.flush();
        if (!f) {
            error = "write failed";
            return false;
        }
    }
    // 差し替え (同じボリューム内なので原子的)
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace config (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

}  // namespace denpachan::engine
