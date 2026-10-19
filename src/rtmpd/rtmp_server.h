#pragma once
// RTMP サーバ (publish の受け口)。設計書 §3.3。
//
// - 1 ポートで待ち受け、connect の app と publish のストリームキーでチャンネルに振り分ける
//   (振り分け表は engine が共有メモリに書く)。一致しなければ拒否する。
// - 同じチャンネルに新しい publish が来たら古い接続を切る (送出側の再接続ですぐ復帰するため)。
// - 受けた音声・映像・メタデータは解析せず、FLV タグにしてそのチャンネルのリングへ書く。
// - 接続ごとにスレッド 1 本。無通信が idle_timeout_ms 続いたら切る。

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "amf0.h"
#include "denpachan/flv_ring.h"
#include "denpachan/rtmp_routes.h"
#include "denpachan/win_util.h"

namespace denpachan::rtmpd {

struct ServerOptions {
    std::string bind = "0.0.0.0";
    int port = 1935;
    int idle_timeout_ms = 5000;
    int max_connections = 16;
};

class Server;

class Connection {
public:
    Connection(Server& server, SOCKET sock, std::string peer);
    ~Connection();

    void run();   // 接続のスレッドで呼ぶ
    void kick();  // 別スレッドから切る
    bool finished() const { return finished_; }
    const std::string& peer() const { return peer_; }

    std::thread thread;

private:
    struct ChunkStream {
        uint32_t timestamp = 0;
        uint32_t delta = 0;
        uint32_t length = 0;
        uint32_t stream_id = 0;
        uint8_t type = 0;
        bool extended = false;
        std::vector<uint8_t> payload;
    };

    bool handshake();
    bool recv_exact(uint8_t* buf, size_t n);
    bool read_chunk();
    bool handle_message(uint8_t type, uint32_t stream_id, uint32_t timestamp, std::vector<uint8_t>& payload);
    bool handle_command(const std::vector<AmfValue>& args);
    bool send_message(uint32_t csid, uint8_t type, uint32_t stream_id, uint32_t timestamp, const uint8_t* data,
                      size_t size);
    bool send_control(uint8_t type, uint32_t value);
    bool send_command(uint32_t csid, uint32_t stream_id, const AmfWriter& w);
    bool send_all(const uint8_t* data, size_t size);
    void update_tcp_stats();

    Server& server_;
    SOCKET sock_;
    const std::string peer_;
    std::atomic<bool> kicked_{false};
    std::atomic<bool> finished_{false};

    uint32_t in_chunk_size_ = 128;
    uint32_t out_chunk_size_ = 128;
    uint32_t ack_window_ = 0;
    uint64_t total_in_ = 0;
    uint64_t last_ack_ = 0;
    std::unordered_map<uint32_t, ChunkStream> streams_;

    std::string app_;
    int ch_ = 0;
    bool publishing_ = false;
    int64_t last_data_qpc_ = 0;
    int64_t last_stats_qpc_ = 0;
};

class Server {
public:
    explicit Server(ServerOptions options);
    ~Server();

    bool start();
    void stop();
    // 1 秒ごとに呼ぶ: 振り分けの変わったチャンネルの publish を切る、リングのハートビート
    void housekeeping();
    int64_t accept_heartbeat_qpc() const { return accept_heartbeat_qpc_; }

    // ---- Connection から呼ぶ ----
    const ServerOptions& options() const { return options_; }
    bool stopping() const { return stop_; }
    int resolve(const std::string& app, const std::string& key);  // チャンネル (なければ 0)
    void begin_publish(int ch, Connection* c);
    void end_publish(int ch, Connection* c);
    bool write_media(int ch, Connection* c, uint8_t type, uint32_t timestamp, const uint8_t* data, uint32_t size);
    void update_tcp_stats(int ch, Connection* c, int32_t rtt_us, uint64_t retrans_bytes);

private:
    struct Channel {
        std::mutex mutex;
        Connection* publisher = nullptr;
        std::string app;  // publish を受けたときの振り分け
        std::string key;
        flv_ring::Writer ring;
        bool ring_open = false;
        // 途中から読み始める worker のために保持する (最新のもの)
        std::vector<uint8_t> metadata;
        std::vector<uint8_t> audio_config;
        std::vector<uint8_t> video_config;
    };

    void accept_loop();
    bool read_routes(rtmp_routes::Route (&out)[rtmp_routes::kMaxRoutes]);

    const ServerOptions options_;
    SOCKET listen_ = INVALID_SOCKET;
    std::thread accept_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> accept_heartbeat_qpc_{0};

    std::mutex conns_mutex_;
    std::vector<std::shared_ptr<Connection>> conns_;

    Channel channels_[rtmp_routes::kMaxRoutes];
    SharedMapping routes_mapping_;
    rtmp_routes::Header* routes_ = nullptr;
};

}  // namespace denpachan::rtmpd
