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
            for (const c of msg.channels as ReceiverChannel[]) next.set(c.ch, c);
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
