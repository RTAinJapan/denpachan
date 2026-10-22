// スラッシュコマンド /techcheck (docs/stream-test-automation.md §4.2)。
// 説明は英語を基本にし、日本語の Discord では日本語で出す (description_localizations)。
// 運営のサブコマンドは誰にでも見えるが、実行時に運営のロールを確かめる。

import { SlashCommandBuilder } from "discord.js";
import type { FlowConfig } from "../config.js";

function d<T extends { setDescription(s: string): T; setDescriptionLocalizations(l: Record<string, string>): T }>(b: T, en: string, ja: string): T {
    return b.setDescription(en).setDescriptionLocalizations({ ja });
}

export function buildCommand(flows: FlowConfig[]) {
    const cmd = d(new SlashCommandBuilder().setName("techcheck"), "Stream test (tech check)", "配信テスト");

    cmd.addSubcommand((s) =>
        d(s.setName("start"), "Start your stream test", "配信テストを始める").addStringOption((o) =>
            d(o.setName("language"), "Language of the instructions", "案内の言語").addChoices(
                { name: "日本語", value: "ja" },
                { name: "English", value: "en" },
            ),
        ),
    );
    cmd.addSubcommand((s) => d(s.setName("leave"), "Leave the waiting list", "待ちの列から抜ける"));

    cmd.addSubcommandGroup((g) =>
        d(g.setName("slot"), "[Staff] Test slots", "[運営] テストの枠")
            .addSubcommand((s) => d(s.setName("show"), "[Staff] Show slots and the waiting list", "[運営] 枠と待ちの列を見る"))
            .addSubcommand((s) =>
                d(s.setName("srt"), "[Staff] Set the SRT slot", "[運営] SRT の枠を設定し直す")
                    .addStringOption((o) =>
                        d(o.setName("flow"), "Flow (paired with its domain)", "フロー (ドメインとの組)")
                            .setRequired(true)
                            .addChoices(...flows.slice(0, 25).map((f) => ({ name: `${f.name} (${f.domain})`, value: f.name }))),
                    )
                    .addIntegerOption((o) => d(o.setName("channel"), "denpachan channel", "denpachan のチャンネル").setRequired(true).setMinValue(1).setMaxValue(8)),
            )
            .addSubcommand((s) =>
                d(s.setName("rtmp"), "[Staff] Set the RTMP slot", "[運営] RTMP の枠を設定し直す").addIntegerOption((o) =>
                    d(o.setName("channel"), "denpachan channel", "denpachan のチャンネル").setRequired(true).setMinValue(1).setMaxValue(8),
                ),
            )
            .addSubcommand((s) =>
                d(s.setName("off"), "[Staff] Stop accepting tests on a slot", "[運営] 枠の受付を止める").addStringOption((o) =>
                    d(o.setName("kind"), "Slot", "枠").setRequired(true).addChoices({ name: "SRT", value: "srt" }, { name: "RTMP", value: "rtmp" }),
                ),
            )
            .addSubcommand((s) =>
                d(s.setName("on"), "[Staff] Resume a slot", "[運営] 枠の受付を再開する").addStringOption((o) =>
                    d(o.setName("kind"), "Slot", "枠").setRequired(true).addChoices({ name: "SRT", value: "srt" }, { name: "RTMP", value: "rtmp" }),
                ),
            ),
    );

    const userOpt = (required: boolean) => (o: import("discord.js").SlashCommandUserOption) =>
        d(o.setName("user"), "Streamer", "配信者").setRequired(required);

    cmd.addSubcommand((s) => d(s.setName("status"), "[Staff] Show tests in progress", "[運営] 進行中のテストを見る").addUserOption(userOpt(false)));
    cmd.addSubcommand((s) => d(s.setName("shot"), "[Staff] Post a still of the stream", "[運営] 配信の静止画を貼る").addUserOption(userOpt(true)));
    cmd.addSubcommand((s) =>
        d(s.setName("latency"), "[Staff] Ask the streamer to use this latency", "[運営] 配信者に案内する latency を決める")
            .addUserOption(userOpt(true))
            .addIntegerOption((o) => d(o.setName("ms"), "Latency (ms)", "latency (ms)").setRequired(true).setMinValue(20).setMaxValue(10000)),
    );
    cmd.addSubcommand((s) =>
        d(s.setName("latency-receiver"), "[Staff] Set denpachan's receiver latency", "[運営] denpachan 側の latency を変える")
            .addIntegerOption((o) => d(o.setName("channel"), "denpachan channel", "denpachan のチャンネル").setRequired(true).setMinValue(1).setMaxValue(8))
            .addIntegerOption((o) => d(o.setName("ms"), "Latency (ms)", "latency (ms)").setRequired(true).setMinValue(20).setMaxValue(10000)),
    );
    cmd.addSubcommand((s) => d(s.setName("rtmp"), "[Staff] Guide the streamer to RTMP", "[運営] RTMP に案内する").addUserOption(userOpt(true)));
    cmd.addSubcommand((s) => d(s.setName("cancel"), "[Staff] Cancel a test", "[運営] テストを中止する").addUserOption(userOpt(true)));
    cmd.addSubcommand((s) =>
        d(s.setName("review"), "[Staff] Record the audio review", "[運営] 録画を聞いた音声の判定を書く")
            .addUserOption(userOpt(true))
            .addStringOption((o) => d(o.setName("audio"), "Audio verdict", "音声の判定").setRequired(true).addChoices({ name: "OK", value: "ok" }, { name: "NG", value: "ng" }))
            .addStringOption((o) => d(o.setName("memo"), "Memo", "メモ")),
    );
    return cmd;
}

export const staffSubcommands = new Set(["status", "shot", "latency", "latency-receiver", "rtmp", "cancel", "review"]);
