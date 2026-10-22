// 配信テストの進行 (docs/stream-test-automation.md §5〜§7)。
// 1 秒ごとの tick() で各テストを進め、Discord の操作 (コマンド、ボタン、モーダル) を受ける。
// 外とのやりとりは ports.ts の口を通すので、試験では偽物に差し替えられる。

import { randomUUID } from "node:crypto";
import { type Advice, type SrtSample, advise, nextLatency } from "./advisor.js";
import { evaluateAudioStep } from "./checks.js";
import type { CheckStep, Config, Lang } from "./config.js";
import type { I18n } from "./i18n.js";
import { langFromLocale } from "./i18n.js";
import type { Button, ChatPort, Clock, DnsPort, FlowPort, OutMessage, ReceiverChannel, ReceiverPort, SheetPort, WebhookPort } from "./ports.js";
import { audioVerdictColumn, buildRow, buildWebhookPayload, columnLetter, formatRecPos, formatTime } from "./record.js";
import type { FinishedRecord, Phase, QueueState, Session, SlotState } from "./session.js";
import type { KeyValueStore } from "./store.js";

export interface Deps {
    config: Config;
    i18n: I18n;
    store: KeyValueStore;
    flow: FlowPort;
    rx: ReceiverPort;
    dns: DnsPort;
    chat: ChatPort;
    sheet: SheetPort;
    webhook: WebhookPort;
    clock: Clock;
    log?: (msg: string) => void;
    /** フローが ACTIVE になるのを待つ間の待ち (試験では差し替える) */
    sleep?: (ms: number) => Promise<void>;
}

export interface UserRef {
    id: string;
    displayName: string;
}

/** ボタンを押したときの返事。modal があれば Discord の側でモーダルを出す */
export interface ButtonReply {
    ephemeral?: string;
    modal?: { kind: "done" | "abort"; sessionId: string; lang: Lang };
}

const kFlowStartWaitMs = 4 * 60_000;
const kCwPollMs = 10_000;
const kCwLookbackSec = 240;
const kCwMissingGraceSec = 120;
const kWarmupSec = 5;
const kOpsWarnAfterMs = 60_000;
const kAssumePressedMs = 30_000;
const kReconnectLiveMs = 30_000;
const kSenderDecodeErrors = 5;

export class TechcheckService {
    private readonly cfg: Config;
    private readonly checks: CheckStep[];
    private sessions: Session[];
    private queue: QueueState;
    private slots: SlotState;
    private langPrefs: Record<string, Lang>;
    private finished: Record<string, FinishedRecord>;
    private readonly samples = new Map<string, Map<number, SrtSample>>();
    private readonly lastCwFetch = new Map<string, number>();
    private chain: Promise<unknown> = Promise.resolve();
    private ticking = false;

    constructor(private readonly d: Deps) {
        this.cfg = d.config;
        this.checks = d.config.checks;
        this.sessions = d.store.get<Session[]>("sessions") ?? [];
        this.queue = d.store.get<QueueState>("queue") ?? { entries: [] };
        this.slots = d.store.get<SlotState>("slots") ?? {
            srt: { flow: d.config.slots.srt.flow, channel: d.config.slots.srt.channel, enabled: true },
            rtmp: { channel: d.config.slots.rtmp.channel, enabled: true },
        };
        this.langPrefs = d.store.get<Record<string, Lang>>("langPrefs") ?? {};
        this.finished = d.store.get<Record<string, FinishedRecord>>("finished") ?? {};
    }

    // ================= 外から呼ぶもの =================

    /** 1 秒ごとに呼ぶ。前の tick が終わっていなければ飛ばす */
    async tick(): Promise<void> {
        if (this.ticking) return;
        this.ticking = true;
        try {
            await this.locked(async () => {
                await this.tickQueue();
                for (const s of [...this.sessions]) {
                    try {
                        await this.tickSession(s);
                    } catch (e) {
                        this.log(`tick ${s.displayName}: ${errorText(e)}`);
                    }
                }
                this.save();
            });
        } finally {
            this.ticking = false;
        }
    }

    /** /techcheck start */
    async start(user: UserRef, channelId: string, langOption: Lang | undefined, locale: string | undefined): Promise<string> {
        return this.locked(async () => {
            const lang = langOption ?? this.langPrefs[user.id] ?? langFromLocale(locale, this.cfg.i18n.fallback);
            if (langOption) this.langPrefs[user.id] = langOption;
            if (channelId !== this.cfg.discord.techcheckChannelId) return this.t(lang, "start.wrongChannel", { channel: `<#${this.cfg.discord.techcheckChannelId}>` });
            const running = this.sessions.find((s) => s.userId === user.id);
            if (running) return this.t(running.lang, "start.alreadyRunning", { url: this.d.chat.threadUrl(running.threadId) });
            const pos = this.queue.entries.findIndex((e) => e.userId === user.id);
            if (pos >= 0) return this.t(lang, "start.alreadyQueued", { position: pos + 1 });

            if (this.queue.entries.length === 0 && this.srtAvailable()) {
                const r = await this.begin(user, lang);
                this.save();
                return r;
            }
            this.queue.entries.push({ userId: user.id, displayName: user.displayName, lang, joinedAt: this.now(), skips: 0 });
            this.save();
            const position = this.queue.entries.length;
            return this.slots.srt.enabled ? this.t(lang, "queue.joined", { position }) : this.t(lang, "queue.paused", { position });
        });
    }

    /** /techcheck leave */
    async leave(userId: string): Promise<string> {
        return this.locked(async () => {
            const i = this.queue.entries.findIndex((e) => e.userId === userId);
            const lang = this.langPrefs[userId] ?? this.cfg.i18n.fallback;
            if (i < 0) return this.t(lang, "queue.notQueued");
            this.queue.entries.splice(i, 1);
            if (this.queue.offer?.userId === userId) this.queue.offer = undefined;
            this.save();
            return this.t(lang, "queue.left");
        });
    }

    /** 待ちの列の知らせの「始める」 */
    async claim(user: UserRef): Promise<string> {
        return this.locked(async () => {
            const entry = this.queue.entries.find((e) => e.userId === user.id);
            const lang = entry?.lang ?? this.langPrefs[user.id] ?? this.cfg.i18n.fallback;
            if (!entry || this.queue.offer?.userId !== user.id) return this.t(lang, "queue.notYourTurn");
            if (!this.srtAvailable()) return this.t(lang, "queue.notYourTurn");
            this.queue.entries = this.queue.entries.filter((e) => e.userId !== user.id);
            this.queue.offer = undefined;
            const r = await this.begin({ id: user.id, displayName: user.displayName || entry.displayName }, entry.lang);
            this.save();
            return r;
        });
    }

    /** スレッドの中のボタン。id は `tc:<sessionId>:<action>` */
    async button(actorId: string, isStaff: boolean, customId: string): Promise<ButtonReply> {
        return this.locked(async () => {
            const [, sessionId, action] = customId.split(":");
            const s = this.sessions.find((x) => x.id === sessionId);
            if (!s) return { ephemeral: this.t(this.langPrefs[actorId] ?? this.cfg.i18n.fallback, "button.expired") };
            if (actorId !== s.userId && !isStaff) return { ephemeral: this.t(s.lang, "button.notYours") };
            s.lastActivity = this.now();
            const reply = await this.onButton(s, action ?? "");
            this.save();
            return reply;
        });
    }

    /** 「完了」のモーダルの送信 */
    async complete(sessionId: string, memo: string): Promise<void> {
        await this.locked(async () => {
            const s = this.sessions.find((x) => x.id === sessionId);
            if (!s || s.phase !== "awaitingDone") return;
            s.memo = memo.trim() || undefined;
            await this.finish(s, "done");
            this.save();
        });
    }

    /** 「中断する」のモーダルの送信 */
    async abortByUser(sessionId: string, reason: string): Promise<void> {
        await this.locked(async () => {
            const s = this.sessions.find((x) => x.id === sessionId);
            if (!s) return;
            await this.finish(s, "aborted", `配信者が中断: ${reason.trim() || "理由なし"}`);
            this.save();
        });
    }

    /** 進行中のテストの配信者 (無ければ undefined) */
    sessionOwner(sessionId: string): string | undefined {
        return this.sessions.find((s) => s.id === sessionId)?.userId;
    }

    sessionLang(sessionId: string): Lang {
        return this.sessions.find((s) => s.id === sessionId)?.lang ?? this.cfg.i18n.fallback;
    }

    // ---- 運営のコマンド (返事は日本語) ----

    async slotShow(): Promise<string> {
        return this.locked(async () => {
            const srtUser = this.sessions.find((s) => s.protocol === "srt" && !s.srt.released);
            const rtmpUser = this.sessions.find((s) => s.rtmp && s.phase !== "finished");
            const q = this.queue.entries.map((e, i) => `${i + 1}. ${e.displayName}${this.queue.offer?.userId === e.userId ? " (知らせ済み)" : ""}`);
            return [
                `SRT の枠: フロー ${this.slots.srt.flow ?? "未設定"} / ch${this.slots.srt.channel ?? "?"} / ${this.slots.srt.enabled ? "受付中" : "止めている"}${srtUser ? ` / 使用中: ${srtUser.displayName}` : ""}`,
                `RTMP の枠: ch${this.slots.rtmp.channel ?? "?"} / ${this.slots.rtmp.enabled ? "受付中" : "止めている"}${rtmpUser ? ` / 使用中: ${rtmpUser.displayName}` : ""}`,
                `待ちの列: ${q.length > 0 ? `\n${q.join("\n")}` : "なし"}`,
            ].join("\n");
        });
    }

    async slotSet(kind: "srt" | "rtmp", channel: number, flowName?: string): Promise<string> {
        return this.locked(async () => {
            const problems = await this.validateSlot(kind, channel, flowName);
            if (problems.length > 0) return `設定し直せませんでした (前の設定のまま):\n${problems.map((p) => `- ${p}`).join("\n")}`;
            const busy = kind === "srt" ? this.sessions.some((s) => s.protocol === "srt" && !s.srt.released) : this.sessions.some((s) => s.rtmp);
            if (kind === "srt") this.slots.srt = { ...this.slots.srt, flow: flowName, channel };
            else this.slots.rtmp = { ...this.slots.rtmp, channel };
            this.save();
            const note = busy ? "\n進行中のテストは前の枠のまま続けます。新しい枠は次のテストから使います。" : "";
            return `${kind.toUpperCase()} の枠を設定しました: ${kind === "srt" ? `フロー ${flowName} / ` : ""}ch${channel}${note}`;
        });
    }

    async slotEnable(kind: "srt" | "rtmp", on: boolean): Promise<string> {
        return this.locked(async () => {
            this.slots[kind].enabled = on;
            this.save();
            return `${kind.toUpperCase()} の枠を${on ? "再開しました" : "止めました (待ちの列は残します)"}`;
        });
    }

    async status(userId?: string): Promise<string> {
        return this.locked(async () => {
            const list = userId ? this.sessions.filter((s) => s.userId === userId) : this.sessions;
            if (list.length === 0) return "進行中のテストはありません";
            return list.map((s) => this.describeSession(s)).join("\n\n");
        });
    }

    async shot(userId: string): Promise<{ text: string; image?: Buffer }> {
        const s = this.sessions.find((x) => x.userId === userId);
        if (!s) return { text: "その配信者のテストは進行中ではありません" };
        if (s.protocol !== "srt") return { text: "RTMP では静止画を取れません (MediaConnect を通らないため)" };
        const image = await this.d.flow.thumbnail(s.srt.flowArn).catch(() => undefined);
        return image ? { text: `${s.displayName} の静止画`, image } : { text: "静止画を取れませんでした (フローのサムネイルが有効か確かめてください)" };
    }

    async setLatency(userId: string, ms: number): Promise<string> {
        return this.locked(async () => {
            const s = this.sessions.find((x) => x.userId === userId);
            if (!s) return "その配信者のテストは進行中ではありません";
            if (s.protocol !== "srt") return "RTMP のテストでは latency を変えられません";
            await this.askLatency(s, Math.min(Math.max(ms, 20), this.cfg.latency.maxMs));
            this.save();
            return `${s.displayName} に latency ${ms} ms を案内しました`;
        });
    }

    async setReceiverLatency(channel: number, ms: number): Promise<string> {
        try {
            await this.d.rx.call("patchChannel", { ch: channel, patch: { source: { latencyMs: ms } } });
            return `denpachan の ch${channel} の latency を ${ms} ms にしました`;
        } catch (e) {
            return `変えられませんでした: ${errorText(e)}`;
        }
    }

    async forceRtmp(userId: string): Promise<string> {
        return this.locked(async () => {
            const s = this.sessions.find((x) => x.userId === userId);
            if (!s) return "その配信者のテストは進行中ではありません";
            if (s.protocol === "rtmp" || s.pending?.kind === "rtmp") return "すでに RTMP です";
            await this.goRtmp(s, "運営の指示");
            this.save();
            return `${s.displayName} を RTMP に案内しました`;
        });
    }

    async cancel(userId: string): Promise<string> {
        return this.locked(async () => {
            const s = this.sessions.find((x) => x.userId === userId);
            if (!s) return "その配信者のテストは進行中ではありません";
            await this.finish(s, "aborted", "運営が中止");
            this.save();
            return `${s.displayName} のテストを中止しました`;
        });
    }

    async review(userId: string, ok: boolean, memo: string): Promise<string> {
        const rec = this.finished[userId];
        if (!rec?.sheetRange) return "その配信者の記録が見つかりません (シートに書けていない可能性があります)";
        const row = /(\d+)(?::[A-Z]+\d+)?$/.exec(rec.sheetRange)?.[1];
        if (!row) return `シートの行が分かりません (${rec.sheetRange})`;
        const tab = rec.sheetRange.includes("!") ? `${rec.sheetRange.split("!")[0]}!` : "";
        const from = columnLetter(audioVerdictColumn);
        const to = columnLetter(audioVerdictColumn + 1);
        await this.d.sheet.update(`${tab}${from}${row}:${to}${row}`, [ok ? "OK" : "NG", memo]);
        return `音声の判定を書きました (${ok ? "OK" : "NG"})`;
    }

    // ================= テストの始まり =================

    private srtAvailable(): boolean {
        return (
            this.slots.srt.enabled &&
            this.slots.srt.flow !== undefined &&
            this.slots.srt.channel !== undefined &&
            !this.sessions.some((s) => s.protocol === "srt" && !s.srt.released)
        );
    }

    private rtmpAvailable(): boolean {
        return this.slots.rtmp.enabled && this.slots.rtmp.channel !== undefined && !this.sessions.some((s) => s.rtmp);
    }

    private async begin(user: UserRef, lang: Lang): Promise<string> {
        const flowCfg = this.cfg.flows.find((f) => f.name === this.slots.srt.flow);
        const channel = this.slots.srt.channel;
        const problems: string[] = [];
        if (!flowCfg || channel === undefined) problems.push("SRT の枠が設定されていません");
        else {
            const ch = this.d.rx.channel(channel);
            if (!this.d.rx.connected()) problems.push("denpachan の受信口に繋がっていません");
            else if (!ch) problems.push(`denpachan に ch${channel} がありません`);
            else {
                if (!ch.testSlot) problems.push(`ch${channel} に「配信テスト用」の印がありません`);
                if (ch.status !== "stopped") problems.push(`ch${channel} が止まっていません (${ch.status})`);
            }
        }
        if (problems.length > 0 || !flowCfg || channel === undefined) {
            await this.blockSlot(problems);
            this.queue.entries.unshift({ userId: user.id, displayName: user.displayName, lang, joinedAt: this.now(), skips: 0 });
            return this.t(lang, "start.prepareProblem");
        }

        const now = this.now();
        const name = `配信テスト ${formatTime(now)} ${user.displayName}`.slice(0, 100);
        const threadId = await this.d.chat.createThread(name, user.id);
        const s: Session = {
            id: randomUUID(),
            userId: user.id,
            displayName: user.displayName,
            lang,
            threadId,
            createdAt: now,
            phase: "preparing",
            phaseSince: now,
            lastActivity: now,
            reminded: false,
            protocol: "srt",
            srt: { flowName: flowCfg.name, flowArn: flowCfg.arn, domain: flowCfg.domain, channel, startedFlow: false, released: false },
            latencyMs: this.cfg.latency.initialMs,
            raises: 0,
            recordFiles: [],
            rx: { fps: [], silenceAlerted: false, opsWarned: false },
            cw: { missingWarned: false },
            disconnects: 0,
            metadataChecked: false,
            checkIndex: 0,
            checkState: "prompt",
            checkPeaks: [],
            results: [],
            summary: {},
        };
        this.sessions.push(s);
        this.langPrefs[user.id] = lang;
        await this.post(s, { content: this.t(lang, "prepare.preparing") });
        await this.d.chat.ops(`配信テスト開始: ${user.displayName} (フロー ${flowCfg.name} / ch${channel}) ${this.d.chat.threadUrl(threadId)}`);
        // フローの開始は 1 分ほどかかるので、ロックの外で待つ
        void this.prepare(s.id);
        return this.t(lang, "start.threadCreated", { url: this.d.chat.threadUrl(threadId) });
    }

    /** フローを開始し (止まっていれば)、ドメインを確かめ、denpachan のチャンネルと録画を始める */
    private async prepare(sessionId: string): Promise<void> {
        const get = () => this.sessions.find((x) => x.id === sessionId);
        const s0 = get();
        if (!s0) return;
        const flowCfg = this.cfg.flows.find((f) => f.name === s0.srt.flowName)!;
        let startedFlow = false;
        try {
            let info = await this.d.flow.describe(flowCfg.arn);
            if (info.status !== "ACTIVE") {
                if (info.status !== "STARTING") {
                    await this.d.flow.start(flowCfg.arn);
                    startedFlow = true;
                }
                const until = this.now() + kFlowStartWaitMs;
                while (info.status !== "ACTIVE") {
                    if (this.now() > until) throw new Error(`フローが ACTIVE になりません (${info.status})`);
                    await (this.d.sleep ?? sleep)(5_000);
                    info = await this.d.flow.describe(flowCfg.arn);
                }
            }
            if (!info.ingestIp || !info.ingestPort) throw new Error("フローの ingest の IP / ポートが分かりません");
            const ips = await this.d.dns.resolve(flowCfg.domain).catch(() => [] as string[]);
            if (!ips.includes(info.ingestIp)) {
                throw new Error(`ドメイン ${flowCfg.domain} がこのフローの ingest の IP (${info.ingestIp}) を指していません (${ips.join(", ") || "引けない"})`);
            }
            await this.d.rx.call("startChannel", { ch: s0.srt.channel });
            await this.d.rx.call("setRecording", { ch: s0.srt.channel, enabled: true });
            await this.locked(async () => {
                const s = get();
                if (!s) return;
                s.srt.startedFlow = startedFlow;
                s.recordStartAt = this.now();
                await this.postGuide(s, info.ingestPort!);
                this.save();
            });
        } catch (e) {
            await this.locked(async () => {
                const s = get();
                if (!s) return;
                s.srt.startedFlow = startedFlow;
                await this.blockSlot([errorText(e)]);
                await this.post(s, { content: this.t(s.lang, "start.prepareProblem") });
                await this.finish(s, "aborted", `準備の失敗: ${errorText(e)}`);
                // 配信者のせいではないので、列の先頭に戻す
                this.queue.entries.unshift({ userId: s.userId, displayName: s.displayName, lang: s.lang, joinedAt: this.now(), skips: 0 });
                this.save();
            });
        }
    }

    /** 枠に問題があったとき: 運営に知らせ、枠を止める (次の人も同じ問題に当たらないように) */
    private async blockSlot(problems: string[]): Promise<void> {
        this.slots.srt.enabled = false;
        await this.d.chat.ops(`SRT の枠に問題があるので止めました。直してから \`/techcheck slot on srt\` で再開してください:\n${problems.map((p) => `- ${p}`).join("\n")}`);
    }

    private srtUrl(s: Session, port: number, latencyMs: number): string {
        const f = this.cfg.flows.find((x) => x.name === s.srt.flowName)!;
        return `srt://${f.domain}:${port}?mode=caller&latency=${latencyMs * 1000}&passphrase=${encodeURIComponent(f.passphrase)}&pbkeylen=${f.pbkeylen}`;
    }

    private srtPort = new Map<string, number>();

    private async postGuide(s: Session, port: number): Promise<void> {
        this.srtPort.set(s.id, port);
        const e = this.cfg.guide.expected;
        const other: Lang = s.lang === "ja" ? "en" : "ja";
        await this.post(s, {
            content: this.t(s.lang, "guide.first", {
                url: this.srtUrl(s, port, s.latencyMs),
                profile: this.cfg.guide.obsProfile,
                width: e.width,
                height: e.height,
                fps: e.fps,
                bitrate: e.bitrateKbps,
                keyframe: e.keyframeSec,
            }),
            buttons: [
                this.btn(s, "started", "button.started", "primary"),
                this.btn(s, "abort", "button.abort", "danger"),
                { id: `tc:${s.id}:lang-${other}`, label: other === "en" ? "English" : "日本語", style: "secondary" },
            ],
        });
        this.setPhase(s, "waitingStream");
    }

    // ================= 1 秒ごとの進行 =================

    private async tickQueue(): Promise<void> {
        const q = this.queue;
        const claimMs = this.cfg.timeouts.claimMin * 60_000;
        if (q.offer && this.now() - q.offer.offeredAt > claimMs) {
            const i = q.entries.findIndex((e) => e.userId === q.offer!.userId);
            q.offer = undefined;
            if (i >= 0) {
                const [entry] = q.entries.splice(i, 1);
                entry!.skips++;
                if (entry!.skips >= 2) {
                    await this.d.chat.callUser(entry!.userId, { content: this.t(entry!.lang, "queue.removed") }).catch(() => undefined);
                } else {
                    q.entries.push(entry!);
                }
            }
        }
        if (!q.offer && q.entries.length > 0 && this.srtAvailable()) {
            const head = q.entries[0]!;
            q.offer = { userId: head.userId, offeredAt: this.now() };
            await this.d.chat
                .callUser(head.userId, {
                    content: this.t(head.lang, "queue.yourTurn", { user: `<@${head.userId}>`, minutes: this.cfg.timeouts.claimMin }),
                    buttons: [{ id: "tcq:claim", label: this.t(head.lang, "button.claim"), style: "primary" }],
                })
                .catch((e) => this.log(`queue notify: ${errorText(e)}`));
        }
    }

    private async tickSession(s: Session): Promise<void> {
        if (s.phase === "preparing" || s.phase === "finished") return;
        const rxCh = this.activeChannel(s);
        this.observeReceiver(s, rxCh);
        if (s.protocol === "srt" && !s.srt.released) await this.pollCloudWatch(s);
        await this.silenceAlert(s, rxCh);

        switch (s.phase) {
            case "waitingStream":
                return this.tickWaiting(s, rxCh);
            case "measuring":
                return s.protocol === "srt" ? this.tickMeasuringSrt(s, rxCh) : this.tickMeasuringRtmp(s, rxCh);
            case "confirming":
                return this.tickConfirming(s, rxCh);
            case "awaitingLatency":
            case "awaitingRtmp":
                return this.tickPending(s);
            case "checks":
                return this.tickChecks(s, rxCh);
            case "awaitingDone":
                return this.idleTimeout(s);
        }
    }

    private activeChannel(s: Session): ReceiverChannel | undefined {
        const ch = s.protocol === "rtmp" && s.rtmp ? s.rtmp.channel : s.srt.channel;
        return this.d.rx.channel(ch);
    }

    /** denpachan の状態から、繋ぎ直し・途絶を見る */
    private observeReceiver(s: Session, ch: ReceiverChannel | undefined): void {
        if (!ch) return;
        const now = this.now();
        const live = ch.status === "live";
        if (s.rx.lastStatus === "live" && !live) s.rx.droppedAt = now;
        if (live && s.rx.droppedAt !== undefined) {
            s.rx.reconnectAt = now;
            s.rx.droppedAt = undefined;
            if (s.phase !== "waitingStream") s.disconnects++;
        }
        const sec = ch.source.connectedSec;
        if (sec !== undefined && s.rx.lastConnectedSec !== undefined && sec + 2 < s.rx.lastConnectedSec && live) s.rx.reconnectAt = now;
        s.rx.lastStatus = ch.status;
        s.rx.lastConnectedSec = sec;
        if (live && ch.video?.fps !== undefined && (s.phase === "measuring" || s.phase === "confirming")) {
            s.rx.fps.push(ch.video.fps);
            if (s.rx.fps.length > 600) s.rx.fps.shift();
        }
    }

    private async pollCloudWatch(s: Session): Promise<void> {
        const now = this.now();
        if (now - (this.lastCwFetch.get(s.id) ?? 0) < kCwPollMs) return;
        this.lastCwFetch.set(s.id, now);
        const toSec = Math.floor(now / 1000);
        const fromSec = Math.max(Math.floor(s.createdAt / 1000), toSec - kCwLookbackSec);
        let got: SrtSample[];
        try {
            got = await this.d.flow.metrics(s.srt.flowArn, fromSec, toSec);
        } catch (e) {
            this.log(`CloudWatch: ${errorText(e)}`);
            return;
        }
        const map = this.samples.get(s.id) ?? new Map<number, SrtSample>();
        for (const x of got) map.set(x.t, x);
        for (const t of map.keys()) if (t < toSec - 900) map.delete(t);
        this.samples.set(s.id, map);
        // SourceUpTime が戻った = 繋ぎ直した
        const latest = [...map.values()].filter((x) => x.upTimeSec !== undefined).sort((a, b) => a.t - b.t);
        for (const x of latest) {
            if (s.cw.lastUpTime !== undefined && x.upTimeSec! + 2 < s.cw.lastUpTime && x.t * 1000 > (s.cw.reconnectAt ?? 0)) s.cw.reconnectAt = x.t * 1000;
            s.cw.lastUpTime = x.upTimeSec;
        }
    }

    private window(s: Session, fromSec: number | undefined): SrtSample[] {
        if (fromSec === undefined) return [];
        const map = this.samples.get(s.id);
        if (!map) return [];
        return [...map.values()].filter((x) => x.t >= fromSec).sort((a, b) => a.t - b.t);
    }

    private mcConnectedFor(s: Session, ms: number): boolean {
        const w = this.window(s, Math.floor((this.now() - ms) / 1000));
        return w.length > 0 && w.every((x) => x.connected);
    }

    private async silenceAlert(s: Session, ch: ReceiverChannel | undefined): Promise<void> {
        if (!ch || ch.status !== "live") return;
        const step = this.checks[s.checkIndex];
        const quietStep = s.phase === "checks" && (step?.kind === "silence" || step?.kind === "quiet");
        const silent = (ch.audio?.silentSec ?? 0) > this.cfg.judge.silentAlertSec;
        if (silent && !s.rx.silenceAlerted && !quietStep && ["measuring", "confirming", "checks"].includes(s.phase)) {
            s.rx.silenceAlerted = true;
            await this.post(s, { content: this.t(s.lang, "audio.silent") });
        } else if (!silent && (ch.audio?.silentSec ?? 0) === 0) {
            s.rx.silenceAlerted = false;
        }
    }

    // ---- 配信の開始を待つ ----

    private async tickWaiting(s: Session, ch: ReceiverChannel | undefined): Promise<void> {
        const now = this.now();
        const rxLive = ch?.status === "live";
        const mcConnected = this.mcConnectedFor(s, 3_000);
        const connected = this.d.rx.connected() ? rxLive : mcConnected;
        if (connected) {
            s.connectedAt = now;
            await this.onConnected(s);
            return;
        }
        if (this.d.rx.connected() && !rxLive && this.mcConnectedFor(s, kOpsWarnAfterMs) && !s.rx.opsWarned) {
            s.rx.opsWarned = true;
            await this.d.chat.ops(`運営側の問題の可能性: ${s.displayName} の配信は MediaConnect に届いているのに、denpachan の ch${s.srt.channel} が live になりません (${ch?.status ?? "不明"} ${ch?.lastError ?? ""})`);
            await this.post(s, { content: this.t(s.lang, "wait.opsChecking") });
        }
        const elapsed = now - s.phaseSince;
        if (!s.reminded && elapsed > this.timeout(this.cfg.timeouts.connectRemindMin)) {
            s.reminded = true;
            await this.post(s, { content: this.t(s.lang, "wait.remind") });
        }
        if (elapsed > this.timeout(this.cfg.timeouts.connectAbortMin)) await this.finish(s, "aborted", "配信が届かないまま時間切れ");
    }

    private async onConnected(s: Session): Promise<void> {
        const image = s.protocol === "srt" ? await this.d.flow.thumbnail(s.srt.flowArn).catch(() => undefined) : undefined;
        await this.post(s, { content: this.t(s.lang, "wait.arrived"), image: image ? { name: "stream.jpg", data: image } : undefined });
        if (s.protocol === "srt" && !s.metadataChecked) {
            s.metadataChecked = true;
            const md = await this.d.flow.metadata(s.srt.flowArn).catch(() => undefined);
            if (md) {
                s.streamFps = md.fps;
                s.summary.codec = md.codec;
                s.summary.width = md.width;
                s.summary.height = md.height;
                const e = this.cfg.guide.expected;
                const diffs: string[] = [];
                if (md.width && md.height && (md.width !== e.width || md.height !== e.height)) diffs.push(`${md.width}x${md.height}`);
                if (md.fps && Math.abs(md.fps - e.fps) > 1) diffs.push(`${md.fps} fps`);
                if (!md.hasAudio) diffs.push(this.t(s.lang, "metadata.noAudio"));
                if (diffs.length > 0) {
                    await this.post(s, { content: this.t(s.lang, "metadata.mismatch", { actual: diffs.join(", "), width: e.width, height: e.height, fps: e.fps }) });
                }
            }
        }
        this.startMeasuring(s, Math.floor(this.now() / 1000) + kWarmupSec);
    }

    private startMeasuring(s: Session, fromSec: number): void {
        s.measureFromSec = fromSec;
        s.rx.fps = [];
        const ch = this.activeChannel(s);
        s.rx.decodeErrorsBase = ch?.video?.decodeErrors;
        s.rx.retransBase = ch?.tcp?.retransBytes;
        this.setPhase(s, "measuring");
    }

    // ---- 映像の判定 (SRT) ----

    private async tickMeasuringSrt(s: Session, ch: ReceiverChannel | undefined): Promise<void> {
        const nowSec = Math.floor(this.now() / 1000);
        const w = this.window(s, s.measureFromSec);
        const elapsed = nowSec - (s.measureFromSec ?? nowSec);
        const cwMissing = w.length === 0 && elapsed > this.cfg.judge.windowSec + kCwMissingGraceSec;
        if (w.length < this.cfg.judge.windowSec && !cwMissing) return;
        if (elapsed < this.cfg.judge.windowSec) return;

        if (cwMissing && !s.cw.missingWarned) {
            s.cw.missingWarned = true;
            await this.d.chat.ops(`CloudWatch のメトリクスが取れません (${s.srt.flowName})。${s.displayName} の映像は denpachan の受信状態だけで判定します`);
        }
        const advice: Advice = cwMissing
            ? { verdict: "ok", recommendMs: 0, headroomMs: null, rttMs: 0, lossPct: 0, drops: 0, multiplier: 0, windowSec: 0 }
            : advise(w.slice(-this.cfg.judge.windowSec), s.latencyMs, this.cfg.judge.multipliers);
        s.lastAdvice = advice;
        this.updateSummary(s, w, advice, ch);

        const jitterBad = w.length > 0 && w.filter((x) => (x.jitterMs ?? 0) > this.cfg.judge.maxJitterMs).length > w.length / 2;
        if (this.senderProblem(s, ch) || jitterBad) {
            await this.goRtmp(s, jitterBad ? "ジッタが大きい" : "デコードが安定しない");
            return;
        }
        if (advice.verdict === "measuring") return;
        if (advice.verdict === "tight" || advice.verdict === "insufficient") {
            await this.raise(s, advice);
            return;
        }
        s.confirmFromSec = nowSec;
        s.rx.fps = [];
        s.rx.decodeErrorsBase = ch?.video?.decodeErrors;
        this.setPhase(s, "confirming");
    }

    private senderProblem(s: Session, ch: ReceiverChannel | undefined): boolean {
        if (!ch || !this.d.rx.connected()) return false;
        const expected = s.streamFps ?? this.cfg.guide.expected.fps;
        const fpsAvg = avg(s.rx.fps);
        const decodeDelta = (ch.video?.decodeErrors ?? 0) - (s.rx.decodeErrorsBase ?? ch.video?.decodeErrors ?? 0);
        return (fpsAvg !== undefined && fpsAvg < expected * this.cfg.judge.minFpsRatio) || decodeDelta >= kSenderDecodeErrors;
    }

    private async tickConfirming(s: Session, ch: ReceiverChannel | undefined): Promise<void> {
        const nowSec = Math.floor(this.now() / 1000);
        const from = s.confirmFromSec ?? nowSec;
        if (nowSec - from < this.cfg.judge.confirmSec) return;
        const w = this.window(s, from);
        // CloudWatch は遅れて届くので、窓の 8 割が揃うまで (最大 90 秒) 待つ
        if (s.protocol === "srt" && !s.cw.missingWarned && w.length < this.cfg.judge.confirmSec * 0.8 && nowSec - from < this.cfg.judge.confirmSec + 90) return;
        const drops = sum(w.map((x) => x.notRecovered ?? 0));
        const broken = sum(w.map((x) => (x.continuityErrors ?? 0) + (x.tsSyncLoss ?? 0)));
        const expected = s.streamFps ?? this.cfg.guide.expected.fps;
        const fpsAvg = avg(s.rx.fps);
        const decodeDelta = ch?.video?.decodeErrors !== undefined && s.rx.decodeErrorsBase !== undefined ? ch.video.decodeErrors - s.rx.decodeErrorsBase : 0;
        const fpsOk = fpsAvg === undefined || fpsAvg >= expected * this.cfg.judge.minFpsRatio;
        if (drops === 0 && broken === 0 && decodeDelta === 0 && fpsOk) {
            await this.confirmed(s);
            return;
        }
        const advice: Advice = { ...(s.lastAdvice ?? advise([], s.latencyMs, this.cfg.judge.multipliers)), verdict: "insufficient", drops };
        if (!fpsOk || decodeDelta >= kSenderDecodeErrors) {
            await this.goRtmp(s, "デコードが安定しない");
            return;
        }
        await this.raise(s, advice);
    }

    private async confirmed(s: Session): Promise<void> {
        s.confirmedLatencyMs = s.latencyMs;
        s.videoVerdict = s.protocol === "rtmp" ? "RTMP" : "OK";
        s.summary.fpsAvg = round1(avg(s.rx.fps));
        const ch = this.activeChannel(s);
        if (ch?.video?.decodeErrors !== undefined) s.summary.decodeErrors = ch.video.decodeErrors;
        if (s.protocol === "srt") {
            const generous = s.lastAdvice?.verdict === "generous" && s.lastAdvice.recommendMs > 0;
            await this.post(s, {
                content:
                    this.t(s.lang, "judge.ok", { latencyMs: s.latencyMs }) +
                    (generous ? `\n${this.t(s.lang, "judge.generous", { recommendMs: s.lastAdvice!.recommendMs })}` : ""),
            });
            await this.d.chat.ops(`映像 OK: ${s.displayName} (SRT、latency ${s.latencyMs} ms)`);
        } else {
            await this.post(s, { content: this.t(s.lang, "judge.okRtmp") });
            await this.d.chat.ops(`映像 OK: ${s.displayName} (RTMP)`);
        }
        await this.startChecks(s);
    }

    // ---- latency を上げてもらう ----

    private async raise(s: Session, advice: Advice): Promise<void> {
        const max = this.cfg.latency.maxMs;
        if (s.raises >= this.cfg.latency.maxRaises || s.latencyMs >= max) {
            await this.goRtmp(s, "latency を上げても直らない");
            return;
        }
        const target = nextLatency(s.latencyMs, advice.recommendMs, max);
        if (target <= s.latencyMs) {
            await this.goRtmp(s, "latency を上げても直らない");
            return;
        }
        s.prevDrops = advice.drops;
        await this.askLatency(s, target);
    }

    private async askLatency(s: Session, target: number): Promise<void> {
        let port = this.srtPort.get(s.id);
        if (port === undefined) {
            port = (await this.d.flow.describe(s.srt.flowArn)).ingestPort ?? 0;
            this.srtPort.set(s.id, port);
        }
        const msgId = await this.post(s, {
            content: this.t(s.lang, "latency.ask", { latencyMs: target, latencyUs: target * 1000, url: this.srtUrl(s, port, target) }),
            buttons: [this.btn(s, "set", "button.set", "primary"), this.btn(s, "abort", "button.abort", "danger")],
        });
        s.pending = { kind: "latency", target, askedAt: this.now(), messageId: msgId, pressed: false, reminded: false };
        this.setPhase(s, "awaitingLatency");
    }

    // ---- RTMP へ ----

    private async goRtmp(s: Session, reason: string): Promise<void> {
        if (!this.rtmpAvailable()) {
            s.videoVerdict = "NG";
            await this.d.chat.ops(`RTMP の枠が使えません。${s.displayName} の映像が安定しません (${reason})。連絡してください ${this.d.chat.threadUrl(s.threadId)}`);
            await this.post(s, { content: this.t(s.lang, "rtmp.unavailable") });
            await this.startChecks(s);
            return;
        }
        const channel = this.slots.rtmp.channel!;
        s.rtmp = { channel };
        try {
            await this.d.rx.call("startChannel", { ch: channel });
            await this.d.rx.call("setRecording", { ch: channel, enabled: true });
        } catch (e) {
            this.log(`RTMP の枠の開始: ${errorText(e)}`);
        }
        const r = this.cfg.rtmp;
        const msgId = await this.post(s, {
            content: this.t(s.lang, "rtmp.ask", { url: `rtmp://${r.domain}/${r.app}`, key: r.key }),
            buttons: [this.btn(s, "set", "button.set", "primary"), this.btn(s, "abort", "button.abort", "danger")],
        });
        s.pending = { kind: "rtmp", askedAt: this.now(), messageId: msgId, pressed: false, reminded: false };
        await this.d.chat.ops(`RTMP に案内: ${s.displayName} (${reason})`);
        this.setPhase(s, "awaitingRtmp");
    }

    private async tickPending(s: Session): Promise<void> {
        const p = s.pending;
        if (!p) return;
        const now = this.now();
        if (p.kind === "latency") {
            if (p.reconnectedAt === undefined) {
                const at = Math.max(s.rx.reconnectAt ?? 0, s.cw.reconnectAt ?? 0);
                if (at > p.askedAt) p.reconnectedAt = at;
            }
            if (p.reconnectedAt !== undefined && (p.pressed || now - p.reconnectedAt > kAssumePressedMs)) {
                s.latencyMs = p.target!;
                s.raises++;
                if (p.messageId) await this.d.chat.clearButtons(s.threadId, p.messageId).catch(() => undefined);
                s.pending = undefined;
                await this.post(s, { content: this.t(s.lang, "latency.remeasure", { latencyMs: s.latencyMs }) });
                this.startMeasuring(s, Math.floor(p.reconnectedAt / 1000) + kWarmupSec);
                return;
            }
        } else {
            const rtmpCh = s.rtmp ? this.d.rx.channel(s.rtmp.channel) : undefined;
            if (rtmpCh?.status === "live") {
                if (p.messageId) await this.d.chat.clearButtons(s.threadId, p.messageId).catch(() => undefined);
                s.pending = undefined;
                s.protocol = "rtmp";
                await this.releaseSrt(s);
                await this.post(s, { content: this.t(s.lang, "rtmp.arrived") });
                s.rx.lastStatus = "live";
                this.startMeasuring(s, Math.floor(now / 1000));
                return;
            }
        }
        if (!p.reminded && now - p.askedAt > this.timeout(this.cfg.timeouts.raiseRemindMin)) {
            p.reminded = true;
            await this.post(s, { content: this.t(s.lang, p.kind === "latency" ? "latency.remind" : "rtmp.remind") });
        }
        if (now - p.askedAt > this.timeout(this.cfg.timeouts.stepAbortMin)) {
            await this.finish(s, "aborted", p.kind === "latency" ? "latency の変更を待って時間切れ" : "RTMP への切り替えを待って時間切れ");
        }
    }

    // ---- 映像の判定 (RTMP) ----

    private async tickMeasuringRtmp(s: Session, ch: ReceiverChannel | undefined): Promise<void> {
        const nowSec = Math.floor(this.now() / 1000);
        if (nowSec - (s.measureFromSec ?? nowSec) < this.cfg.judge.windowSec) return;
        const expected = this.cfg.guide.expected.fps;
        const fpsAvg = avg(s.rx.fps);
        const decodeDelta = (ch?.video?.decodeErrors ?? 0) - (s.rx.decodeErrorsBase ?? ch?.video?.decodeErrors ?? 0);
        const live = ch?.status === "live";
        s.summary.opsRttMs = ch?.tcp?.rttMs;
        if (live && (fpsAvg === undefined || fpsAvg >= expected * this.cfg.judge.minFpsRatio) && decodeDelta === 0) {
            await this.confirmed(s);
            return;
        }
        s.videoVerdict = "NG";
        await this.d.chat.ops(`RTMP でも安定しません: ${s.displayName} (fps ${round1(fpsAvg) ?? "?"}、デコードエラー +${decodeDelta})。連絡してください ${this.d.chat.threadUrl(s.threadId)}`);
        await this.post(s, { content: this.t(s.lang, "rtmp.unstable") });
        await this.startChecks(s);
    }

    // ---- 音声とその他のチェック ----

    private async startChecks(s: Session): Promise<void> {
        s.checkIndex = 0;
        this.setPhase(s, "checks");
        await this.post(s, { content: this.t(s.lang, "checks.intro", { count: this.checks.length }) });
        await this.promptStep(s);
    }

    private async promptStep(s: Session): Promise<void> {
        const step = this.checks[s.checkIndex];
        if (!step) {
            await this.checksDone(s);
            return;
        }
        s.checkState = "prompt";
        s.checkPeaks = [];
        s.lastActivity = this.now();
        const header = this.t(s.lang, "checks.header", { n: s.checkIndex + 1, total: this.checks.length });
        if (step.kind === "confirm") {
            const image = s.protocol === "srt" ? await this.d.flow.thumbnail(s.srt.flowArn).catch(() => undefined) : undefined;
            await this.post(s, {
                content: `${header}\n${step.text[s.lang]}`,
                image: image ? { name: "stream.jpg", data: image } : undefined,
                buttons: [this.btn(s, "yes", "button.yes", "success"), this.btn(s, "no", "button.no", "danger")],
            });
            s.checkRecFrom = this.recPos(s);
            return;
        }
        await this.post(s, {
            content: `${header}\n${step.text[s.lang]}`,
            buttons: [this.btn(s, "step-start", "button.stepStart", "primary")],
        });
    }

    private async tickChecks(s: Session, ch: ReceiverChannel | undefined): Promise<void> {
        const step = this.checks[s.checkIndex];
        if (!step) return;
        const now = this.now();
        if (s.checkState !== "running") return this.idleTimeout(s);
        const elapsed = now - (s.checkStartedAt ?? now);
        if (step.kind === "reconnect") {
            const ok = (s.rx.reconnectAt ?? 0) > (s.checkStartedAt ?? now) || (s.cw.reconnectAt ?? 0) > (s.checkStartedAt ?? now);
            const live = !this.d.rx.connected() || ch?.status === "live";
            if (ok && live) return this.stepResult(s, step, { verdict: "ok" });
            if (elapsed > step.seconds * 1000 + kReconnectLiveMs) return this.stepResult(s, step, { verdict: "ng" });
            return;
        }
        // 押してから 2 秒は手を動かす時間なので使わない
        const peak = ch?.audio?.peakDb ? Math.max(ch.audio.peakDb[0], ch.audio.peakDb[1]) : undefined;
        if (peak !== undefined && elapsed >= 2_000 && ch?.status === "live") s.checkPeaks.push(peak);
        if (elapsed >= (step.seconds + 2) * 1000) {
            const r = evaluateAudioStep(step, this.d.rx.connected() ? s.checkPeaks : [], s.noiseDb, this.cfg.judge.silentDb);
            if (step.kind === "silence" && r.value !== undefined) s.noiseDb = r.value;
            return this.stepResult(s, step, r);
        }
    }

    private async stepResult(s: Session, step: CheckStep, r: { verdict: "ok" | "ng" | "unknown"; value?: number }): Promise<void> {
        const rec = { id: step.id, verdict: r.verdict, value: r.value, recFrom: s.checkRecFrom, recTo: this.recPos(s) };
        s.results = s.results.filter((x) => x.id !== step.id);
        s.results.push(rec);
        s.checkState = "result";
        s.lastActivity = this.now();
        if (r.verdict === "ng") {
            await this.post(s, {
                content: `${this.t(s.lang, "checks.ng")}\n${step.failHint[s.lang]}`,
                buttons: [this.btn(s, "step-retry", "button.retry", "primary"), this.btn(s, "step-next", "button.next", "secondary")],
            });
            return;
        }
        await this.post(s, { content: this.t(s.lang, r.verdict === "ok" ? "checks.ok" : "checks.recorded") });
        s.checkIndex++;
        await this.promptStep(s);
    }

    private async checksDone(s: Session): Promise<void> {
        const lines = s.results.map((r) => `- ${r.id}: ${this.t(s.lang, `verdict.${r.verdict}`)}${r.skipped ? ` (${this.t(s.lang, "checks.skipped")})` : ""}`);
        await this.post(s, {
            content: `${this.t(s.lang, "checks.done")}\n${lines.join("\n")}`,
            buttons: [this.btn(s, "done", "button.done", "success"), this.btn(s, "abort", "button.abort", "danger")],
        });
        if (s.results.some((r) => r.skipped)) {
            await this.d.chat.ops(`チェックで「おかしい」のまま進めた手順があります: ${s.displayName} (${s.results.filter((r) => r.skipped).map((r) => r.id).join(", ")})`);
        }
        this.setPhase(s, "awaitingDone");
    }

    private async idleTimeout(s: Session): Promise<void> {
        if (this.now() - s.lastActivity > this.timeout(this.cfg.timeouts.stepAbortMin)) {
            await this.finish(s, "aborted", "操作が無いまま時間切れ");
        }
    }

    // ---- ボタン ----

    private async onButton(s: Session, action: string): Promise<ButtonReply> {
        switch (action) {
            case "started": {
                const ch = this.activeChannel(s);
                if (s.phase === "waitingStream" && ch?.status !== "live") return { ephemeral: this.t(s.lang, "wait.notYet") };
                return { ephemeral: this.t(s.lang, "button.ack") };
            }
            case "lang-ja":
            case "lang-en": {
                s.lang = action === "lang-ja" ? "ja" : "en";
                this.langPrefs[s.userId] = s.lang;
                if (s.phase === "waitingStream") {
                    const port = this.srtPort.get(s.id) ?? (await this.d.flow.describe(s.srt.flowArn)).ingestPort ?? 0;
                    await this.postGuide(s, port);
                }
                return {};
            }
            case "set": {
                if (!s.pending) return { ephemeral: this.t(s.lang, "button.ack") };
                s.pending.pressed = true;
                return { ephemeral: this.t(s.lang, s.pending.kind === "latency" ? "latency.waitRestart" : "rtmp.waitArrive") };
            }
            case "step-start": {
                if (s.phase !== "checks" || s.checkState !== "prompt") return { ephemeral: this.t(s.lang, "button.ack") };
                const step = this.checks[s.checkIndex]!;
                s.checkState = "running";
                s.checkStartedAt = this.now();
                s.checkRecFrom = this.recPos(s);
                s.checkPeaks = [];
                await this.post(s, { content: this.t(s.lang, "checks.running", { seconds: step.seconds }) });
                return {};
            }
            case "yes":
            case "no": {
                const step = this.checks[s.checkIndex];
                if (s.phase !== "checks" || step?.kind !== "confirm" || s.checkState !== "prompt") return { ephemeral: this.t(s.lang, "button.ack") };
                await this.stepResult(s, step, { verdict: action === "yes" ? "ok" : "ng" });
                return {};
            }
            case "step-retry": {
                if (s.phase !== "checks" || s.checkState !== "result") return { ephemeral: this.t(s.lang, "button.ack") };
                await this.promptStep(s);
                return {};
            }
            case "step-next": {
                if (s.phase !== "checks" || s.checkState !== "result") return { ephemeral: this.t(s.lang, "button.ack") };
                const rec = s.results.find((r) => r.id === this.checks[s.checkIndex]?.id);
                if (rec) rec.skipped = true;
                s.checkIndex++;
                await this.promptStep(s);
                return {};
            }
            case "done":
                if (s.phase !== "awaitingDone") return { ephemeral: this.t(s.lang, "button.ack") };
                return { modal: { kind: "done", sessionId: s.id, lang: s.lang } };
            case "abort":
                return { modal: { kind: "abort", sessionId: s.id, lang: s.lang } };
            default:
                return { ephemeral: this.t(s.lang, "button.expired") };
        }
    }

    // ================= 終わり =================

    private async finish(s: Session, result: "done" | "aborted", reason?: string): Promise<void> {
        if (s.phase === "finished") return;
        this.setPhase(s, "finished");
        if (reason) s.abortReason = reason;
        const ch = this.activeChannel(s);
        if (ch?.record?.file) this.addRecordFile(s, ch.record.file);
        await this.releaseSrt(s);
        if (s.rtmp) await this.stopChannel(s.rtmp.channel);
        this.sessions = this.sessions.filter((x) => x.id !== s.id);
        this.samples.delete(s.id);
        this.lastCwFetch.delete(s.id);
        this.srtPort.delete(s.id);

        const threadUrl = this.d.chat.threadUrl(s.threadId);
        let sheetRange: string | undefined;
        let sheetError: string | undefined;
        try {
            sheetRange = await this.d.sheet.append(buildRow(s, this.checks, result, threadUrl));
        } catch (e) {
            sheetError = errorText(e);
            this.log(`シート: ${sheetError}`);
        }
        await this.post(s, { content: this.closingMessage(s, result) }).catch(() => undefined);
        try {
            await this.d.webhook.send(buildWebhookPayload({ session: s, checks: this.checks, result, threadUrl, sheetUrl: this.d.sheet.url(), sheetError }));
        } catch (e) {
            this.log(`webhook: ${errorText(e)}`);
        }
        await this.d.chat.closeThread(s.threadId).catch((e) => this.log(`スレッドを閉じる: ${errorText(e)}`));
        this.finished[s.userId] = { sessionId: s.id, sheetRange, threadUrl, at: this.now() };
    }

    private closingMessage(s: Session, result: "done" | "aborted"): string {
        const lines = [this.t(s.lang, result === "done" ? "finish.summaryTitle" : "finish.abortedTitle")];
        lines.push(this.t(s.lang, "finish.protocol", { protocol: s.protocol.toUpperCase() }));
        if (s.protocol === "srt" && s.confirmedLatencyMs) lines.push(this.t(s.lang, "finish.latency", { latencyMs: s.confirmedLatencyMs }));
        for (const r of s.results) {
            const range = r.recFrom !== undefined ? ` ${formatRecPos(r.recFrom)}-${formatRecPos(r.recTo)}` : "";
            lines.push(`- ${r.id}: ${this.t(s.lang, `verdict.${r.verdict}`)}${range}`);
        }
        lines.push(this.t(s.lang, "finish.bye"));
        return lines.join("\n");
    }

    /** SRT の枠を空ける: 録画とチャンネルを止め、bot が開始したフローなら止める */
    private async releaseSrt(s: Session): Promise<void> {
        if (s.srt.released) return;
        s.srt.released = true;
        const ch = this.d.rx.channel(s.srt.channel);
        if (ch?.record?.file) this.addRecordFile(s, ch.record.file);
        await this.stopChannel(s.srt.channel);
        if (s.srt.startedFlow) {
            await this.d.flow.stop(s.srt.flowArn).catch((e) => this.log(`StopFlow: ${errorText(e)}`));
        }
    }

    private async stopChannel(channel: number): Promise<void> {
        await this.d.rx.call("setRecording", { ch: channel, enabled: false }).catch((e) => this.log(`setRecording: ${errorText(e)}`));
        await this.d.rx.call("stopChannel", { ch: channel }).catch((e) => this.log(`stopChannel: ${errorText(e)}`));
    }

    // ================= 枠の検証 (§5.6) =================

    private async validateSlot(kind: "srt" | "rtmp", channel: number, flowName?: string): Promise<string[]> {
        const problems: string[] = [];
        if (!this.d.rx.connected()) return ["denpachan の受信口に繋がっていません"];
        const ch = this.d.rx.channel(channel);
        if (!ch) return [`denpachan に ch${channel} がありません`];
        if (!ch.testSlot) problems.push(`ch${channel} に「配信テスト用」の印がありません`);
        const usedBySession = this.sessions.some((s) => (s.srt.channel === channel && !s.srt.released) || s.rtmp?.channel === channel);
        if (ch.status !== "stopped" && !usedBySession) problems.push(`ch${channel} が止まっていません (${ch.status})`);
        if (kind === "srt" && ch.source.protocol !== "srt") problems.push(`ch${channel} の受信が SRT ではありません (${ch.source.protocol})`);
        if (kind === "rtmp" && (ch.source.protocol !== "rtmp" || ch.source.mode !== "pull")) {
            problems.push(`ch${channel} の受信が RTMP の pull ではありません (${ch.source.protocol} ${ch.source.mode})`);
        }
        if (kind === "srt") {
            const f = this.cfg.flows.find((x) => x.name === flowName);
            if (!f) return [...problems, `フローの組 "${flowName}" が設定にありません`];
            try {
                const info = await this.d.flow.describe(f.arn);
                if (info.protocol && info.protocol !== "srt-listener") problems.push(`フローの source が SRT listener ではありません (${info.protocol})`);
                if (info.minLatencyMs !== undefined && info.minLatencyMs > this.cfg.latency.initialMs) {
                    problems.push(`フローの MinLatency (${info.minLatencyMs} ms) が案内の初期値 (${this.cfg.latency.initialMs} ms) より大きい`);
                }
                if (info.status === "ACTIVE" && info.ingestIp) {
                    const ips = await this.d.dns.resolve(f.domain).catch(() => [] as string[]);
                    if (!ips.includes(info.ingestIp)) problems.push(`ドメイン ${f.domain} がこのフローの ingest の IP (${info.ingestIp}) を指していません`);
                }
            } catch (e) {
                problems.push(`フローを読めません: ${errorText(e)}`);
            }
        } else {
            const ips = await this.d.dns.resolve(this.cfg.rtmp.domain).catch(() => [] as string[]);
            if (ips.length === 0) problems.push(`RTMP サーバのドメイン ${this.cfg.rtmp.domain} が引けません`);
        }
        return problems;
    }

    // ================= 小物 =================

    private describeSession(s: Session): string {
        const a = s.lastAdvice;
        const step = this.checks[s.checkIndex];
        return [
            `${s.displayName}: ${s.phase}${s.phase === "checks" && step ? ` (${step.id})` : ""} / ${s.protocol.toUpperCase()} / latency ${s.latencyMs} ms / 上げた回数 ${s.raises}`,
            a ? `推奨 ${a.recommendMs || "計測中"} ms / RTT 最大 ${a.rttMs} ms / ロス ${a.lossPct}% / 回復できなかった ${a.drops} / 判定 ${a.verdict}` : "計測前",
            this.d.chat.threadUrl(s.threadId),
        ].join("\n");
    }

    private updateSummary(s: Session, w: SrtSample[], advice: Advice, ch: ReceiverChannel | undefined): void {
        const m = s.summary;
        if (advice.recommendMs) m.recommendMs = advice.recommendMs;
        m.rttMaxMs = Math.max(m.rttMaxMs ?? 0, advice.rttMs);
        m.lossPct = advice.lossPct;
        m.drops = (m.drops ?? 0) + advice.drops;
        const jit = w.map((x) => x.jitterMs ?? 0);
        if (jit.length > 0) m.jitterMaxMs = Math.max(m.jitterMaxMs ?? 0, ...jit);
        const br = avg(w.map((x) => x.bitrateBps ?? 0).filter((x) => x > 0));
        if (br !== undefined) m.bitrateKbps = Math.round(br / 1000);
        if (ch?.latency) {
            m.opsRttMs = ch.latency.rttMs;
            m.opsDrops = ch.latency.drops;
        }
    }

    private recPos(s: Session): number | undefined {
        const ch = this.activeChannel(s);
        if (ch?.record?.seconds !== undefined && ch.record.state === "recording") return ch.record.seconds;
        return s.recordStartAt ? (this.now() - s.recordStartAt) / 1000 : undefined;
    }

    private addRecordFile(s: Session, file: string): void {
        if (!s.recordFiles.includes(file)) s.recordFiles.push(file);
    }

    private setPhase(s: Session, phase: Phase): void {
        s.phase = phase;
        s.phaseSince = this.now();
        s.lastActivity = this.now();
        s.reminded = false;
    }

    /** 待っている人がいれば半分にする (枠が 1 つなので) */
    private timeout(minutes: number): number {
        const half = this.cfg.timeouts.halveWhenQueued && this.queue.entries.length > 0;
        return (minutes * 60_000) / (half ? 2 : 1);
    }

    private btn(s: Session, action: string, labelKey: string, style: Button["style"]): Button {
        return { id: `tc:${s.id}:${action}`, label: this.t(s.lang, labelKey), style };
    }

    private async post(s: Session, msg: OutMessage): Promise<string> {
        return this.d.chat.post(s.threadId, msg);
    }

    private t(lang: Lang, key: string, vars: Record<string, string | number> = {}): string {
        return this.d.i18n.t(lang, key, vars);
    }

    private now(): number {
        return this.d.clock.now();
    }

    private log(msg: string): void {
        (this.d.log ?? console.error)(msg);
    }

    private save(): void {
        this.d.store.set("sessions", this.sessions);
        this.d.store.set("queue", this.queue);
        this.d.store.set("slots", this.slots);
        this.d.store.set("langPrefs", this.langPrefs);
        this.d.store.set("finished", this.finished);
    }

    /** 操作を 1 つずつ順に行う (tick と Discord の操作が混ざらないように) */
    private locked<T>(fn: () => Promise<T>): Promise<T> {
        const run = this.chain.then(fn, fn);
        this.chain = run.catch(() => undefined);
        return run;
    }
}

function sleep(ms: number): Promise<void> {
    return new Promise((r) => setTimeout(r, ms));
}

function avg(xs: number[]): number | undefined {
    return xs.length === 0 ? undefined : xs.reduce((a, b) => a + b, 0) / xs.length;
}

function sum(xs: number[]): number {
    return xs.reduce((a, b) => a + b, 0);
}

function round1(v: number | undefined): number | undefined {
    return v === undefined ? undefined : Math.round(v * 10) / 10;
}

export function errorText(e: unknown): string {
    return e instanceof Error ? e.message : String(e);
}
