// denpachan のメトリクスの受信口 (docs/stream-test-automation.md §3.2) の WebSocket クライアント。
// Node の WebSocket (ヘッダを付けられない) を使うので、トークンは ?token= で渡す。切れたら 2 秒後から繋ぎ直す。

import type { ReceiverChannel, ReceiverPort } from "../ports.js";

interface Pending {
    resolve: (v: unknown) => void;
    reject: (e: Error) => void;
    timer: NodeJS.Timeout;
}

export class WsReceiver implements ReceiverPort {
    private ws?: WebSocket;
    private channels = new Map<number, ReceiverChannel>();
    private lastChannelsAt = 0;
    private nextId = 1;
    private readonly pending = new Map<number, Pending>();
    private stopped = false;
    private retryMs = 2_000;

    constructor(
        private readonly url: string,
        private readonly log: (msg: string) => void = console.error,
    ) {}

    static urlFor(host: string, port: number, token: string): string {
        const h = host.includes(":") ? `[${host}]` : host;
        return `ws://${h}:${port}/ws${token ? `?token=${encodeURIComponent(token)}` : ""}`;
    }

    start(): void {
        this.stopped = false;
        this.open();
    }

    stop(): void {
        this.stopped = true;
        this.ws?.close();
    }

    connected(): boolean {
        return this.ws?.readyState === WebSocket.OPEN && Date.now() - this.lastChannelsAt < 5_000;
    }

    channel(ch: number): ReceiverChannel | undefined {
        return this.connected() ? this.channels.get(ch) : undefined;
    }

    call(method: string, params: Record<string, unknown>): Promise<unknown> {
        const ws = this.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN) return Promise.reject(new Error("denpachan の受信口に繋がっていません"));
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`denpachan が応答しません (${method})`));
            }, 10_000);
            this.pending.set(id, { resolve, reject, timer });
            ws.send(JSON.stringify({ id, method, params }));
        });
    }

    /** 届いたメッセージを処理する (試験からも呼ぶ) */
    handle(text: string): void {
        let msg: Record<string, unknown>;
        try {
            msg = JSON.parse(text) as Record<string, unknown>;
        } catch {
            return;
        }
        if (msg.event === "channels" && Array.isArray(msg.channels)) {
            const next = new Map<number, ReceiverChannel>();
            for (const raw of msg.channels) {
                const c = normalizeChannel(raw);
                if (c) next.set(c.ch, c);
            }
            this.channels = next;
            this.lastChannelsAt = Date.now();
            return;
        }
        if (typeof msg.id === "number") {
            const p = this.pending.get(msg.id);
            if (!p) return;
            this.pending.delete(msg.id);
            clearTimeout(p.timer);
            if (msg.error !== undefined) p.reject(new Error(String(msg.error)));
            else p.resolve(msg.result);
        }
    }

    private open(): void {
        if (this.stopped) return;
        let ws: WebSocket;
        try {
            ws = new WebSocket(this.url);
        } catch (e) {
            this.log(`denpachan: ${e instanceof Error ? e.message : String(e)}`);
            this.retry();
            return;
        }
        this.ws = ws;
        ws.addEventListener("open", () => {
            this.retryMs = 2_000;
            this.log("denpachan: 受信口に繋がりました");
        });
        ws.addEventListener("message", (ev) => {
            if (typeof ev.data === "string") this.handle(ev.data);
        });
        ws.addEventListener("close", () => {
            for (const [id, p] of this.pending) {
                clearTimeout(p.timer);
                p.reject(new Error("denpachan の受信口が切れました"));
                this.pending.delete(id);
            }
            this.retry();
        });
        ws.addEventListener("error", () => {
            // close も来るので、そちらで繋ぎ直す
        });
    }

    private retry(): void {
        if (this.stopped) return;
        setTimeout(() => this.open(), this.retryMs);
        this.retryMs = Math.min(this.retryMs * 2, 30_000);
    }
}

// ---- engine の channels の形を、bot の形にそろえる ----
// engine は、値の無い数を null、当てはまらないかたまり (video、latency、tcp、audio、record) を null で送り、
// lastError はメッセージのオブジェクト ({"id": ...}) で送る。bot の側では undefined と文字列にそろえる。

type Obj = Record<string, unknown>;

function obj(v: unknown): Obj | undefined {
    return v !== null && typeof v === "object" && !Array.isArray(v) ? (v as Obj) : undefined;
}

function num(v: unknown): number | undefined {
    return typeof v === "number" && Number.isFinite(v) ? v : undefined;
}

function str(v: unknown): string | undefined {
    return typeof v === "string" ? v : undefined;
}

/** {"id": "...", ...} のメッセージを 1 行の文字列にする */
export function messageText(v: unknown): string {
    if (typeof v === "string") return v;
    const o = obj(v);
    if (!o) return "";
    const { id, ...rest } = o;
    const args = Object.keys(rest).length > 0 ? ` ${JSON.stringify(rest)}` : "";
    return `${typeof id === "string" ? id : ""}${args}`;
}

export function normalizeChannel(raw: unknown): ReceiverChannel | undefined {
    const c = obj(raw);
    const ch = num(c?.ch);
    if (!c || ch === undefined) return undefined;
    const source = obj(c.source) ?? {};
    const video = obj(c.video);
    const latency = obj(c.latency);
    const tcp = obj(c.tcp);
    const audio = obj(c.audio);
    const record = obj(c.record);
    const peak = Array.isArray(audio?.peakDb) ? (audio.peakDb as unknown[]).map(num) : [];
    return {
        ch,
        name: str(c.name) ?? `CH${ch}`,
        enabled: c.enabled === true,
        testSlot: c.testSlot === true,
        status: str(c.status) ?? "unknown",
        source: { protocol: str(source.protocol) ?? "", mode: str(source.mode) ?? "", peer: str(source.peer), connectedSec: num(source.connectedSec) },
        video: video && {
            width: num(video.width),
            height: num(video.height),
            fps: num(video.fps),
            codec: str(video.codec),
            bitrateMbps: num(video.bitrateMbps),
            decodeErrors: num(video.decodeErrors),
        },
        latency: latency && { rttMs: num(latency.rttMs), drops: num(latency.drops), verdict: str(latency.verdict) },
        tcp: tcp && { rttMs: num(tcp.rttMs), retransBytes: num(tcp.retransBytes) },
        audio: audio && {
            peakDb: peak.length === 2 && peak[0] !== undefined && peak[1] !== undefined ? [peak[0], peak[1]] : undefined,
            gaps: num(audio.gaps),
            silentSec: num(audio.silentSec),
        },
        record: record && { state: str(record.state), file: str(record.file) || undefined, seconds: num(record.seconds) },
        lastError: messageText(c.lastError),
    };
}
