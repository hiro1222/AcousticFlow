# -*- coding: utf-8 -*-
"""wwise_waapi.py ── Wwise のプロジェクトを、画面を開かずに操作する小さな道具（WAAPI の HTTP 口）。

■ 全体の中の位置
  自前のエンジン（Unity 側）と自前の Wwise プラグイン（WwisePlugin/AcousticFlow）を、Wwise のプロジェクトへ繋ぐ「組み立て役」。
  WwiseConsole の waapi-server を裏で立て、HTTP POST で WAAPI を呼ぶ。標準ライブラリだけ（pip を使わない）。

■ 使い方
  python tools/wwise_waapi.py info            … サーバーを立ててプロジェクトの様子を出す（調べもの用）
  python tools/wwise_waapi.py call URI JSON   … WAAPI を 1 回呼ぶ（調べもの用）
  （組み立て本体は tools/wwise_setup.py）

■ 壊れる所
  ・Wwise の画面（Authoring）で同じプロジェクトを開いていると、保存がぶつかる。画面は閉じてから使う。
  ・サーバーは使い終わったら必ず止める（止め忘れるとプロジェクトを掴んだままになる）。
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

WWISE = os.environ.get("WWISEROOT", r"C:\Audiokinetic\Wwise_2025.1.8.9170")
CONSOLE = os.path.join(WWISE, "Authoring", "x64", "Release", "bin", "WwiseConsole.exe")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 組む先の Wwise プロジェクト（Projects/ の直下に、ゲームのプロジェクトと並べて置いてある）。
PROJECTS = {
    "unity":  os.path.join(ROOT, "Projects", "UnityDemo_WwiseProject", "UnityDemo_WwiseProject.wproj"),
    "unreal": os.path.join(ROOT, "Projects", "UnrealDemo_WwiseProject", "UnrealDemo_WwiseProject.wproj"),
}
PROJECT = PROJECTS["unity"]


def use(name):
    """組む先を選ぶ（unity / unreal）。Server() を立てる前に呼ぶ。"""
    global PROJECT
    PROJECT = PROJECTS[name]
# ★既定の 8090 は、この機械では別のアプリ（WsToastNotification）が使っていて HTTP の口が開けなかった（2026-09-29）。
PORT = 18090


def call(uri, args=None, options=None, timeout=60):
    """WAAPI を 1 回呼ぶ。失敗したら RuntimeError（Wwise の答えをそのまま載せる）。"""
    body = json.dumps({"uri": uri, "args": args or {}, "options": options or {}}).encode("utf-8")
    req = urllib.request.Request("http://127.0.0.1:%d/waapi" % PORT, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8") or "{}")
    except urllib.error.HTTPError as e:
        raise RuntimeError("%s → %s" % (uri, e.read().decode("utf-8", "replace")))


class Server:
    """with Server(): の間だけ waapi-server を立てる。"""
    def __enter__(self):
        self.p = subprocess.Popen([CONSOLE, "waapi-server", PROJECT, "--allow-migration", "--http-port", str(PORT),
                                   "--wamp-port", "0", "--no-source-control"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(240):                       # 立ち上がりを最大 2 分待つ（プロジェクトの読み込み）
            time.sleep(0.5)
            try:
                call("ak.wwise.core.getInfo", timeout=2)
                return self
            except Exception:
                if self.p.poll() is not None:
                    raise RuntimeError("waapi-server が落ちた:\n" + self.p.stdout.read().decode("utf-8", "replace"))
        self.p.kill()                              # ★立ち上がらなかったときも必ず止める（プロジェクトを掴んだまま残さない）
        raise RuntimeError("waapi-server が立ち上がらない")

    def __exit__(self, *exc):
        try:
            call("ak.wwise.core.project.save", timeout=30)
        except Exception:
            pass
        self.p.terminate()
        try:
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
    with Server():
        if cmd == "info":
            info = call("ak.wwise.core.getInfo")
            print("Wwise", info.get("version", {}).get("displayName"))
            for q in ["$ from type Bus", "$ from type AudioDevice", "$ from type WorkUnit"]:
                r = call("ak.wwise.core.object.get", {"waql": q}, {"return": ["name", "type", "path", "id"]})
                print("==", q)
                for o in r.get("return", []):
                    print("   ", o.get("type"), o.get("path"))
        elif cmd == "call":
            print(json.dumps(call(sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else {},
                                  json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}), ensure_ascii=False, indent=1))
