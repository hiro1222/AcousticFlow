/* AcousticToolsWindow.cs
 * 音響まわりの道具を 1 枚の窓にタブで束ねたもの。
 *
 *   メニュー: AcousticFlow > ツール (履歴・予算)
 *
 * なぜ 1 枚に束ねるか:
 *   監視窓が既に 5 枚あり（Status / Band / Output / Reverb / OutputScope）、
 *   そこへ道具を 1 つずつ足していくと「どの窓に何があるか」を探す代金のほうが高くつく。
 *   道具を足すたびにタブが 1 つ増える形にしておく。
 *
 * いま入っているタブ:
 *   履歴 … 道具2。毎フレームの状態を巻き戻して見る（記録は AcousticHistory）
 *   予算 … 道具10。audio thread の実費と音源数の余裕
 *   音源 … 道具A。ソロ／ミュートと寄与度順の一覧
 *   配分 … 2026-09-12 追加。いまの耳に、どの向きから・どれだけ届いているか（AF_WorldArrivals を足して描くだけ）。
 *   情報 … 2026-09-09 追加。いま何で鳴っているか（模型の切り替え・尾・部屋・音の出どころ）。
 *          ★画面の HUD は箱が 480x300 で固定なので、下に足した行から順に**切れて見えなくなる**
 *            （尾の切り替えを足しても表示が変わらず「効いていない」と読めてしまった）。
 *            聞き比べの判断はこのタブで行う。ここは窓なので伸びるし、切り替えも押せる。
 */
using System.IO;
using UnityEditor;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public class AcousticToolsWindow : EditorWindow
    {
        private enum Tab { Info, Mix, History, Budget, Sources }
        private static readonly string[] kTabNames = { "情報", "配分", "履歴", "予算", "音源" };

        private Tab _tab = Tab.Info;
        private Vector2 _scroll;

        // --- 履歴タブの状態 ---
        private bool _follow = true;          // 最新に追従するか
        private int _cursor;                  // 見ているフレーム（0 = 一番古い）
        private Vector3[] _pts = new Vector3[0];

        // --- 予算タブの状態 ---
        // 36 は実測（1 スレッドが飽和するまで。SYSTEM_BRIEF §6）。推定値ではない。
        private int _budget = 36;

        [MenuItem("AcousticFlow/ツール (履歴・予算)")]
        public static void Open()
        {
            var w = GetWindow<AcousticToolsWindow>("AF ツール");
            w.minSize = new Vector2(420f, 420f);
            w.Show();
        }

        private void OnEnable()
        {
            EditorApplication.update += Repaint;
            SceneView.duringSceneGui += OnSceneGui;
        }

        private void OnDisable()
        {
            EditorApplication.update -= Repaint;
            SceneView.duringSceneGui -= OnSceneGui;
            // 窓を閉じたらソロは必ず解除する。切ったまま忘れると「音が出ない」で悩む。
            Solo.Reset();
        }

        private void OnGUI()
        {
            _tab = (Tab)GUILayout.Toolbar((int)_tab, kTabNames);
            EditorGUILayout.Space(4f);

            if (_tab == Tab.Info) DrawInfo();
            else if (_tab == Tab.Mix) DrawMix();
            else if (_tab == Tab.History) DrawHistory();
            else if (_tab == Tab.Budget) DrawBudget();
            else DrawSources();
        }

        // =====================================================================
        // 情報タブ ── いま何で鳴っているか（読むだけでなく、ここから切り替える）
        // =====================================================================
        private Vector2 _infoScroll;

        private static string OnOff(bool b) { return b ? "ON" : "OFF"; }
        private static string Db(float lin) { return (lin > 1e-6f) ? (20f * Mathf.Log10(lin)).ToString("F1") + " dB" : "無音"; }

        // ── 新コア（Flow）の帳簿 ──
        //   AcousticWorld が場面にあれば、音源ごとに 総量 と 五成分の内訳 を出す。数値は DLL の AF_WorldMixInfo（生の帳簿）。
        private static string Db2(double e) { return (e > 1e-12) ? (10.0 * System.Math.Log10(e)).ToString("F1") + " dB" : "無音"; }
        private void DrawFlowInfo()
        {
            var world = AcousticWorld.Instance;
            if (world == null || !EditorApplication.isPlaying) return;
            EditorGUILayout.LabelField("新コア（Flow）", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("  部屋", world.RoomCount + " 個   リスナーの部屋 " + world.ListenerRoom + "   更新 " + world.UpdateMs.ToString("F2") + " ms   箱 " + world.BoxCount + "（動く物 " + world.DynamicCount + "）");
            EditorGUILayout.LabelField("  予算", "飛ばしたレイ " + world.SpentRays + " / 総予算 " + (world.totalRays > 0 ? world.totalRays.ToString() : "無制限")
                + "   枠 厳密 " + world.fullSlots + " / 簡易 " + world.lightSlots + "   探り " + world.probesPerFrame + " 本/フレーム"
                + "   分散 " + world.rayGroups + " 組   スレッド " + world.workers);
            if (world.ApertureCount > 0)
            {
                string ap = "";
                for (int i = 0; i < world.ApertureCount; i++) ap += (i > 0 ? "  " : "") + "戸口" + i + " 素通し " + world.ApertureOpenFrac(i).ToString("F2");
                EditorGUILayout.LabelField("  戸口", ap);
            }
            if (world.TailHost != null)
                EditorGUILayout.LabelField("  後期の器", world.TailHost.FdnRoomCount + " 部屋   出力 " + Db(world.TailHost.FdnRms) + "   方向バス " + (world.TailHost.DirectionBusHandle != System.IntPtr.Zero ? "ON" : "OFF"));
            // HRTF: 声（直接音）と方向バス（反射・尾・戸口の線音源）。差さっていないと、反射と尾は耳の時間差だけになる。
            {
                int shared = 0;
                foreach (var v in world.Voices) if (v != null && v.UsesSharedHrtf) shared++;
                string voiceHrtf = (shared > 0) ? world.HrtfName + "（" + shared + "/" + world.Voices.Count + " 本）" : "合成（球の頭）";
                string busHrtf = (world.TailHost != null) ? (world.TailHost.BusHasHrtf ? world.TailHost.BusHrtfName : "★差さっていない") : "-";
                EditorGUILayout.LabelField("  HRTF", "声 " + voiceHrtf + "   方向バス " + busHrtf + "   （合成は前後・上下の手がかり無し）");
            }
            foreach (var v in world.Voices)
            {
                if (v == null || !world.TryGetMixInfo(v, out var mi)) continue;
                double tot = 0; var comp = new double[5];
                for (int b = 0; b < 6; b++) { tot += mi.energy6[b]; for (int c = 0; c < 5; c++) comp[c] += mi.component[c * 6 + b]; }
                string pct(int c) { return tot > 1e-12 ? (100.0 * comp[c] / tot).ToString("F0") + "%" : "-"; }
                int tier = world.TierOf(v);
                string tierName = tier == 0 ? "厳密" : tier == 1 ? "簡易" : "保持";
                EditorGUILayout.LabelField("  " + v.name, tierName + " " + world.RaysOf(v) + " 本   部屋 " + mi.room + "   総量 " + Db2(tot) + "（1/m²、出力 1 に対して）   見通し " + mi.visibleFraction.ToString("F2") + "（遮る物 " + mi.shadowers + "）   壁の横切り " + mi.directCrossings + " 枚");
                EditorGUILayout.LabelField("     配分", "直接 " + pct(0) + " / 初期 " + pct(1) + " / 後期 " + pct(2) + " / 回折 " + pct(3) + " / 透過 " + pct(4)
                    + "   タップ " + mi.tapCount + " 送り " + mi.sendCount + "   虚像 " + mi.imageCount + "/" + mi.imageCandidates);
                EditorGUILayout.LabelField("     時刻", "直接 " + (mi.directSec * 1000f).ToString("F1") + " ms   最初の反射 " + (mi.firstReflectSec * 1000f).ToString("F1") + " ms   尾の開始 " + (mi.onsetSec * 1000f).ToString("F1") + " ms"
                    + "   レイ " + mi.raysTraced + " 本 / ヒット " + mi.hits);
            }
            EditorGUILayout.Space(6f);
        }

        // =====================================================================
        // 配分タブ ── いまの耳に、どの向きから・どれだけ届いているか（2026-09-12）
        //   数値は DLL の AF_WorldArrivals（配分の出口 ＝ 耳に届く量）。C# は足して描くだけ。
        //   ★試聴の問い「隣の部屋の残響が全体から聞こえる」を見て確かめるための物。
        //     後期を「耳の部屋の響き／戸口から直接／戸口から流した響き」に分けて出す。
        // =====================================================================
        private static readonly string[] kArrivalNames = {
            "直接", "初期（虚像）", "初期（方向なし）", "回折", "透過",
            "後期・耳の部屋の響き", "後期・戸口から直接", "後期・戸口から流した響き", "後期・戸口の向きの点" };
        private static readonly Color[] kArrivalColors = {
            new Color(0.40f, 1.00f, 0.50f), new Color(1.00f, 0.85f, 0.30f), new Color(0.75f, 0.65f, 0.35f),
            new Color(0.35f, 0.90f, 1.00f), new Color(0.75f, 0.50f, 1.00f), new Color(0.35f, 0.55f, 1.00f),
            new Color(1.00f, 0.55f, 0.15f), new Color(1.00f, 0.78f, 0.55f), new Color(1.00f, 0.40f, 0.80f) };
        private readonly AFArrival[] _arrBuf = new AFArrival[128];
        private readonly System.Collections.Generic.List<AFArrival> _arr = new System.Collections.Generic.List<AFArrival>();
        private int _mixVoice = -1;                       // -1 = 全音源
        private readonly Vector3[] _ring = new Vector3[65];

        private static bool HasDir(AFArrival a) { return a.spread < 0.5f && (a.dirX != 0f || a.dirY != 0f || a.dirZ != 0f); }

        private void DrawMix()
        {
            var world = AcousticWorld.Instance;
            if (world == null || !EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に、AcousticWorld がある場面（新コア）で出ます。", MessageType.Info);
                return;
            }
            var names = new System.Collections.Generic.List<string> { "全音源" };
            foreach (var v in world.Voices) names.Add(v != null ? v.name : "(なし)");
            _mixVoice = Mathf.Clamp(EditorGUILayout.Popup("音源", _mixVoice + 1, names.ToArray()) - 1, -1, world.Voices.Count - 1);

            _arr.Clear();
            for (int i = 0; i < world.Voices.Count; i++)
            {
                if (_mixVoice >= 0 && i != _mixVoice) continue;
                int n = world.GetArrivals(world.Voices[i], _arrBuf);
                for (int k = 0; k < n; k++) _arr.Add(_arrBuf[k]);
            }
            if (_arr.Count == 0)
            {
                EditorGUILayout.HelpBox("到来がありません。DLL が古いと出ません（Unity を閉じて tools/dev.ps1 deploy）。", MessageType.Warning);
                return;
            }

            var sum = new double[kArrivalNames.Length];
            double total = 0.0, lateAll = 0.0, lateDir = 0.0, dirAll = 0.0, xw = 0.0;
            foreach (var a in _arr)
            {
                if (a.kind < 0 || a.kind >= sum.Length) continue;
                sum[a.kind] += a.energy; total += a.energy;
                bool d = HasDir(a);
                if (a.kind >= 5) { lateAll += a.energy; if (d) lateDir += a.energy; }
                if (d) { dirAll += a.energy; xw += a.energy * a.dirX; }
            }
            EditorGUILayout.LabelField("耳に届く総量", Db2(total) + "（1/m²、音源の出力 1 に対して）");
            EditorGUILayout.LabelField("後期のうち向きのある分", lateAll > 1e-12 ? (100.0 * lateDir / lateAll).ToString("F1") + "%" : "-");
            EditorGUILayout.LabelField("向きのある音の左右の重心",
                dirAll > 1e-12 ? ((xw / dirAll) >= 0.0 ? "右 " : "左 ") + System.Math.Abs(xw / dirAll).ToString("F2") + "（1 で真横）" : "-");
            EditorGUILayout.Space(4f);

            for (int k = 0; k < sum.Length; k++)
            {
                if (sum[k] <= 0.0) continue;
                Rect row = EditorGUILayout.GetControlRect(false, 16f);
                float share = total > 1e-12 ? (float)(sum[k] / total) : 0f;
                const float labelW = 170f, valueW = 130f;
                float barMax = Mathf.Max(10f, row.width - labelW - valueW);
                EditorGUI.DrawRect(new Rect(row.x, row.y + 4f, 8f, 8f), kArrivalColors[k]);
                GUI.Label(new Rect(row.x + 12f, row.y, labelW - 12f, row.height), kArrivalNames[k], EditorStyles.miniLabel);
                EditorGUI.DrawRect(new Rect(row.x + labelW, row.y + 3f, barMax, row.height - 6f), new Color(1f, 1f, 1f, 0.05f));
                EditorGUI.DrawRect(new Rect(row.x + labelW, row.y + 3f, Mathf.Max(1f, barMax * share), row.height - 6f), kArrivalColors[k]);
                GUI.Label(new Rect(row.xMax - valueW + 4f, row.y, valueW - 4f, row.height), (share * 100f).ToString("F1") + "%   " + Db2(sum[k]), EditorStyles.miniLabel);
            }
            EditorGUILayout.Space(6f);

            float size = Mathf.Min(position.width - 20f, 360f);
            Rect r = GUILayoutUtility.GetRect(size, size);
            r.x = (position.width - size) * 0.5f; r.width = size; r.height = size;
            DrawMixPolar(r, total);
            GUILayout.Space(4f);
            EditorGUILayout.HelpBox("円: 上が前（リスナーの向き）、右が右。中心からの距離は総量に対する割合（外周 0 dB、中心 −40 dB、薄い輪は −10/−20/−30 dB）。"
                + "点は向きのある到来（大きさも割合）、色の輪は全方向から来る分、オレンジの弧は戸口の線音源の横幅。", MessageType.None);

            // ── 地図（メートル。耳を中心に前が上）──
            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("地図（上から。耳を中心に前が上）", EditorStyles.boldLabel);
            _mapRadius = EditorGUILayout.Slider("見える半径 (m)", _mapRadius, 2f, 20f);
            float msize = Mathf.Min(position.width - 20f, 420f);
            Rect mr = GUILayoutUtility.GetRect(msize, msize);
            mr.x = (position.width - msize) * 0.5f; mr.width = msize; mr.height = msize;
            DrawMixMap(mr, world, total, sum);
            GUILayout.Space(4f);
            EditorGUILayout.HelpBox("地図: 灰の線は壁（耳の高さを通る箱）、オレンジの線は動く物（扉の板）、水色の線は戸口（数字は素通しの割合）。"
                + "壁の黄色い塗りは、そこで返った初期反射の量（いちばん多い壁が濃い）。"
                + "緑の線は直接音、紫は透過、水色は回折（耳 → 稜線の点 → 音源）、黄の点は壁の上の反射点、オレンジの点は戸口の線音源。"
                + "全方向から来る後期は場所を持たないので、左上に割合で出す。", MessageType.None);
        }

        private void DrawMixPolar(Rect r, double total)
        {
            EditorGUI.DrawRect(r, new Color(0.11f, 0.11f, 0.13f));
            if (Event.current.type != EventType.Repaint || total <= 1e-12) return;
            Vector2 c = r.center;
            float R = r.width * 0.5f - 8f;
            System.Func<double, float> radius = e =>
            {
                double db = 10.0 * System.Math.Log10(System.Math.Max(e / total, 1e-12));
                return R * Mathf.Clamp01((float)((db + 40.0) / 40.0));
            };
            Handles.BeginGUI();
            for (int k = 1; k <= 3; k++) DrawRing(c, R * (1f - k * 0.25f), new Color(1f, 1f, 1f, 0.07f), 1f);
            DrawRing(c, R, new Color(1f, 1f, 1f, 0.15f), 1f);
            Handles.color = new Color(1f, 1f, 1f, 0.55f);
            Handles.DrawAAPolyLine(2f, new Vector3(c.x - 6f, c.y + 4f), new Vector3(c.x, c.y - 9f), new Vector3(c.x + 6f, c.y + 4f));   // 前

            // 全方向から来る分（種類ごとに足して 1 本の輪）
            var ringSum = new double[kArrivalNames.Length];
            foreach (var a in _arr) if (!HasDir(a) && a.kind >= 0 && a.kind < ringSum.Length) ringSum[a.kind] += a.energy;
            for (int k = 0; k < ringSum.Length; k++)
                if (ringSum[k] > 0.0) DrawRing(c, radius(ringSum[k]), kArrivalColors[k], 1.5f + 6f * Mathf.Sqrt((float)(ringSum[k] / total)));

            // 戸口の線音源の弧（点の方位の端から端。±180° をまたいでも繋がるよう最初の点に寄せて開く）
            double doorE = 0.0; float azRef = 0f, azMin = 0f, azMax = 0f; bool first = true;
            foreach (var a in _arr)
            {
                if (a.kind != 6) continue;
                doorE += a.energy;
                float az = Mathf.Atan2(a.dirX, a.dirZ) * Mathf.Rad2Deg;
                if (first) { azRef = az; azMin = az; azMax = az; first = false; continue; }
                az = azRef + Mathf.DeltaAngle(azRef, az);
                azMin = Mathf.Min(azMin, az); azMax = Mathf.Max(azMax, az);
            }
            if (doorE > 0.0)
            {
                float rr = radius(doorE);
                const int segs = 24;
                var arc = new Vector3[segs + 1];
                for (int i = 0; i <= segs; i++)
                {
                    float az = Mathf.Lerp(azMin, azMax, (float)i / segs) * Mathf.Deg2Rad;
                    arc[i] = new Vector3(c.x + rr * Mathf.Sin(az), c.y - rr * Mathf.Cos(az));
                }
                Handles.color = kArrivalColors[6];
                Handles.DrawAAPolyLine(4f, arc);
            }

            // 向きのある到来（点）
            foreach (var a in _arr)
            {
                if (!HasDir(a)) continue;
                float az = Mathf.Atan2(a.dirX, a.dirZ);
                float rr = radius(a.energy);
                var p = new Vector3(c.x + rr * Mathf.Sin(az), c.y - rr * Mathf.Cos(az));
                Handles.color = kArrivalColors[Mathf.Clamp(a.kind, 0, kArrivalColors.Length - 1)];
                Handles.DrawSolidDisc(p, Vector3.forward, 2.5f + 7f * Mathf.Sqrt((float)(a.energy / total)));
            }
            Handles.EndGUI();
        }

        private float _mapRadius = 8f;

        // 地図: エンジンの箱と戸口（AF_WorldBoxInfo / AF_WorldApertureInfo）と、到来の出どころ（AF_Arrival.origin）を上から描く。
        //   ★Handles.BeginGUI は使わず GUI.BeginClip の中で描く（地図の枠の外へ壁の線がはみ出さないように）。
        private void DrawMixMap(Rect r, AcousticWorld world, double total, double[] sum)
        {
            EditorGUI.DrawRect(r, new Color(0.10f, 0.10f, 0.12f));
            if (Event.current.type != EventType.Repaint || total <= 1e-12) return;
            Transform lt = world.listener;
            if (lt == null) { var al = Object.FindFirstObjectByType<AudioListener>(); if (al != null) lt = al.transform; }
            if (lt == null) return;
            Vector3 L = lt.position;
            Vector3 fwd = lt.forward; fwd.y = 0f;
            if (fwd.sqrMagnitude < 1e-6f) fwd = Vector3.forward;
            fwd.Normalize();
            Vector3 right = new Vector3(fwd.z, 0f, -fwd.x);                 // 上から見て前の右
            Vector2 c = new Vector2(r.width * 0.5f, r.height * 0.5f);
            float scale = (r.width * 0.5f - 6f) / Mathf.Max(0.5f, _mapRadius);
            System.Func<Vector3, Vector3> toMap = p =>
            {
                Vector3 d = p - L;
                return new Vector3(c.x + Vector3.Dot(d, right) * scale, c.y - Vector3.Dot(d, fwd) * scale, 0f);
            };
            Vector3 center = new Vector3(c.x, c.y, 0f);

            // 壁ごとの初期反射（虚像の到来を、耳の側で返った箱で足す）
            var wallE = new System.Collections.Generic.Dictionary<int, double>();
            double wallMax = 0.0;
            foreach (var a in _arr)
            {
                if (a.kind != 1 || a.box < 0) continue;
                wallE.TryGetValue(a.box, out double v0);
                wallE[a.box] = v0 + a.energy;
                wallMax = System.Math.Max(wallMax, wallE[a.box]);
            }

            GUI.BeginClip(r);
            // 目盛りの輪（1 m ごと、5 m は濃く）
            for (int m = 1; m <= Mathf.CeilToInt(_mapRadius); m++)
                DrawRing(c, m * scale, new Color(1f, 1f, 1f, (m % 5 == 0) ? 0.10f : 0.035f), 1f);

            // 箱（耳の高さを通る物だけ。床と天井は描かない）
            int nb = world.NativeBoxCount;
            var quad = new Vector3[5];
            for (int i = 0; i < nb; i++)
            {
                if (!world.TryGetBox(i, out AFBoxInfo b) || b.active == 0) continue;
                Vector3 ax = new Vector3(b.xx, b.xy, b.xz) * b.hx, ay = new Vector3(b.yx, b.yy, b.yz) * b.hy, az = new Vector3(b.zx, b.zy, b.zz) * b.hz;
                float yHalf = Mathf.Abs(ax.y) + Mathf.Abs(ay.y) + Mathf.Abs(az.y);
                if (Mathf.Abs(L.y - b.cy) > yHalf) continue;
                // 足跡の四角: いちばん上下を向いた軸を捨て、残りの 2 本で作る
                float sx = Mathf.Abs(b.xy), sy = Mathf.Abs(b.yy), sz = Mathf.Abs(b.zy);
                Vector3 u, v;
                if (sy >= sx && sy >= sz) { u = ax; v = az; }
                else if (sx >= sy && sx >= sz) { u = ay; v = az; }
                else { u = ax; v = ay; }
                Vector3 cc = new Vector3(b.cx, b.cy, b.cz);
                quad[0] = toMap(cc + u + v); quad[1] = toMap(cc - u + v); quad[2] = toMap(cc - u - v); quad[3] = toMap(cc + u - v); quad[4] = quad[0];
                if (wallE.TryGetValue(i, out double we) && wallMax > 0.0)
                {
                    Handles.color = new Color(1f, 0.85f, 0.3f, 0.12f + 0.6f * (float)(we / wallMax));
                    Handles.DrawAAConvexPolygon(quad[0], quad[1], quad[2], quad[3]);
                }
                Handles.color = (b.dynamic != 0) ? new Color(1f, 0.6f, 0.2f, 0.95f) : new Color(0.72f, 0.72f, 0.78f, 0.85f);
                Handles.DrawAAPolyLine((b.dynamic != 0) ? 3f : 2f, quad);
            }

            // 戸口
            int nap = world.ApertureCount;
            for (int i = 0; i < nap; i++)
            {
                if (!world.TryGetAperture(i, out AFApertureInfo ap)) continue;
                Vector3 cc = new Vector3(ap.cx, ap.cy, ap.cz);
                Vector3 u = new Vector3(ap.ux, ap.uy, ap.uz), v = new Vector3(ap.vx, ap.vy, ap.vz);
                Vector3 wv = (Mathf.Abs(u.y) <= Mathf.Abs(v.y)) ? u * ap.halfU : v * ap.halfV;
                Handles.color = new Color(0.35f, 0.9f, 1f, 0.9f);
                Handles.DrawAAPolyLine(3f, toMap(cc - wv), toMap(cc + wv));
                Vector3 lp = toMap(cc);
                GUI.Label(new Rect(lp.x + 5f, lp.y + 2f, 120f, 14f), "戸口" + i + " 素通し " + ap.openFrac.ToString("F2"), EditorStyles.miniLabel);
            }

            // 到来の出どころ
            foreach (var a in _arr)
            {
                if (a.hasOrigin == 0 || a.kind < 0 || a.kind >= kArrivalColors.Length) continue;
                float share = (float)(a.energy / total);
                Color col = kArrivalColors[a.kind];
                Vector3 o = toMap(new Vector3(a.originX, a.originY, a.originZ));
                if (a.kind == 0 || a.kind == 3 || a.kind == 4)
                {
                    // 直接・回折・透過: 耳から出どころへ線（太さは割合）
                    Handles.color = new Color(col.r, col.g, col.b, 0.9f);
                    Handles.DrawAAPolyLine(1.5f + 6f * Mathf.Sqrt(share), center, o);
                    if (a.kind == 3)
                    {
                        foreach (var vv in world.Voices)
                        {
                            if (vv == null || vv.EmitterId != a.emitter) continue;
                            Handles.color = new Color(col.r, col.g, col.b, 0.35f);
                            Handles.DrawAAPolyLine(1.5f, o, toMap(vv.transform.position));
                        }
                        Handles.color = col;
                        Handles.DrawSolidDisc(o, Vector3.forward, 3f);
                    }
                }
                else
                {
                    // 壁の上の反射点・戸口の点: 点（大きさは割合）と、耳への細い線
                    Handles.color = new Color(col.r, col.g, col.b, 0.22f);
                    Handles.DrawAAPolyLine(1f, center, o);
                    Handles.color = col;
                    Handles.DrawSolidDisc(o, Vector3.forward, 2f + 6f * Mathf.Sqrt(share));
                }
            }

            // 音源と耳
            foreach (var vv in world.Voices)
            {
                if (vv == null) continue;
                Vector3 sp = toMap(vv.transform.position);
                Handles.color = Color.white;
                Handles.DrawWireDisc(sp, Vector3.forward, 5f);
                GUI.Label(new Rect(sp.x + 7f, sp.y - 7f, 140f, 14f), vv.name, EditorStyles.miniLabel);
            }
            Handles.color = Color.white;
            Handles.DrawAAPolyLine(2.5f, new Vector3(c.x - 7f, c.y + 5f), new Vector3(c.x, c.y - 10f), new Vector3(c.x + 7f, c.y + 5f), new Vector3(c.x - 7f, c.y + 5f));

            // 縮尺と、全方向の後期の割合
            Handles.color = new Color(1f, 1f, 1f, 0.7f);
            Handles.DrawAAPolyLine(2f, new Vector3(10f, r.height - 12f), new Vector3(10f + scale, r.height - 12f));
            GUI.Label(new Rect(10f, r.height - 28f, 60f, 14f), "1 m", EditorStyles.miniLabel);
            GUI.Label(new Rect(6f, 4f, r.width - 12f, 14f),
                      "全方向の後期: 耳の部屋の響き " + (100.0 * sum[5] / total).ToString("F1") + "%  ／  戸口から流した響き " + (100.0 * sum[7] / total).ToString("F1") + "%",
                      EditorStyles.miniLabel);
            GUI.EndClip();
        }

        private void DrawRing(Vector2 c, float radius, Color col, float width)
        {
            for (int i = 0; i < _ring.Length; i++)
            {
                float t = (float)i / (_ring.Length - 1) * Mathf.PI * 2f;
                _ring[i] = new Vector3(c.x + radius * Mathf.Cos(t), c.y + radius * Mathf.Sin(t));
            }
            Handles.color = col;
            Handles.DrawAAPolyLine(width, _ring);
        }

        private void DrawInfo()
        {
            DrawFlowInfo();
            var demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo == null) { EditorGUILayout.HelpBox("AcousticFlowSceneDemo が場面にありません。", MessageType.Warning); return; }
            bool playing = EditorApplication.isPlaying;
            _infoScroll = EditorGUILayout.BeginScrollView(_infoScroll);

            // ── 後期の尾（聞き比べの主役）──
            EditorGUILayout.LabelField("後期の尾（この作品の尾 ＝ tailModel）", EditorStyles.boldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("模型", GUILayout.Width(60f));
                int now = Mathf.Clamp(demo.tailModel, 0, 1);
                int next = GUILayout.Toolbar(now, new[] { "0 畳み込み（IR）", "1 FDN（部屋ごと）" }, GUILayout.Width(260f));
                if (next != now) { Undo.RecordObject(demo, "tailModel"); demo.tailModel = next; EditorUtility.SetDirty(demo); }
                GUILayout.Label("(K キーでも切替)", EditorStyles.miniLabel);
            }
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("画面の HUD", GUILayout.Width(60f));
                bool d = GUILayout.Toggle(demo.hudDetail, "一覧も画面に出す（既定 OFF＝2 行）", "Button", GUILayout.Width(260f));
                if (d != demo.hudDetail) { Undo.RecordObject(demo, "hudDetail"); demo.hudDetail = d; EditorUtility.SetDirty(demo); }
                GUILayout.Label("(ビルドで見るとき用)", EditorStyles.miniLabel);
            }
            var tb = Object.FindFirstObjectByType<TailBusRenderer>();
            if (!playing)
            {
                EditorGUILayout.HelpBox("再生中に、尾の器と音の出どころを表示します。", MessageType.Info);
            }
            else if (demo.tailModel == 1)
            {
                string why = AcousticFlowSceneDemo.Status.TailFdnWhy;
                bool live = tb != null && tb.FdnMixHandle != System.IntPtr.Zero;
                if (!live)
                    EditorGUILayout.HelpBox("FDN の器がありません: " + (string.IsNullOrEmpty(why) ? "作成待ち" : why), MessageType.Warning);
                else
                {
                    EditorGUILayout.LabelField("  部屋の FDN", tb.FdnRoomCount + " 本   出力 " + Db(tb.FdnRms));
                    EditorGUILayout.LabelField("  生きた RT60 (500Hz)", AcousticFlowSceneDemo.Status.TailFdnRt60.ToString("F2") + " s"
                        + "    口の素通し " + AcousticFlowSceneDemo.Status.TailFdnOpen.ToString("F2"));
                    EditorGUILayout.LabelField("  向き", tb.DirectionBusHandle != System.IntPtr.Zero
                        ? "方向バス（戸口の向き・自室は一様）" : "L/R の 2 本（方向バスが OFF）");
                }
            }
            else
            {
                EditorGUILayout.LabelField("  尾の共有バス", OnOff(tb != null && tb.enableSharedTail)
                    + "   （音源ごとの尾の出力は「音源」タブ、または下の一覧の 出力 を見る）");
            }

            // ── 音の出どころ（M を押しても消えない、の切り分け）──
            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("音の出どころ", EditorStyles.boldLabel);
            if (!playing) { EditorGUILayout.EndScrollView(); return; }
            var all = Object.FindObjectsByType<AudioSource>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
            if (all.Length == 0) EditorGUILayout.HelpBox("鳴っている AudioSource がありません。", MessageType.Warning);
            int muted = 0, playingN = 0;
            foreach (var a in all) { if (a.mute) muted++; if (a.isPlaying) playingN++; }
            EditorGUILayout.LabelField("  AudioSource", all.Length + " 本（再生中 " + playingN + "・ミュート " + muted + "）");
            // ★ミュートが効かないときの正体はたいてい「デモが知らない AudioSource が鳴っている」。
            //   デモの M キーは Inspector の sources に並んだ物しか触らないので、一覧で見分ける。
            foreach (var a in all)
            {
                // ★音を作るのは音響エンジン(C++)の VoiceConvolver だけ。AudioSource は「何を鳴らすか」を持つ入口で、
                //   その中身（data）を VoiceConvolver がエンジンへ渡し、返ってきた音で上書きする。
                var vc = a.GetComponent<VoiceConvolver>();
                string dsp = (vc == null) ? "★音響エンジンに繋がっていない（VoiceConvolver なし）"
                           : vc.enabled ? "音響エンジン(C++)" : "★VoiceConvolver が無効";
                string outRms = (vc != null) ? "  出力 " + Db(vc.rmsOut) : "";
                // ★クリップ名を必ず出す。畳み込み器は clip が空だと "ir_silence"（無音 1 秒）を差し込んで
                //   再生してしまうので、「鳴っているのに何も聞こえない」が起きる。名前で見分ける。
                string clip = (a.clip == null) ? "クリップ無し"
                            : (a.clip.name == "ir_silence") ? "★無音の差し込み（クリップ未設定）"
                            : a.clip.name + " " + a.clip.length.ToString("F1") + "s";
                using (new EditorGUILayout.HorizontalScope())
                {
                    GUILayout.Label((a.mute ? "[消] " : a.isPlaying ? "[鳴] " : "[停] ") + a.gameObject.name, GUILayout.Width(190f));
                    GUILayout.Label(clip + "   " + dsp + "   音量 " + a.volume.ToString("F2") + outRms, EditorStyles.miniLabel);
                    if (GUILayout.Button(a.mute ? "解除" : "消す", EditorStyles.miniButton, GUILayout.Width(44f))) a.mute = !a.mute;
                }
            }

            // ── 診断の一覧 ──
            //   ★画面の HUD と**同じ関数**を呼ぶ（AcousticFlowSceneDemo.DrawDiagnosticsGui）。
            //     ここで書き写すと、片方だけ直して食い違う。窓は伸びるので、全部ここで見られる。
            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("診断（画面の HUD と同じ中身）", EditorStyles.boldLabel);
            if (_diagStyle == null) _diagStyle = new GUIStyle(GUI.skin.label) { fontSize = 12, wordWrap = true };
            demo.DrawDiagnosticsGui(_diagStyle);
            EditorGUILayout.EndScrollView();
        }
        private GUIStyle _diagStyle;

        // =====================================================================
        // 履歴タブ（道具2）
        // =====================================================================
        private void DrawHistory()
        {
            int n = AcousticHistory.Count;

            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                _follow = GUILayout.Toggle(_follow, "最新に追従", EditorStyles.toolbarButton, GUILayout.Width(80f));
                if (GUILayout.Button("いまに印", EditorStyles.toolbarButton, GUILayout.Width(64f)))
                    AcousticHistory.MarkLatest(AcousticHistory.MarkManual);
                if (GUILayout.Button("クリア", EditorStyles.toolbarButton, GUILayout.Width(52f)))
                { AcousticHistory.Clear(); _cursor = 0; }
                if (GUILayout.Button("CSV", EditorStyles.toolbarButton, GUILayout.Width(44f)))
                {
                    string p = EditorUtility.SaveFilePanel("履歴を書き出す", "", "acoustic_history.csv", "csv");
                    if (!string.IsNullOrEmpty(p))
                    {
                        AcousticHistory.ExportCsv(p);
                        Debug.Log("[AFツール] 履歴を書き出した: " + p + "（" + n + " フレーム）");
                    }
                }
                GUILayout.FlexibleSpace();
                GUILayout.Label(n + " / " + AcousticHistory.Capacity + " フレーム", EditorStyles.miniLabel);
            }

            // 跳びの検出しきい値。ここは道具1（不連続ウォッチャ）の芽。
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("跳びと見なす差", GUILayout.Width(84f));
                AcousticHistory.JumpDb = EditorGUILayout.FloatField(AcousticHistory.JumpDb, GUILayout.Width(40f));
                GUILayout.Label("dB / 遮蔽", GUILayout.Width(50f));
                AcousticHistory.JumpOcc = EditorGUILayout.FloatField(AcousticHistory.JumpOcc, GUILayout.Width(40f));
                GUILayout.FlexibleSpace();
                var st = new GUIStyle(EditorStyles.miniLabel);
                if (AcousticHistory.JumpCount > 0) st.normal.textColor = new Color(1f, 0.45f, 0.35f);
                GUILayout.Label("検出 " + AcousticHistory.JumpCount + " 件", st);
            }

            if (n <= 0)
            {
                EditorGUILayout.HelpBox(
                    "まだ履歴がありません。\n再生すると自動で溜まります（窓を開いていなくても記録しています）。",
                    MessageType.Info);
                return;
            }

            if (_follow) _cursor = n - 1;
            _cursor = Mathf.Clamp(_cursor, 0, n - 1);

            Rect graph = GUILayoutUtility.GetRect(10f, 10f, GUILayout.ExpandWidth(true), GUILayout.Height(132f));
            DrawGraph(graph, n);

            // 掃引バー。触ったら追従を切る（見たい所で止まってほしいので）。
            int newCursor = Mathf.RoundToInt(GUILayout.HorizontalSlider(_cursor, 0f, n - 1));
            if (newCursor != _cursor) { _cursor = newCursor; _follow = false; }

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("◀", GUILayout.Width(28f))) { _cursor = Mathf.Max(0, _cursor - 1); _follow = false; }
                if (GUILayout.Button("▶", GUILayout.Width(28f))) { _cursor = Mathf.Min(n - 1, _cursor + 1); _follow = false; }
                if (GUILayout.Button("◀ 前の印", GUILayout.Width(70f))) { _cursor = FindMark(_cursor, -1, n); _follow = false; }
                if (GUILayout.Button("次の印 ▶", GUILayout.Width(70f))) { _cursor = FindMark(_cursor, +1, n); _follow = false; }
                GUILayout.FlexibleSpace();
                float t0 = AcousticHistory.Time[AcousticHistory.Index(0)];
                float t = AcousticHistory.Time[AcousticHistory.Index(_cursor)];
                GUILayout.Label("t = " + (t - t0).ToString("F2") + " s"
                                + "   （最新から " + (n - 1 - _cursor) + " フレーム前）", EditorStyles.miniLabel);
            }

            EditorGUILayout.Space(4f);
            _scroll = EditorGUILayout.BeginScrollView(_scroll);
            DrawFrameDetail(AcousticHistory.Index(_cursor));
            EditorGUILayout.EndScrollView();
        }

        private int FindMark(int from, int dir, int n)
        {
            for (int i = from + dir; i >= 0 && i < n; i += dir)
                if (AcousticHistory.Mark[AcousticHistory.Index(i)] != AcousticHistory.MarkNone) return i;
            return from;
        }

        /// <summary>出力レベル(dB)の推移。跳びと印は縦線で出す。</summary>
        private void DrawGraph(Rect r, int n)
        {
            EditorGUI.DrawRect(r, new Color(0.13f, 0.13f, 0.15f));

            const float dbMin = -80f, dbMax = 0f;
            // 目盛り（20dB ごと）
            for (float db = dbMin; db <= dbMax; db += 20f)
            {
                float y = r.y + r.height * (1f - (db - dbMin) / (dbMax - dbMin));
                EditorGUI.DrawRect(new Rect(r.x, y, r.width, 1f), new Color(1f, 1f, 1f, 0.07f));
                GUI.Label(new Rect(r.x + 2f, y - 7f, 40f, 14f), db.ToString("F0"), EditorStyles.miniLabel);
            }

            // 印（縦線）。先に描いて線の下に敷く。
            for (int i = 0; i < n; i++)
            {
                int k = AcousticHistory.Index(i);
                int m = AcousticHistory.Mark[k];
                if (m == AcousticHistory.MarkNone) continue;
                float x = r.x + r.width * (n <= 1 ? 0f : (float)i / (n - 1));
                Color c = (m == AcousticHistory.MarkJump)
                    ? new Color(1f, 0.35f, 0.25f, 0.75f) : new Color(0.3f, 0.85f, 1f, 0.75f);
                EditorGUI.DrawRect(new Rect(x, r.y, 1f, r.height), c);
            }

            if (Event.current.type == EventType.Repaint)
            {
                Handles.BeginGUI();
                Series(r, n, AcousticHistory.RmsTail, new Color(0.35f, 0.6f, 1f), dbMin, dbMax);
                Series(r, n, AcousticHistory.RmsEarly, new Color(1f, 0.85f, 0.3f), dbMin, dbMax);
                Series(r, n, AcousticHistory.RmsDirect, new Color(0.4f, 1f, 0.5f), dbMin, dbMax);
                Series(r, n, AcousticHistory.RmsOut, Color.white, dbMin, dbMax);
                Handles.EndGUI();
            }

            // いま見ているフレーム
            float cx = r.x + r.width * (n <= 1 ? 0f : (float)_cursor / (n - 1));
            EditorGUI.DrawRect(new Rect(cx, r.y, 1f, r.height), new Color(1f, 1f, 1f, 0.85f));

            GUI.Label(new Rect(r.xMax - 210f, r.y + 2f, 208f, 14f),
                      "出力 / 直接 / 早期 / 尾   (dB)", EditorStyles.miniLabel);
        }

        private void Series(Rect r, int n, float[] lin, Color c, float dbMin, float dbMax)
        {
            int w = Mathf.Max(2, Mathf.Min((int)r.width, n));
            if (_pts.Length != w) _pts = new Vector3[w];

            for (int p = 0; p < w; p++)
            {
                int i = (w <= 1) ? 0 : Mathf.RoundToInt((float)p / (w - 1) * (n - 1));
                float db = AcousticHistory.ToDb(lin[AcousticHistory.Index(i)]);
                float y = r.y + r.height * (1f - Mathf.InverseLerp(dbMin, dbMax, db));
                _pts[p] = new Vector3(r.x + r.width * ((w <= 1) ? 0f : (float)p / (w - 1)), y, 0f);
            }
            Handles.color = c;
            Handles.DrawAAPolyLine(1.6f, _pts);
        }

        private void DrawFrameDetail(int k)
        {
            int mark = AcousticHistory.Mark[k];
            if (mark == AcousticHistory.MarkJump)
                EditorGUILayout.HelpBox("★このフレームで跳びを検出しています。\n"
                    + "設計の第一制約は連続性なので、ここは原因を見る価値があります。", MessageType.Warning);

            EditorGUILayout.LabelField(
                "FPS " + AcousticHistory.Fps[k].ToString("F0")
                + "    音響計算 " + AcousticHistory.AcousticMs[k].ToString("F2") + " ms/f"
                + "    音源 " + AcousticHistory.SrcCount[k], EditorStyles.miniBoldLabel);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("出力の段別", EditorStyles.boldLabel);
            DbBar("出力", AcousticHistory.RmsOut[k]);
            DbBar("直接", AcousticHistory.RmsDirect[k]);
            DbBar("早期", AcousticHistory.RmsEarly[k]);
            DbBar("散乱", AcousticHistory.RmsScatter[k]);
            DbBar("尾", AcousticHistory.RmsTail[k]);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("部屋と残響", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("  RT60 " + AcousticHistory.RoomRt60[k].ToString("F2") + " s"
                + "    体積 " + AcousticHistory.RoomVolume[k].ToString("F0") + " m3"
                + "    wet " + AcousticHistory.Wet[k].ToString("F2")
                + "    mixing " + AcousticHistory.MixingMs[k].ToString("F0") + " ms", EditorStyles.miniLabel);
            EditorGUILayout.LabelField("  回折δ "
                + (AcousticHistory.DiffDelta[k] < 0f ? "（迂回路なし）"
                                                     : AcousticHistory.DiffDelta[k].ToString("F2") + " m")
                + "    直線透過 " + AcousticHistory.SourceLevel[k].ToString("F3")
                + "    タップ " + AcousticHistory.TapCount[k]
                + "（反射 " + AcousticHistory.ErActive[k] + " / 回折 " + AcousticHistory.DiffActive[k] + "）",
                EditorStyles.miniLabel);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("主音源の 6 帯域（125 / 250 / 500 / 1k / 2k / 4k Hz）", EditorStyles.boldLabel);
            int bb = k * AcousticHistory.NumBands;
            BandRow("透過", AcousticHistory.BandsTx, bb);
            BandRow("回折", AcousticHistory.BandsDf, bb);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("音源ごとの遮蔽（1 = 完全に遮られている）", EditorStyles.boldLabel);
            int ob = k * AcousticHistory.MaxSources;
            int ns = Mathf.Min(AcousticHistory.SrcCount[k], AcousticHistory.MaxSources);
            for (int i = 0; i < ns; i++)
            {
                string name = (AcousticHistory.SourceNames != null && i < AcousticHistory.SourceNames.Length)
                    ? AcousticHistory.SourceNames[i] : ("音源" + i);
                Bar01(name, AcousticHistory.Occ[ob + i],
                      Color.Lerp(new Color(0.35f, 0.9f, 0.45f), new Color(1f, 0.4f, 0.35f),
                                 AcousticHistory.Occ[ob + i]),
                      "生存 " + AcousticHistory.Surv[ob + i].ToString("F2"));
            }
        }

        private void DbBar(string label, float lin)
        {
            float db = AcousticHistory.ToDb(lin);
            Bar01(label, Mathf.InverseLerp(-80f, 0f, db), new Color(0.55f, 0.75f, 1f), db.ToString("F1") + " dB");
        }

        private void BandRow(string label, float[] arr, int baseIdx)
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("  " + label, GUILayout.Width(40f));
                for (int b = 0; b < AcousticHistory.NumBands; b++)
                {
                    float v = arr[baseIdx + b];
                    Rect r = GUILayoutUtility.GetRect(10f, 16f, GUILayout.ExpandWidth(true));
                    EditorGUI.DrawRect(r, new Color(0.16f, 0.16f, 0.18f));
                    EditorGUI.DrawRect(new Rect(r.x, r.yMax - r.height * Mathf.Clamp01(v),
                                                r.width - 2f, r.height * Mathf.Clamp01(v)),
                                       new Color(0.5f, 0.8f, 1f, 0.85f));
                    GUI.Label(r, " " + v.ToString("F2"), EditorStyles.miniLabel);
                }
            }
        }

        private void Bar01(string label, float v01, Color c, string right)
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("  " + label, GUILayout.Width(150f));
                Rect r = GUILayoutUtility.GetRect(10f, 14f, GUILayout.ExpandWidth(true));
                EditorGUI.DrawRect(r, new Color(0.16f, 0.16f, 0.18f));
                EditorGUI.DrawRect(new Rect(r.x, r.y, r.width * Mathf.Clamp01(v01), r.height), c);
                GUILayout.Label(right, EditorStyles.miniLabel, GUILayout.Width(78f));
            }
        }

        // =====================================================================
        // 予算タブ（道具10）
        // =====================================================================
        private void DrawBudget()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に audio thread の実費を表示します。", MessageType.Info);
                return;
            }

            int len, num;
            AudioSettings.GetDSPBufferSize(out len, out num);
            int sr = Scope.SampleRate > 0 ? Scope.SampleRate : AudioSettings.outputSampleRate;
            float periodMs = (sr > 0) ? (len * 1000f / sr) : 0f;

            EditorGUILayout.LabelField("DSP ブロック " + len + " サンプル × " + num
                + "    " + sr + " Hz    周期 " + periodMs.ToString("F2") + " ms", EditorStyles.miniBoldLabel);

            // 実測の合計。推定（0.292ms × 本数）ではない ──
            // コストは「遮蔽された音源数」で効くので、本数からの推定は実際と数倍ずれる。
            float total = 0f;
            int active = 0;
            int maxIdx = Mathf.Min(Scope.MeteredMax, Scope.MaxMeteredSources - 1);
            for (int i = 0; i <= maxIdx; i++)
            {
                if (Scope.BlockMs[i] <= 0f) continue;
                total += Scope.BlockMs[i];
                active++;
            }

            float use = (periodMs > 0f) ? total / periodMs : 0f;
            Color barCol = (use < 0.5f) ? new Color(0.35f, 0.9f, 0.45f)
                         : (use < 0.8f) ? new Color(1f, 0.85f, 0.3f)
                                        : new Color(1f, 0.4f, 0.35f);

            EditorGUILayout.Space(4f);
            EditorGUILayout.LabelField("audio thread の占有", EditorStyles.boldLabel);
            Bar01("実測 " + total.ToString("F3") + " ms/block", Mathf.Clamp01(use), barCol,
                  (use * 100f).ToString("F1") + " %");

            EditorGUILayout.Space(4f);
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("音源数の上限（実測）", GUILayout.Width(150f));
                _budget = EditorGUILayout.IntField(_budget, GUILayout.Width(48f));
                GUILayout.Label("本", GUILayout.Width(24f));
                GUILayout.FlexibleSpace();
            }
            int nSrc = (AcousticFlowSceneDemo.Status.SourceNames != null)
                ? AcousticFlowSceneDemo.Status.SourceNames.Length : 0;
            Bar01("いま鳴っている音源 " + nSrc + " / " + _budget,
                  (_budget > 0) ? Mathf.Clamp01((float)nSrc / _budget) : 0f,
                  (nSrc < _budget * 0.8f) ? new Color(0.35f, 0.9f, 0.45f) : new Color(1f, 0.4f, 0.35f),
                  active + " 本計測中");

            EditorGUILayout.Space(6f);
            EditorGUILayout.LabelField("音源ごとの実費", EditorStyles.boldLabel);
            float worst = 0.0001f;
            for (int i = 0; i <= maxIdx; i++) worst = Mathf.Max(worst, Scope.BlockMs[i]);
            for (int i = 0; i <= maxIdx; i++)
            {
                float ms = Scope.BlockMs[i];
                if (ms <= 0f) continue;
                string name = (AcousticFlowSceneDemo.Status.SourceNames != null
                               && i < AcousticFlowSceneDemo.Status.SourceNames.Length)
                    ? AcousticFlowSceneDemo.Status.SourceNames[i] : ("音源" + i);
                float occ = (AcousticFlowSceneDemo.Status.Occlusion != null
                             && i < AcousticFlowSceneDemo.Status.Occlusion.Length)
                    ? AcousticFlowSceneDemo.Status.Occlusion[i] : 0f;
                Bar01(name, ms / worst, new Color(0.55f, 0.75f, 1f),
                      ms.ToString("F3") + " ms  遮" + occ.ToString("F2"));
            }

            EditorGUILayout.Space(6f);
            EditorGUILayout.HelpBox(
                "上限 36 本は実測（1 スレッドが飽和するまで。SYSTEM_BRIEF §6）。\n"
                + "★合計は推定ではなく実測です。コストは「遮蔽された音源数」で効くので、\n"
                + "　本数 × 0.292ms の推定は実際と数倍ずれます。",
                MessageType.None);
        }

        // =====================================================================
        // 音源タブ（道具A ソロ／ミュート・寄与度順の一覧）
        // =====================================================================
        private enum SortBy { OutRms, PathGain }
        private static readonly string[] kSortNames = { "実出力", "経路ゲイン" };

        private SortBy _sortBy = SortBy.OutRms;
        private int _selSource;
        private Vector2 _srcScroll;
        private bool _drawPaths = true;
        private readonly int[] _order = new int[Solo.MaxSources];
        private readonly float[] _metric = new float[Solo.MaxSources];

        private void DrawSources()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox("再生中に、いま鳴っている音源を寄与度順に並べます。", MessageType.Info);
                return;
            }
            var taps = AcousticFlowSceneDemo.Status.Taps;
            string[] names = AcousticFlowSceneDemo.Status.SourceNames;
            int n = (names != null) ? names.Length : 0;
            if (n <= 0 || taps == null) { EditorGUILayout.HelpBox("音源がまだありません。", MessageType.Warning); return; }
            n = Mathf.Min(n, Solo.MaxSources);

            // --- 並べ替えの基準 ---
            // ★2 つの意味がある。混ぜると読めなくなるので明示的に切り替える。
            //   実出力     … 実際に耳へ届いている量。マスキングの切り分けはこちら
            //   経路ゲイン … 届きやすさ。音源が鳴っていなくても出る（経路の良し悪しを見る用）
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                GUILayout.Label("並べ替え", EditorStyles.miniLabel, GUILayout.Width(50f));
                _sortBy = (SortBy)GUILayout.Toolbar((int)_sortBy, kSortNames,
                                                    EditorStyles.toolbarButton, GUILayout.Width(140f));
                GUILayout.FlexibleSpace();
                _drawPaths = GUILayout.Toggle(_drawPaths, "Scene に経路", EditorStyles.toolbarButton, GUILayout.Width(84f));
                if (GUILayout.Button("すべて解除", EditorStyles.toolbarButton, GUILayout.Width(70f)))
                { Solo.Reset(); SceneView.RepaintAll(); }
            }

            // --- 経路単位のゲート ---
            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Label("経路", GUILayout.Width(34f));
                Solo.PassDirect = GUILayout.Toggle(Solo.PassDirect, "直接 D", "Button", GUILayout.Width(60f));
                Solo.PassReflect = GUILayout.Toggle(Solo.PassReflect, "反射 R", "Button", GUILayout.Width(60f));
                Solo.PassDiffract = GUILayout.Toggle(Solo.PassDiffract, "回折 F", "Button", GUILayout.Width(60f));
                Solo.PassTail = GUILayout.Toggle(Solo.PassTail, "尾", "Button", GUILayout.Width(44f));
                GUILayout.FlexibleSpace();
            }
            if (!Solo.IsDefault)
                EditorGUILayout.HelpBox("いま経路を切っています。窓を閉じれば自動で戻ります。", MessageType.Warning);

            // --- 寄与度を測って並べる ---
            float worst = 1e-6f;
            for (int i = 0; i < n; i++)
            {
                _order[i] = i;
                _metric[i] = (_sortBy == SortBy.OutRms) ? Scope.OutRms[i] : PathGain(taps, i);
                worst = Mathf.Max(worst, _metric[i]);
            }
            for (int a = 0; a < n - 1; a++)          // 音源は多くて数十本なので単純な選択ソートで足りる
                for (int b = a + 1; b < n; b++)
                    if (_metric[_order[b]] > _metric[_order[a]])
                    { int t = _order[a]; _order[a] = _order[b]; _order[b] = t; }

            EditorGUILayout.Space(4f);
            _srcScroll = EditorGUILayout.BeginScrollView(_srcScroll, GUILayout.Height(180f));
            for (int r = 0; r < n; r++)
            {
                int i = _order[r];
                bool sel = (i == _selSource);
                using (new EditorGUILayout.HorizontalScope(sel ? EditorStyles.helpBox : GUIStyle.none))
                {
                    bool solo = (Solo.Only == i);
                    bool newSolo = GUILayout.Toggle(solo, "S", "Button", GUILayout.Width(22f));
                    if (newSolo != solo) { Solo.Only = newSolo ? i : -1; SceneView.RepaintAll(); }

                    Solo.Mute[i] = GUILayout.Toggle(Solo.Mute[i], "M", "Button", GUILayout.Width(22f));

                    string nm = (names[i] != null) ? names[i] : ("音源" + i);
                    if (GUILayout.Button(nm, EditorStyles.label, GUILayout.Width(120f)))
                    { _selSource = i; SceneView.RepaintAll(); }

                    Rect br = GUILayoutUtility.GetRect(10f, 14f, GUILayout.ExpandWidth(true));
                    EditorGUI.DrawRect(br, new Color(0.16f, 0.16f, 0.18f));
                    float v = Mathf.Clamp01(_metric[i] / worst);
                    EditorGUI.DrawRect(new Rect(br.x, br.y, br.width * v, br.height),
                                       Solo.AllowsSource(i) ? new Color(0.55f, 0.75f, 1f)
                                                                        : new Color(0.4f, 0.4f, 0.45f));
                    GUILayout.Label(AcousticHistory.ToDb(_metric[i]).ToString("F1") + " dB",
                                    EditorStyles.miniLabel, GUILayout.Width(56f));
                    GUILayout.Label("×" + ((i < taps.Length && taps[i] != null) ? taps[i].Count : 0),
                                    EditorStyles.miniLabel, GUILayout.Width(32f));
                }
            }
            EditorGUILayout.EndScrollView();

            // --- 選んだ音源のタップ内訳 ---
            EditorGUILayout.Space(4f);
            _selSource = Mathf.Clamp(_selSource, 0, n - 1);
            var ts = (_selSource < taps.Length) ? taps[_selSource] : null;
            if (ts == null) { EditorGUILayout.HelpBox("この音源のタップがまだありません。", MessageType.Info); return; }

            EditorGUILayout.LabelField(
                ((names[_selSource] != null) ? names[_selSource] : ("音源" + _selSource))
                + " のタップ内訳（" + ts.Count + " 本）"
                + "   ITDG " + ts.ItdgMs.ToString("F1") + " ms", EditorStyles.boldLabel);

            _scroll = EditorGUILayout.BeginScrollView(_scroll);
            for (int i = 0; i < ts.Count && i < AcousticFlowSceneDemo.SourceTaps.MaxTaps; i++)
            {
                char ty = (ts.Type != null && i < ts.Type.Length) ? ts.Type[i] : 'D';
                bool passed = Solo.AllowsType(ty);
                using (new EditorGUILayout.HorizontalScope())
                {
                    var st = new GUIStyle(EditorStyles.miniLabel);
                    if (!passed) st.normal.textColor = new Color(0.5f, 0.5f, 0.55f);
                    GUILayout.Label(TypeLabel(ty), st, GUILayout.Width(52f));
                    GUILayout.Label(ts.DelayMs[i].ToString("F1") + " ms", st, GUILayout.Width(58f));
                    GUILayout.Label(AcousticHistory.ToDb(ts.Gain[i]).ToString("F1") + " dB", st, GUILayout.Width(58f));
                    for (int b = 0; b < AcousticHistory.NumBands; b++)
                    {
                        Rect r = GUILayoutUtility.GetRect(6f, 13f, GUILayout.ExpandWidth(true));
                        EditorGUI.DrawRect(r, new Color(0.16f, 0.16f, 0.18f));
                        int o = i * AcousticHistory.NumBands + b;
                        float g = (ts.BandGain != null && o < ts.BandGain.Length) ? Mathf.Clamp01(ts.BandGain[o]) : 0f;
                        EditorGUI.DrawRect(new Rect(r.x, r.yMax - r.height * g, r.width - 2f, r.height * g),
                                           passed ? new Color(0.5f, 0.8f, 1f, 0.85f) : new Color(0.45f, 0.45f, 0.5f, 0.6f));
                    }
                }
            }
            EditorGUILayout.EndScrollView();
        }

        private static string TypeLabel(char t)
        {
            if (t == 'R') return "反射 R";
            if (t == 'F') return "回折 F";
            return "直接 D";
        }

        private static float PathGain(AcousticFlowSceneDemo.SourceTaps[] taps, int i)
        {
            if (i >= taps.Length || taps[i] == null) return 0f;
            var ts = taps[i];
            float e = 0f;
            for (int k = 0; k < ts.Count && k < ts.Gain.Length; k++) e += ts.Gain[k] * ts.Gain[k];
            return Mathf.Sqrt(e);
        }

        /// <summary>選んだ音源の経路を Scene ビューへ描く（種別で色分け）。実行時コードは触らない。</summary>
        private void OnSceneGui(SceneView sv)
        {
            if (_tab != Tab.Sources || !_drawPaths || !EditorApplication.isPlaying) return;
            var taps = AcousticFlowSceneDemo.Status.Taps;
            if (taps == null || _selSource >= taps.Length || taps[_selSource] == null) return;

            var demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo == null || demo.listener == null) return;
            Vector3 ear = demo.listener.position;

            var ts = taps[_selSource];
            for (int i = 0; i < ts.Count && i < AcousticFlowSceneDemo.SourceTaps.MaxTaps; i++)
            {
                char ty = (ts.Type != null && i < ts.Type.Length) ? ts.Type[i] : 'D';
                if (!Solo.AllowsType(ty)) continue;
                Vector3 p = (ts.Arrival != null && i < ts.Arrival.Length) ? ts.Arrival[i] : ear;

                // 直接=緑 / 反射=黄 / 回折=水色。強いタップほど濃く太く。
                float a = Mathf.Clamp01(Mathf.InverseLerp(-60f, 0f, AcousticHistory.ToDb(ts.Gain[i])));
                Handles.color = (ty == 'R') ? new Color(1f, 0.85f, 0.3f, 0.25f + 0.75f * a)
                              : (ty == 'F') ? new Color(0.2f, 0.9f, 1f, 0.25f + 0.75f * a)
                                            : new Color(0.4f, 1f, 0.5f, 0.35f + 0.65f * a);
                Handles.DrawAAPolyLine(1f + 3f * a, p, ear);
                Handles.SphereHandleCap(0, p, Quaternion.identity, 0.10f + 0.25f * a, EventType.Repaint);
            }
        }
    }
}
