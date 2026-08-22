// BellGameConsole.cs
// 検証中の操作を**1 枚にまとめた卓**。別ウィンドウなので、別モニターへ出せます。
//
// ★なぜ作ったか
//   鍵が増えすぎました。Tab / 5 / 6 / E / F1〜F12 / [ ] / , . / - = / J N O K \ …
//   どれが何だったかを覚えるのが仕事になっていて、**確かめたいことより操作のほうが重い**。
//
// ★とくに扉。
//   「6 を押し続けて離す」では、§4.3 の帯域が立ち上がる角度を探せません。
//   スライダーなら 11°→ 20°→ 35° と**狙って置ける**。開口の窓と並べて見るための道具です。
//
// ★鍵は残してあります。手が覚えているぶんは使えたほうがいいので、
//   こちらは「増やした入口」であって、置き換えではありません。
using UnityEditor;
using UnityEngine;
using BellGame;
using AcousticFlow;

namespace BellGameEditor
{
    public sealed class BellGameConsole : EditorWindow
    {
        [MenuItem("BellGame/操作卓（別ウィンドウ）")]
        public static void Open()
        {
            var w = GetWindow<BellGameConsole>("BellGame 操作卓");
            w.minSize = new Vector2(360f, 420f);
        }

        private Vector2 _scroll;
        private HalfWorldLab _lab;
        private LoadTestRig _rig;
        private LoadTestSilence _mute;
        private AcousticFlowSceneDemo _demo;
        private AudioClip _sharedClip;
        private PhysicsDoor _door;

        private void Update()
        {
            if (!EditorApplication.isPlaying) { _sweeping = false; return; }
            if (_sweeping) StepSweep();
            Repaint();
        }

        private void Find()
        {
            if (_lab == null) _lab = Object.FindFirstObjectByType<HalfWorldLab>();
            if (_rig == null) _rig = Object.FindFirstObjectByType<LoadTestRig>();
            if (_mute == null) _mute = Object.FindFirstObjectByType<LoadTestSilence>();
            if (_demo == null) _demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (_door == null) _door = Object.FindFirstObjectByType<PhysicsDoor>();
        }

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に操作できます。", MessageType.Info);
                return;
            }
            Find();
            _scroll = EditorGUILayout.BeginScrollView(_scroll);

            DrawWorld();
            DrawDoor();
            DrawMix();
            DrawPaths();
            DrawLoad();

            EditorGUILayout.EndScrollView();
        }

        // ── 世界 ──────────────────────────────────────────────
        private void DrawWorld()
        {
            EditorGUILayout.LabelField("世界", EditorStyles.boldLabel);
            if (_lab == null) { DrawWorldSet(); return; }

            EditorGUILayout.LabelField($"いま = {_lab.live}　居る側 = "
                + (_lab.playerSide > 0 ? "手前" : "奥（くぐった後）")
                + $"　組み直し {_lab.lastSwapMs:F1}ms　転移 {_lab.lastCrossMs:F1}ms");

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("なし")) _lab.Apply(HalfWorldLab.Half.None, false);
                if (GUILayout.Button("A世界（赤）")) _lab.Apply(HalfWorldLab.Half.A, false);
                if (GUILayout.Button("B世界（緑）")) _lab.Apply(HalfWorldLab.Half.B, false);
            }

            using (new EditorGUILayout.HorizontalScope())
            {
                bool can = _lab.CanCross(out string why);
                using (new EditorGUI.DisabledScope(!can))
                    if (GUILayout.Button("くぐる（E）")) _lab.Cross();
                if (GUILayout.Button("定常音を替える（滝→川→風）")) _lab.CycleLoopKind();
                if (!can) EditorGUILayout.LabelField(why, EditorStyles.miniLabel);
            }

            // ★渡り方。**既定は E（いまの仕組み）。**切り替えても元へ戻せます。
            _lab.freeCross = EditorGUILayout.ToggleLeft(
                "自由に渡る（歩いて跨ぐだけで転移。OFF＝E でくぐる／既定）", _lab.freeCross);
            EditorGUILayout.LabelField("　★置き換えではなく増やしただけ。OFF に戻せば元の挙動に完全に戻ります。",
                                       EditorStyles.miniLabel);

            if (GUILayout.Button("いまの状態をコンソールへ")) _lab.Dump("操作卓");

            // ── 音源ごとの音 ──────────────────────────────────
            //
            // ★順序を聴くときは**両側を同じ音**にしてください。
            //   鳥（単発）と滝（定常）では「どちらが先に抜けてきたか」を比べられません
            //   ── 比べるものが違うので、耳は「別の音がした」としか言えない。
            EditorGUILayout.Space(2f);
            EditorGUILayout.LabelField("音源ごとの音", EditorStyles.miniBoldLabel);
            foreach (var s in _lab.LiveSpecs())
            {
                if (s == null || s.t == null) continue;
                using (new EditorGUILayout.HorizontalScope())
                {
                    EditorGUILayout.LabelField(s.t.name, GUILayout.Width(110f));

                    // 音声ファイルを直接鳴らす。合成音より聴き分けやすい。
                    var cur = s.t.GetComponent<AudioSource>();
                    var clip = (AudioClip)EditorGUILayout.ObjectField(
                        s.ambient ? null : (cur != null ? cur.clip : null),
                        typeof(AudioClip), false, GUILayout.Width(120f));
                    if (clip != null && (s.ambient || cur == null || cur.clip != clip))
                        _lab.SetClip(s, clip);

                    if (!s.ambient) { EditorGUILayout.LabelField("WAV", GUILayout.Width(34f)); }
                    else
                    {
                        var k = (AmbientKind)EditorGUILayout.EnumPopup(s.kind, GUILayout.Width(70f));
                        if (k != s.kind) _lab.SetKind(s, k);
                    }
                    float v = EditorGUILayout.Slider(s.volume, 0f, 1f);
                    if (!Mathf.Approximately(v, s.volume))
                    {
                        s.volume = v;
                        var a = s.t.GetComponent<AudioSource>();
                        if (a != null && s.IsLoop) a.volume = v;
                    }
                }
            }
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("全部そろえる", GUILayout.Width(110f));
                if (GUILayout.Button("滝")) _lab.SetAllKinds(AmbientKind.Falls);
                if (GUILayout.Button("川")) _lab.SetAllKinds(AmbientKind.Stream);
                if (GUILayout.Button("風")) _lab.SetAllKinds(AmbientKind.Wind);
            }
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("全部この WAV に", GUILayout.Width(110f));
                _sharedClip = (AudioClip)EditorGUILayout.ObjectField(
                    _sharedClip, typeof(AudioClip), false);
                using (new EditorGUI.DisabledScope(_sharedClip == null))
                    if (GUILayout.Button("鳴らす", GUILayout.Width(60f)))
                        _lab.SetAllClips(_sharedClip);
            }
            // ── 音量 0 が重い件の切り分け ──────────────────────
            //
            // ★「音量を 0 にすると一気に重くなる」という報告の裏を取るための 3 択。
            //   0 と 0.0001 は**耳には同じ**（-80dB）ですが、
            //   もし 0 だけ重いなら、原因は「音が小さいこと」ではなく
            //   **ちょうど 0 であること**です（ゼロ割り・仮想化・組み直しの誘発など）。
            //   その場合 0.0001 がそのまま回避策になります。
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("音量そろえ", GUILayout.Width(110f));
                if (GUILayout.Button("0")) SetAllVolume(0f);
                if (GUILayout.Button("0.0001")) SetAllVolume(0.0001f);
                if (GUILayout.Button("0.5")) SetAllVolume(0.5f);
                EditorGUILayout.LabelField($"音響 {PerfMeter.AcousticMs:F2}ms", GUILayout.Width(110f));
            }
            EditorGUILayout.LabelField("　★0 と 0.0001 は耳には同じ（-80dB）。差が出たら『0 であること』が原因。",
                                       EditorStyles.miniLabel);

            EditorGUILayout.LabelField("　★順序を聴くなら全部そろえてください（比べる物が違うと分かりません）。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.LabelField("　★WAV は頭出しを揃えて同時に鳴らします（ずれると『先に鳴った』と混ざる）。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.LabelField("　⚠ 曲は帯域が偏り途中で切れます。迷ったら滝（全帯域・定常）で確かめて。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.Space(6f);
        }

        /// 生きている世界の音源の音量をそろえる。0 と 0.0001 の比較用。
        private void SetAllVolume(float v)
        {
            if (_lab == null) return;
            foreach (var s in _lab.LiveSpecs())
            {
                if (s == null || s.t == null) continue;
                s.volume = v;
                var a = s.t.GetComponent<AudioSource>();
                if (a != null) a.volume = v;
            }
            Debug.Log($"[BellGame] 音源の音量を {v} にそろえた。直前の音響 {PerfMeter.AcousticMs:F2}ms。\n"
                      + "  ★数フレーム待ってからモニターの『うち音響』を読んでください。", _lab);
        }

        // ── 本番シーン（WorldSet）の世界切り替え ──────────────
        //
        // ★ラボの `HalfWorldLab` ではなく `WorldSet` を相手にします。
        //   本番はベルを集めて行き先が増える進行ですが、**検証ではそこを通しません** ──
        //   通すと「進行の不具合」と「世界の入れ替えの不具合」が混ざります。
        //   ここから直に繋いで、入れ替えだけを見ます。
        private static readonly WorldId[] AllWorlds =
        {
            WorldId.White, WorldId.Grassland, WorldId.Temple,
            WorldId.Cave, WorldId.Snow, WorldId.Sea,
        };

        private WorldSet _set;

        private void DrawWorldSet()
        {
            if (_set == null) _set = Object.FindFirstObjectByType<WorldSet>();
            if (_set == null)
            {
                EditorGUILayout.HelpBox("HalfWorldLab も WorldSet も居ません。", MessageType.None);
                EditorGUILayout.Space(4f);
                return;
            }

            var rigDoor = _set.RigDoor != null ? _set.RigDoor.GetComponentInChildren<WorldDoor>(true) : null;
            EditorGUILayout.LabelField(
                $"いま = {BellVoices.DisplayName(_set.active)}　"
                + $"行き先 = {(rigDoor != null ? BellVoices.DisplayName(rigDoor.destination) : "—")}"
                + (_set.busy ? "　（切り替え中）" : ""));

            EditorGUILayout.LabelField("扉の行き先を選ぶ", EditorStyles.miniBoldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                foreach (var id in AllWorlds)
                {
                    if (!_set.IsLoaded(id)) continue;
                    using (new EditorGUI.DisabledScope(id == _set.active))
                        if (GUILayout.Button(BellVoices.DisplayName(id)) && rigDoor != null)
                        {
                            rigDoor.destination = id;
                            Debug.Log($"[BellGame] 扉の行き先を {BellVoices.DisplayName(id)} にした"
                                      + "（操作卓。ベルの進行は通していません）", rigDoor);
                        }
                }

                // ★どこにも繋がない。
                //   世界が 3 つ以上あると「行き先＝1 つだけ」が効いてくるので、
                //   繋いでいない状態も選べないと確かめられません
                //  （扉の向こうが空になるのが正しい姿）。
                if (GUILayout.Button("繋がない") && rigDoor != null)
                {
                    rigDoor.destination = WorldId.None;
                    Debug.Log("[BellGame] 扉の行き先を「なし」にした（操作卓）", rigDoor);
                }
            }

            // ★くぐらずに直接その世界へ飛ぶ。入れ替えだけを何度も試すため。
            EditorGUILayout.LabelField("くぐらずに直接その世界へ", EditorStyles.miniBoldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                foreach (var id in AllWorlds)
                {
                    if (!_set.IsLoaded(id)) continue;
                    using (new EditorGUI.DisabledScope(id == _set.active))
                        if (GUILayout.Button(BellVoices.DisplayName(id)))
                        {
                            var sw = System.Diagnostics.Stopwatch.StartNew();
                            _set.Activate(id);
                            sw.Stop();
                            Debug.Log($"[BellGame] {BellVoices.DisplayName(id)} へ直接切り替えた"
                                      + $"（同期部分 {sw.Elapsed.TotalMilliseconds:F1}ms）", _set);
                        }
                }
            }
            EditorGUILayout.LabelField("　★扉をくぐらずに入れ替えます。置き直しだけを何度も試せます。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.Space(6f);
        }

        // ── 扉 ────────────────────────────────────────────────
        //
        // ★ここが一番効きます。角度を**狙って置ける**ようになるので、
        //   開口（§4.3）の窓と並べて「何度で低域が立ち上がるか」を探せます。
        private void DrawDoor()
        {
            EditorGUILayout.LabelField("扉", EditorStyles.boldLabel);
            if (_door == null) { EditorGUILayout.LabelField("　扉が見つかりません"); return; }

            float a = EditorGUILayout.Slider("開き（度）", _door.AngleDeg, 0f, _door.maxAngle);
            if (!Mathf.Approximately(a, _door.AngleDeg)) _door.ForceAngle(a);

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("閉")) _door.ForceAngle(0f);
                if (GUILayout.Button("5°")) _door.ForceAngle(5f);
                if (GUILayout.Button("15°")) _door.ForceAngle(15f);
                if (GUILayout.Button("30°")) _door.ForceAngle(30f);
                if (GUILayout.Button("60°")) _door.ForceAngle(60f);
                if (GUILayout.Button("全開")) _door.ForceAngle(_door.maxAngle);
            }
            EditorGUILayout.LabelField("　★狭い角度ほど §4.3 の差が出ます。5°→15° を往復してみてください。",
                                       EditorStyles.miniLabel);

            using (new EditorGUI.DisabledScope(_sweeping))
                if (GUILayout.Button(_sweeping ? $"掃いています… {_step + 1}/{Angles.Length}"
                                               : "★角度を掃いて表にする（§4.3 の確認）"))
                    StartSweep();
            EditorGUILayout.LabelField("　目で 6 個の数字を追って比べるのは無理なので、機械に採らせます。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.Space(6f);
        }

        // ── 角度掃き ──────────────────────────────────────────
        //
        // ★これが**作品の芯の答え合わせ**です。
        //   隙間が広がるにつれ、**低域から順に**高域まで通るか。
        //   同時に全部上がる／段で跳ぶなら、§4.3 が効いていません。
        //
        // ★狭い側を細かく刻んであります。
        //   フレネルは隙間が波長に近いところで効くので、差が出るのは 0〜20° です。
        //   等間隔で刻むと、いちばん見たい所を数点で飛ばしてしまいます。
        private static readonly float[] Angles =
            { 0f, 1f, 2f, 3f, 5f, 8f, 12f, 18f, 25f, 35f, 50f, 70f, 95f, 140f };

        private bool _sweeping;
        private int _step;
        private double _nextAt;
        private System.Text.StringBuilder _table;

        private void StartSweep()
        {
            if (_door == null || _demo == null) return;
            _sweeping = true;
            _step = 0;
            _table = new System.Text.StringBuilder();
            _table.AppendLine("[BellGame] §4.3 角度掃き ── 開口を抜けてくる 6 帯域の通過ゲイン");
            _table.AppendLine("  角度    125    250    500     1k     2k     4k   （全経路の合計）");
            _door.ForceAngle(Angles[0]);
            // ★1 段ごとに間を置く。角度を変えた次のフレームには、まだ前の答えが載っています
            //   （エンジンは数フレームかけて解き直す）。待たずに読むと 1 段ずれた表になります。
            _nextAt = EditorApplication.timeSinceStartup + 0.35;
        }

        private void StepSweep()
        {
            if (EditorApplication.timeSinceStartup < _nextAt) return;

            // 名前の対応を取り直す（掃きは窓の描画と別に走るので、ここでも要る）。
            SoundToolShared.Ready(out var _);

            var taps = AcousticFlowSceneDemo.Status.Taps;
            if (taps != null)
            {
                // ★**全経路の合計**を採る。耳が聞くのはこれです。
                //
                // ⚠ 一度ここを「回折タップだけ」にしていて、判断を誤りました。
                //   回折は既定で平坦（diffractionDistanceOnly）なので、そこだけ見ると
                //   「帯域差が出ていない ＝ §4.3 が死んでいる」に見えます。
                //   実際は**閉じている間は扉の板を抜ける透過がこもりを担い**、
                //   開くにつれて平坦な回折が増える ── **担い手が入れ替わることで音色が変わる**。
                //   片方の経路だけ見ていては、その入れ替わりは絶対に見えません。
                for (int i = 0; i < taps.Length; i++)
                {
                    var ts = taps[i];
                    if (ts == null || ts.Count <= 0) continue;

                    var sum = new float[6];
                    float diffPart = 0f, allPart = 0f;
                    for (int t = 0; t < ts.Count; t++)
                    {
                        bool isF = ts.Type != null && t < ts.Type.Length && ts.Type[t] == 'F';
                        for (int b = 0; b < 6; b++)
                        {
                            float g = ts.BandGain[t * 6 + b];
                            sum[b] += g;
                            allPart += g;
                            if (isF) diffPart += g;
                        }
                    }
                    if (allPart <= 1e-6f) continue;

                    _table.Append(string.Format("  {0,4:F0}°", Angles[_step]));
                    for (int b = 0; b < 6; b++) _table.Append(string.Format("{0,7:F3}", sum[b]));
                    // 低域/高域の比 ＝ こもり具合。1 に近いほど晴れている。
                    float tilt = (sum[5] > 1e-6f) ? sum[0] / sum[5] : 0f;
                    _table.AppendLine(string.Format("  こもり{0,5:F1}倍  回折{1,3:F0}%   {2}",
                        tilt, 100f * diffPart / allPart, SoundToolShared.NameOf(i)));
                }
            }

            _step++;
            if (_step >= Angles.Length)
            {
                _sweeping = false;
                _table.AppendLine();
                _table.AppendLine("  ★見るのは『こもり倍率』（125Hz ÷ 4kHz）。");
                _table.AppendLine("    閉じている間は大きく、開くほど 1 に近づけば ③ が成立しています。");
                _table.AppendLine("    ── 閉扉時は扉の板を抜ける透過（こもる）が主役、");
                _table.AppendLine("       開くほど平坦な回折が増えて晴れる。担い手の入れ替わりが音色になります。");
                _table.AppendLine("    『回折%』が増えていれば、その入れ替わりが実際に起きています。");
                Debug.Log(_table.ToString());
                return;
            }
            _door.ForceAngle(Angles[_step]);
            _nextAt = EditorApplication.timeSinceStartup + 0.35;
        }

        // ── 音の配分 ──────────────────────────────────────────
        private void DrawMix()
        {
            EditorGUILayout.LabelField("音の配分", EditorStyles.boldLabel);
            if (_demo == null) { EditorGUILayout.LabelField("　音響ホストが居ません"); return; }

            _demo.transmissionGainDb = EditorGUILayout.Slider(
                "透過 dB（壁・板を抜ける）", _demo.transmissionGainDb, -24f, 24f);
            _demo.diffractionGainDb = EditorGUILayout.Slider(
                "回折 dB（開口を回る）", _demo.diffractionGainDb, -24f, 24f);
            _demo.transmissionHighCutDb = EditorGUILayout.Slider(
                "こもり dB（壁の奥で鳴る感じ）", _demo.transmissionHighCutDb, 0f, 24f);
            _demo.transmissionTilt = EditorGUILayout.Slider(
                "こもりの傾き", _demo.transmissionTilt, 0.2f, 3f);
            _demo.apertureContrast = EditorGUILayout.Slider(
                "開口コントラスト", _demo.apertureContrast, 1f, 12f);

            using (new EditorGUILayout.HorizontalScope())
            {
                _demo.diffractionHrtf = EditorGUILayout.ToggleLeft(
                    "回折を HRTF に載せる", _demo.diffractionHrtf);
                _demo.useDirectionalSteering = EditorGUILayout.ToggleLeft(
                    "定位のステア", _demo.useDirectionalSteering);
            }
            EditorGUILayout.LabelField("　⚠ ステアは入れると遮蔽中の定位が左右反転します（エンジン側に報告済み）。",
                                       EditorStyles.miniLabel);

            // ★これが「開けるほど高域まで通る」の担い手を決めるつまみ。
            //
            //   ON（既定）… 回折は**平坦**。帯域差は壁を抜ける透過が担う、という役割分担。
            //                定位の手がかりを削らないための設計（700Hz 以下は ITD しか無い）。
            //   OFF        … 回折に前川の 6 帯域減衰が掛かる。開口の帯域差が回折に出る。
            //
            // ⚠ いまの構成（壁は通さない・開口が唯一の入口）では、ON だと
            //   **帯域差の担い手が誰も居ません。**実測で 6 帯域が真っ平らでした。
            bool flat = EditorGUILayout.ToggleLeft(
                "回折は平坦のまま（帯域差は透過が担う）", _demo.diffractionDistanceOnly);
            if (flat != _demo.diffractionDistanceOnly)
            {
                _demo.diffractionDistanceOnly = flat;
                Rebuild();
            }
            EditorGUILayout.LabelField("　★切ると回折に 6 帯域が乗ります。壁を塞いだ構成ではこちらが要ります。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.Space(6f);
        }

        // ── 経路 ──────────────────────────────────────────────
        //
        // ★「どの経路が鳴っているか」を切って確かめる。
        //   合計を聴いても、直接・反射・回折・尾のどれが効いているかは分かりません。
        private void DrawPaths()
        {
            EditorGUILayout.LabelField("経路を切る（IrConvolver.Solo）", EditorStyles.boldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                IrConvolver.Solo.PassDirect = EditorGUILayout.ToggleLeft("直接", IrConvolver.Solo.PassDirect);
                IrConvolver.Solo.PassReflect = EditorGUILayout.ToggleLeft("反射", IrConvolver.Solo.PassReflect);
                IrConvolver.Solo.PassDiffract = EditorGUILayout.ToggleLeft("回折", IrConvolver.Solo.PassDiffract);
                IrConvolver.Solo.PassTail = EditorGUILayout.ToggleLeft("尾", IrConvolver.Solo.PassTail);
                if (GUILayout.Button("全部戻す", GUILayout.Width(70f))) IrConvolver.Solo.Reset();
            }
            EditorGUILayout.Space(6f);
        }

        // ── 負荷 ──────────────────────────────────────────────
        //
        // ★本数を 1 本ずつ動かせるのが要点。
        //   費用が本数に比例するのか途中で跳ねるのかで、直し方がまるで変わります。
        private void DrawLoad()
        {
            EditorGUILayout.LabelField("負荷の傾きを測る", EditorStyles.boldLabel);

            EditorGUILayout.LabelField(
                $"実働 {PerfMeter.WorkMs:F1} / {PerfMeter.BudgetMs:F1} ms　"
                + $"うち音響 {PerfMeter.AcousticMs:F2} ms　山の余り {PerfMeter.WorstFreeMs:F1} ms");


            // ★張り付き先。**予算そのもの**なので、費用の話はここと必ず対で読むこと。
            //   45 = 22.2ms / 60 = 16.7ms。数字を緩めても音は軽くなりません。
            var pm = Object.FindFirstObjectByType<PerfMeter>();
            if (pm != null)
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    EditorGUILayout.LabelField($"張り付き先 {pm.targetFps}fps"
                        + $"（予算 {1000f / Mathf.Max(1, pm.targetFps):F1} ms）", GUILayout.Width(220f));
                    if (GUILayout.Button("30")) pm.targetFps = 30;
                    if (GUILayout.Button("45")) pm.targetFps = 45;
                    if (GUILayout.Button("60")) pm.targetFps = 60;
                }
            }

            // ★ポータルの描き直し。**この方式でいちばん高い部分**です。
            //   開口が見えている間、行き先の世界を画面と同じ解像度で丸ごと描いています。
            //   1.0 のままなら、これまでと 1 ミリも変わりません。
            var ps = Object.FindFirstObjectByType<WorldPortalStencil>();
            if (ps != null)
            {
                EditorGUILayout.LabelField($"ポータルの描き直し（{ps.status}）", EditorStyles.miniBoldLabel);
                ps.resolutionScale = EditorGUILayout.Slider(
                    "解像度（1=画面と同じ）", ps.resolutionScale, 0.25f, 1f);
                ps.portalMsaa = EditorGUILayout.ToggleLeft("ポータルにも MSAA（既定 ON）", ps.portalMsaa);
                EditorGUILayout.LabelField("　★0.5 にすると描く画素が 1/4 になります。粗さは目で決めてください。",
                                           EditorStyles.miniLabel);
            }
            if (_rig != null)
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    EditorGUILayout.LabelField($"負荷用の音源 {_rig.liveLoadSources} 本",
                                               GUILayout.Width(140f));
                    if (GUILayout.Button("−1")) _rig.SetCount(_rig.liveLoadSources - 1);
                    if (GUILayout.Button("+1")) _rig.SetCount(_rig.liveLoadSources + 1);
                    if (GUILayout.Button("0 本")) _rig.SetCount(0);
                    if (GUILayout.Button("全部")) _rig.SetCount(99);
                }

                bool col = EditorGUILayout.ToggleLeft("後から足した当たり判定", _rig.addedCollidersOn);
                if (col != _rig.addedCollidersOn) _rig.ToggleColliders();
            }

            if (_demo != null)
            {
                bool rev = EditorGUILayout.ToggleLeft("反響（部屋グラフ）", _demo.enableReverb);
                bool refl = EditorGUILayout.ToggleLeft("反射", _demo.useReflections);
                if (rev != _demo.enableReverb || refl != _demo.useReflections)
                {
                    _demo.enableReverb = rev;
                    _demo.useReflections = refl;
                    Rebuild();
                }
                _demo.roomCellSize = EditorGUILayout.Slider("部屋の格子(m)", _demo.roomCellSize, 0.1f, 0.5f);
            }

            if (_mute != null)
                _mute.silent = EditorGUILayout.ToggleLeft("音を消す（費用はそのまま）", _mute.silent);

            EditorGUILayout.Space(6f);
            DrawRates();

            EditorGUILayout.Space(2f);
            if (GUILayout.Button("組み直す（設定が効かないとき）")) Rebuild();
            EditorGUILayout.LabelField("　★数字は組み直しの数フレーム後に落ち着きます。",
                                       EditorStyles.miniLabel);
        }

        // ── 更新間隔 ──────────────────────────────────────────
        //
        // ★「解く回数を減らす」の粗い版。
        //   本命は**音源ごと**の間引き（遮蔽が深いほど間引く）ですが、
        //   それはエンジン側の仕事なので、こちらでは**系統ごと**で効果を測ります。
        //
        //   ここで「回折を 2→8 にすると何 ms 減って、聞いてどう変わるか」が出れば、
        //   エンジンへの依頼が「音源ごとにこれをやってほしい／効果はこれだけ」
        //   という**要求仕様**の形になります。いまは「重い」としか言えていません。
        //
        // ⚠ 間引きすぎると、動きながら聞いたときに**追従が遅れて段に聞こえます**
        //   （連続性が第一制約なので、そこが崩れたら本末転倒）。
        //   数字だけでなく、歩きながら必ず聴いて決めてください。
        private void DrawRates()
        {
            EditorGUILayout.LabelField("更新間隔（何フレームに 1 回解くか）", EditorStyles.boldLabel);
            if (_demo == null) return;

            _demo.diffractionUpdateEveryFrames = EditorGUILayout.IntSlider(
                "回折 ★高い", _demo.diffractionUpdateEveryFrames, 1, 16);
            _demo.catalogUpdateEveryFrames = EditorGUILayout.IntSlider(
                "稜線カタログ", _demo.catalogUpdateEveryFrames, 1, 16);
            _demo.reverbUpdateEveryFrames = EditorGUILayout.IntSlider(
                "反響", _demo.reverbUpdateEveryFrames, 1, 16);
            _demo.earlyReflectUpdateEveryFrames = EditorGUILayout.IntSlider(
                "早期反射", _demo.earlyReflectUpdateEveryFrames, 1, 16);
            _demo.probeUpdateEveryFrames = EditorGUILayout.IntSlider(
                "プローブ", _demo.probeUpdateEveryFrames, 1, 32);

            EditorGUILayout.LabelField("　★回折から動かしてください。実測で費用の大半がそこです。",
                                       EditorStyles.miniLabel);
            EditorGUILayout.LabelField("　⚠ 間引きすぎると歩いたときに段に聞こえます。必ず動きながら聴いて。",
                                       EditorStyles.miniLabel);
        }

        /// 効かない設定があるとき用。OnEnable で読まれる値はこれを通さないと届きません。
        private void Rebuild()
        {
            if (_demo == null) return;
            _demo.enabled = false;
            _demo.enabled = true;
        }
    }
}
