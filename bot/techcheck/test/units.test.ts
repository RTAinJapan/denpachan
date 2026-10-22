import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { describe, it } from "node:test";
import { parseFrameRate } from "../src/adapters/aws.js";
import { DiscordWebhook, retryAfterMs } from "../src/adapters/webhook.js";
import { advise, multiplierFor, nextLatency, verdictFor, type SrtSample } from "../src/advisor.js";
import { defaultChecks, evaluateAudioStep } from "../src/checks.js";
import { ConfigError, expandEnv, parseConfig } from "../src/config.js";
import { I18n, checkParity, langFromLocale } from "../src/i18n.js";
import { buildRow, columnLetter, formatRecPos, sheetColumns } from "../src/record.js";
import { baseYaml, goodSample, here, messagesDir, testConfig } from "./helpers.js";

const table = testConfig().judge.multipliers;

describe("advisor (srt_advisor.h と同じ規則)", () => {
    it("ロス率で倍率が決まる", () => {
        assert.equal(multiplierFor(0, table), 3);
        assert.equal(multiplierFor(1, table), 3);
        assert.equal(multiplierFor(2, table), 4);
        assert.equal(multiplierFor(8, table), 8);
        assert.equal(multiplierFor(50, table), 10);
    });

    it("10 秒分そろうまでは計測中", () => {
        const w = Array.from({ length: 9 }, (_, i) => goodSample(i));
        assert.equal(advise(w, 120, table).verdict, "measuring");
    });

    it("RTT の最大 × 倍率を 10 ms 単位に切り上げる", () => {
        const w: SrtSample[] = Array.from({ length: 60 }, (_, i) => ({ ...goodSample(i), rttMs: i === 30 ? 41 : 20 }));
        const a = advise(w, 120, table);
        assert.equal(a.recommendMs, 130); // 41 × 3 = 123 → 130
        assert.equal(a.verdict, "tight");
        assert.equal(a.headroomMs, -10);
        assert.equal(advise(w, 300, table).verdict, "generous");
        assert.equal(advise(w, 200, table).verdict, "ok");
    });

    it("回復できなかったパケットがあれば足りない (今の 1.5 倍以上)", () => {
        const w: SrtSample[] = Array.from({ length: 60 }, (_, i) => ({ ...goodSample(i), notRecovered: i === 10 ? 3 : 0 }));
        const a = advise(w, 200, table);
        assert.equal(a.verdict, "insufficient");
        assert.equal(a.recommendMs, 300);
        assert.equal(a.drops, 3);
    });

    it("ロス率は受信数とロス率から戻して求める", () => {
        const w: SrtSample[] = Array.from({ length: 60 }, (_, i) => ({ ...goodSample(i), lossPct: 5 }));
        const a = advise(w, 120, table);
        assert.ok(Math.abs(a.lossPct - 5) < 0.01);
        assert.equal(a.multiplier, 6);
    });

    it("下限 20 ms", () => {
        const w: SrtSample[] = Array.from({ length: 20 }, (_, i) => ({ ...goodSample(i), rttMs: 1 }));
        assert.equal(advise(w, 120, table).recommendMs, 20);
    });

    it("次の latency は max(推奨, 今 × 1.5) で、上限で抑える", () => {
        assert.equal(nextLatency(120, 130, 2000), 180);
        assert.equal(nextLatency(120, 400, 2000), 400);
        assert.equal(nextLatency(1500, 0, 2000), 2000);
        assert.equal(verdictFor(120, 0, 0), "measuring");
    });
});

describe("checks (音声の参考の判定)", () => {
    const step = (id: string) => defaultChecks.find((s) => s.id === id)!;
    it("sound: 8 割以上の秒で -40 dB 以上なら OK", () => {
        assert.equal(evaluateAudioStep(step("game-audio"), Array(10).fill(-20), undefined, -60).verdict, "ok");
        assert.equal(evaluateAudioStep(step("game-audio"), [...Array(7).fill(-20), -70, -70, -70], undefined, -60).verdict, "ng");
    });
    it("silence: -60 dB 未満なら OK で、最大をノイズとして返す", () => {
        assert.deepEqual(evaluateAudioStep(step("silence"), [-80, -75, -72], undefined, -60), { verdict: "ok", value: -72 });
        assert.equal(evaluateAudioStep(step("silence"), [-80, -30], undefined, -60).verdict, "ng");
    });
    it("quiet: ノイズ (下限 -60 dB) + 6 dB を超えたら載っている", () => {
        assert.equal(evaluateAudioStep(step("mic"), [-70, -58], -72, -60).verdict, "ok"); // 基準 -60 + 6 = -54
        assert.equal(evaluateAudioStep(step("mic"), [-70, -30], -72, -60).verdict, "ng");
    });
    it("レベルが無ければ判定しない", () => {
        assert.equal(evaluateAudioStep(step("mic"), [], -72, -60).verdict, "unknown");
    });
});

describe("config", () => {
    it("${ENV} を環境変数で置き換え、無いものを返す", () => {
        assert.deepEqual(expandEnv("a ${X} b ${Y}", { X: "1" }), { text: "a 1 b ", missing: ["Y"] });
    });
    it("既定値が入る", () => {
        const c = testConfig();
        assert.equal(c.latency.initialMs, 120);
        assert.equal(c.latency.maxMs, 2000);
        assert.equal(c.flows[0]!.pbkeylen, 16);
        assert.equal(c.i18n.fallback, "en");
        assert.equal(c.guide.expected.fps, 60);
        assert.equal(c.checks.length, defaultChecks.length);
    });
    it("webhook の URL は省ける", () => {
        const c = parseConfig(baseYaml.replace(/notify:\n {2}webhookUrl: .*\n/, ""), {});
        assert.equal(c.notify.webhookUrl, "");
    });
    it("必須の項目が無ければ、場所と環境変数の名前を出す", () => {
        assert.throws(
            () => parseConfig(baseYaml.replace("token: t", "token: ${DISCORD_TOKEN}"), {}),
            (e: unknown) => e instanceof ConfigError && /discord\.token/.test(e.message) && /DISCORD_TOKEN/.test(e.message),
        );
    });
    it("slots.srt.flow は flows にある名前", () => {
        assert.throws(() => parseConfig(baseYaml.replace("flow: flow-a", "flow: nope"), {}), /slots\.srt\.flow/);
    });
    it("quiet の手順の前に silence が要る", () => {
        const yaml = `${baseYaml}checks:\n  - { id: q, kind: quiet, seconds: 5, text: { ja: a, en: a }, failHint: { ja: b, en: b } }\n`;
        assert.throws(() => parseConfig(yaml, {}), /silence/);
    });
    it("denpachan を loopback 以外にするならトークンが要る", () => {
        assert.throws(() => parseConfig(`${baseYaml}denpachan: { host: 192.168.1.5 }\n`, {}), /denpachan\.token/);
    });
});

describe("i18n", () => {
    it("ja と en のキーと差し込みがそろっている", () => {
        I18n.load(messagesDir); // 食い違っていれば例外
        assert.deepEqual(checkParity({ ja: { a: "x {n}" }, en: { a: "y" } }).length, 1);
        assert.deepEqual(checkParity({ ja: { a: "x" }, en: {} }).length, 1);
    });
    it("コードで使っているキーがすべてある", () => {
        const i18n = I18n.load(messagesDir);
        const src = join(here, "..", "src");
        const files = readdirSync(src, { recursive: true }).filter((f) => String(f).endsWith(".ts"));
        const used = new Set<string>();
        for (const f of files) {
            const text = readFileSync(join(src, String(f)), "utf8");
            for (const m of text.matchAll(/\bt\([^,]+,\s*"([a-zA-Z]+\.[a-zA-Z.]+)"/g)) used.add(m[1]!);
            for (const m of text.matchAll(/"(button\.[a-zA-Z]+|modal\.[a-zA-Z]+)"/g)) used.add(m[1]!);
        }
        for (const v of ["ok", "ng", "unknown"]) used.add(`verdict.${v}`);
        assert.ok(used.size > 40, `見つけたキーが少なすぎる (${used.size})`);
        const missing = [...used].filter((k) => !i18n.has(k));
        assert.deepEqual(missing, []);
    });
    it("Discord の言語設定から決める", () => {
        assert.equal(langFromLocale("ja", "en"), "ja");
        assert.equal(langFromLocale("en-US", "ja"), "en");
        assert.equal(langFromLocale("ko", "en"), "en");
        assert.equal(langFromLocale(undefined, "ja"), "ja");
    });
    it("差し込み", () => {
        const i18n = I18n.load(messagesDir);
        assert.match(i18n.t("en", "judge.ok", { latencyMs: 180 }), /180 ms/);
        assert.match(i18n.t("ja", "judge.ok", { latencyMs: 180 }), /180 ms/);
    });
});

describe("record", () => {
    it("列の文字", () => {
        assert.equal(columnLetter(0), "A");
        assert.equal(columnLetter(25), "Z");
        assert.equal(columnLetter(26), "AA");
        assert.equal(columnLetter(31), "AF");
    });
    it("録画の位置", () => {
        assert.equal(formatRecPos(75), "01:15");
        assert.equal(formatRecPos(3675), "1:01:15");
        assert.equal(formatRecPos(undefined), "?");
    });
    it("行は見出しと同じ数", () => {
        const s = {
            createdAt: 0, displayName: "a", userId: "1", lang: "ja", srt: { flowName: "f", channel: 7 }, protocol: "srt",
            summary: {}, disconnects: 0, raises: 0, results: [], recordFiles: [],
        } as unknown as Parameters<typeof buildRow>[0];
        assert.equal(buildRow(s, defaultChecks, "done", "u").length, sheetColumns.length);
    });
});

describe("webhook", () => {
    const ok = () => new Response(null, { status: 204 });
    it("URL が無ければ送らない", async () => {
        let calls = 0;
        await new DiscordWebhook("", async () => (calls++, ok())).send({});
        assert.equal(calls, 0);
    });
    it("429 は Retry-After に従ってやり直す", async () => {
        const waits: number[] = [];
        let calls = 0;
        const fetchFn = async () => (++calls < 3 ? new Response("slow", { status: 429, headers: { "retry-after": "2" } }) : ok());
        await new DiscordWebhook("https://x", fetchFn as typeof fetch, async (ms) => void waits.push(ms)).send({});
        assert.equal(calls, 3);
        assert.deepEqual(waits, [2000, 2000]);
    });
    it("4xx はやり直さずに失敗、5xx は 3 回まで", async () => {
        let calls = 0;
        await assert.rejects(new DiscordWebhook("https://x", (async () => (calls++, new Response("bad", { status: 400 }))) as typeof fetch).send({}));
        assert.equal(calls, 1);
        calls = 0;
        await assert.rejects(new DiscordWebhook("https://x", (async () => (calls++, new Response("x", { status: 503 }))) as typeof fetch, async () => {}).send({}));
        assert.equal(calls, 4);
        assert.equal(retryAfterMs(null, 2), 4000);
    });
});

describe("aws", () => {
    it("フレームレートの形", () => {
        assert.equal(parseFrameRate("60"), 60);
        assert.equal(parseFrameRate("59.94"), 59.94);
        assert.equal(parseFrameRate("60000/1001"), 59.94);
        assert.equal(parseFrameRate(""), undefined);
    });
});
