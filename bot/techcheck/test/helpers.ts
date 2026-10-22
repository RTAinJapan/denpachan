// 試験用の偽物。外の口 (ports.ts) を置き換え、時計を進めながら tick() を回す。

import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import type { SrtSample } from "../src/advisor.js";
import { parseConfig, type Config } from "../src/config.js";
import { I18n } from "../src/i18n.js";
import type { ChatPort, Clock, DnsPort, FlowInfo, FlowMetadata, FlowPort, OutMessage, ReceiverChannel, ReceiverPort, SheetPort, WebhookPort } from "../src/ports.js";
import { TechcheckService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";

export const here = dirname(fileURLToPath(import.meta.url));
export const messagesDir = join(here, "..", "messages");

export const baseYaml = `
discord:
  token: t
  guildId: g
  techcheckChannelId: C-test
  staffRoleId: R-staff
  opsChannelId: C-ops
notify:
  webhookUrl: https://discord.com/api/webhooks/1/x
aws:
  region: ap-northeast-1
flows:
  - name: flow-a
    arn: arn:flow-a
    domain: srt-a.example.jp
    passphrase: pass-a
rtmp:
  domain: rtmp.example.jp
  app: live
  key: key-1
slots:
  srt: { flow: flow-a, channel: 7 }
  rtmp: { channel: 8 }
sheet:
  spreadsheetId: sheet-1
  credentialsFile: none.json
guide:
  obsProfile: RTA 2026
`;

export function testConfig(extra = ""): Config {
    return parseConfig(baseYaml + extra, {});
}

export class FakeClock implements Clock {
    t = Date.UTC(2026, 10, 1, 3, 0, 0);
    now(): number {
        return this.t;
    }
}

export class FakeFlow implements FlowPort {
    status = "STANDBY";
    ingestIp = "203.0.113.10";
    ingestPort = 5000;
    minLatencyMs = 120;
    started = 0;
    stopped = 0;
    /** t (epoch 秒) から 1 秒分のメトリクスを作る。undefined なら CloudWatch にまだ無い */
    sampleAt: (t: number) => SrtSample | undefined = () => undefined;
    meta: FlowMetadata | undefined = { width: 1920, height: 1080, fps: 60, codec: "h264", hasAudio: true };

    async describe(arn: string): Promise<FlowInfo> {
        return { arn, status: this.status, ingestIp: this.ingestIp, ingestPort: this.ingestPort, protocol: "srt-listener", minLatencyMs: this.minLatencyMs };
    }
    async start(): Promise<void> {
        this.started++;
        this.status = "ACTIVE";
    }
    async stop(): Promise<void> {
        this.stopped++;
        this.status = "STANDBY";
    }
    async metrics(_arn: string, fromSec: number, toSec: number): Promise<SrtSample[]> {
        const out: SrtSample[] = [];
        for (let t = fromSec; t < toSec; t++) {
            const s = this.sampleAt(t);
            if (s) out.push(s);
        }
        return out;
    }
    async metadata(): Promise<FlowMetadata | undefined> {
        return this.meta;
    }
    async thumbnail(): Promise<Buffer | undefined> {
        return Buffer.from("jpeg");
    }
}

export class FakeRx implements ReceiverPort {
    up = true;
    calls: { method: string; params: Record<string, unknown> }[] = [];
    channels = new Map<number, ReceiverChannel>();

    constructor() {
        for (const [ch, protocol, mode] of [
            [7, "srt", "caller"],
            [8, "rtmp", "pull"],
        ] as const) {
            this.channels.set(ch, { ch, name: `CH${ch}`, enabled: false, testSlot: true, status: "stopped", source: { protocol, mode } });
        }
    }
    connected(): boolean {
        return this.up;
    }
    channel(ch: number): ReceiverChannel | undefined {
        return this.up ? this.channels.get(ch) : undefined;
    }
    async call(method: string, params: Record<string, unknown>): Promise<unknown> {
        this.calls.push({ method, params });
        const c = this.channels.get(params.ch as number);
        if (!c) throw new Error("no channel");
        if (method === "startChannel") {
            c.enabled = true;
            c.status = c.source.protocol === "srt" ? "connecting" : "listening";
        }
        if (method === "stopChannel") {
            c.enabled = false;
            c.status = "stopped";
        }
        if (method === "setRecording") c.record = params.enabled ? { state: "recording", file: `rec-ch${c.ch}.mkv`, seconds: 0 } : { state: "off" };
        return {};
    }
    /** 配信が届いている状態にする */
    live(ch: number, opts: { fps?: number; peak?: number; decodeErrors?: number } = {}): void {
        const c = this.channels.get(ch)!;
        c.status = "live";
        c.source.connectedSec = (c.source.connectedSec ?? 0) + 1;
        c.video = { width: 1920, height: 1080, fps: opts.fps ?? 60, codec: "h264", decodeErrors: opts.decodeErrors ?? c.video?.decodeErrors ?? 0 };
        const p = opts.peak ?? -20;
        c.audio = { peakDb: [p, p], gaps: 0, silentSec: p < -60 ? (c.audio?.silentSec ?? 0) + 1 : 0 };
        c.latency = { rttMs: 3, drops: 0, verdict: "ok" };
        if (c.record?.state === "recording") c.record.seconds = (c.record.seconds ?? 0) + 1;
    }
    drop(ch: number): void {
        const c = this.channels.get(ch)!;
        c.status = c.source.protocol === "srt" ? "connecting" : "listening";
        c.source.connectedSec = 0;
    }
}

export interface Posted {
    threadId: string;
    msg: OutMessage;
}

export class FakeChat implements ChatPort {
    threads: { id: string; name: string; userId: string; closed: boolean }[] = [];
    posts: Posted[] = [];
    called: { userId: string; msg: OutMessage }[] = [];
    opsLog: string[] = [];
    cleared: string[] = [];
    private n = 0;

    async createThread(name: string, userId: string): Promise<string> {
        const id = `T${++this.n}`;
        this.threads.push({ id, name, userId, closed: false });
        return id;
    }
    async post(threadId: string, msg: OutMessage): Promise<string> {
        this.posts.push({ threadId, msg });
        return `M${this.posts.length}`;
    }
    async clearButtons(_t: string, messageId: string): Promise<void> {
        this.cleared.push(messageId);
    }
    async closeThread(threadId: string): Promise<void> {
        this.threads.find((t) => t.id === threadId)!.closed = true;
    }
    threadUrl(threadId: string): string {
        return `https://discord.com/channels/g/${threadId}`;
    }
    async callUser(userId: string, msg: OutMessage): Promise<void> {
        this.called.push({ userId, msg });
    }
    async ops(text: string): Promise<void> {
        this.opsLog.push(text);
    }
    lastPost(threadId?: string): OutMessage | undefined {
        const list = threadId ? this.posts.filter((p) => p.threadId === threadId) : this.posts;
        return list[list.length - 1]?.msg;
    }
    texts(threadId: string): string[] {
        return this.posts.filter((p) => p.threadId === threadId).map((p) => p.msg.content);
    }
    /** スレッドに最後に出たボタンの id (action で探す) */
    buttonId(threadId: string, action: string): string {
        for (const p of [...this.posts].reverse()) {
            if (p.threadId !== threadId) continue;
            const b = p.msg.buttons?.find((x) => x.id.endsWith(`:${action}`));
            if (b) return b.id;
        }
        throw new Error(`button ${action} not found`);
    }
}

export class FakeSheet implements SheetPort {
    rows: (string | number)[][] = [];
    updates: { range: string; values: (string | number)[] }[] = [];
    fail = false;
    async append(row: (string | number)[]): Promise<string> {
        if (this.fail) throw new Error("sheet down");
        this.rows.push(row);
        return `techcheck!A${this.rows.length + 1}:AF${this.rows.length + 1}`;
    }
    async update(range: string, values: (string | number)[]): Promise<void> {
        this.updates.push({ range, values });
    }
    url(): string {
        return "https://docs.google.com/spreadsheets/d/sheet-1";
    }
}

export class FakeWebhook implements WebhookPort {
    sent: unknown[] = [];
    async send(payload: unknown): Promise<void> {
        this.sent.push(payload);
    }
}

export class FakeDns implements DnsPort {
    answers: Record<string, string[]> = { "srt-a.example.jp": ["203.0.113.10"], "rtmp.example.jp": ["198.51.100.1"] };
    async resolve(domain: string): Promise<string[]> {
        const a = this.answers[domain];
        if (!a) throw new Error("NXDOMAIN");
        return a;
    }
}

export function makeWorld(extraYaml = "") {
    const config = testConfig(extraYaml);
    const clock = new FakeClock();
    const flow = new FakeFlow();
    const rx = new FakeRx();
    const chat = new FakeChat();
    const sheet = new FakeSheet();
    const webhook = new FakeWebhook();
    const dns = new FakeDns();
    const store = new MemoryStore();
    const logs: string[] = [];
    const service = new TechcheckService({
        config,
        i18n: I18n.load(messagesDir),
        store,
        flow,
        rx,
        dns,
        chat,
        sheet,
        webhook,
        clock,
        log: (m) => logs.push(m),
        sleep: async () => {
            clock.t += 5_000;
        },
    });
    /** 非同期の準備 (prepare) が進むよう、いくつか待つ */
    const flush = async () => {
        for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r));
    };
    /** 1 秒ずつ進めて tick する。each で毎秒の状態を作る */
    const run = async (seconds: number, each?: (t: number) => void) => {
        for (let i = 0; i < seconds; i++) {
            clock.t += 1_000;
            each?.(Math.floor(clock.t / 1000));
            await service.tick();
            await flush();
        }
    };
    return { config, clock, flow, rx, chat, sheet, webhook, dns, store, service, logs, flush, run };
}

export type World = ReturnType<typeof makeWorld>;

/** よい回線のメトリクス (RTT 20 ms、ロスなし) */
export function goodSample(t: number): SrtSample {
    return { t, connected: true, rttMs: 20, lossPct: 0, totalPackets: 500, notRecovered: 0, continuityErrors: 0, tsSyncLoss: 0, jitterMs: 5, bitrateBps: 6_000_000, upTimeSec: t };
}
