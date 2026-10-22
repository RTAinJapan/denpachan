// MediaConnect と CloudWatch (docs/stream-test-automation.md §3.1)。
// CloudWatch のメトリクスは名前空間 AWS/MediaConnect、ディメンション FlowARN (フローの source は 1 つ) で、1 秒の粒度で取る。

import { CloudWatchClient, GetMetricDataCommand, type MetricDataQuery } from "@aws-sdk/client-cloudwatch";
import {
    DescribeFlowCommand,
    DescribeFlowSourceMetadataCommand,
    DescribeFlowSourceThumbnailCommand,
    MediaConnectClient,
    StartFlowCommand,
    StopFlowCommand,
} from "@aws-sdk/client-mediaconnect";
import type { SrtSample } from "../advisor.js";
import type { FlowInfo, FlowMetadata, FlowPort } from "../ports.js";

export const kNamespace = "AWS/MediaConnect";
export const kDimension = "FlowARN";

/** 取るメトリクス: SrtSample の項目、メトリクス名、統計 */
const metrics: { key: keyof SrtSample; name: string; stat: string }[] = [
    { key: "connected", name: "SourceConnected", stat: "Minimum" },
    { key: "upTimeSec", name: "SourceUpTime", stat: "Maximum" },
    { key: "rttMs", name: "SourceRoundTripTime", stat: "Maximum" },
    { key: "lossPct", name: "SourcePacketLossPercent", stat: "Average" },
    { key: "totalPackets", name: "SourceTotalPackets", stat: "Sum" },
    { key: "notRecovered", name: "SourceNotRecoveredPackets", stat: "Sum" },
    { key: "continuityErrors", name: "SourceContinuityCounter", stat: "Sum" },
    { key: "tsSyncLoss", name: "SourceTSSyncLoss", stat: "Sum" },
    { key: "jitterMs", name: "SourceJitter", stat: "Maximum" },
    { key: "bitrateBps", name: "SourceBitRate", stat: "Average" },
];

export class AwsFlowPort implements FlowPort {
    private readonly mc: MediaConnectClient;
    private readonly cw: CloudWatchClient;

    constructor(region: string) {
        this.mc = new MediaConnectClient({ region });
        this.cw = new CloudWatchClient({ region });
    }

    async describe(arn: string): Promise<FlowInfo> {
        const r = await this.mc.send(new DescribeFlowCommand({ FlowArn: arn }));
        const src = r.Flow?.Source;
        return {
            arn,
            status: r.Flow?.Status ?? "UNKNOWN",
            ingestIp: src?.IngestIp,
            ingestPort: src?.IngestPort,
            protocol: src?.Transport?.Protocol,
            minLatencyMs: src?.Transport?.MinLatency,
        };
    }

    async start(arn: string): Promise<void> {
        await this.mc.send(new StartFlowCommand({ FlowArn: arn }));
    }

    async stop(arn: string): Promise<void> {
        await this.mc.send(new StopFlowCommand({ FlowArn: arn }));
    }

    async metrics(arn: string, fromSec: number, toSec: number): Promise<SrtSample[]> {
        const queries: MetricDataQuery[] = metrics.map((m, i) => ({
            Id: `m${i}`,
            MetricStat: {
                Metric: { Namespace: kNamespace, MetricName: m.name, Dimensions: [{ Name: kDimension, Value: arn }] },
                Period: 1,
                Stat: m.stat,
            },
            ReturnData: true,
        }));
        const byT = new Map<number, SrtSample>();
        let next: string | undefined;
        do {
            const r = await this.cw.send(
                new GetMetricDataCommand({
                    MetricDataQueries: queries,
                    StartTime: new Date(fromSec * 1000),
                    EndTime: new Date(toSec * 1000),
                    NextToken: next,
                }),
            );
            for (const res of r.MetricDataResults ?? []) {
                const m = metrics[Number(res.Id?.slice(1))];
                if (!m) continue;
                (res.Timestamps ?? []).forEach((ts, j) => {
                    const t = Math.floor(ts.getTime() / 1000);
                    const v = res.Values?.[j];
                    if (v === undefined) return;
                    const s = byT.get(t) ?? { t, connected: false };
                    if (m.key === "connected") s.connected = v >= 1;
                    else (s as unknown as Record<string, number>)[m.key] = v;
                    byT.set(t, s);
                });
            }
            next = r.NextToken;
        } while (next);
        return [...byT.values()].sort((a, b) => a.t - b.t);
    }

    async metadata(arn: string): Promise<FlowMetadata | undefined> {
        const r = await this.mc.send(new DescribeFlowSourceMetadataCommand({ FlowArn: arn }));
        const streams = (r.TransportMediaInfo?.Programs ?? []).flatMap((p) => p.Streams ?? []);
        if (streams.length === 0) return undefined;
        const video = streams.find((s) => /video/i.test(s.StreamType ?? ""));
        const hasAudio = streams.some((s) => /audio/i.test(s.StreamType ?? ""));
        return {
            width: video?.FrameResolution?.FrameWidth,
            height: video?.FrameResolution?.FrameHeight,
            fps: parseFrameRate(video?.FrameRate),
            codec: video?.Codec,
            hasAudio,
        };
    }

    async thumbnail(arn: string): Promise<Buffer | undefined> {
        const r = await this.mc.send(new DescribeFlowSourceThumbnailCommand({ FlowArn: arn }));
        const b64 = r.ThumbnailDetails?.Thumbnail;
        return b64 ? Buffer.from(b64, "base64") : undefined;
    }
}

/** "60"、"59.94"、"60000/1001" の形を数にする */
export function parseFrameRate(s: string | undefined): number | undefined {
    if (!s) return undefined;
    const m = /^\s*([\d.]+)\s*(?:\/\s*([\d.]+))?/.exec(s);
    if (!m) return undefined;
    const v = m[2] ? Number(m[1]) / Number(m[2]) : Number(m[1]);
    return Number.isFinite(v) && v > 0 ? Math.round(v * 100) / 100 : undefined;
}
