# -*- coding: utf-8 -*-
"""wwise_setup.py ── Unity の Wwise プロジェクトに、自前のエンジンで鳴らすための最小の形を組む（何度回しても同じ形になる）。

■ 全体の中の位置
  Unity（AcousticWorld / WorldVoice）が自前のエンジンで配分を解く
    → Wwise の Event が音を鳴らし、その音はバス「AcousticFlow」に届く
    → バスに差した自前のプラグイン（WwisePlugin/AcousticFlow、会社 64・ID 20311）が、位置で Voice を引いて両耳の音にする。
  このスクリプトは、その「バス」「プラグイン」「試験の音」「Event」を Wwise のプロジェクトに作る係。

■ 組む物
  1) バス AcousticFlow（Main Audio Bus の子）。形は「Audio Objects」（768）── 音が 1 本ずつ位置付きで届くように。
     ★Audio Objects でないと、音はバスの中で混ぜられてから届き、どの音源の音か（位置）が分からなくなる。
  2) エフェクト AF_Renderer（自前のプラグイン）を作り、バスの 1 枠目に差す。
  3) 試験の音 AF_TestTone（Unity の場面と同じ FreeTestSound.wav。2026-10-10 に差し替え。その前は DemonDanceTokyo_Eve.wav・videoplayback.wav）。出口はバス AcousticFlow、無限ループ、
     立体化は「Position」（位置を持たせる。持たないと対応付けできない）、Wwise の距離減衰は切る（距離は自前のエンジンが持つ）。
  4) Event Play_AF_Test（AF_TestTone を鳴らす）。
  5) 保存して、Windows のバンクを作る（このプロジェクトは Event ごとに自動でバンクを作る設定）。

■ 退けた書き方
  ・Wwise の画面で手で組む: 同じ形を二度と作れない（作り直すたびに手順を思い出す）。スクリプトなら何度でも同じ。
  ・Wwise の距離減衰を残す: 自前のエンジンも 1/r を掛けるので二重に小さくなる。

■ 壊れる所
  ・Wwise の画面でこのプロジェクトを開いたまま回すと、保存がぶつかる。画面は閉じてから。
  ・試験の音の立体化を None（2D）にすると、位置が聞き手の場所のまま届き、対応付けが全部外れる（素通しで鳴る）。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwise_waapi as w  # noqa: E402

COMPANY_ID = 64
PLUGIN_ID = 20311
AK_PLUGIN_TYPE_EFFECT = 3
# AKMAKECLASSID(type, company, plugin) ＝ 種類（下位 4 bit）| 会社（12 bit を 4 bit 上へ）| プラグイン（16 bit を 16 bit 上へ）
CLASS_ID = (AK_PLUGIN_TYPE_EFFECT & 0xF) | ((COMPANY_ID & 0xFFF) << 4) | ((PLUGIN_ID & 0xFFFF) << 16)
AUDIO_OBJECTS = 768
# 試験の音の素材（2026-10-10 にフリー音源 FreeTestSound.wav へ差し替え。発注者の指定。別の PC へ git で持っていくため）
#   ★Unity の Assets/Audio の物を共用する（Unity の場面の AudioSource と同じ物）。.wav はリポジトリに入れない決まりだが、
#     このフリー音源だけは .gitignore の例外にして入れる。前の 2 曲（DemonDanceTokyo_Eve・videoplayback）は著作物なので入れない。
#     前の素材は AF_TestTone の中に残してあり、この PC の Wwise の画面では戻せる（向こうの PC には素材が無い）。
WAV = os.path.join(w.ROOT, "Projects", "UnityDemo", "Assets", "Audio", "FreeTestSound.wav")

MAIN_BUS = "\\Busses\\Default Work Unit\\Main Audio Bus"
BUS = MAIN_BUS + "\\AcousticFlow"
EFFECTS_WU = "\\Effects\\Default Work Unit"
EFFECT = EFFECTS_WU + "\\AF_Renderer"
CONTAINERS_WU = "\\Containers\\Default Work Unit"
SOUND = CONTAINERS_WU + "\\AF_TestTone"
EVENTS_WU = "\\Events\\Default Work Unit"
EVENT = EVENTS_WU + "\\Play_AF_Test"


def get(path, ret=("id", "name", "type", "path")):
    r = w.call("ak.wwise.core.object.get", {"from": {"path": [path]}}, {"return": list(ret)}).get("return", [])
    return r[0] if r else None


def main():
    # 引数で組む先を選ぶ: python tools/wwise_setup.py unity|unreal（既定 unity）
    target = sys.argv[1] if len(sys.argv) > 1 else "unity"
    w.use(target)
    print("組む先:", w.PROJECT)
    with w.Server():
        print("Wwise", w.call("ak.wwise.core.getInfo").get("version", {}).get("displayName"))

        # 1) バス
        w.call("ak.wwise.core.object.create", {"parent": MAIN_BUS, "type": "Bus", "name": "AcousticFlow", "onNameConflict": "merge"})
        bus = get(BUS)
        w.call("ak.wwise.core.object.setProperty", {"object": bus["id"], "property": "BusChannelConfig", "value": AUDIO_OBJECTS})
        print("バス:", BUS, "形 = Audio Objects")

        # 2) 自前のプラグインを、バスの 1 枠目にその場で作って差す。
        #    ★2025.1 の WAAPI は object.create で classId を受け付けない（「Argument classId is unknown」）。
        #      公式の例どおり、object.set の @Effects の中で { type: Effect, classId } を書くと、その枠専用のエフェクトができる。
        #    ★listMode は replaceAll（枠を全部置き換える）。既定の append（付け足す）だと、回すたびに枠が 1 つ増える。
        #      2026-09-30、Unreal 用に 2 回回して AF_Renderer が 2 段直列になり、器が 1 ブロックに 2 回回って千切れ、
        #      プラグインの見張り（1 ブロックに 1 回）を入れたら 2 段目が 1 段目の音を消して無音になった。
        w.call("ak.wwise.core.object.set", {"objects": [{"object": bus["id"], "@Effects": [
            {"type": "EffectSlot", "name": "", "@Effect": {"type": "Effect", "name": "AF_Renderer", "classId": CLASS_ID}}]}],
            "listMode": "replaceAll"})
        chk = w.call("ak.wwise.core.object.get", {"from": {"path": [BUS]}},
                     {"return": ["@Effects"]}).get("return", [{}])[0]
        slots = chk.get("@Effects") or []
        print("バスの枠:", len(slots), "個", slots, "（classId", CLASS_ID, "）")
        if len(slots) != 1:
            raise RuntimeError(f"バス AcousticFlow のエフェクトの枠が {len(slots)} 個（1 個でないと、直列に 2 回回って千切れる／無音になる）")

        # 3) 試験の音
        w.call("ak.wwise.core.audio.import", {"importOperation": "useExisting", "default": {"importLanguage": "SFX"},
                                               "imports": [{"audioFile": WAV, "objectPath": CONTAINERS_WU + "\\<Sound SFX>AF_TestTone"}]})
        snd = get(SOUND)
        for prop, val in [("OverrideOutput", True), ("IsLoopingEnabled", True), ("IsLoopingInfinite", True),
                          ("OverridePositioning", True), ("3DSpatialization", 1), ("EnableAttenuation", False)]:
            try:
                w.call("ak.wwise.core.object.setProperty", {"object": snd["id"], "property": prop, "value": val})
            except RuntimeError as e:
                print("  （設定できなかった）", prop, str(e)[:160])
        w.call("ak.wwise.core.object.setReference", {"object": snd["id"], "reference": "OutputBus", "value": bus["id"]})
        s2 = get(SOUND, ("id", "OutputBus", "3DSpatialization", "EnableAttenuation", "IsLoopingEnabled"))
        print("音:", SOUND, {k: (v.get("name") if isinstance(v, dict) else v) for k, v in s2.items() if k != "id"})

        # 4) Event
        w.call("ak.wwise.core.object.create", {"parent": EVENTS_WU, "type": "Event", "name": "Play_AF_Test", "onNameConflict": "replace",
                                                "children": [{"type": "Action", "name": "", "@ActionType": 1, "@Target": snd["id"]}]})
        print("Event:", EVENT)

        # 5) プロジェクトの設定（バンクの作り方）。Unreal 版の Wwise が読むのに要る 10 個（統合の AkAssetMigrationHelper と同じ）。
        #    ★Unity 用は Launcher が入れてくれたが、Unreal 用は空だった（Unreal のエディタを開いたときに統合が自分で直す作り）。
        #      エディタを開かずに進めるので、ここで入れる。
        #    ★MediaAutoBankSubFolders（バンクと素材を番号のサブフォルダに分ける）は Unreal 用だけ。2026-10-10 まで Unity 用にも入れていて、
        #      バンクが Event/33/Play_AF_Test.bnk に出て、Unity の統合（既定はサブフォルダを使わない）が見つけられず無音になっていた
        #      （Unity のログ: Bank Play_AF_Test failed to load (AK_FileNotFound)）。Unity 用は切る。
        proj = w.call("ak.wwise.core.object.get", {"waql": "$ from type Project"}, {"return": ["id", "name"]})["return"][0]
        for prop in ["AutoSoundBankEnabled", "CopyLooseStreamedMedia", "GenerateMultipleBanks", "GenerateSoundBankJSON",
                     "MediaAutoBankSubFolders", "RemoveUnusedGeneratedFiles", "SoundBankGenerateEstimatedDuration",
                     "SoundBankGenerateMaxAttenuationInfo", "SoundBankGeneratePrintGUID", "SoundBankGeneratePrintPath"]:
            value = not (prop == "MediaAutoBankSubFolders" and target == "unity")
            try:
                w.call("ak.wwise.core.object.setProperty", {"object": proj["id"], "property": prop, "value": value})
            except RuntimeError as e:
                print("  （設定できなかった）", prop, str(e)[:160])
        print("プロジェクトの設定: 自動バンク・JSON など 10 個を入")

        # 6) 保存とバンク
        w.call("ak.wwise.core.project.save")
        try:
            r = w.call("ak.wwise.core.soundbank.generate", {"writeToDisk": True, "platforms": ["Windows"], "rebuildInitBank": True}, timeout=600)
            print("バンク:", "作った", len(r.get("soundBanks", [])) if isinstance(r, dict) else r)
        except RuntimeError as e:
            print("バンクの生成に失敗:", str(e)[:400])


if __name__ == "__main__":
    main()
