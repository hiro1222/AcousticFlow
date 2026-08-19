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
            grep -oE 'AF_[A-Za-z0-9_]+' | sort -u); do
  if ! grep -q "$fn" "$CS"; then
    [ "$miss" -eq 0 ] && echo "[ABI 注意] C# に宣言が無いエクスポート（使っていなければ問題なし）:"
    echo "    $fn"
    miss=$((miss+1))
  fi
done
[ "$miss" -gt 0 ] && echo "  （$miss 件。ホストが呼ぶ予定なら足すこと）"

exit $rc
