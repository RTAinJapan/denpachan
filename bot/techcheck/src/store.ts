// 状態の保存 (SQLite)。bot を起動し直しても、進行中のテスト・待ちの列・枠の設定し直し・言語の好みが残る。
// 1 つのプロセスが小さな状態を持つだけなので、キーごとに JSON を 1 つ置く。

import { DatabaseSync } from "node:sqlite";

export interface KeyValueStore {
    get<T>(key: string): T | undefined;
    set(key: string, value: unknown): void;
}

export class SqliteStore implements KeyValueStore {
    private readonly db: DatabaseSync;

    constructor(path: string) {
        this.db = new DatabaseSync(path);
        this.db.exec("PRAGMA journal_mode = WAL");
        this.db.exec("CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    }

    get<T>(key: string): T | undefined {
        const row = this.db.prepare("SELECT value FROM kv WHERE key = ?").get(key) as { value: string } | undefined;
        return row ? (JSON.parse(row.value) as T) : undefined;
    }

    set(key: string, value: unknown): void {
        this.db
            .prepare("INSERT INTO kv (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value")
            .run(key, JSON.stringify(value));
    }

    close(): void {
        this.db.close();
    }
}

export class MemoryStore implements KeyValueStore {
    private readonly map = new Map<string, string>();
    get<T>(key: string): T | undefined {
        const v = this.map.get(key);
        return v === undefined ? undefined : (JSON.parse(v) as T);
    }
    set(key: string, value: unknown): void {
        this.map.set(key, JSON.stringify(value));
    }
}
