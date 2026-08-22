// WorldTransition.cs
// 扉をくぐって世界を移る。§0 の中心のループが、ここで閉じる。
//
//   閉じた扉 → 音が漏れる → 奥を想像する → 開ける → 音が広がる → **踏み越える** → 答え合わせ
//
// ★暗転しない。
//   行き先は WorldSet が先に読み込んで眠らせてあるので、くぐる動作は**写像だけ**で済む。
//   扉をくぐる瞬間は芯そのものなので、そこに読み込みの引っかかりを置きたくない。
//
// ★写像の式は WorldPortalView とまったく同じ。
//   ポータルは「こちらの扉 → 向こうの扉」でカメラを写している。
//   踏み越えた先が別の式だと、**見えていた景色と着いた場所がずれる**。
//   ずれた瞬間に答え合わせが鈍るので、ここは絶対に揃える。
//
// ★行きも帰りも同じ仕掛けで通れる。
//   抜けた先の扉は「来た道」へ向けておくので、振り返ってもう一度くぐれば戻れる。
//   行き先は扉の前で断片を鳴らせば付け替えられるので、選べることは失われない。
//
// ⚠ 音には継ぎ目が残る。
//   世界を入れ替えるとき AcousticFlowSceneDemo が OnDisable でエンジンのシーンを
//   Dispose し、OnEnable で作り直すので、**残響の尾はそこで切れる**。
//   画は繋がるが音は繋がらない。ここを繋ぐのはサウンドシステム側の仕事。
using UnityEngine;

namespace BellGame
{
    // ★跨ぎの判定は LateUpdate、しかも**カメラが確定した後**でやる。
    //
    //   1 フレームの中の順番はこう並んでいる：
    //     デモ Update         リスナーを動かす
    //     WorldBounds  (0)    壁から押し出す
    //     デモ LateUpdate (0) カメラ = リスナー ＋ eyeHeight  ← カメラが決まるのはここ
    //     ★ここ       (50)   カメラの位置で跨ぎを判定する
    //     ポータル    (100)   そのカメラで向こうを描き、開口に貼る
    //
    //   ★判定するものと、見るものを一致させる（発注者の指摘）。
    //     以前はリスナーを見ていたうえに実行順が 0 だったので、
    //     **デモより先に走ればカメラは 1 フレーム前**だった。
    //     Unity は同じ実行順どうしの前後を保証しないので、
    //     「たまに一瞬だけ世界が変わっていない」が出る。
    //     継ぎ目は**目**の話なので、目が跨いだ瞬間を見るのが正しい。
    [DefaultExecutionOrder(50)]
    [DisallowMultipleComponent]
    public sealed class WorldTransition : MonoBehaviour
    {
        [Header("配線")]
        public Transform listener;
        public WorldDoor worldDoor;
        [Tooltip("開口の面。forward(+Z) がプレイヤー側。WorldPortalView と同じ物を指す。")]
        public Transform aperture;
        [Tooltip("世界を抱えている係。空なら実行時に探す。")]
        public WorldSet worlds;

        [Header("くぐる操作")]
        // ★歩いた位置から意図を推測しない。
        //
        //   自動判定でずっと戦っていたもの（帯・解除・クールダウン・裏側で発動・
        //   たまに発動しない）は、全部「位置から意図を推測しよう」としたことから来ていた。
        //   押した時点で意図は確定しているので、推測が要らなくなる。
        //
        //   副産物として、**カメラの通り道をこちらで決められる**。
        //   開口が視界を埋めた瞬間に世界を入れ替えられるので、周辺の入れ替わりを隠せる。
        [Tooltip("扉をくぐるキー。")]
        public KeyCode interactKey = KeyCode.E;
        [Tooltip("この距離まで寄ると入れる。")]
        [Range(1f, 6f)] public float interactDistance = 3.0f;
        [Tooltip("くぐる動作の長さ(秒)。")]
        [Range(0.4f, 3f)] public float enterSeconds = 1.3f;
        [Tooltip("扉の向こうへどれだけ踏み込むか(m)。")]
        [Range(0.5f, 4f)] public float enterDepth = 2.0f;
        // ★既定は **E でくぐる**（発注者の判断で差し戻し）。
        //
        //   仕組みとしては自由に跨げる形は**完成している**。切り戻した理由は 1 つだけ ──
        //   跨ぐたびに 25ms のフレームが出て、自由に歩いていると**そこで引っかかる**。
        //  （原因は音響エンジンのシーン再構築。実測済み・連絡板でサウンド側へ依頼中。）
        //
        //   E なら通り道をこちらで決められるので、その 1 フレームを
        //   開口が画面を覆っているあいだに置ける ── 引っかかりが見えにくくなる。
        //
        // ★自由に入れる形は**今後の課題**として残す（発注者の指定）。
        //   `autoCross` を入にすれば今すぐ動く。戻すのに要るのは 25ms の解消だけで、
        //   判定も描画もそのまま使える。**捨てていない。**
        [Tooltip("歩いた位置で自動的に跨ぐ。既定は切（E でくぐる）。今後の課題として残してある。")]
        public bool autoCross;

        [Header("くぐる条件")]
        [Tooltip("開口の幅。ここから外れて面を跨いでも移動しない（壁抜け対策）。")]
        public float apertureWidth = 1.1f;
        public float apertureHeight = 2.2f;
        [Tooltip("これ以上開いていないとくぐれない。")]
        [Range(0f, 60f)] public float minAngleDeg = 10f;

        [Header("診断（読み取り専用）")]
        public string status = "—";
        public float localZ;
        [Tooltip("いまキーを押せば入れる。")]
        public bool canEnter;

        [Tooltip("1 フレームで動ける上限(m)。これを超える変化は飛び（読み込み・押し出し）として弾く。")]
        [Range(0.2f, 5f)] public float maxStepPerFrame = 1.5f;

        private float _prevRaw;
        private bool _hasPrev;

        // ★判定に使う点は**目**（カメラ）。体（リスナー）ではない。
        //
        //   継ぎ目は「画面がいつ切り替わるか」の話なので、跨いだかどうかは
        //   目の位置で決めるのが正しい。いまは eyeHeight=0 で体と目が同じ位置に
        //   居るので値は変わらないが、**別の物を見ていた**ことが問題だった ──
        //   一方が動いた瞬間にずれる形を残さない。
        private Transform Eye
        {
            get
            {
                if (_eye == null) _eye = (Camera.main != null) ? Camera.main.transform : listener;
                return (_eye != null) ? _eye : listener;
            }
        }
        private Transform _eye;

        /// 扉の面からの符号付きの奥行き（側で揃えない**生の値**）。
        ///   符号が変わった瞬間が「跨いだ瞬間」。
        private float RawZ() => aperture.InverseTransformPoint(Eye.position).z;

        // 眠りから起きた直後は跨ぎ判定をやり直す。
        //   前に生きていたときの位置が残っていると、起きた瞬間に跨いだ扱いになる。
        private void OnEnable()
        {
            _hasPrev = false;
        }

        private void Start()
        {
            if (aperture == null) aperture = transform;
            if (worlds == null) worlds = WorldSet.Current;
            if (listener == null)
            {
                var demo = FindFirstObjectByType<AcousticFlow.AcousticFlowSceneDemo>();
                if (demo != null) listener = demo.listener;
            }
        }

        private void LateUpdate()
        {
            if (listener == null || aperture == null || worldDoor == null) return;
            if (worlds == null) worlds = WorldSet.Current;
            if (worlds == null || worlds.busy) return;

            // ★開口の基準は**扉そのもの**。生成時に扉と同じ姿勢で置いた別の物ではない。
            //
            //   別の物にしていると、片方だけ動いた瞬間に食い違う。実際
            //   カメラが (0.1, 0.1) に居るのに「開口の外（横 3.96m）」と出ていた ──
            //   扉は目の前なのに、判定はよそを見ていた。
            //   扉は 1 枚しか無いのだから、開口の答えも 1 つでなければならない。
            if (worlds.RigDoor != null) aperture = worlds.RigDoor;

            var local = aperture.InverseTransformPoint(Eye.position);   // ★目で見る（体ではない）

            // ★扉のどちら側に立っていても「前が正」になるように向きを揃える。
            //   扉は一度も動かないので、くぐるとプレイヤーは裏側に出る。
            //   ここで揃えておけば、下の判定（裏側・距離・帯・進行方向）は書き換え不要。
            local.z *= worlds.PlayerSide;

            localZ = local.z;

            // ── くぐる操作（既定） ────────────────────────────
            if (!autoCross)
            {
                var t = ReadyTarget(local);
                canEnter = t != null;
                if (canEnter && Input.GetKeyDown(interactKey))
                {
                    status = $"{BellVoices.DisplayName(worldDoor.destination)}へ入る";
                    worlds.lastCrossTime = Time.time;
                    worlds.StartCoroutine(EnterRoutine(t));
                }
                return;
            }

            // ★切り替えるのは**面を跨いだその瞬間**。
            //
            //   以前は扉の 0.17m 手前で切り替えていた。理由は板（RT を貼った矩形）が
            //   near 平面に切られて消えるからで、**板の都合**でしかなかった。
            //   ステンシルで切り抜く方式にして板が無くなったので、その理由は消えている。
            //
            //   手前で切り替えていると、後ろ向きに入ったときに壊れる ──
            //   まだ跨いでいないのに世界が変わり、周辺が丸ごと入れ替わるのが見える。
            //
            //   ★帯・進行方向・再武装は全部やめた。
            //     どれも「位置から意図を推測する」ための道具で、
            //     **符号が変わった瞬間**を見るなら 1 つも要らない。
            //     ・止まっても取りこぼさない（符号は変わらないので何も起きない）
            //     ・裏へ回っても飛ばない（開口の矩形を通っていないので弾かれる）
            //     ・前向きでも後ろ向きでも同じ（跨いだかどうかしか見ていない）
            float raw = RawZ();
            if (!_hasPrev) { _prevRaw = raw; _hasPrev = true; return; }
            float prevRaw = _prevRaw;
            _prevRaw = raw;

            if (!worlds.CrossReady)
            {
                status = $"移った直後（あと {worlds.CrossWait:F1}s）";
                return;
            }

            // 符号が変わったか。飛び（読み込み・押し出し）は弾く。
            bool crossed = (raw < 0f) != (prevRaw < 0f)
                        && Mathf.Abs(raw - prevRaw) < maxStepPerFrame;
            if (!crossed) { status = "—"; return; }

            // 開口の内側を通ったか。
            //   ★余裕は柱の厚み（0.25m）ぶんまで見る。
            //     柱そのものは WorldBounds の押し出しが体を止めるので、
            //     「柱の内側に居る」＝「開口を通った」と見なしてよい。
            //     ここを厳しくすると、少し斜めに入っただけで**素通りしてしまい**、
            //     プレイヤーには「扉が効かない」としか見えない（実際そうなった）。
            float halfW = apertureWidth * 0.5f + 0.30f;
            float halfH = apertureHeight * 0.5f + 0.8f;
            if (Mathf.Abs(local.x) > halfW || Mathf.Abs(local.y) > halfH)
            {
                // ★弾いた理由を数字で残す。「たまに移動しない」を推測で追わないため。
                status = $"開口の外（横 {local.x:F2}m / 許容 ±{halfW:F2}m"
                       + $" ・縦 {local.y:F2}m / 許容 ±{halfH:F2}m）";
                return;
            }

            var dest = worldDoor.destination;
            if (dest == WorldId.None) { status = "行き先が決まっていない"; return; }
            if (worldDoor.door != null && worldDoor.door.AngleDeg < minAngleDeg)
            {
                status = "扉が開いていない";
                return;
            }

            // ★扉が 1 枚になったので、行き先の世界に扉は無い。読み込まれてさえいればよい。
            var target = worlds.Get(dest);
            if (target == null)
            {
                status = $"{BellVoices.DisplayName(dest)}がまだ読み込まれていない";
                return;
            }

            status = $"{BellVoices.DisplayName(dest)}へ抜ける";
            Cross(target);
        }


        // ★跨いだ直後の 3 フレームを見て、進むはずが**戻されていない**かを数字で出す。
        //
        //   目で見た「ぶれ」は、フレームが重いのか位置が戻されているのか区別が付かない。
        //   進行方向と逆へ動いたフレームがあれば、それは押し戻し（WorldBounds か当たり）。
        private System.Collections.IEnumerator WatchRollback(Vector3 posBefore)
        {
            // ★測るのは**扉の面からの符号付き奥行き（生の値）**そのもの。
            //
            //   前は進行方向を PlayerSide から出していたが、PlayerSide は
            //   跨いだ瞬間に反転するので、**同じ前進が跨いだ後だけ符号が逆に出た**
            //  （「★戻されている」は私の計算ミスで、ゲームは正常だった）。
            //   生の奥行きなら反転しない ── 単調に減り続けていれば前進、
            //   増えた瞬間があれば本当に戻されている。
            //
            // ★測る場所も直した。`yield return null` は Update の直後に戻るので、
            //   カメラが決まる LateUpdate より**前**だった（1 フレーム目が 0.0cm）。
            //   WaitForEndOfFrame なら、そのフレームの位置が確定している。
            // ★★奥行きだけでは足りない。
            //
            //   発注者の「若干中央に戻される感じ」は、**横（x）**か**視線**の話かもしれない。
            //   前回は奥行き（z）しか測っていなかったので、
            //   横に中央へ引かれていても数字には一切出なかった。
            //   測っていない軸は「無い」ことにはならない ── 全部出す。
            var p0 = aperture.InverseTransformPoint(posBefore);
            float prevX = p0.x, prevY = p0.y, prevZ = p0.z;

            var cam = Camera.main;
            float prevYaw = (cam != null) ? cam.transform.eulerAngles.y : 0f;
            float prevPitch = (cam != null) ? Fold(cam.transform.eulerAngles.x) : 0f;

            for (int f = 0; f < 4; f++)
            {
                yield return new WaitForEndOfFrame();

                var lp = aperture.InverseTransformPoint(Eye.position);
                float dt = Mathf.Max(1e-5f, Time.deltaTime);
                float yaw = (cam != null) ? cam.transform.eulerAngles.y : 0f;
                float pitch = (cam != null) ? Fold(cam.transform.eulerAngles.x) : 0f;

                // 中央（扉の面の中心）へ寄ったかどうか。負なら中央へ引かれている。
                float toCenter = Mathf.Abs(lp.x) - Mathf.Abs(prevX);

                Debug.Log($"[BellGame] 跨いだ後 {f + 1} フレーム目: {dt * 1000f:F1}ms"
                    + (dt > 1f / 55f ? "（★重い）" : "") + "\n"
                    + $"    扉ローカル = ({lp.x:F3}, {lp.y:F3}, {lp.z:F3})\n"
                    + $"    横 {(lp.x - prevX) * 100f:+0.0;-0.0}cm"
                    + (toCenter < -0.002f ? "  ★中央へ寄せられている" : "")
                    + $" / 高さ {(lp.y - prevY) * 100f:+0.0;-0.0}cm"
                    + $" / 奥行き {(lp.z - prevZ) * 100f:+0.0;-0.0}cm\n"
                    + $"    視線 ヨー {Mathf.DeltaAngle(prevYaw, yaw):+0.0;-0.0}° "
                    + $"/ 上下 {pitch - prevPitch:+0.0;-0.0}°"
                    + (Mathf.Abs(pitch) < Mathf.Abs(prevPitch) - 0.5f ? "  ★視線が水平へ戻されている" : ""));

                prevX = lp.x; prevY = lp.y; prevZ = lp.z;
                prevYaw = yaw; prevPitch = pitch;
            }
        }

        // 0..360 を -180..180 へ畳む。上下角の比較に要る。
        private static float Fold(float deg) => Mathf.Repeat(deg + 180f, 360f) - 180f;

        /// いま入れるか。入れないなら理由を status に残して null を返す。
        private WorldSet.World ReadyTarget(Vector3 local)
        {
            if (local.z < 0f) { status = "扉の裏側"; return null; }
            if (local.z > interactDistance) { status = $"扉から遠い（{local.z:F1}m）"; return null; }
            if (Mathf.Abs(local.x) > apertureWidth * 0.5f + 1.0f)
            { status = $"扉の正面から外れている（横 {local.x:F2}m）"; return null; }

            var dest = worldDoor.destination;
            if (dest == WorldId.None) { status = "行き先が決まっていない"; return null; }
            if (worldDoor.door != null && worldDoor.door.AngleDeg < minAngleDeg)
            { status = "扉が開いていない"; return null; }
            if (!worlds.CrossReady) { status = $"移った直後（あと {worlds.CrossWait:F1}s）"; return null; }

            var target = worlds.Get(dest);
            // ★扉が 1 枚になったので、行き先の世界に扉は無い。読み込まれてさえいればよい。
            if (target == null)
            { status = $"{BellVoices.DisplayName(dest)}がまだ読み込まれていない"; return null; }

            status = $"[{interactKey}] で{BellVoices.DisplayName(dest)}へ入る";
            return target;
        }

        // ★くぐる動作。通り道をこちらで決めるので、
        //   **開口が視界を埋めた瞬間**に世界を入れ替えられる。
        //   自分で歩いているあいだは、その瞬間を狙えなかった。
        private System.Collections.IEnumerator EnterRoutine(WorldSet.World target)
        {
            var rig = worlds.Rig;
            var cam = Camera.main;
            if (rig == null) yield break;

            // 動かないようにする。動かせるとカメラの通り道が保証できない。
            var demo = FindFirstObjectByType<AcousticFlow.AcousticFlowSceneDemo>();
            bool hadMovement = demo != null && demo.enableMovement;
            if (demo != null) demo.enableMovement = false;


            // 敷居を開ける。開けないと押し戻しがくぐる動作を妨げる。
            var sill = GetComponentInChildren<DoorThreshold>(true);
            if (sill == null && worldDoor != null) sill = worldDoor.GetComponentInChildren<DoorThreshold>(true);
            if (sill != null) sill.SetPassable(true);

            var cameFrom = worlds.active;
            GameProgress.CameFrom = cameFrom;

            Vector3 from = rig.position;
            Quaternion fromRot = rig.rotation;

            // ★開口の**中心を通す**。端をかすめると周辺が見えて入れ替わりが露見する。
            //   終点は扉の向こう enterDepth。
            //
            //   ★進む向きは立っている側で決まる。扉は動かないので、
            //     一度くぐった後は**逆向きに**くぐることになる。
            int side = worlds.PlayerSide;
            Vector3 through = -side * aperture.forward;

            Vector3 mid = aperture.position; mid.y = from.y;
            Vector3 to = mid + through * enterDepth;
            Quaternion toRot = Quaternion.LookRotation(through, Vector3.up);

            bool swapped = false;
            float t = 0f;
            while (t < 1f)
            {
                t += Time.deltaTime / Mathf.Max(0.05f, enterSeconds);
                float e = Mathf.SmoothStep(0f, 1f, Mathf.Clamp01(t));

                var p = Vector3.Lerp(from, to, e);
                var r = Quaternion.Slerp(fromRot, toRot, Mathf.Clamp01(e * 1.6f));
                rig.SetPositionAndRotation(p, r);
                if (cam != null) cam.transform.SetPositionAndRotation(p, r);

                // ★入れ替えるのは開口の面を越える瞬間。
                //   そこでは開口が視界のほとんどを占めているので、周辺が変わっても見えない。
                if (!swapped)
                {
                    // ★面を越えた瞬間。near を縮めてあるので板はまだ画面を覆っている。
                    float z = aperture.InverseTransformPoint(p).z * side;
                    if (z <= 0f)
                    {
                        swapped = true;

                        // ★運ばない。行き先は既に扉の裏側に在るので、歩き続けるだけでよい。
                        //   世界を起こす**前に**側を入れ替える（Activate が置き方を側で決める）。
                        worlds.FlipPlayerSide();
                        worlds.Activate(target.id);
                        AfterCross(target, cameFrom);
                    }
                }
                yield return null;
            }

            // 面を越えないまま終わった場合の受け皿（フレーム落ち・極端に短い enterSeconds）。
            if (!swapped)
            {
                worlds.FlipPlayerSide();
                worlds.Activate(target.id);
                AfterCross(target, cameFrom);
            }

            rig.SetPositionAndRotation(to, toRot);
            if (cam != null) cam.transform.SetPositionAndRotation(to, toRot);

            if (demo != null) demo.enableMovement = hadMovement;


            if (sill != null) sill.SetPassable(false);
            worlds.lastCrossTime = Time.time;

            Debug.Log($"[BellGame] {BellVoices.DisplayName(cameFrom)} → "
                      + $"{BellVoices.DisplayName(target.id)} へ入った（くぐる動作 {enterSeconds}s）");
        }

        private void Cross(WorldSet.World target)
        {
            // ★扉が 1 枚で、行き先が既にその裏側へ置いてあるなら、**何も動かさない。**
            //
            //   向こうの空間はもう足元から続いている。入れ替わるのは
            //   「どちらを描くか」と「どちらが音響的に生きているか」だけ。
            //   位置も姿勢も変えないので、継ぎ目は原理的に最小になる ──
            //   そして**どこからどう跨いでも成立する**（渡り方が自由になる）。
            //
            //   この下の「扉 → 扉 の写像」は使えない。写像先の target.door は
            //   もう存在しない（扉は装置側に 1 枚だけ）。
            if (worlds.RigDoor != null)
            {
                var from = worlds.active;
                GameProgress.CameFrom = from;

                // ★門を閉じるのは入れ替える**前**。Activate は自分を無効にするので、
                //   後に書くと確実に実行される保証が無い。
                worlds.lastCrossTime = Time.time;

                // ★跨いだフレームの長さを計る。
                //
                //   「通り過ぎる瞬間に若干ぶれる」の候補は 2 つあり、推測で選ばない。
                //     A) このフレームが**重い** ── Activate が音響エンジンのシーンを
                //        作り直す（コライダー収集＋部屋グラフのボクセル化）。
                //        重ければそのフレームだけ長くなり、移動が飛んで見える
                //     B) 位置が**戻されている** ── 世界が入れ替わった直後、
                //        新しい世界のコライダーや範囲に押し出されている
                //
                //   両方そのまま出す。数字を見てから直す。
                var sw = System.Diagnostics.Stopwatch.StartNew();
                var posBefore = Eye.position;

                // ★世界を起こす前に側を入れ替える。Activate は側を見て置き方を決める。
                //   順番が逆だと、跨ぎきったところで世界が 180 度飛ぶ。
                worlds.FlipPlayerSide();
                worlds.Activate(target.id);
                AfterCross(target, from);

                sw.Stop();
                Debug.Log($"[BellGame] {BellVoices.DisplayName(from)} → "
                          + $"{BellVoices.DisplayName(target.id)}（つながっているので歩いただけ）\n"
                          + $"  ★入れ替えに掛かった時間 = {sw.Elapsed.TotalMilliseconds:F1} ms"
                          + $"（16.7ms を超えていれば、そのフレームだけ止まる＝ぶれの正体）\n"
                          + $"  ★入れ替えでの位置の動き = {(Eye.position - posBefore).magnitude * 100f:F1} cm"
                          + "（0 でなければ押し戻されている）");

                // 次のフレームで、押し出しに戻されていないかも見る。
                worlds.StartCoroutine(WatchRollback(posBefore));
                return;
            }

            var here = FindFirstObjectByType<BellCallResponse>();
            var cameFrom = (here != null) ? here.world : worlds.active;
            GameProgress.CameFrom = cameFrom;

            // 扉 → 扉 の写像。180 度回すのは「くぐる」ものだから
            //（手前で −Z へ進むことが、向こうで +Z へ進むことに対応する）。
            //
            // ★写像に使うのは**カメラの姿勢**。ポータル描画とまったく同じ式にする。
            //   以前はここだけリスナー（ヨーだけ）で写していたので、
            //   ポータルごしに見えていた絵と、抜けた先の絵が別の向きになっていた。
            //   §0 の答え合わせは、ここがずれた瞬間に鈍る。
            var cam = Camera.main;
            var eye = (cam != null) ? cam.transform : listener;

            Matrix4x4 flip = Matrix4x4.Rotate(Quaternion.Euler(0f, 180f, 0f));
            Matrix4x4 m = target.door.localToWorldMatrix * flip
                        * aperture.worldToLocalMatrix
                        * eye.localToWorldMatrix;
            Vector3 pos = m.GetColumn(3);
            Quaternion rot = m.rotation;

            // ★門を閉じるのは**入れ替える前**。Activate は自分を無効にするので、
            //   後に書くと確実に実行される保証が無い。
            worlds.lastCrossTime = Time.time;

            // ★世界がつながっているなら、**プレイヤーは動かさない。**
            //   行き先の扉をこちらの扉に重ねてあるので、向こうの空間はもう足元から続いている。
            //   入れ替わるのは「どちらを描くか」と「どちらが音響的に生きているか」だけ。
            //   位置も姿勢も一切変わらないので、継ぎ目は原理的に最小になる。
            if (worlds.stitchWorlds)
            {
                worlds.Activate(target.id);
                AfterCross(target, cameFrom);
                Debug.Log($"[BellGame] {BellVoices.DisplayName(cameFrom)} → "
                          + $"{BellVoices.DisplayName(target.id)}（つながっているので歩いただけ）");
                return;
            }

            // ★生きている世界を入れ替える。古い方を先に落とす（2 面立てないため）。
            //   装置（リスナー・カメラ・耳・音響デモ）は世界に属さず、そのまま持ち歩く。
            worlds.Activate(target.id);

            // 装置を写像した位置へ運ぶ。**カメラの姿勢はそのまま**なので、
            //   視線の上下も繋がる（世界ごとにデモを持っていた頃は水平に飛んでいた）。
            var rig = worlds.Rig;
            if (rig != null)
            {
                var p = pos;
                p.y = rig.position.y;               // 高さは変えない（重力が無い）
                rig.SetPositionAndRotation(p, Quaternion.Euler(0f, rot.eulerAngles.y, 0f));
                if (cam != null) cam.transform.SetPositionAndRotation(p, rot);
            }

            AfterCross(target, cameFrom);

            Debug.Log($"[BellGame] {BellVoices.DisplayName(cameFrom)} → "
                      + $"{BellVoices.DisplayName(target.id)} へ抜けた（写像した）");
        }

        // 抜けた後の後始末。つないでいても写像しても、ここは同じ。
        private void AfterCross(WorldSet.World target, WorldId cameFrom)
        {
            // 眠っている間に断片が増えていることがあるので、向こうへ映し直す。
            var bells = target.root.GetComponentInChildren<PlayerBells>(true);
            if (bells != null) bells.Sync();
            var bell = target.root.GetComponentInChildren<BellCallResponse>(true);
            if (bell != null) bell.RebuildPlayerClip();

            // ★扉は 1 枚しかない。**開き具合も行き先も向こうへ渡す。**
            //   開けて抜けたのに向こうで閉まっている、は設定違反
            //  （「扉は世界に属さない。同じ 1 枚を両側から見ている」）。
            // ★扉は 1 枚しかないので、同期する相手が居ない。
            //   角度はそのまま（同じ板）。行き先だけ「来た道」へ向け直す。
            //   世界ごとに扉を持っていた頃は、ここで角度と行き先を 2 枚で同期していた。
            //
            // ★行き先の付け替えは**すぐにやらない**。扉から離れるまで待つ。
            //   跨いだ瞬間に「来た道」へ向け直すと、板がその場で元の世界を映し始める。
            //   このとき開口はまだ画面の大半を占めているので、全画面が一瞬入れ替わる。
            //   離れてから付け替えれば、扉は画面の小さな一部でしかない。
            // ★その場で向け直す。遅らせない。
            //
            //   遅らせていたのは、板の都合で扉の手前で切り替えていたから ──
            //   その瞬間は開口が画面を覆っていて、中身が変わると全画面が入れ替わって見えた。
            //
            //   面ちょうどで切り替えるようになったいま、その瞬間の開口は
            //   **カメラの位置に在る**（＝ near 平面に切られて何も描かれない）。
            //   だから今すぐ向け直しても見えない。むしろ遅らせると、
            //   離れきるまで開口が「いま居る世界」を映すことになり、そちらが嘘になる。
            if (cameFrom != WorldId.None) worldDoor.destination = cameFrom;

            // 使っていない世界を眠らせる。
            //   ★自分ではなく WorldSet に走らせる ── この時点で自分は
            //     Activate に無効化されているので、コルーチンを開始できない。
            worlds.StartCoroutine(worlds.Trim(target.id, cameFrom));
        }
    }
}
