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
