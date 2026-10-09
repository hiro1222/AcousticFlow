# -*- coding: utf-8 -*-
"""make_site.py ── docs/study/ の 11 章を「サイト」にする（Artifact「AcousticFlow を学ぶ」の元）。

使い方:  python docs/study/make_site.py [出力フォルダ]          … Artifact 用（docs/study/_site/）
        python docs/study/make_site.py --web [出力フォルダ]    … ふつうの Web サイト用（docs/study/_site_web/。GitHub Pages など）
  既定の出力先は docs/study/_site/（作り直せる物なので Git には入れない）。
  出来る物: index.html（表紙。Artifact の本体）、ch00.html〜ch10.html（章ごとのページ）、site.css、site.js、search-index.js（検索の索引）。
  章を直したら、これを回して同じ Artifact に出し直す（URL は記憶 study-guide-docs に控えてある）。

中身:
  ・章の Markdown を自前の小さな変換で HTML にする（見出し・段落・箇条書き・表・コード・引用・横線・太字・`コード`・リンク）。
    章どうしのリンク（01_files.md など）は章のページへ。リポジトリのファイルへのリンクは名前だけ残す（サイトからは開けない）。
  ・どのページにも同じ左の目次（スマホでは ☰ で開く）、上の検索（節ごとの索引を文字列で引く）、コードのコピーボタン。
"""
import html
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# --web: ふつうの Web サイト用（GitHub Pages など）。表紙にも <!doctype> と <head> を付け、.nojekyll を置く。既定の出力先は _site_web/。
#   付けないとき（Artifact 用）は、表紙を Artifact が包むので <head> を書かない。
WEB = "--web" in sys.argv
_args = [a for a in sys.argv[1:] if a != "--web"]
OUT = _args[0] if _args else os.path.join(HERE, "_site_web" if WEB else "_site")

# (ファイル, 短い名前, 群, 中身, こんなときに読む)
CHAPTERS = [
    ("00_README.md", "はじめに", "全体", "全体の絵（3 つの層）、1 フレームと 1 ブロックの流れ、用語、決めごと", "最初に"),
    ("01_files.md", "ファイル構成", "全体", "今動いている 50 ファイルの役割と、写さなくてよい旧コアの区別", "どのファイルが何かを引きたいとき"),
    ("02_engine_geometry.md", "形と部屋", "エンジン", "箱・材質・部屋グラフ（ボクセル・侵食）・戸口・Sabine の RT60", "部屋分けや響きの長さを知りたいとき"),
    ("03_engine_propagation.md", "音の届き方", "エンジン", "レイ（NEE）・見通しの半影・前川の回折・虚像の面音源・予算・GPU", "音がどう届くかを知りたいとき"),
    ("04_engine_distribute_frame.md", "配分と 1 フレーム", "エンジン", "五成分の配分・DSP への橋・1 フレームの順番・戸口の線音源", "「量」がどこで決まるかを知りたいとき"),
    ("05_engine_dsp.md", "音を作る所", "エンジン", "Voice・HRTF・方向バス・帯域ごとの FDN・部屋ごとの配線", "波形がどう作られるかを知りたいとき"),
    ("06_c_api.md", "C の窓口", "エンジン", "DLL の関数を群ごとに、台帳（seqlock）、どのホストでも同じ呼ぶ順番", "エンジンを外から使うとき"),
    ("07_connector_wwise.md", "Wwise", "コネクタ層", "自前のプラグイン AF_Renderer、作り方と配り方、Wwise プロジェクトの組み立て", "Wwise で鳴らす所"),
    ("08_connector_unreal.md", "Unreal", "コネクタ層", "司令塔・音源・扉・座標の橋・地図の置き方・ビルド", "Unreal に載せる所"),
    ("09_connector_unity.md", "Unity", "コネクタ層", "AcousticWorld・WorldVoice・耳の側の器・場面に置く物", "Unity に載せる所"),
    ("10_rebuild.md", "作り直し", "作る", "別のフォルダに同じ物を作る手順。段ごとに写す → ビルド → 検査", "自分で作り直すとき"),
]
PAGE = {c[0]: "ch%02d.html" % i for i, c in enumerate(CHAPTERS)}
GROUPS = ["全体", "エンジン", "コネクタ層", "作る"]


# ── 行の中（インライン）──
def inline(s):
    out = []
    for p in re.split(r"(`[^`]+`)", s):
        if len(p) >= 2 and p.startswith("`") and p.endswith("`"):
            out.append("<code>%s</code>" % html.escape(p[1:-1]))
            continue
        t = html.escape(p, quote=False)

        def link(m):
            text, href = m.group(1), m.group(2)
            base, _, frag = href.partition("#")
            if base in PAGE:
                return '<a href="%s%s">%s</a>' % (PAGE[base], ("#" + frag) if frag else "", text)
            if href.startswith("http"):
                return '<a href="%s" target="_blank" rel="noopener">%s</a>' % (html.escape(href), text)
            return '<span class="path">%s</span>' % text
        t = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, t)
        t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
        out.append(t)
    return "".join(out)


def plain(s):
    """検索の索引用に Markdown の記号を落とす。"""
    s = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", s)
    s = s.replace("**", "").replace("`", "")
    s = re.sub(r"^\s*[|#>\-*]+\s*", "", s)
    s = s.replace("|", " ")
    return re.sub(r"\s+", " ", s).strip()


LANG = {"cpp": "C++", "c": "C", "csharp": "C#", "cs": "C#", "bash": "bash", "cmake": "CMake", "ini": "ini", "python": "Python", "hlsl": "HLSL"}


def code_html(lang, lines):
    esc = []
    for ln in lines:
        e = html.escape(ln, quote=False)
        if lang not in ("bash", "sh", "ini"):
            k = e.find("//")
            if k >= 0:
                e = e[:k] + '<span class="cm">' + e[k:] + "</span>"
        if lang in ("bash", "sh", "ini", "cmake", "python", ""):
            m = re.match(r"^(\s*)(#.*)$", e)
            if m and not re.match(r"\s*#(include|define|if|endif|else|pragma)", e):
                e = m.group(1) + '<span class="cm">' + m.group(2) + "</span>"
        if lang == "ini":
            m = re.match(r"^(\s*)(;.*)$", e)
            if m:
                e = m.group(1) + '<span class="cm">' + m.group(2) + "</span>"
        esc.append(e)
    return ('<figure class="code"><div class="codehead"><span>%s</span>'
            '<button class="copy" type="button">コピー</button></div><pre><code>%s</code></pre></figure>'
            % (LANG.get(lang, ""), "\n".join(esc)))


def convert(md):
    """Markdown → (表題, 節の一覧 [(id, 見出し)], 本文の HTML, 索引 [(id, 見出し, 文字)])"""
    lines = md.replace("\r\n", "\n").split("\n")
    out, toc, idx = [], [], []
    title, sec, i, n = "", 0, 0, len(lines)
    cur_id, cur_head, cur_text = "top", "", []

    def flush():
        if cur_text:
            idx.append((cur_id, cur_head, " ".join(t for t in cur_text if t)))

    def is_row(s):
        return s.strip().startswith("|") and s.strip().endswith("|")

    while i < n:
        ln = lines[i]
        s = ln.strip()
        if not s:
            i += 1
            continue
        m = re.match(r"^\s*```(\w*)\s*$", ln)
        if m:
            lang = m.group(1).lower()
            i += 1
            buf = []
            while i < n and not re.match(r"^\s*```\s*$", lines[i]):
                buf.append(lines[i])
                i += 1
            i += 1
            out.append(code_html(lang, buf))
            cur_text.append(plain(" ".join(buf)))
            continue
        m = re.match(r"^(#{1,4})\s+(.*)$", ln)
        if m:
            lv, txt = len(m.group(1)), m.group(2).strip()
            if lv == 1:
                title = txt
                cur_head = txt
                out.append('<h1>%s</h1>' % inline(txt))
            elif lv == 2:
                flush()
                sec += 1
                cur_id, cur_head, cur_text = "s%d" % sec, plain(txt), []
                toc.append((cur_id, txt))
                out.append('<h2 id="%s">%s</h2>' % (cur_id, inline(txt)))
            else:
                out.append("<h%d>%s</h%d>" % (lv, inline(txt), lv))
                cur_text.append(plain(txt))
            i += 1
            continue
        if re.match(r"^-{3,}$", s):
            out.append("<hr>")
            i += 1
            continue
        if is_row(ln) and i + 1 < n and re.match(r"^\s*\|[\s:\-|]+\|\s*$", lines[i + 1]):
            hdr = [c.strip() for c in s.strip("|").split("|")]
            i += 2
            rows = []
            while i < n and is_row(lines[i]):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            th = "".join("<th>%s</th>" % inline(c) for c in hdr)
            tb = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c).replace("&lt;br&gt;", "<br>") for c in r) for r in rows)
            out.append('<div class="tablewrap"><table><thead><tr>%s</tr></thead><tbody>%s</tbody></table></div>' % (th, tb))
            cur_text.append(plain(" ".join(hdr + [c for r in rows for c in r])))
            continue
        if s.startswith(">"):
            buf = []
            while i < n and lines[i].strip().startswith(">"):
                buf.append(lines[i].strip()[1:].strip())
                i += 1
            out.append("<blockquote><p>%s</p></blockquote>" % inline(" ".join(buf)))
            cur_text.append(plain(" ".join(buf)))
            continue
        if re.match(r"^\s*(-|\d+\.)\s+", ln):
            items = []
            while i < n and lines[i].strip():
                l2 = lines[i]
                m2 = re.match(r"^(\s*)(-|\d+\.)\s+(.*)$", l2)
                if m2:
                    items.append([len(m2.group(1)) // 2, m2.group(2) != "-", m2.group(3), m2.group(2).rstrip(".")])
                elif items and re.match(r"^\s+\S", l2) and not re.match(r"^\s*```", l2):
                    items[-1][2] += " " + l2.strip()
                else:
                    break
                i += 1
            h, stack = [], []
            for depth, ordered, text, mark in items:
                while len(stack) > depth + 1:
                    h.append("</li></%s>" % stack.pop())
                if len(stack) < depth + 1:
                    while len(stack) < depth + 1:
                        tag = "ol" if ordered else "ul"
                        start = (' start="%s"' % mark) if (ordered and mark.isdigit() and mark != "1") else ""
                        h.append("<%s%s>" % (tag, start))
                        stack.append(tag)
                    h.append("<li>%s" % inline(text))
                else:
                    h.append("</li><li>%s" % inline(text))
                cur_text.append(plain(text))
            while stack:
                h.append("</li></%s>" % stack.pop())
            out.append("".join(h))
            continue
        buf = []
        while i < n and lines[i].strip() and not re.match(r"^\s*```", lines[i]) and not re.match(r"^#{1,4}\s", lines[i]) \
                and not is_row(lines[i]) and not re.match(r"^\s*(-|\d+\.)\s+", lines[i]) and not re.match(r"^-{3,}$", lines[i].strip()):
            buf.append(lines[i].strip())
            i += 1
        if buf:
            out.append("<p>%s</p>" % "<br>".join(inline(b) for b in buf))
            cur_text.append(plain(" ".join(buf)))
        else:
            i += 1
    flush()
    return title, toc, "\n".join(out), idx


# ── 共通の部品 ──
HEAD_LINKS = ('<link rel="preconnect" href="https://fonts.googleapis.com">'
              '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>'
              '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600'
              '&family=Noto+Sans+JP:wght@400;500;700&family=Zen+Kaku+Gothic+New:wght@500;700&display=swap">'
              '<link rel="stylesheet" href="site.css">')
MENU_SVG = '<svg viewBox="0 0 24 24" width="22" height="22" aria-hidden="true"><path d="M4 7h16M4 12h16M4 17h16" stroke="currentColor" stroke-width="2" stroke-linecap="round" fill="none"/></svg>'


def topbar():
    return ('<header class="top"><div class="top-in">'
            '<button class="navbtn" id="navbtn" type="button" aria-label="目次を開く" aria-controls="side" aria-expanded="false">%s</button>'
            '<a class="brand" href="index.html">AcousticFlow を学ぶ</a>'
            '<div class="search"><input id="q" type="search" placeholder="検索（関数名・用語）" autocomplete="off" aria-label="サイトの中を検索">'
            '<div class="results" id="results" hidden></div></div>'
            '</div></header>') % MENU_SVG


def sidebar(current, tocs):
    h = ['<aside class="side" id="side" aria-label="章"><nav>']
    h.append('<a class="home%s" href="index.html">表紙</a>' % (" on" if current is None else ""))
    for g in GROUPS:
        h.append('<p class="grp">%s</p><ol>' % g)
        for k, c in enumerate(CHAPTERS):
            if c[2] != g:
                continue
            on = (k == current)
            h.append('<li><a class="ch%s" href="%s"%s><b>%02d</b>%s</a>' % (" on" if on else "", PAGE[c[0]], ' aria-current="page"' if on else "", k, c[1]))
            if on and tocs[k]:
                h.append('<ul>%s</ul>' % "".join('<li><a href="#%s">%s</a></li>' % (sid, inline(t)) for sid, t in tocs[k]))
            h.append("</li>")
        h.append("</ol>")
    h.append("</nav></aside><div class=\"scrim\" id=\"scrim\"></div>")
    return "".join(h)


def foot_scripts():
    return '<script src="search-index.js"></script><script src="site.js"></script>'


def chapter_page(k, title, toc, body, tocs):
    c = CHAPTERS[k]
    prev_ = '<a class="prev" href="%s"><small>前の章</small>%02d %s</a>' % (PAGE[CHAPTERS[k - 1][0]], k - 1, CHAPTERS[k - 1][1]) if k > 0 else '<a class="prev" href="index.html"><small>戻る</small>表紙</a>'
    next_ = '<a class="next" href="%s"><small>次の章</small>%02d %s</a>' % (PAGE[CHAPTERS[k + 1][0]], k + 1, CHAPTERS[k + 1][1]) if k + 1 < len(CHAPTERS) else "<span></span>"
    otp_items = "".join('<li><a href="#%s">%s</a></li>' % (sid, inline(t)) for sid, t in toc)
    otp_m = ('<details class="otp-m"><summary>このページの内容</summary><ol>%s</ol></details>' % otp_items) if toc else ""
    otp = ('<aside class="otp" aria-label="このページの内容"><p class="grp">このページ</p><ol>%s</ol></aside>' % otp_items) if toc else ""
    return ('<!doctype html><html lang="ja"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">'
            '<title>%02d %s · AcousticFlow を学ぶ</title><meta name="description" content="%s">%s</head><body>'
            '%s<div class="layout">%s<main class="content" id="main"><p class="eyebrow">%s · %02d</p>%s<article class="doc">%s</article>'
            '<nav class="pn" aria-label="前後の章">%s%s</nav></main>%s</div>%s</body></html>'
            % (k, c[1], html.escape(c[3]), HEAD_LINKS, topbar(), sidebar(k, tocs), c[2], k, otp_m, body, prev_, next_, otp, foot_scripts()))


def index_page(tocs):
    cards = []
    for g in GROUPS:
        items = []
        for k, c in enumerate(CHAPTERS):
            if c[2] != g:
                continue
            items.append('<a class="card" href="%s"><span class="num">%02d</span><span class="ttl">%s</span>'
                         '<span class="sum">%s</span><span class="when">%s</span></a>' % (PAGE[c[0]], k, c[1], c[3], c[4]))
        cards.append('<section class="cgroup"><h2>%s</h2><div class="cards">%s</div></section>' % (g, "".join(items)))
    layers = (
        '<div class="layers" role="img" aria-label="ホスト、コネクタ層、エンジンの 3 つの層">'
        '<div class="layer host"><p class="lname">ホスト（ゲーム）</p><p class="ldesc">壁・扉・音源・プレイヤーを置く。音の計算はしない</p>'
        '<div class="chips"><a href="ch08.html">Unreal の地図</a><a href="ch09.html">Unity の場面</a></div></div>'
        '<p class="flow">毎フレーム：箱の位置・耳・音源・摘み ↓</p>'
        '<div class="layer conn"><p class="lname">コネクタ層（器）</p><p class="ldesc">値を渡すだけ。Unity と Unreal で同じ名前・同じ既定値</p>'
        '<div class="chips"><a href="ch08.html">AAcousticFlowWorld</a><a href="ch09.html">AcousticWorld</a><a href="ch07.html">Wwise の AF_Renderer</a></div></div>'
        '<p class="flow">C の窓口（acoustic_world.h ほか）↓</p>'
        '<div class="layer eng"><p class="lname">エンジン AcousticEngine.dll</p><p class="ldesc">C++17、ほぼヘッダだけ。50 ファイル・16,374 行</p>'
        '<div class="chips"><a href="ch02.html">Core 形と部屋</a><a href="ch03.html">Flow 音の届き方</a><a href="ch04.html">Flow 配分</a>'
        '<a href="ch05.html">Dsp 音を作る</a><a href="ch03.html#s3">Gpu レイ</a><a href="ch06.html">Export C の窓口</a></div></div>'
        '</div>')
    head = ('<!doctype html><html lang="ja"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">'
            '<meta name="description" content="扉が開くときの音色の変化を形から計算して鳴らす音響エンジン AcousticFlow の、仕組み・コード・ファイル構成・'
            'Wwise／Unreal／Unity のコネクタ層と、同じ物を作り直す手順。">') if WEB else ""
    open_body = "</head><body>" if WEB else ""
    close = "</body></html>" if WEB else ""
    tpl = (head + '<title>AcousticFlow を学ぶ</title>%s' + open_body + '%s<div class="layout">%s<main class="content home" id="main">'
            '<section class="hero"><p class="eyebrow">学習用のまとめ · 2026-10-07 の実装</p><h1>AcousticFlow を学ぶ</h1>'
            '<p class="lede">扉が開くときの音色の変化を、置いた形から計算して鳴らすエンジンの、仕組み・コード・ファイル構成・コネクタ層と、'
            '同じ物をもう一度作る手順。</p>'
            '<div class="badges" aria-label="作り直しで確かめた検査"><span>DSP 160/160</span><span>Flow 196/196</span><span>台帳 10/10</span><span>関数 108/108</span></div>'
            '<p class="note">エンジンの作り直し（段 0〜3）は、別のフォルダで手順どおりに回して検査が全部通ることを確かめてあります。</p></section>'
            '<section><h2>全体の絵</h2>%s<p class="more"><a href="ch00.html">1 フレームの流れと用語 →</a></p></section>'
            '%s'
            '<section class="howto"><h2>読み方</h2><ul>'
            '<li>各ソースファイルの頭に、役割・中の仕組み・繋がり・退けた書き方・壊れる所の 5 項目が書いてあります。このサイトはその要約と読む順です。</li>'
            '<li>エンジンは下の層から：<a href="ch02.html">形と部屋</a> → <a href="ch03.html">音の届き方</a> → <a href="ch04.html">配分</a> → <a href="ch05.html">音を作る所</a> → <a href="ch06.html">C の窓口</a>。</li>'
            '<li>作り直すなら <a href="ch10.html">作り直し</a>。写す代わりに自分で書いても、その段の検査が通れば正解です。</li>'
            '</ul></section></main></div>%s' + close)
    return tpl % (HEAD_LINKS, topbar(), sidebar(None, tocs), layers, "".join(cards), foot_scripts())


CSS = r"""/* 読み物のサイト: 左に章の目次（スマホでは引き出し）、本文は 46rem の 1 段、広い画面では右に「このページ」 */
:root {
  --bg: #f6f7f8; --surface: #ffffff; --fg: #1d242c; --muted: #5d6874; --rule: #dfe3e7;
  --accent: #1f6f78; --accent-soft: #e1eff0; --code-bg: #eef1f3; --code-fg: #22303b; --cm: #6e7f8c;
  --font-display: "Zen Kaku Gothic New", "Hiragino Sans", "Yu Gothic", sans-serif;
  --font-body: "Noto Sans JP", "Hiragino Sans", "Yu Gothic", "Meiryo", sans-serif;
  --font-mono: "JetBrains Mono", "Cascadia Mono", Consolas, "Noto Sans JP", monospace;
  --measure: 46rem; --side: 16.5rem; --top-h: 56px;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #12171c; --surface: #182027; --fg: #e3e8ec; --muted: #9aa7b2; --rule: #2a343d;
  --accent: #6cc3c9; --accent-soft: #1c3236; --code-bg: #1b232b; --code-fg: #d9e1e7; --cm: #7f909c; color-scheme: dark; } }
:root[data-theme="dark"] {
  --bg: #12171c; --surface: #182027; --fg: #e3e8ec; --muted: #9aa7b2; --rule: #2a343d;
  --accent: #6cc3c9; --accent-soft: #1c3236; --code-bg: #1b232b; --code-fg: #d9e1e7; --cm: #7f909c; color-scheme: dark; }

*, *::before, *::after { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body { margin: 0; background: var(--bg); color: var(--fg); font-family: var(--font-body); font-size: 16px; line-height: 1.8; }
[hidden] { display: none !important; }
a { color: var(--accent); text-underline-offset: 3px; }
a:focus-visible, button:focus-visible, input:focus-visible, summary:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }

/* 上の帯 */
.top { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 30; background: var(--surface); border-bottom: 1px solid var(--rule); }
.top-in { height: var(--top-h); display: flex; align-items: center; gap: 10px; padding: 0 16px; }
.navbtn { display: inline-flex; align-items: center; justify-content: center; width: 40px; height: 40px; border: 0; border-radius: 10px; background: transparent; color: var(--fg); cursor: pointer; flex: none; }
.navbtn:hover { background: var(--code-bg); }
.brand { font-family: var(--font-display); font-weight: 700; font-size: 16px; color: var(--fg); text-decoration: none; white-space: nowrap; }
.search { position: relative; flex: 1; min-width: 0; max-width: 26rem; margin-left: auto; }
.search input { width: 100%; height: 38px; border: 1px solid var(--rule); border-radius: 999px; background: var(--bg); color: var(--fg); font: 14px var(--font-body); padding: 0 14px; }
.results { position: absolute; right: 0; top: 46px; width: min(30rem, calc(100vw - 32px)); max-height: 70vh; overflow-y: auto; background: var(--surface); border: 1px solid var(--rule); border-radius: 12px; box-shadow: 0 12px 32px rgba(0,0,0,.14); padding: 6px; }
.results a { display: block; padding: 8px 10px; border-radius: 8px; color: var(--fg); text-decoration: none; }
.results a:hover, .results a.sel { background: var(--accent-soft); }
.results .rc { font: 600 11px var(--font-mono); color: var(--accent); letter-spacing: .04em; }
.results .rh { display: block; font-weight: 700; font-size: 14px; line-height: 1.5; }
.results .rs { display: block; font-size: 13px; color: var(--muted); line-height: 1.55; }
.results mark { background: transparent; color: var(--accent); font-weight: 700; }
.results .none { padding: 10px; color: var(--muted); font-size: 14px; }

/* 枠組み */
.layout { display: grid; grid-template-columns: minmax(0, 1fr); }
.content { min-width: 0; width: 100%; max-width: calc(var(--measure) + 32px); margin: 0 auto; padding-inline: 16px; padding-block: 8px 64px; }
.side { position: fixed; z-index: 40; top: 0; bottom: 0; left: 0; width: min(20rem, 86vw); background: var(--surface); border-right: 1px solid var(--rule);
  overflow-y: auto; padding: calc(env(safe-area-inset-top, 0px) + 12px) 12px calc(env(safe-area-inset-bottom, 0px) + 24px); transform: translateX(-102%); transition: transform .2s ease; }
body.nav-open .side { transform: none; }
.scrim { position: fixed; inset: 0; z-index: 35; background: rgba(10, 16, 22, .38); opacity: 0; pointer-events: none; transition: opacity .2s ease; }
body.nav-open .scrim { opacity: 1; pointer-events: auto; }
@media (min-width: 960px) {
  .navbtn { display: none; }
  .layout { grid-template-columns: var(--side) minmax(0, 1fr); }
  .side { position: sticky; top: calc(env(safe-area-inset-top, 0px) + var(--top-h)); height: calc(100vh - var(--top-h)); width: auto; transform: none; transition: none; z-index: 1; padding: 16px 12px 32px; }
  .scrim { display: none; }
}
@media (min-width: 1280px) { .layout { grid-template-columns: var(--side) minmax(0, 1fr) 14rem; } }

/* 左の目次 */
.side nav { display: grid; gap: 2px; font-size: 14px; }
.side .home { display: block; padding: 6px 10px; border-radius: 8px; color: var(--fg); text-decoration: none; font-weight: 500; }
.grp { margin: 14px 10px 4px; font: 600 11px var(--font-mono); letter-spacing: .08em; color: var(--muted); text-transform: uppercase; }
.side ol { list-style: none; margin: 0; padding: 0; }
.side a.ch { display: flex; gap: 8px; padding: 6px 10px; border-radius: 8px; color: var(--fg); text-decoration: none; line-height: 1.5; }
.side a.ch b { font: 600 12px var(--font-mono); color: var(--muted); padding-top: 2px; font-variant-numeric: tabular-nums; }
.side a:hover { background: var(--code-bg); }
.side a.on, .side .home.on { background: var(--accent-soft); color: var(--accent); font-weight: 700; }
.side a.on b { color: var(--accent); }
.side ul { list-style: none; margin: 2px 0 6px; padding: 0 0 0 30px; }
.side ul a { display: block; padding: 3px 8px; border-radius: 6px; color: var(--muted); text-decoration: none; font-size: 13px; line-height: 1.5; }

/* 右の「このページ」 */
.otp { display: none; }
@media (min-width: 1280px) {
  .otp { display: block; position: sticky; top: calc(env(safe-area-inset-top, 0px) + var(--top-h)); align-self: start; max-height: calc(100vh - var(--top-h)); overflow-y: auto; padding: 24px 16px; font-size: 13px; }
  .otp ol { list-style: none; margin: 0; padding: 0; display: grid; gap: 2px; }
  .otp a { display: block; padding: 3px 0; color: var(--muted); text-decoration: none; line-height: 1.5; }
  .otp a:hover { color: var(--accent); }
  .otp-m { display: none; }
}
.otp-m { margin: 12px 0 4px; border: 1px solid var(--rule); border-radius: 10px; background: var(--surface); }
.otp-m summary { cursor: pointer; padding: 8px 14px; font-weight: 500; font-size: 14px; color: var(--muted); }
.otp-m ol { margin: 0; padding: 0 14px 10px 2.2em; font-size: 14px; }
.otp-m a { color: var(--fg); text-decoration: none; }

/* 本文 */
.eyebrow { margin: 22px 0 0; font: 600 12px var(--font-mono); letter-spacing: .08em; color: var(--accent); text-transform: uppercase; }
.doc h1, .hero h1 { font-family: var(--font-display); font-weight: 700; font-size: 1.75rem; line-height: 1.35; margin: 6px 0 14px; text-wrap: balance; }
.doc h2, .home h2 { font-family: var(--font-display); font-weight: 700; font-size: 1.3rem; line-height: 1.45; margin: 44px 0 10px; padding-top: 12px; border-top: 1px solid var(--rule); text-wrap: balance; scroll-margin-top: calc(var(--top-h) + 12px); }
.doc h3 { font-family: var(--font-display); font-weight: 700; font-size: 1.07rem; margin: 28px 0 6px; text-wrap: balance; }
.doc h4 { font-size: 1rem; margin: 20px 0 4px; }
.doc p { margin: 10px 0; }
.doc ul, .doc ol { margin: 8px 0; padding-left: 1.4em; }
.doc li { margin: 3px 0; }
.doc li > ul, .doc li > ol { margin: 2px 0; }
.doc hr { border: 0; margin: 8px 0; }
.doc strong { font-weight: 700; }
code { font-family: var(--font-mono); font-size: .86em; background: var(--code-bg); color: var(--code-fg); padding: 1px 5px; border-radius: 4px; overflow-wrap: anywhere; }
.path { font-family: var(--font-mono); font-size: .86em; color: var(--muted); overflow-wrap: anywhere; }
blockquote { margin: 12px 0; padding: 4px 14px; border-left: 3px solid var(--rule); color: var(--muted); }
figure.code { margin: 14px 0; background: var(--code-bg); border-radius: 10px; overflow: hidden; }
.codehead { display: flex; align-items: center; justify-content: space-between; padding: 6px 8px 0 12px; font: 600 11px var(--font-mono); letter-spacing: .06em; color: var(--muted); text-transform: uppercase; min-height: 28px; }
.copy { font: 500 12px var(--font-body); color: var(--accent); background: transparent; border: 1px solid var(--rule); border-radius: 6px; padding: 2px 10px; cursor: pointer; text-transform: none; letter-spacing: 0; }
.copy:hover { background: var(--surface); }
figure.code pre { margin: 0; padding: 8px 12px 12px; overflow-x: auto; -webkit-overflow-scrolling: touch; }
figure.code pre code { background: none; padding: 0; font-size: 12.5px; line-height: 1.6; white-space: pre; overflow-wrap: normal; color: var(--code-fg); }
.cm { color: var(--cm); }
.tablewrap { margin: 14px 0; overflow-x: auto; -webkit-overflow-scrolling: touch; border: 1px solid var(--rule); border-radius: 10px; background: var(--surface); }
table { border-collapse: collapse; width: 100%; font-size: 14px; line-height: 1.6; }
th, td { text-align: left; vertical-align: top; padding: 8px 10px; border-bottom: 1px solid var(--rule); }
th { font-weight: 700; background: var(--code-bg); white-space: nowrap; }
tr:last-child td { border-bottom: 0; }
td code { white-space: nowrap; }
.pn { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 56px; }
.pn a { display: block; min-width: 0; text-decoration: none; color: var(--fg); background: var(--surface); border: 1px solid var(--rule); border-radius: 12px; padding: 10px 14px; font-weight: 500; }
.pn a:hover { border-color: var(--accent); }
.pn small { display: block; color: var(--muted); font-size: 12px; font-weight: 400; }
.pn .next { grid-column: 2; text-align: right; }

/* 表紙 */
.hero { padding-top: 8px; }
.hero h1 { font-size: 2.1rem; margin-top: 8px; }
.lede { font-size: 1.05rem; color: var(--fg); max-width: 40rem; margin: 0 0 14px; }
.badges { display: flex; flex-wrap: wrap; gap: 8px; margin: 14px 0 6px; }
.badges span { font: 600 12px var(--font-mono); color: var(--accent); background: var(--accent-soft); border-radius: 999px; padding: 5px 11px; font-variant-numeric: tabular-nums; }
.note { color: var(--muted); font-size: 14px; margin: 6px 0 0; }
.layers { display: grid; gap: 0; margin: 14px 0 6px; }
.layer { border: 1px solid var(--rule); border-radius: 12px; background: var(--surface); padding: 12px 14px 14px; }
.layer.eng { border-color: var(--accent); }
.lname { margin: 0; font-family: var(--font-display); font-weight: 700; font-size: 1.02rem; }
.ldesc { margin: 2px 0 8px; color: var(--muted); font-size: 14px; }
.chips { display: flex; flex-wrap: wrap; gap: 6px; }
.chips a { font: 500 13px var(--font-body); text-decoration: none; color: var(--fg); background: var(--code-bg); border-radius: 8px; padding: 4px 10px; }
.chips a:hover { background: var(--accent-soft); color: var(--accent); }
.flow { margin: 0; padding: 6px 0 6px 16px; font: 12px var(--font-mono); color: var(--muted); }
.more { margin: 10px 0 0; font-size: 14px; }
.cgroup .cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(15.5rem, 1fr)); gap: 10px; }
.card { display: grid; grid-template-columns: auto 1fr; column-gap: 10px; row-gap: 2px; align-content: start; text-decoration: none; color: var(--fg); background: var(--surface); border: 1px solid var(--rule); border-radius: 12px; padding: 12px 14px; min-width: 0; }
.card:hover { border-color: var(--accent); }
.card .num { grid-row: 1 / span 3; font: 600 13px var(--font-mono); color: var(--accent); padding-top: 3px; font-variant-numeric: tabular-nums; }
.card .ttl { font-family: var(--font-display); font-weight: 700; font-size: 1.02rem; }
.card .sum { font-size: 14px; line-height: 1.6; }
.card .when { font-size: 12.5px; color: var(--muted); }
.howto ul { padding-left: 1.3em; }
.howto li { margin: 4px 0; }
@media (max-width: 480px) {
  body { font-size: 15px; }
  .doc h1 { font-size: 1.45rem; } .hero h1 { font-size: 1.7rem; }
  .brand { font-size: 15px; }
  .search input { font-size: 16px; } /* iOS が拡大しないように */
}
@media (prefers-reduced-motion: reduce) { .side, .scrim { transition: none; } }
@media (prefers-reduced-motion: no-preference) { html { scroll-behavior: smooth; } }
"""

JS = r"""(function () {
  var body = document.body;
  // 目次の引き出し（スマホ）
  var navbtn = document.getElementById("navbtn"), scrim = document.getElementById("scrim"), side = document.getElementById("side");
  function setNav(open) { body.classList.toggle("nav-open", open); if (navbtn) navbtn.setAttribute("aria-expanded", open ? "true" : "false"); }
  if (navbtn) navbtn.addEventListener("click", function () { setNav(!body.classList.contains("nav-open")); });
  if (scrim) scrim.addEventListener("click", function () { setNav(false); });
  if (side) side.addEventListener("click", function (e) { if (e.target.closest && e.target.closest("a")) setNav(false); });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") { setNav(false); closeResults(); } });

  // コードのコピー
  document.querySelectorAll("figure.code").forEach(function (fig) {
    var btn = fig.querySelector(".copy"), code = fig.querySelector("pre code");
    if (!btn || !code) return;
    btn.addEventListener("click", function () {
      var text = code.innerText;
      function done(ok) { btn.textContent = ok ? "コピーした" : "選んだ（長押しでコピー）"; setTimeout(function () { btn.textContent = "コピー"; }, 1600); }
      function fallback() {
        var r = document.createRange(); r.selectNodeContents(code);
        var s = window.getSelection(); s.removeAllRanges(); s.addRange(r); done(false);
      }
      try { navigator.clipboard.writeText(text).then(function () { done(true); }, fallback); } catch (e) { fallback(); }
    });
  });

  // 検索（節ごとの索引を文字列で引く）
  var q = document.getElementById("q"), res = document.getElementById("results");
  var index = window.AF_INDEX || [];
  var sel = -1;
  function closeResults() { if (res) { res.hidden = true; sel = -1; } }
  function esc(s) { return s.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  function snippet(t, terms) {
    var low = t.toLowerCase(), at = -1;
    for (var i = 0; i < terms.length && at < 0; i++) at = low.indexOf(terms[i]);
    var from = Math.max(0, at - 30), s = (from > 0 ? "…" : "") + t.substr(from, 110) + (t.length > from + 110 ? "…" : "");
    s = esc(s);
    terms.forEach(function (w) { if (w) s = s.replace(new RegExp(w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi"), function (m) { return "<mark>" + m + "</mark>"; }); });
    return s;
  }
  function search() {
    var v = (q.value || "").trim().toLowerCase();
    if (!v) { closeResults(); return; }
    var terms = v.split(/\s+/).filter(Boolean), hits = [];
    index.forEach(function (e) {
      var h = e.h.toLowerCase(), t = e.t.toLowerCase(), score = 0, ok = true;
      terms.forEach(function (w) { var inH = h.indexOf(w) >= 0, inT = t.indexOf(w) >= 0; if (!inH && !inT) ok = false; score += inH ? 10 : 1; });
      if (ok) hits.push({ e: e, s: score });
    });
    hits.sort(function (a, b) { return b.s - a.s; });
    hits = hits.slice(0, 20);
    if (!hits.length) { res.innerHTML = '<div class="none">見つかりませんでした。別の言い方や、関数名の一部で探してください。</div>'; res.hidden = false; sel = -1; return; }
    res.innerHTML = hits.map(function (x) {
      var e = x.e;
      return '<a href="' + e.p + (e.a && e.a !== "top" ? "#" + e.a : "") + '"><span class="rc">' + esc(e.c) + '</span><span class="rh">' + esc(e.h) + '</span><span class="rs">' + snippet(e.t, terms) + "</span></a>";
    }).join("");
    res.hidden = false; sel = -1;
  }
  if (q && res) {
    q.addEventListener("input", search);
    q.addEventListener("focus", function () { if (q.value.trim()) search(); });
    q.addEventListener("keydown", function (e) {
      var links = res.querySelectorAll("a");
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        if (!links.length) return; e.preventDefault();
        sel = (sel + (e.key === "ArrowDown" ? 1 : -1) + links.length) % links.length;
        links.forEach(function (a, i) { a.classList.toggle("sel", i === sel); });
      } else if (e.key === "Enter") {
        var a = links[sel >= 0 ? sel : 0]; if (a) { e.preventDefault(); location.href = a.getAttribute("href"); }
      }
    });
    document.addEventListener("click", function (e) { if (!e.target.closest || !e.target.closest(".search")) closeResults(); });
  }
})();
"""


def main():
    os.makedirs(OUT, exist_ok=True)
    parsed, tocs, idx_all = [], [], []
    for k, c in enumerate(CHAPTERS):
        md = open(os.path.join(HERE, c[0]), encoding="utf-8").read()
        title, toc, body, idx = convert(md)
        parsed.append((title, toc, body))
        tocs.append(toc)
        for sid, head, text in idx:
            idx_all.append({"p": PAGE[c[0]], "a": sid, "c": "%02d %s" % (k, c[1]), "h": head or c[1], "t": text})
    for k, (title, toc, body) in enumerate(parsed):
        open(os.path.join(OUT, "ch%02d.html" % k), "w", encoding="utf-8", newline="\n").write(chapter_page(k, title, toc, body, tocs))
    open(os.path.join(OUT, "index.html"), "w", encoding="utf-8", newline="\n").write(index_page(tocs))
    open(os.path.join(OUT, "site.css"), "w", encoding="utf-8", newline="\n").write(CSS)
    open(os.path.join(OUT, "site.js"), "w", encoding="utf-8", newline="\n").write(JS)
    open(os.path.join(OUT, "search-index.js"), "w", encoding="utf-8", newline="\n").write(
        "window.AF_INDEX = " + json.dumps(idx_all, ensure_ascii=False) + ";\n")
    if WEB:
        open(os.path.join(OUT, ".nojekyll"), "w").close()   # GitHub Pages に Jekyll を通させない（そのまま配る）
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("書いた:", OUT, "（%d ファイル、%d バイト、索引 %d 節）" % (len(os.listdir(OUT)), total, len(idx_all)))


if __name__ == "__main__":
    main()
