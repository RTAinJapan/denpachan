// 結果の通知の Discord webhook (docs/stream-test-automation.md §5.5)。
// URL が未設定なら何もしない。429 と 5xx は Retry-After に従って 3 回までやり直す。

import type { WebhookPort } from "../ports.js";

export class DiscordWebhook implements WebhookPort {
    constructor(
        private readonly url: string,
        private readonly fetchFn: typeof fetch = fetch,
        private readonly sleep: (ms: number) => Promise<void> = (ms) => new Promise((r) => setTimeout(r, ms)),
    ) {}

    async send(payload: unknown): Promise<void> {
        if (!this.url) return;
        let lastError = "";
        for (let attempt = 0; attempt <= 3; attempt++) {
            const res = await this.fetchFn(this.url, {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify(payload),
            });
            if (res.ok) return;
            lastError = `${res.status} ${(await res.text()).slice(0, 200)}`;
            if (res.status !== 429 && res.status < 500) break;
            if (attempt === 3) break;
            await this.sleep(retryAfterMs(res.headers.get("retry-after"), attempt));
        }
        throw new Error(`webhook に送れませんでした: ${lastError}`);
    }
}

/** Retry-After (秒) が無ければ 1、2、4 秒 */
export function retryAfterMs(header: string | null, attempt: number): number {
    const sec = header ? Number(header) : NaN;
    if (Number.isFinite(sec) && sec >= 0) return Math.min(sec * 1000, 60_000);
    return 1000 * 2 ** attempt;
}
