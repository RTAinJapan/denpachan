// 設定ファイル (config.yaml) の読み込みと検証。項目は docs/stream-test-automation.md §4.1 の表のとおり。
// 秘密は `${環境変数名}` で環境変数から読める。

import { readFileSync } from "node:fs";
import { parse as parseYaml } from "yaml";
import { z } from "zod";
import { defaultChecks } from "./checks.js";

const nonEmpty = z.string().min(1);
const lang = z.enum(["ja", "en"]);
const localized = z.object({ ja: nonEmpty, en: nonEmpty });

export const checkStepSchema = z.object({
    id: nonEmpty,
    // sound: 音が載ることを期待 / silence: 静かにする (ノイズの大きさを測る) / quiet: 載らないことを期待 (基準は silence)
    // confirm: 静止画を見て はい / いいえ / reconnect: 配信を止めて始め直す
    kind: z.enum(["sound", "silence", "quiet", "confirm", "reconnect"]),
    seconds: z.number().int().min(0).max(600),
    text: localized,
    failHint: localized,
    thresholdDb: z.number().optional(), // sound: これ以上の秒を「音が載った」とみなす / silence: これ未満なら静か
    ratio: z.number().min(0).max(1).optional(), // sound: 音が載った秒の割合の下限
    marginDb: z.number().optional(), // quiet: silence で測ったノイズにこれを足した値を超えたら「載った」
});
export type CheckStep = z.infer<typeof checkStepSchema>;

const flowSchema = z.object({
    name: nonEmpty,
    arn: nonEmpty,
    domain: nonEmpty,
    passphrase: nonEmpty,
    pbkeylen: z.number().int().prefault(16),
});
export type FlowConfig = z.infer<typeof flowSchema>;

export const configSchema = z
    .object({
        discord: z.object({
            token: nonEmpty,
            guildId: nonEmpty,
            techcheckChannelId: nonEmpty,
            staffRoleId: nonEmpty,
            opsChannelId: nonEmpty,
        }),
        notify: z.object({ webhookUrl: z.string().prefault("") }).prefault({}),
        aws: z.object({ region: nonEmpty }),
        flows: z.array(flowSchema).min(1),
        rtmp: z.object({
            domain: nonEmpty,
            app: nonEmpty,
            key: nonEmpty,
            statsUrl: z.string().prefault(""),
        }),
        slots: z
            .object({
                srt: z.object({ flow: z.string().optional(), channel: z.number().int().min(1).max(8).optional() }).prefault({}),
                rtmp: z.object({ channel: z.number().int().min(1).max(8).optional() }).prefault({}),
            })
            .prefault({}),
        denpachan: z
            .object({
                host: z.string().prefault("127.0.0.1"),
                port: z.number().int().prefault(9100),
                token: z.string().prefault(""),
            })
            .prefault({}),
        sheet: z.object({
            spreadsheetId: nonEmpty,
            tabName: z.string().prefault("techcheck"),
            credentialsFile: nonEmpty,
        }),
        guide: z.object({
            obsProfile: nonEmpty,
            expected: z
                .object({
                    width: z.number().int().prefault(1920),
                    height: z.number().int().prefault(1080),
                    fps: z.number().prefault(60),
                    bitrateKbps: z.number().int().prefault(6000),
                    keyframeSec: z.number().prefault(2),
                })
                .prefault({}),
        }),
        latency: z
            .object({
                initialMs: z.number().int().min(20).prefault(120),
                maxMs: z.number().int().prefault(2000),
                maxRaises: z.number().int().min(0).prefault(4),
            })
            .prefault({}),
        judge: z
            .object({
                windowSec: z.number().int().min(10).prefault(60),
                confirmSec: z.number().int().min(0).prefault(60),
                // [ロス率 (%) の上限, 倍率] を小さい順に。最後の行より大きいロス率も最後の倍率
                multipliers: z
                    .array(z.tuple([z.number(), z.number()]))
                    .min(1)
                    .prefault([
                        [1, 3],
                        [3, 4],
                        [7, 6],
                        [10, 8],
                        [100, 10],
                    ]),
                minFpsRatio: z.number().min(0).max(1).prefault(0.9),
                maxJitterMs: z.number().prefault(500),
                silentDb: z.number().prefault(-60),
                silentAlertSec: z.number().int().prefault(10),
            })
            .prefault({}),
        timeouts: z
            .object({
                claimMin: z.number().prefault(5),
                connectRemindMin: z.number().prefault(10),
                connectAbortMin: z.number().prefault(30),
                raiseRemindMin: z.number().prefault(10),
                stepAbortMin: z.number().prefault(20),
                halveWhenQueued: z.boolean().prefault(true),
            })
            .prefault({}),
        checks: z.array(checkStepSchema).min(1).optional(),
        i18n: z.object({ dir: z.string().prefault("./messages"), fallback: lang.prefault("en") }).prefault({}),
        db: z.object({ path: z.string().prefault("./techcheck.db") }).prefault({}),
    })
    .superRefine((c, ctx) => {
        const names = new Set<string>();
        for (const f of c.flows) {
            if (names.has(f.name)) ctx.addIssue({ code: "custom", path: ["flows"], message: `flows: 名前 "${f.name}" が重複しています` });
            names.add(f.name);
        }
        if (c.slots.srt.flow && !names.has(c.slots.srt.flow)) {
            ctx.addIssue({ code: "custom", path: ["slots", "srt", "flow"], message: `slots.srt.flow: "${c.slots.srt.flow}" は flows にありません` });
        }
        if (c.notify.webhookUrl && !/^https:\/\//.test(c.notify.webhookUrl)) {
            ctx.addIssue({ code: "custom", path: ["notify", "webhookUrl"], message: "notify.webhookUrl は https:// で始まる URL にしてください" });
        }
        if (c.latency.maxMs < c.latency.initialMs) {
            ctx.addIssue({ code: "custom", path: ["latency", "maxMs"], message: "latency.maxMs は latency.initialMs 以上にしてください" });
        }
        const isLoopback = ["127.0.0.1", "localhost", "::1"].includes(c.denpachan.host);
        if (!isLoopback && !c.denpachan.token) {
            ctx.addIssue({ code: "custom", path: ["denpachan", "token"], message: "denpachan.host が loopback でないときは denpachan.token が必要です" });
        }
        const checks = c.checks ?? defaultChecks;
        const ids = new Set<string>();
        for (const s of checks) {
            if (ids.has(s.id)) ctx.addIssue({ code: "custom", path: ["checks"], message: `checks: id "${s.id}" が重複しています` });
            ids.add(s.id);
        }
        const firstQuiet = checks.findIndex((s) => s.kind === "quiet");
        const firstSilence = checks.findIndex((s) => s.kind === "silence");
        if (firstQuiet >= 0 && (firstSilence < 0 || firstSilence > firstQuiet)) {
            ctx.addIssue({ code: "custom", path: ["checks"], message: "checks: quiet の手順の前に silence の手順が要ります (ノイズの基準を測るため)" });
        }
    });

export type Config = z.infer<typeof configSchema> & { checks: CheckStep[] };
export type Lang = z.infer<typeof lang>;

/** `${NAME}` を環境変数で置き換える。無い変数は空文字にし、名前を返す (必須の項目は検証で引っかかる) */
export function expandEnv(text: string, env: NodeJS.ProcessEnv): { text: string; missing: string[] } {
    const missing: string[] = [];
    const out = text.replace(/\$\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (_, name: string) => {
        const v = env[name];
        if (v === undefined) {
            missing.push(name);
            return "";
        }
        return v;
    });
    return { text: out, missing };
}

export class ConfigError extends Error {}

export function parseConfig(yamlText: string, env: NodeJS.ProcessEnv = process.env): Config {
    const { text, missing } = expandEnv(yamlText, env);
    const raw: unknown = parseYaml(text);
    const result = configSchema.safeParse(raw);
    if (!result.success) {
        const lines = result.error.issues.map((i) => `- ${i.path.join(".") || "(全体)"}: ${i.message}`);
        if (missing.length > 0) lines.push(`(設定されていない環境変数: ${[...new Set(missing)].join(", ")})`);
        throw new ConfigError(`設定ファイルが正しくありません:\n${lines.join("\n")}`);
    }
    return { ...result.data, checks: result.data.checks ?? defaultChecks };
}

export function loadConfig(path: string, env: NodeJS.ProcessEnv = process.env): Config {
    return parseConfig(readFileSync(path, "utf8"), env);
}
