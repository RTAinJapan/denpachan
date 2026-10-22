// Google スプレッドシート (docs/stream-test-automation.md §8)。サービスアカウントで Sheets API v4 を直接呼ぶ。
// シートはサービスアカウントのメールアドレスに編集の権限を共有しておく。

import { JWT } from "google-auth-library";
import { readFileSync } from "node:fs";
import type { SheetPort } from "../ports.js";
import { sheetColumns } from "../record.js";

const kScope = "https://www.googleapis.com/auth/spreadsheets";

export class GoogleSheet implements SheetPort {
    private readonly jwt: JWT;
    private headerChecked = false;

    constructor(
        private readonly spreadsheetId: string,
        private readonly tab: string,
        credentialsFile: string,
        private readonly fetchFn: typeof fetch = fetch,
    ) {
        const key = JSON.parse(readFileSync(credentialsFile, "utf8")) as { client_email: string; private_key: string };
        this.jwt = new JWT({ email: key.client_email, key: key.private_key, scopes: [kScope] });
    }

    url(): string {
        return `https://docs.google.com/spreadsheets/d/${this.spreadsheetId}`;
    }

    async append(row: (string | number)[]): Promise<string> {
        await this.ensureHeader();
        const range = encodeURIComponent(`${this.tab}!A1`);
        const r = await this.request(`/values/${range}:append?valueInputOption=RAW&insertDataOption=INSERT_ROWS`, "POST", { values: [row] });
        const updated = (r as { updates?: { updatedRange?: string } }).updates?.updatedRange;
        if (!updated) throw new Error("書いた行の場所が返ってきません");
        return updated;
    }

    async update(range: string, values: (string | number)[]): Promise<void> {
        await this.request(`/values/${encodeURIComponent(range)}?valueInputOption=RAW`, "PUT", { values: [values] });
    }

    /** 1 行目が空なら見出しを書く */
    private async ensureHeader(): Promise<void> {
        if (this.headerChecked) return;
        const r = (await this.request(`/values/${encodeURIComponent(`${this.tab}!1:1`)}`, "GET")) as { values?: string[][] };
        if (!r.values || r.values.length === 0) await this.update(`${this.tab}!A1`, [...sheetColumns]);
        this.headerChecked = true;
    }

    private async request(path: string, method: string, body?: unknown): Promise<unknown> {
        const { token } = await this.jwt.getAccessToken();
        const res = await this.fetchFn(`https://sheets.googleapis.com/v4/spreadsheets/${this.spreadsheetId}${path}`, {
            method,
            headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
            body: body === undefined ? undefined : JSON.stringify(body),
        });
        if (!res.ok) throw new Error(`Sheets API ${res.status}: ${(await res.text()).slice(0, 200)}`);
        return res.json();
    }
}
