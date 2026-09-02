#!/usr/bin/env bash
# C ABI の構造体と C# の写しが一致しているかを検査する。
#
# ★なぜ要るか（2026-08-19 に実際に刺さった）
#   AF_VoiceTap に dirX/dirY/dirZ を足したとき、C++ と VoiceConvolver.cs は直したのに
#   AcousticEngineBindings.cs の AFVoiceTap を直し忘れ、**Unity 全体が数回ぶん
#   ビルドできない状態**を出荷した。ゲームシステムレーンが越境して直してくれた。
#   Roslyn での C# コンパイル検査は環境の都合で当てにならなかった（起動失敗や
#   参照の重複で、本物のエラーを取りこぼす）。ならば「実際に刺さった failure mode」
#   ＝**構造体の並びのずれ**を直接見るのがいちばん確実で速い。
#
#   ★並びが 1 つずれると例外も出ずにタップの中身が化ける。バイト数だけでなく
#     **順番**も見ること。
set -u
cd "$(dirname "$0")/.." || exit 2
H=AcousticEngine/include/acoustic_voice.h
CS=UnityDemo/Assets/Scripts/AcousticFlow/AcousticEngineBindings.cs
rc=0

# C 側: typedef struct AF_VoiceTap { ... } の中のフィールドを「型 個数」に展開する。
c_fields() {
  awk '/typedef struct AF_VoiceTap/,/} AF_VoiceTap;/' "$H" |
  grep -E '^\s+(int|float)\b' |
  sed 's://.*::' |
  while read -r line; do
    t=$(echo "$line" | awk '{print $1}')
    names=$(echo "$line" | sed "s/^\s*$t\s*//; s/;.*//")
    echo "$names" | tr ',' '\n' | while read -r nm; do
      nm=$(echo "$nm" | tr -d ' ')
      [ -z "$nm" ] && continue
      case "$nm" in
        *\[*\]) n=$(echo "$nm" | sed 's/.*\[\(.*\)\]/\1/');
                i=0; while [ "$i" -lt "$n" ]; do echo "$t"; i=$((i+1)); done ;;
        *) echo "$t" ;;
      esac
    done
  done
}

# C# 側: struct AFVoiceTap { ... } の public フィールドを同じ形に展開する。
cs_fields() {
  awk '/public struct AFVoiceTap/,/^        \}/' "$CS" |
  grep -E '^\s+public\s+(int|float)\b' |
  sed 's://.*::' |
  while read -r line; do
    t=$(echo "$line" | awk '{print $2}')
    names=$(echo "$line" | sed "s/^\s*public\s*$t\s*//; s/;.*//")
    echo "$names" | tr ',' '\n' | while read -r nm; do
      nm=$(echo "$nm" | tr -d ' ')
      [ -z "$nm" ] && continue
      echo "$t"
    done
  done
}

A=$(c_fields); B=$(cs_fields)
na=$(printf '%s\n' "$A" | grep -c .); nb=$(printf '%s\n' "$B" | grep -c .)
if [ "$A" != "$B" ]; then
  echo "[ABI FAIL] AF_VoiceTap の並びが一致しません（C 側 $na 個 / C# 側 $nb 個）"
  echo "  C   : $(printf '%s ' $A)"
  echo "  C#  : $(printf '%s ' $B)"
  rc=1
else
  echo "[ABI OK] AF_VoiceTap $na フィールド（$((na * 4)) バイト）一致"
fi

# ── AF_UpdateConfig の並びも一致しているか ──────────────────────────
#
# ★なぜ要るか（2026-08-23 に足した）
#   AF_UpdateConfig は**ホストが埋めて渡す構造体**。並びがずれると設定が丸ごと化けるのに、
#   ここには検査が無かった。エッジカタログを削除するときにフィールドを 4 つ抜くので、
#   その前に守りを作った。**守りの無い状態で並びを変えるのがいちばん危ない。**
#   ⚠ 型ではなく**名前**まで比べる。int と int の入れ替えは型だけでは捕まらない。
cfg_c_names() {
  awk '/typedef struct AF_UpdateConfig/,/\} AF_UpdateConfig;/' \
      AcousticEngine/include/acoustic_scene.h |
  sed 's:/\*.*\*/::' |
  grep -E '^\s+(int|float)\s' |
  while read -r line; do
    t=$(echo "$line" | awk '{print $1}')
    names=$(echo "$line" | sed "s/^\s*$t\s*//; s/;.*//")
    echo "$names" | tr ',' '\n' | while read -r nm; do
      nm=$(echo "$nm" | tr -d ' ')
      [ -z "$nm" ] && continue
      echo "$t $nm"
    done
  done
}
cfg_cs_names() {
  awk '/public struct AcousticUpdateConfig/,/^    \}/' "$CS" |
  sed 's://.*::' |
  grep -E '^\s+public\s+(int|float)\s' |
  while read -r line; do
    t=$(echo "$line" | awk '{print $2}')
    names=$(echo "$line" | sed "s/^\s*public\s*$t\s*//; s/;.*//")
    echo "$names" | tr ',' '\n' | while read -r nm; do
      nm=$(echo "$nm" | tr -d ' ')
      [ -z "$nm" ] && continue
      echo "$t $nm"
    done
  done
}
CA=$(cfg_c_names); CB=$(cfg_cs_names)
ca=$(printf '%s\n' "$CA" | grep -c .); cb=$(printf '%s\n' "$CB" | grep -c .)
if [ "$CA" != "$CB" ]; then
  echo "[ABI FAIL] AF_UpdateConfig の並びが一致しません（C 側 $ca 個 / C# 側 $cb 個）"
  echo "  C のみ : $(comm -23 <(printf '%s\n' "$CA" | sort) <(printf '%s\n' "$CB" | sort) | tr '\n' ' ')"
  echo "  C# のみ: $(comm -13 <(printf '%s\n' "$CA" | sort) <(printf '%s\n' "$CB" | sort) | tr '\n' ' ')"
  rc=1
else
  echo "[ABI OK] AF_UpdateConfig $ca フィールド（$((ca * 4)) バイト）一致"
fi

# ── AF_CaptureInfo / AF_CaptureMark の並び（2026-08-24 に足した）──────
#
# ★なぜ要るか
#   どちらも**DLL が埋めてホストが読む**構造体。並びがずれても例外は出ず、
#   数字が入れ替わって表示されるだけ ── **静かに壊れて気づけない**。
#   AF_CaptureMark は文字列も持つので、ずれると文字化けとして出る（これも例外なし）。
struct_c() {   # $1 = 構造体名
  awk "/typedef struct $1/,/\} $1;/" AcousticEngine/include/acoustic_scene.h |
  sed 's:/\*.*\*/::' |
  grep -E '^\s+(int|float|unsigned int|unsigned long long|char)\s' |
  sed -E 's/^\s+//; s/;.*//; s/\[[0-9]+\]//' |
  sed -E 's/unsigned long long/u64/; s/unsigned int/u32/' |
  tr -s ' '
}
struct_cs() { # $1 = C# の構造体名
  awk "/public struct $1/,/^        \}/" "$CS" |
  sed 's://.*::' |
  grep -E '^\s+(\[MarshalAs[^]]*\]\s*)?public\s+(int|float|uint|ulong|byte\[\])\s' |
  sed -E 's/^\s*\[MarshalAs[^]]*\]\s*//; s/^\s*public\s+//; s/;.*//' |
  sed -E 's/^ulong/u64/; s/^uint/u32/; s/^byte\[\]/char/'
}
for pair in "AF_CaptureInfo AF_CaptureInfo" "AF_CaptureMark AF_CaptureMark" \
            "AF_CaptureRun AF_CaptureRun"; do
  set -- $pair
  A=$(struct_c "$1"); B=$(struct_cs "$2")
  na=$(printf '%s\n' "$A" | grep -c .); nb=$(printf '%s\n' "$B" | grep -c .)
  if [ "$A" != "$B" ]; then
    echo "[ABI FAIL] $1 の並びが一致しません（C 側 $na 個 / C# 側 $nb 個）"
    diff <(printf '%s\n' "$A") <(printf '%s\n' "$B") | sed 's/^/    /'
    rc=1
  else
    echo "[ABI OK] $1 $na フィールド一致"
  fi
done

# 版番号も一致しているか。
V=$(grep -E '^#define AF_ABI_VERSION' "$H" | awk '{print $3}')
W=$(grep -E 'ExpectedAbiVersion = ' "$CS" | sed 's/.*= *\([0-9]*\).*/\1/')
if [ "$V" != "$W" ]; then
  echo "[ABI FAIL] 版番号が違います（C $V / C# $W）"
  rc=1
else
  echo "[ABI OK] 版番号 $V"
fi

# 新しいエクスポートが C# に写されているか（宣言漏れの検出）。
miss=0
for fn in $(grep -oE 'ACOUSTIC_API [a-zA-Z_0-9 ]*\b(AF_[A-Za-z0-9_]+)\(' \
              AcousticEngine/include/acoustic_scene.h "$H" |
            grep -oE 'AF_[A-Za-z0-9_]+\(' | tr -d '(' | sort -u); do
  # ★「(」の直前だけを取る。以前は行内の AF_ を全部拾っていたので、
  #   `ACOUSTIC_API AF_VoiceHandle AF_VoiceCreate(` の**返り値の型**まで
  #   「宣言が無いエクスポート」として並んでいた（AF_VoiceHandle / AF_TailBusHandle など）。
  #   警告一覧に偽物が混じると読まれなくなるので、名前だけを取る。
  if ! grep -q "$fn" "$CS"; then
    [ "$miss" -eq 0 ] && echo "[ABI 注意] C# に宣言が無いエクスポート（使っていなければ問題なし）:"
    echo "    $fn"
    miss=$((miss+1))
  fi
done
[ "$miss" -gt 0 ] && echo "  （$miss 件。ホストが呼ぶ予定なら足すこと）"

# ─────────────────────────────────────────────────────────────
# 既定値の一致検査
#
# ★なぜ要るか（2026-08-20 に実際に刺さった）
#   C++ の既定と Unity の既定が食い違っていて、**回帰テストが守る音と
#   出荷する音が別物**になっていた。同じ角の回折が 19.7dB 違い、
#   `apertureContrast` に至っては Unity 側の 4.0 が 10°開けた扉を完全な無音
#   （実測 0.0000）にしていた。看板チェックが通っていたのは、テストが
#   C++ 既定の 1.0 で走っていたから。
#
#   回帰は 87 シーンあるが、つまみを明示しているのは 4〜11 箇所しかない。
#   残りは既定に依存しているので、**既定がずれた時点で検査は無力になる**。
#   全テストで明示するのは現実的でないので、既定そのものを突き合わせる。
SC=AcousticEngine/src/Core/scene.h
DEMO=UnityDemo/Assets/Scripts/AcousticFlow/AcousticFlowSceneDemo.cs

# C++ のメンバ既定値を取る（例: apertureContrast_ = 4.0f → 4.0）
cpp_def() {
  grep -oE "^\s+(bool|float|int)\s+$1\s*=\s*[^;]+;" "$SC" | head -1 |
    sed 's/.*=\s*//; s/;.*//; s/f$//; s/\s*$//'
}
# C# の public フィールド既定値を取る（[Range(..)] などの属性は前置なので無視される）
cs_def() {
  grep -oE "public\s+(bool|float|int)\s+$1\s*=\s*[^;]+;" "$DEMO" | head -1 |
    sed 's/.*=\s*//; s/;.*//; s/f$//; s/\s*$//'
}
# 数値を正規化して比べる（1 と 1.0、true と true）
same() {
  a=$(echo "$1" | sed 's/^\([0-9]*\)\.0*$/\1/; s/\.\([0-9]*[1-9]\)0*$/.\1/')
  b=$(echo "$2" | sed 's/^\([0-9]*\)\.0*$/\1/; s/\.\([0-9]*[1-9]\)0*$/.\1/')
  [ "$a" = "$b" ]
}

# 「C++ のメンバ名:C# のフィールド名」。Unity が毎フレーム押している設定が対象。
PAIRS="apertureContrast_:apertureContrast
apertureTimbre_:apertureTimbre
apertureIsTransmission_:apertureIsTransmission
autoPortals_:autoPortals
autoPortalMinArea_:autoPortalMinArea
portalGovernRange_:portalGovernRange
useBtm_:useBtmDiffraction"

bad=0
for p in $PAIRS; do
  cm=${p%%:*}; sm=${p##*:}
  cv=$(cpp_def "$cm"); sv=$(cs_def "$sm")
  if [ -z "$cv" ] || [ -z "$sv" ]; then
    echo "[既定 注意] 読めませんでした（$cm / $sm）。名前が変わった可能性"
    continue
  fi
  if ! same "$cv" "$sv"; then
    [ "$bad" -eq 0 ] && echo "[既定 FAIL] C++ と Unity の既定が食い違っています:"
    printf "    %-28s C++ %-8s / Unity %s\n" "$cm" "$cv" "$sv"
    bad=$((bad+1))
    rc=1
  fi
done
[ "$bad" -eq 0 ] && echo "[既定 OK] Unity が押す設定 7 件の既定が C++ と一致"

# ホストが**設定していない**つまみ。C++ の既定がそのまま出荷の音になる。
# 既定を変えるときは「Unity が押していないので直接効く」ことを意識するための表示。
for k in ApertureSpread DiffractionFlat; do
  grep -q "Set$k" "$DEMO" || echo "[既定 注意] Unity は Set$k を呼んでいません（C++ 既定がそのまま出荷の音）"
done

exit $rc
