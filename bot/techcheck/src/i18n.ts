// 配信者に向けた文言 (日本語 / 英語)。docs/stream-test-automation.md §4.3。
// messages/ja.yaml と messages/en.yaml に同じキーで置く。キーが片方にしか無ければ起動しない。

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { parse as parseYaml } from "yaml";
import type { Lang } from "./config.js";

export type Messages = Record<Lang, Record<string, string>>;

export class I18n {
    constructor(private readonly messages: Messages) {}

    static load(dir: string): I18n {
        const read = (l: Lang) => flatten(parseYaml(readFileSync(join(dir, `${l}.yaml`), "utf8")));
        const messages = { ja: read("ja"), en: read("en") };
        const problems = checkParity(messages);
        if (problems.length > 0) throw new Error(`文言のファイルが食い違っています:\n${problems.join("\n")}`);
        return new I18n(messages);
    }

    /** {name} を vars で置き換える。無いキーは例外 (起動時の検査で防いでいるので、ここに来たら不具合) */
    t(lang: Lang, key: string, vars: Record<string, string | number> = {}): string {
        const text = this.messages[lang][key];
        if (text === undefined) throw new Error(`文言のキーがありません: ${lang}.${key}`);
        return text.replace(/\{(\w+)\}/g, (m, name: string) => (name in vars ? String(vars[name]) : m));
    }

    has(key: string): boolean {
        return key in this.messages.ja;
    }
}

/** ネストした YAML を "a.b.c" のキーに平らにする */
export function flatten(value: unknown, prefix = "", out: Record<string, string> = {}): Record<string, string> {
    if (value === null || value === undefined) return out;
    if (typeof value === "object" && !Array.isArray(value)) {
        for (const [k, v] of Object.entries(value as Record<string, unknown>)) flatten(v, prefix ? `${prefix}.${k}` : k, out);
        return out;
    }
    out[prefix] = String(value);
    return out;
}

/** 2 つの言語でキーと差し込み ({name}) がそろっているか */
export function checkParity(messages: Messages): string[] {
    const problems: string[] = [];
    const keys = new Set([...Object.keys(messages.ja), ...Object.keys(messages.en)]);
    const vars = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(",");
    for (const key of [...keys].sort()) {
        const ja = messages.ja[key];
        const en = messages.en[key];
        if (ja === undefined) problems.push(`- ja.yaml に ${key} がありません`);
        else if (en === undefined) problems.push(`- en.yaml に ${key} がありません`);
        else if (vars(ja) !== vars(en)) problems.push(`- ${key}: 差し込みが違います (ja: ${vars(ja) || "なし"} / en: ${vars(en) || "なし"})`);
    }
    return problems;
}

/** Discord の言語設定 (interaction.locale) から案内の言語を決める */
export function langFromLocale(locale: string | undefined, fallback: Lang): Lang {
    if (!locale) return fallback;
    if (locale === "ja") return "ja";
    if (locale.startsWith("en")) return "en";
    return fallback;
}
