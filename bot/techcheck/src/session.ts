// テストのセッション、待ちの列、枠の型。すべて JSON にして保存できる形にする (bot を起動し直しても続きから)。

import type { Advice } from "./advisor.js";
import type { CheckVerdict } from "./checks.js";
import type { Lang } from "./config.js";

export type Phase =
    | "preparing" // フローの開始、denpachan のチャンネルと録画の開始
    | "waitingStream" // 初回の案内を出して、配信の開始を待つ
    | "measuring" // 計測の窓を溜めている
    | "confirming" // 安定の確定
    | "awaitingLatency" // latency の変更を頼んで、始め直すのを待つ
    | "awaitingRtmp" // RTMP への切り替えを頼んで、届くのを待つ
    | "checks" // 音声とその他のチェック
    | "awaitingDone" // 「完了」を待つ
    | "finished";

export interface StepRecord {
    id: string;
    verdict: CheckVerdict;
    value?: number;
    /** 録画の中の区間 (秒) */
    recFrom?: number;
    recTo?: number;
    /** 「おかしい」のまま配信者が先に進めた */
    skipped?: boolean;
}

export interface Pending {
    kind: "latency" | "rtmp";
    target?: number;
    askedAt: number;
    messageId?: string;
    pressed: boolean;
    reconnectedAt?: number;
    reminded: boolean;
}

export interface Session {
    id: string;
    userId: string;
    displayName: string;
    lang: Lang;
    threadId: string;
    createdAt: number;
    phase: Phase;
    phaseSince: number;
    lastActivity: number;
    reminded: boolean;
    protocol: "srt" | "rtmp";
    srt: { flowName: string; flowArn: string; domain: string; channel: number; startedFlow: boolean; released: boolean };
    rtmp?: { channel: number };
    latencyMs: number;
    raises: number;
    recordStartAt?: number;
    recordFiles: string[];
    connectedAt?: number;
    measureFromSec?: number;
    confirmFromSec?: number;
    lastAdvice?: Advice;
    prevDrops?: number;
    pending?: Pending;
    rx: {
        lastStatus?: string;
        lastConnectedSec?: number;
        droppedAt?: number;
        reconnectAt?: number;
        decodeErrorsBase?: number;
        retransBase?: number;
        fps: number[];
        silenceAlerted: boolean;
        opsWarned: boolean;
    };
    cw: { lastUpTime?: number; reconnectAt?: number; missingWarned: boolean };
    disconnects: number;
    metadataChecked: boolean;
    streamFps?: number;
    checkIndex: number;
    checkState: "prompt" | "running" | "result";
    checkStartedAt?: number;
    checkRecFrom?: number;
    checkPeaks: number[];
    results: StepRecord[];
    noiseDb?: number;
    videoVerdict?: "OK" | "RTMP" | "NG";
    confirmedLatencyMs?: number;
    summary: Summary;
    memo?: string;
    abortReason?: string;
}

export interface Summary {
    rttMaxMs?: number;
    lossPct?: number;
    drops?: number;
    jitterMaxMs?: number;
    bitrateKbps?: number;
    fpsAvg?: number;
    codec?: string;
    width?: number;
    height?: number;
    decodeErrors?: number;
    opsRttMs?: number;
    opsDrops?: number;
    recommendMs?: number;
}

export interface QueueEntry {
    userId: string;
    displayName: string;
    lang: Lang;
    joinedAt: number;
    skips: number;
}

export interface QueueState {
    entries: QueueEntry[];
    offer?: { userId: string; offeredAt: number };
}

export interface SlotState {
    srt: { flow?: string; channel?: number; enabled: boolean };
    rtmp: { channel?: number; enabled: boolean };
}

export interface FinishedRecord {
    sessionId: string;
    sheetRange?: string;
    threadUrl: string;
    at: number;
}
