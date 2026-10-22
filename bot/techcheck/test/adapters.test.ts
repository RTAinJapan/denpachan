import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, it } from "node:test";
import { WsReceiver } from "../src/adapters/receiver.js";
import { SqliteStore } from "../src/store.js";

describe("denpachan の受信口のクライアント", () => {
    it("URL にトークンを付ける", () => {
        assert.equal(WsReceiver.urlFor("127.0.0.1", 9100, ""), "ws://127.0.0.1:9100/ws");
        assert.equal(WsReceiver.urlFor("::1", 9100, "a b"), "ws://[::1]:9100/ws?token=a%20b");
    });

    it("channels を覚え、繋がっていないときは返さない", () => {
        const rx = new WsReceiver("ws://127.0.0.1:1/ws", () => {});
        rx.handle(JSON.stringify({ event: "channels", channels: [{ ch: 7, status: "live", testSlot: true, source: { protocol: "srt", mode: "caller" } }] }));
        // WebSocket を開いていないので connected() は false
        assert.equal(rx.connected(), false);
        assert.equal(rx.channel(7), undefined);
        rx.handle("not json");
    });

    it("engine の channels の形 (null、メッセージのオブジェクト) をそろえる", async () => {
        const { normalizeChannel } = await import("../src/adapters/receiver.js");
        // engine (src/engine/metrics_view.cpp) が送る形: 止まっているチャンネル
        const stopped = normalizeChannel({
            ch: 7, name: "CH7", enabled: false, inUse: true, testSlot: true, status: "stopped",
            source: { protocol: "srt", mode: "caller", host: "mc.example", port: 5000, peer: "", connectedSec: 0 },
            video: null, latency: null, tcp: null, audio: null, record: null, rejectedSenders: null, lastError: null,
        })!;
        assert.equal(stopped.status, "stopped");
        assert.equal(stopped.testSlot, true);
        assert.equal(stopped.video, undefined);
        assert.equal(stopped.lastError, "");
        // 受信中: 値の無い数は null で来る
        const live = normalizeChannel({
            ch: 8, name: "CH8", enabled: true, testSlot: true, status: "live",
            source: { protocol: "rtmp", mode: "pull", peer: "x", connectedSec: 12 },
            video: { codec: "h264", width: 1920, height: 1080, fps: 59.9, outputFps: 60, bitrateMbps: 6, hwDecode: true, decodeErrors: null, droppedLate: 0, droppedQueue: 0 },
            latency: null, tcp: null,
            audio: { peakDb: [-12.5, -13], gaps: 0, silentSec: 0 },
            record: { state: "recording", file: "D:\\rec\\a.mkv", seconds: null },
            lastError: { id: "srt.unreachable", reason: "timeout" },
        })!;
        assert.equal(live.video?.fps, 59.9);
        assert.equal(live.video?.decodeErrors, undefined);
        assert.deepEqual(live.audio?.peakDb, [-12.5, -13]);
        assert.equal(live.record?.seconds, undefined);
        assert.equal(live.lastError, 'srt.unreachable {"reason":"timeout"}');
        assert.equal(normalizeChannel({ name: "no ch" }), undefined);
    });

    it("繋がっていなければ call は失敗する", async () => {
        const rx = new WsReceiver("ws://127.0.0.1:1/ws", () => {});
        await assert.rejects(rx.call("getState", {}), /繋がっていません/);
    });
});

describe("SQLite の保存", () => {
    it("起動し直しても残る", () => {
        const dir = mkdtempSync(join(tmpdir(), "techcheck-"));
        try {
            const path = join(dir, "t.db");
            const a = new SqliteStore(path);
            a.set("queue", { entries: [{ userId: "U1" }] });
            a.set("queue", { entries: [{ userId: "U2" }] });
            a.close();
            const b = new SqliteStore(path);
            assert.deepEqual(b.get("queue"), { entries: [{ userId: "U2" }] });
            assert.equal(b.get("nothing"), undefined);
            b.close();
        } finally {
            rmSync(dir, { recursive: true, force: true });
        }
    });
});

describe("スラッシュコマンドの定義", () => {
    it("Discord の決まり (名前、説明の長さ、選択肢) に合う", async () => {
        const { buildCommand } = await import("../src/discord/commands.js");
        const { testConfig } = await import("./helpers.js");
        const json = buildCommand(testConfig().flows).toJSON();
        assert.equal(json.name, "techcheck");
        const names = (json.options ?? []).map((o) => o.name);
        assert.deepEqual(names, ["start", "leave", "slot", "status", "shot", "latency", "latency-receiver", "rtmp", "cancel", "review"]);
    });
});
