import { resolve4 } from "node:dns/promises";
import type { DnsPort } from "../ports.js";

export class NodeDns implements DnsPort {
    resolve(domain: string): Promise<string[]> {
        return resolve4(domain);
    }
}
