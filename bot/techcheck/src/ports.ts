// bot の外とのやりとりの口。本物 (src/adapters/、src/discord/) と試験用の偽物を差し替えられるようにする。

import type { SrtSample } from "./advisor.js";

// ---- MediaConnect / CloudWatch ----

export interface FlowInfo {
    arn: string;
    /** ACTIVE / STANDBY / STARTING / STOPPING / UPDATING / ERROR */
    status: string;
    ingestIp?: string;
    ingestPort?: number;
    /** source のプロトコル (srt-listener など) */
    protocol?: string;
    minLatencyMs?: number;
}

export interface FlowMetadata {
    width?: number;
    height?: number;
    fps?: number;
    codec?: string;
    hasAudio: boolean;
}

export interface FlowPort {
    describe(arn: string): Promise<FlowInfo>;
    start(arn: string): Promise<void>;
    stop(arn: string): Promise<void>;
    /** [fromSec, toSec) の 1 秒ごとのメトリクス (t は epoch 秒) */
    metrics(arn: string, fromSec: number, toSec: number): Promise<SrtSample[]>;
    metadata(arn: string): Promise<FlowMetadata | undefined>;
    thumbnail(arn: string): Promise<Buffer | undefined>;
}

// ---- denpachan のメトリクスの受信口 (docs/stream-test-automation.md §3.2 の channels メッセージ) ----

export interface ReceiverChannel {
    ch: number;
    name: string;
    enabled: boolean;
    testSlot: boolean;
    status: string;
    source: { protocol: string; mode: string; peer?: string; connectedSec?: number };
    video?: { width?: number; height?: number; fps?: number; codec?: string; bitrateMbps?: number; decodeErrors?: number };
    latency?: { rttMs?: number; drops?: number; verdict?: string } | null;
    tcp?: { rttMs?: number; retransBytes?: number } | null;
    audio?: { peakDb?: [number, number]; gaps?: number; silentSec?: number };
    record?: { state?: string; file?: string; seconds?: number };
    lastError?: string;
}

export interface ReceiverPort {
    /** 受信口に繋がっていて、最近 channels が届いているか */
    connected(): boolean;
    channel(ch: number): ReceiverChannel | undefined;
    call(method: string, params: Record<string, unknown>): Promise<unknown>;
}

// ---- DNS ----

export interface DnsPort {
    resolve(domain: string): Promise<string[]>;
}

// ---- Discord ----

export type ButtonStyle = "primary" | "secondary" | "success" | "danger";

export interface Button {
    /** customId。bot が解釈する (src/discord/bot.ts) */
    id: string;
    label: string;
    style?: ButtonStyle;
}

export interface OutMessage {
    content: string;
    buttons?: Button[];
    image?: { name: string; data: Buffer };
}

export interface ChatPort {
    /** 配信テストのチャンネルにプライベートスレッドを作り、配信者を招待する。スレッドの ID を返す */
    createThread(name: string, userId: string): Promise<string>;
    post(threadId: string, msg: OutMessage): Promise<string>;
    /** 送ったメッセージのボタンを外す (押したあと) */
    clearButtons(threadId: string, messageId: string): Promise<void>;
    closeThread(threadId: string): Promise<void>;
    threadUrl(threadId: string): string;
    /** 配信テストのチャンネルでメンションし、DM も送る (待ちの列の順番の知らせ) */
    callUser(userId: string, msg: OutMessage): Promise<void>;
    /** 運営への通知のチャンネル (日本語) */
    ops(text: string): Promise<void>;
}

// ---- シート、webhook ----

export interface SheetPort {
    /** 1 行足し、その行の場所 (A1 形式) を返す */
    append(row: (string | number)[]): Promise<string>;
    /** 行の一部を書き換える (後日の音声の判定) */
    update(range: string, values: (string | number)[]): Promise<void>;
    url(): string;
}

export interface WebhookPort {
    /** URL が設定されていなければ何もしない */
    send(payload: unknown): Promise<void>;
}

export interface Clock {
    now(): number;
}
