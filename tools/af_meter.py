# af_meter.py ── 左右の計器のビューア（別の窓、2026-10-05）。ゲームが鳴らしている音の左右の大きさをグラフで見る。
#
# ■ 全体の中の位置
#   Wwise の自前プラグイン（AcousticFlowFX）が 1 ブロックごとに、左右の RMS とピークを段ごとに
#   名前付きの共有メモリ "Local\AcousticFlowMeterV1" へ書く（エンジンの AF_HostMeterWrite、acoustic_host.h）。
#   ここはそれを読んで描くだけ。Unity でも Unreal でも、Wwise で鳴らしていれば同じように見える。
# ■ 見える物
#   1) 全体の左右のバー（RMS、細い印はピーク）と、左右の差 L−R（正なら左が大きい）
#   2) 直近の左右の大きさの線（dBFS）
#   3) 左右の差の線（±24 dB）── 扉の左右・頭の向きで音がどちらへ寄るかが一目で分かる
#   4) 段ごとの左右: 直接と反射（Voice の和）／戸口の響き（FDN の直出し）／方向バス（反射・響きのレーン）
# ■ 使い方
#   python tools/af_meter.py      （ゲームより先に立ち上げてもよい。データが来るまで「待ち」と出す）
#   キー: スペース 止める／再開、+ / − 見る長さ（5・10・20・40 秒）
# ■ 壊れる所
#   ・Wwise が無音の状態（出力の機器が止まって "Hardware audio subsystem stopped responding"）だと、プラグインが呼ばれず何も来ない。
#   ・共有メモリの並び（acoustic_host.h の AF_METER_*）を変えたら、ここの SIZE と FMT も合わせる（版の文字 AFMETER1 で見分ける）。
#   ・Unity の Wwise を通さない鳴らし方（TailBusRenderer）は、まだ書いていない。
import math
import mmap
import struct
import time
import tkinter as tk

NAME = "Local\\AcousticFlowMeterV1"
CAP, STAGES = 2048, 4
HEAD = struct.Struct("<8sIIIIq")              # magic, version, capacity, stages, reserved, writeCount（32 バイト）
ENTRY = struct.Struct("<dii" + "f" * 16)       # timeSec, frames, sampleRate, rmsL[4], rmsR[4], peakL[4], peakR[4]（80 バイト）
SIZE = HEAD.size + ENTRY.size * CAP
STAGE_NAMES = ["全体", "直接と反射", "戸口の響き", "方向バス"]

# 色（暗い地）
BG, PANEL, GRID, TEXT, MUTED = "#12181b", "#1a2226", "#2c373d", "#e3e9ec", "#8a99a3"
COL_L, COL_R, COL_D = "#4cc3cc", "#e3a64f", "#b48ef0"
FONT = ("Yu Gothic UI", 10)
FONT_B = ("Yu Gothic UI", 12, "bold")
FONT_BIG = ("Yu Gothic UI", 20, "bold")
DB_MIN = -60.0


def db(x):
    return 20.0 * math.log10(max(x, 1e-6))


class Meter:
    def __init__(self, root):
        self.root = root
        self.mm = mmap.mmap(-1, SIZE, tagname=NAME)   # 開く（無ければ作る。ゲームの CreateFileMapping と同じ物を指す）
        self.last = None                              # 読んだ writeCount
        self.hist = []                                # [(time, frames, eL[4], eR[4], pL[4], pR[4])]（エネルギー＝RMS²）
        self.window = 10.0
        self.paused = False
        self.arrived = 0.0                            # 最後に新しい欄が来た時刻（こちらの時計）
        self.rate = 0.0
        self.peak_hold = [[0.0, 0.0, 0.0] for _ in range(STAGES)]   # [L, R, 保った時刻]
        root.title("AcousticFlow 左右の計器")
        root.configure(bg=BG)
        self.cv = tk.Canvas(root, width=960, height=680, bg=BG, highlightthickness=0)
        self.cv.pack(fill="both", expand=True)
        root.bind("<space>", lambda e: self.toggle())
        root.bind("<plus>", lambda e: self.zoom(2.0))
        root.bind("<KP_Add>", lambda e: self.zoom(2.0))
        root.bind("<minus>", lambda e: self.zoom(0.5))
        root.bind("<KP_Subtract>", lambda e: self.zoom(0.5))
        self.tick()

    def toggle(self):
        self.paused = not self.paused

    def zoom(self, f):
        self.window = min(40.0, max(5.0, self.window * f))

    # ── 読む ──
    def read(self):
        magic, ver, cap, stages, _, count = HEAD.unpack_from(self.mm, 0)
        if magic != b"AFMETER1" or cap != CAP or stages != STAGES:
            return
        if self.last is None or count < self.last:     # 初回・ゲームを立ち上げ直した（数が戻った）
            self.last = max(0, count - CAP)
            self.hist.clear()
        start = max(self.last, count - CAP + 1)
        n = 0
        for i in range(start, count):
            v = ENTRY.unpack_from(self.mm, HEAD.size + ENTRY.size * (i % CAP))
            t, fr, sr = v[0], v[1], v[2]
            rl, rr, pl, pr = v[3:7], v[7:11], v[11:15], v[15:19]
            self.hist.append((t, fr, [x * x for x in rl], [x * x for x in rr], pl, pr))
            n += 1
        self.last = count
        if n:
            now = time.monotonic()
            dt = now - self.arrived if self.arrived else 0.0
            self.arrived = now
            if dt > 0: self.rate = 0.8 * self.rate + 0.2 * (n / dt) if self.rate else n / dt
        if self.hist:                                   # 見る長さ＋余裕だけ残す
            tmax = self.hist[-1][0]
            k = 0
            while k < len(self.hist) and self.hist[k][0] < tmax - 45.0: k += 1
            if k: del self.hist[:k]

    # ── まとめる ──
    def recent(self, sec):
        """直近 sec 秒の段ごとの RMS（L, R）とピーク。"""
        if not self.hist: return None
        tmax = self.hist[-1][0]
        eL = [0.0] * STAGES; eR = [0.0] * STAGES; pL = [0.0] * STAGES; pR = [0.0] * STAGES; w = 0
        for t, fr, el, er, pl, pr in reversed(self.hist):
            if t < tmax - sec: break
            for s in range(STAGES):
                eL[s] += el[s] * fr; eR[s] += er[s] * fr
                pL[s] = max(pL[s], pl[s]); pR[s] = max(pR[s], pr[s])
            w += fr
        if w == 0: return None
        return [math.sqrt(x / w) for x in eL], [math.sqrt(x / w) for x in eR], pL, pR

    def series(self, bin_sec=0.05):
        """全体の左右を bin_sec ごとの RMS にした線（時間は最新を 0 とする）。"""
        if not self.hist: return []
        tmax = self.hist[-1][0]
        bins = {}
        for t, fr, el, er, pl, pr in self.hist:
            age = tmax - t
            if age > self.window: continue
            b = int(age / bin_sec)
            a = bins.setdefault(b, [0.0, 0.0, 0])
            a[0] += el[0] * fr; a[1] += er[0] * fr; a[2] += fr
        out = []
        for b in sorted(bins):
            l, r, w = bins[b]
            out.append((b * bin_sec, db(math.sqrt(l / w)), db(math.sqrt(r / w))))
        return out

    # ── 描く ──
    def tick(self):
        try:
            if not self.paused: self.read()
            self.draw()
        finally:
            self.root.after(33, self.tick)

    def bar(self, x, y, w, h, value_db, peak_db, color):
        cv = self.cv
        cv.create_rectangle(x, y, x + w, y + h, fill=PANEL, outline="")
        f = (max(DB_MIN, min(0.0, value_db)) - DB_MIN) / -DB_MIN
        cv.create_rectangle(x, y, x + w * f, y + h, fill=color, outline="")
        fp = (max(DB_MIN, min(0.0, peak_db)) - DB_MIN) / -DB_MIN
        cv.create_line(x + w * fp, y - 2, x + w * fp, y + h + 2, fill=TEXT, width=2)

    def draw(self):
        cv = self.cv
        cv.delete("all")
        W = max(600, cv.winfo_width()); H = max(500, cv.winfo_height())
        pad = 20
        alive = self.arrived and (time.monotonic() - self.arrived) < 1.0
        state = "受信中" if alive else ("止めている" if self.paused else "待ち（Play していない／Wwise が止まっている）")
        cv.create_text(pad, 14, anchor="nw", fill=MUTED, font=FONT,
                       text=f"{state}　{self.rate:5.1f} ブロック/秒　見る長さ {self.window:.0f} 秒（+/−）　スペースで止める")
        rec = self.recent(0.15)
        # 1) 全体の左右
        y = 44
        cv.create_text(pad, y, anchor="nw", fill=TEXT, font=FONT_B, text="全体")
        if rec:
            rl, rr, pl, pr = rec
            now = time.monotonic()
            for s in range(STAGES):   # ピークを 1 秒保つ
                hold = self.peak_hold[s]
                if pl[s] >= hold[0] or pr[s] >= hold[1] or now - hold[2] > 1.0:
                    self.peak_hold[s] = [max(pl[s], hold[0] if now - hold[2] <= 1.0 else 0.0),
                                         max(pr[s], hold[1] if now - hold[2] <= 1.0 else 0.0), now]
            L, R = db(rl[0]), db(rr[0])
            bw = W - pad * 2 - 260
            self.bar(pad + 40, y + 28, bw, 18, L, db(self.peak_hold[0][0]), COL_L)
            self.bar(pad + 40, y + 52, bw, 18, R, db(self.peak_hold[0][1]), COL_R)
            cv.create_text(pad + 20, y + 37, fill=COL_L, font=FONT_B, text="L")
            cv.create_text(pad + 20, y + 61, fill=COL_R, font=FONT_B, text="R")
            cv.create_text(pad + 50 + bw, y + 37, anchor="w", fill=COL_L, font=FONT, text=f"{L:6.1f} dBFS")
            cv.create_text(pad + 50 + bw, y + 61, anchor="w", fill=COL_R, font=FONT, text=f"{R:6.1f} dBFS")
            d = L - R
            side = "左が大きい" if d > 0.5 else ("右が大きい" if d < -0.5 else "ほぼ同じ")
            cv.create_text(W - pad, y + 30, anchor="ne", fill=COL_D, font=FONT_BIG, text=f"L−R {d:+5.1f} dB")
            cv.create_text(W - pad, y + 62, anchor="ne", fill=MUTED, font=FONT, text=side)
        # 目盛り（−60〜0 dB）
        bw = W - pad * 2 - 260
        for g in range(-60, 1, 10):
            gx = pad + 40 + bw * (g - DB_MIN) / -DB_MIN
            cv.create_text(gx, y + 80, fill=MUTED, font=("Yu Gothic UI", 8), text=str(g))
        # 2) 左右の線
        top, gh = y + 100, int((H - y - 100 - 200) * 0.6)
        self.graph_frame(pad, top, W - pad * 2, gh, DB_MIN, 0.0, [-60, -40, -20, 0], "左右の大きさ（dBFS）")
        ser = self.series()
        if len(ser) > 1:
            gw = W - pad * 2
            for idx, col in ((1, COL_L), (2, COL_R)):
                pts = []
                for age, l, r in ser:
                    v = (l, r)[idx - 1]
                    px = pad + gw * (1.0 - age / self.window)
                    py = top + gh * (1.0 - (max(DB_MIN, min(0.0, v)) - DB_MIN) / -DB_MIN)
                    pts += [px, py]
                cv.create_line(*pts, fill=col, width=2)
        # 3) 左右の差
        top2, gh2 = top + gh + 34, int((H - y - 100 - 200) * 0.4)
        self.graph_frame(pad, top2, W - pad * 2, gh2, -24.0, 24.0, [-24, -12, 0, 12, 24], "左右の差 L−R（dB、上が左）")
        if len(ser) > 1:
            gw = W - pad * 2
            pts = []
            for age, l, r in ser:
                v = max(-24.0, min(24.0, l - r))
                pts += [pad + gw * (1.0 - age / self.window), top2 + gh2 * (1.0 - (v + 24.0) / 48.0)]
            cv.create_line(*pts, fill=COL_D, width=2)
        # 4) 段ごと
        y4 = top2 + gh2 + 36
        cv.create_text(pad, y4 - 22, anchor="nw", fill=TEXT, font=FONT_B, text="段ごとの左右（直近 0.15 秒）")
        colw = (W - pad * 2) / 3
        for i, s in enumerate((1, 2, 3)):
            x0 = pad + colw * i
            cv.create_text(x0, y4 + 4, anchor="nw", fill=TEXT, font=FONT, text=STAGE_NAMES[s])
            if rec:
                rl, rr, pl, pr = rec
                L, R = db(rl[s]), db(rr[s])
                self.bar(x0, y4 + 28, colw - 30, 14, L, db(self.peak_hold[s][0]), COL_L)
                self.bar(x0, y4 + 48, colw - 30, 14, R, db(self.peak_hold[s][1]), COL_R)
                txt = f"L {L:6.1f}  R {R:6.1f}  差 {L - R:+5.1f} dB" if max(L, R) > -119 else "鳴っていない"
                cv.create_text(x0, y4 + 70, anchor="nw", fill=MUTED, font=FONT, text=txt)

    def graph_frame(self, x, y, w, h, vmin, vmax, ticks, title):
        cv = self.cv
        cv.create_text(x, y - 18, anchor="nw", fill=TEXT, font=FONT_B, text=title)
        cv.create_rectangle(x, y, x + w, y + h, fill=PANEL, outline="")
        for t in ticks:
            ty = y + h * (1.0 - (t - vmin) / (vmax - vmin))
            cv.create_line(x, ty, x + w, ty, fill=GRID)
            cv.create_text(x + w - 4, ty - 2, anchor="se", fill=MUTED, font=("Yu Gothic UI", 8), text=str(t))
        for s in range(0, int(self.window) + 1, max(1, int(self.window / 5))):
            tx = x + w * (1.0 - s / self.window)
            cv.create_line(tx, y, tx, y + h, fill=GRID)
            cv.create_text(tx, y + h + 2, anchor="n", fill=MUTED, font=("Yu Gothic UI", 8), text=f"-{s}s" if s else "今")


if __name__ == "__main__":
    root = tk.Tk()
    root.geometry("960x680")
    Meter(root)
    root.mainloop()
