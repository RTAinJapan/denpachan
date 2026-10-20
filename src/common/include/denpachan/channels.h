#pragma once
// チャンネル数の上限。engine / worker / rtmpd / 仮想カメラ (DLL) で共通。設計書 §10.1。
//
// 画面に出して使うチャンネル数は全体設定 (general.channelCount、1〜kMaxChannels) で選ぶ。
// 仮想カメラ (denpachan Cam 1〜kMaxChannels) は使う数によらず全部登録する。
// 上限を変えるときは、仮想カメラの CLSID (src/vcam/guids.h と画面の VcamRegistration.cs) を足す。

namespace denpachan {

inline constexpr int kMaxChannels = 8;

}  // namespace denpachan
