// Discord とのやりとり: ChatPort の実装と、コマンド・ボタン・モーダルの受け付け。

import {
    ActionRowBuilder,
    AttachmentBuilder,
    ButtonBuilder,
    ButtonStyle,
    ChannelType,
    type ChatInputCommandInteraction,
    type Client,
    type GuildMember,
    type Interaction,
    MessageFlags,
    ModalBuilder,
    type ButtonInteraction,
    type ModalSubmitInteraction,
    type SendableChannels,
    TextInputBuilder,
    TextInputStyle,
    ThreadAutoArchiveDuration,
    type ThreadChannel,
    type TextChannel,
    type APIInteractionGuildMember,
} from "discord.js";
import type { Config, Lang } from "../config.js";
import type { I18n } from "../i18n.js";
import type { Button, ChatPort, OutMessage } from "../ports.js";
import { errorText, type TechcheckService } from "../service.js";
import { staffSubcommands } from "./commands.js";

const styles: Record<NonNullable<Button["style"]>, ButtonStyle> = {
    primary: ButtonStyle.Primary,
    secondary: ButtonStyle.Secondary,
    success: ButtonStyle.Success,
    danger: ButtonStyle.Danger,
};

function rows(buttons: Button[] | undefined): ActionRowBuilder<ButtonBuilder>[] {
    if (!buttons || buttons.length === 0) return [];
    const out: ActionRowBuilder<ButtonBuilder>[] = [];
    for (let i = 0; i < buttons.length; i += 5) {
        out.push(
            new ActionRowBuilder<ButtonBuilder>().addComponents(
                buttons.slice(i, i + 5).map((b) => new ButtonBuilder().setCustomId(b.id).setLabel(b.label).setStyle(styles[b.style ?? "secondary"])),
            ),
        );
    }
    return out;
}

function payload(msg: OutMessage) {
    return {
        content: msg.content.length > 2000 ? `${msg.content.slice(0, 1997)}...` : msg.content,
        components: rows(msg.buttons),
        files: msg.image ? [new AttachmentBuilder(msg.image.data, { name: msg.image.name })] : [],
        allowedMentions: { parse: [] as never[] },
    };
}

export class DiscordChat implements ChatPort {
    constructor(
        private readonly client: Client,
        private readonly cfg: Config,
    ) {}

    private async channel(id: string): Promise<SendableChannels> {
        const ch = await this.client.channels.fetch(id);
        if (!ch || !ch.isSendable()) throw new Error(`チャンネル ${id} に書けません`);
        return ch;
    }

    private async thread(id: string): Promise<ThreadChannel> {
        const ch = await this.client.channels.fetch(id);
        if (!ch || !ch.isThread()) throw new Error(`スレッド ${id} がありません`);
        return ch;
    }

    async createThread(name: string, userId: string): Promise<string> {
        const parent = (await this.client.channels.fetch(this.cfg.discord.techcheckChannelId)) as TextChannel | null;
        if (!parent || parent.type !== ChannelType.GuildText) throw new Error("配信テストのチャンネルがテキストチャンネルではありません");
        const thread = await parent.threads.create({
            name,
            type: ChannelType.PrivateThread,
            invitable: false,
            autoArchiveDuration: ThreadAutoArchiveDuration.OneDay,
        });
        await thread.members.add(userId);
        return thread.id;
    }

    async post(threadId: string, msg: OutMessage): Promise<string> {
        const t = await this.thread(threadId);
        if (t.archived) await t.setArchived(false);
        const m = await t.send(payload(msg));
        return m.id;
    }

    async clearButtons(threadId: string, messageId: string): Promise<void> {
        const t = await this.thread(threadId);
        const m = await t.messages.fetch(messageId);
        await m.edit({ components: [] });
    }

    async closeThread(threadId: string): Promise<void> {
        const t = await this.thread(threadId);
        await t.setLocked(true);
        await t.setArchived(true);
    }

    threadUrl(threadId: string): string {
        return `https://discord.com/channels/${this.cfg.discord.guildId}/${threadId}`;
    }

    async callUser(userId: string, msg: OutMessage): Promise<void> {
        const ch = await this.channel(this.cfg.discord.techcheckChannelId);
        await ch.send({ ...payload(msg), allowedMentions: { users: [userId] } });
        try {
            const user = await this.client.users.fetch(userId);
            await user.send(payload(msg));
        } catch {
            // DM を受け取らない設定の人もいる。チャンネルのメンションは届いている
        }
    }

    async ops(text: string): Promise<void> {
        const ch = await this.channel(this.cfg.discord.opsChannelId);
        await ch.send({ content: text.slice(0, 2000), allowedMentions: { parse: [] } });
    }
}

function displayName(i: Interaction): string {
    const m = i.member as GuildMember | APIInteractionGuildMember | null;
    if (m && "displayName" in m && typeof m.displayName === "string") return m.displayName;
    if (m && "nick" in m && m.nick) return m.nick;
    return i.user.globalName ?? i.user.username;
}

function hasRole(i: Interaction, roleId: string): boolean {
    const m = i.member as GuildMember | APIInteractionGuildMember | null;
    if (!m) return false;
    const roles = m.roles;
    return Array.isArray(roles) ? roles.includes(roleId) : roles.cache.has(roleId);
}

export class InteractionHandler {
    constructor(
        private readonly service: TechcheckService,
        private readonly cfg: Config,
        private readonly i18n: I18n,
        private readonly log: (msg: string) => void = console.error,
    ) {}

    async handle(i: Interaction): Promise<void> {
        try {
            if (i.isChatInputCommand() && i.commandName === "techcheck") await this.command(i);
            else if (i.isButton()) await this.button(i);
            else if (i.isModalSubmit()) await this.modal(i);
        } catch (e) {
            this.log(`interaction: ${errorText(e)}`);
            if (i.isRepliable()) {
                const content = `エラー / Error: ${errorText(e)}`.slice(0, 2000);
                if (i.deferred || i.replied) await i.editReply({ content }).catch(() => undefined);
                else await i.reply({ content, flags: MessageFlags.Ephemeral }).catch(() => undefined);
            }
        }
    }

    private async command(i: ChatInputCommandInteraction): Promise<void> {
        const group = i.options.getSubcommandGroup(false);
        const sub = i.options.getSubcommand();
        const staff = hasRole(i, this.cfg.discord.staffRoleId);
        if ((group === "slot" || staffSubcommands.has(sub)) && !staff) {
            await i.reply({ content: "運営のコマンドです / Staff only", flags: MessageFlags.Ephemeral });
            return;
        }
        await i.deferReply({ flags: MessageFlags.Ephemeral });
        const s = this.service;
        const user = (name: string) => i.options.getUser(name, true);
        let text: string;
        if (group === "slot") {
            switch (sub) {
                case "show":
                    text = await s.slotShow();
                    break;
                case "srt":
                    text = await s.slotSet("srt", i.options.getInteger("channel", true), i.options.getString("flow", true));
                    break;
                case "rtmp":
                    text = await s.slotSet("rtmp", i.options.getInteger("channel", true));
                    break;
                default:
                    text = await s.slotEnable(i.options.getString("kind", true) as "srt" | "rtmp", sub === "on");
            }
        } else {
            switch (sub) {
                case "start":
                    text = await s.start(
                        { id: i.user.id, displayName: displayName(i) },
                        i.channelId,
                        (i.options.getString("language") as Lang | null) ?? undefined,
                        i.locale,
                    );
                    break;
                case "leave":
                    text = await s.leave(i.user.id);
                    break;
                case "status":
                    text = await s.status(i.options.getUser("user")?.id);
                    break;
                case "shot": {
                    const r = await s.shot(user("user").id);
                    await i.editReply({ content: r.text, files: r.image ? [new AttachmentBuilder(r.image, { name: "stream.jpg" })] : [] });
                    return;
                }
                case "latency":
                    text = await s.setLatency(user("user").id, i.options.getInteger("ms", true));
                    break;
                case "latency-receiver":
                    text = await s.setReceiverLatency(i.options.getInteger("channel", true), i.options.getInteger("ms", true));
                    break;
                case "rtmp":
                    text = await s.forceRtmp(user("user").id);
                    break;
                case "cancel":
                    text = await s.cancel(user("user").id);
                    break;
                case "review":
                    text = await s.review(user("user").id, i.options.getString("audio", true) === "ok", i.options.getString("memo") ?? "");
                    break;
                default:
                    text = "?";
            }
        }
        await i.editReply({ content: text.slice(0, 2000) });
    }

    private async button(i: ButtonInteraction): Promise<void> {
        const id = i.customId;
        if (id === "tcq:claim") {
            await i.deferReply({ flags: MessageFlags.Ephemeral });
            const text = await this.service.claim({ id: i.user.id, displayName: displayName(i) });
            await i.editReply({ content: text });
            return;
        }
        if (!id.startsWith("tc:")) return;
        const [, sessionId, action] = id.split(":");
        const staff = hasRole(i, this.cfg.discord.staffRoleId);
        // モーダルは最初の返事でしか出せないので、ここで決める
        if (action === "done" || action === "abort") {
            const owner = this.service.sessionOwner(sessionId ?? "");
            const lang = this.service.sessionLang(sessionId ?? "");
            if (!owner) {
                await i.reply({ content: this.i18n.t(lang, "button.expired"), flags: MessageFlags.Ephemeral });
                return;
            }
            if (owner !== i.user.id && !staff) {
                await i.reply({ content: this.i18n.t(lang, "button.notYours"), flags: MessageFlags.Ephemeral });
                return;
            }
            await i.showModal(this.modalFor(action, sessionId!, lang));
            return;
        }
        await i.deferReply({ flags: MessageFlags.Ephemeral });
        const r = await this.service.button(i.user.id, staff, id);
        if (["set", "step-start", "yes", "no", "step-retry", "step-next"].includes(action ?? "") && !r.ephemeral) {
            await i.message.edit({ components: [] }).catch(() => undefined);
        }
        if (r.ephemeral) await i.editReply({ content: r.ephemeral });
        else await i.deleteReply().catch(() => undefined);
    }

    private modalFor(kind: "done" | "abort", sessionId: string, lang: Lang): ModalBuilder {
        const input = new TextInputBuilder()
            .setCustomId("text")
            .setLabel(this.i18n.t(lang, kind === "done" ? "modal.doneLabel" : "modal.abortLabel"))
            .setStyle(TextInputStyle.Paragraph)
            .setRequired(false)
            .setMaxLength(1000);
        return new ModalBuilder()
            .setCustomId(`tcm:${kind}:${sessionId}`)
            .setTitle(this.i18n.t(lang, kind === "done" ? "modal.doneTitle" : "modal.abortTitle"))
            .addComponents(new ActionRowBuilder<TextInputBuilder>().addComponents(input));
    }

    private async modal(i: ModalSubmitInteraction): Promise<void> {
        const [prefix, kind, sessionId] = i.customId.split(":");
        if (prefix !== "tcm" || !sessionId) return;
        const lang = this.service.sessionLang(sessionId);
        await i.deferReply({ flags: MessageFlags.Ephemeral });
        const text = i.fields.getTextInputValue("text") ?? "";
        if (kind === "done") await this.service.complete(sessionId, text);
        else await this.service.abortByUser(sessionId, text);
        await i.editReply({ content: this.i18n.t(lang, "modal.received") });
    }
}
