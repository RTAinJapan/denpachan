// 音声とその他のチェック (docs/stream-test-automation.md §7)。既定の手順と、音声のレベルによる参考の判定。
// 参考の判定は 1 秒ごとのピーク (dBFS、左右の大きい方) で行う。最終的な判定は録画を人が聞いて行う。

import type { CheckStep } from "./config.js";

export const defaultChecks: CheckStep[] = [
    {
        id: "game-audio",
        kind: "sound",
        seconds: 15,
        thresholdDb: -40,
        ratio: 0.8,
        text: {
            ja: "ゲーム (ゲーム機・エミュレータ) の音を鳴らしてください。BGM や効果音が出る場面で、15 秒ほど続けます。",
            en: "Play your game (console or emulator) with sound. Stay in a scene with music or sound effects for about 15 seconds.",
        },
        failHint: {
            ja: "ゲームの音が届いていません。OBS のゲーム音の入力 (キャプチャボード・アプリの音声キャプチャ) と、ミュートになっていないかを確かめてください。",
            en: "Game audio is not reaching us. Check the game audio source in OBS (capture card or application audio capture) and make sure it is not muted.",
        },
    },
    {
        id: "silence",
        kind: "silence",
        seconds: 10,
        thresholdDb: -60,
        text: {
            ja: "ゲームの音を止めて (ミュートして)、10 秒ほど静かにしてください。",
            en: "Mute the game and stay quiet for about 10 seconds.",
        },
        failHint: {
            ja: "何も鳴らしていないのに音が載っています (ノイズの可能性)。キャプチャボードの音量や、ゲーム機の設定を確かめてください。",
            en: "We hear sound although nothing is playing (possibly noise). Check the capture card volume and your console settings.",
        },
    },
    {
        id: "mic",
        kind: "quiet",
        seconds: 10,
        marginDb: 6,
        text: {
            ja: "ゲームの音を止めたまま、マイクに向かって普段の声で 10 秒ほど話してください。",
            en: "Keep the game muted and talk into your microphone in your normal voice for about 10 seconds.",
        },
        failHint: {
            ja: "マイクの音が配信に入っています。OBS のマイクの入力を、配信の音声から外してください (マイクは別の経路で送ってもらいます)。",
            en: "Your microphone is in the stream. Remove the microphone from the stream audio in OBS (we receive your voice separately).",
        },
    },
    {
        id: "desktop",
        kind: "quiet",
        seconds: 10,
        marginDb: 6,
        text: {
            ja: "ゲームの音を止めたまま、PC の音 (ブラウザの動画、Discord の通知音など) を 10 秒ほど鳴らしてください。",
            en: "Keep the game muted and play some PC sound (a video in your browser, a Discord notification, etc.) for about 10 seconds.",
        },
        failHint: {
            ja: "PC の音が配信に入っています。OBS のデスクトップ音声を、配信の音声から外してください。",
            en: "PC sound is in the stream. Remove desktop audio from the stream audio in OBS.",
        },
    },
    {
        id: "picture",
        kind: "confirm",
        seconds: 0,
        text: {
            ja: "配信に映っている画面です。正しいですか (ゲーム画面が全体に映っている、余計なウィンドウが無い)。",
            en: "This is what your stream shows. Is it correct (the game fills the screen, no other windows)?",
        },
        failHint: {
            ja: "OBS のシーンを直してから「やり直す」を押してください。",
            en: "Fix the scene in OBS, then press \"Retry\".",
        },
    },
    {
        id: "reconnect",
        kind: "reconnect",
        seconds: 60,
        text: {
            ja: "OBS の配信を一度止めて、もう一度始めてください (本番で切れたときの戻し方の練習です)。",
            en: "Stop streaming in OBS once and start it again (practice for recovering if you drop during the event).",
        },
        failHint: {
            ja: "配信が戻ってきません。OBS の配信の設定 (URL) が保存されているかを確かめてください。",
            en: "Your stream did not come back. Check that the stream settings (URL) in OBS were saved.",
        },
    },
];

export type CheckVerdict = "ok" | "ng" | "unknown";

export interface CheckResult {
    verdict: CheckVerdict;
    /** 記録用の数字 (sound: 音が載った秒の割合、silence: ノイズの大きさ、quiet: 最大のピーク) */
    value?: number;
}

/** 1 秒ごとのピークから、手順の参考の判定を出す。peaks が空 (denpachan の受信口が無い) なら unknown */
export function evaluateAudioStep(step: CheckStep, peaks: number[], noiseDb: number | undefined, silentDb: number): CheckResult {
    if (peaks.length === 0) return { verdict: "unknown" };
    const max = Math.max(...peaks);
    switch (step.kind) {
        case "sound": {
            const threshold = step.thresholdDb ?? -40;
            const ratio = peaks.filter((p) => p >= threshold).length / peaks.length;
            return { verdict: ratio >= (step.ratio ?? 0.8) ? "ok" : "ng", value: round1(ratio) };
        }
        case "silence":
            return { verdict: max < (step.thresholdDb ?? silentDb) ? "ok" : "ng", value: round1(max) };
        case "quiet": {
            const base = Math.max(noiseDb ?? silentDb, silentDb);
            return { verdict: max <= base + (step.marginDb ?? 6) ? "ok" : "ng", value: round1(max) };
        }
        default:
            return { verdict: "unknown" };
    }
}

function round1(v: number): number {
    return Math.round(v * 10) / 10;
}
