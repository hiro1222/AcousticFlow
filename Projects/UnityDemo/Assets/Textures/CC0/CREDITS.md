# CC0 素材の出どころ

ここに入っているものは**すべて CC0（パブリックドメイン）**。商用利用可・改変可・
クレジット表記は不要。ただし出どころを記録しておくのは実務上の作法なので残す。

配布物に含めても問題ないが、**配布前にこのファイルごと同梱するか、
README にまとめ直すこと**（「どこから持ってきたか説明できる」状態にしておく）。

> ⚠ **2026-08-19 追記（造形レーン）**: このファイルを一度壊してしまい、以下は
> 冒頭 20 行の控えとフォルダの中身から**組み直したもの**です。文面が元と違う
> 可能性があります（表の中身とリンクはフォルダを見て確認済み）。
> 元の版を持っているセッションがあれば上書きしてください。

## テクスチャ — [ambientCG](https://ambientcg.com/) (CC0 1.0)

2K / JPG。`Color` / `NormalGL` / `Roughness` / `AmbientOcclusion` / `Displacement` の 5 枚組。

| フォルダ | 用途 | 出どころ |
|---|---|---|
| `Travertine009` | 神殿の壁 | https://ambientcg.com/view?id=Travertine009 |
| `Marble016` | 神殿の柱・床 | https://ambientcg.com/view?id=Marble016 |
| `Rock063` | 洞窟 | https://ambientcg.com/view?id=Rock063 |
| `Snow014` | 雪山 | https://ambientcg.com/view?id=Snow014 |
| `Grass005` | 草原 | https://ambientcg.com/view?id=Grass005 |

### 透過付きのアトラス（1K / JPG）

草の葉を 1 枚ずつスキャンして並べたもの。**`Opacity` が別ファイル**なので、
Unity の Standard(Cutout) に渡すには `Color` のアルファへ合成する必要がある
（`BellGameDressing` が Roughness→Smoothness でやっているのと同じ手口）。

| フォルダ | 用途 | 出どころ |
|---|---|---|
| `Foliage001` | 草原の草。長い葉 10 枚。穂・背の高い株に | https://ambientcg.com/view?id=Foliage001 |
| `Foliage006` | 草原の草。短い葉と反った葉 10 枚。下草に | https://ambientcg.com/view?id=Foliage006 |

不要なファイル（`.blend` / `.mtlx` / `.tres` / `.usdc` / `NormalDX` / プレビュー PNG）は
落としてある。残しているのは `Color` / `NormalGL` / `Roughness` / `Displacement` / `Opacity`。

## HDRI — [Poly Haven](https://polyhaven.com/) (CC0 1.0)

2K / HDR。スカイボックスに使う（`BellGameDressing.SetSkybox`）。

| ファイル | 用途 | 出どころ |
|---|---|---|
| `HDRI/kloofendal_48d_partly_cloudy_puresky_2k.hdr` | 晴天（`Sky_Day`） | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky |
| `HDRI/moonless_golf_2k.hdr` | 夜（`Sky_Night`） | https://polyhaven.com/a/moonless_golf |
