// WorldPortalStencil.cs
// 扉の開口を**ステンシルで切り抜き**、そこにだけ行き先の絵を貼る。Built-in RP。
//
// ★印の付け方は 1 つの規則だけ。
//
//     ある画素が向こう側か ＝ **目からその画素へ引いた線分が、扉の矩形を横切っているか**
//
//   これは「カメラと扉の面の関係」そのもので、連続している。
//     ・扉から離れている  → 開口の形に切り抜かれる
//     ・面の直前で前を向く → ほぼ全画面が向こう
//     ・面の直後で前を向く → 線分が矩形を横切らないので全画面がこちら
//     ・面に重なって真横  → 画面が扉の面で左右に割れる
//   途中に段が無い。場合分けも閾値も無い。
//
// ★ここへ来るまでに 4 つ試して、後ろの 3 つは同じ穴で破れた（記録として残す）：
//   ① 板を世界に置いて RT を貼る
//        → RT が 0.75 倍で寄ると解像度が変わる／板が near に切られて穴が開く／
//          別カメラなので投影と斜めクリップの符号がずれる／板の外へ漏れうる
//   ② 置換シェーダで行き先だけステンシル判定して描く
//        → **別のレンダラを書くのと同じ**。影も空も霧も陰影も書いた分しか出ない
//   ③ 開口の矩形を描いて印にする
//        → 目が矩形に入ると near に潰れ、印がほとんど付かない
//   ④ 戸口の中では画面全体を印にする（③の穴埋め）／開口を厚みのある箱にする
//        → 跨いだ後も全画面が続き、**出てきたばかりの世界が全画面に出る**。
//          範囲を詰めても段は残る（発注者の「連続した切り替えが一番の違和感」）
//
//   ③④ の根は「**矩形を描いて印にする**」こと。目が矩形に入ると必ず破れる。
//   線分と矩形の交差を直に見れば、その前提ごと消える。
//
// ★この形にして消えたもの
//   ・戸口の中では全画面にする場合分け（`doorwayDepth`）
//   ・開口を厚みのある箱にする案
//   ・扉に寄ったら near を縮める処理
//   ・扉板のところだけ印を消す処理
//     （奥へ開いた板の画素は、線分が面を横切る前に板に当たるので自動的に外れる）
//   規則が 1 つになると、例外も一緒に消える。
//
// ★行き先は「普通のカメラで普通に描いた絵」。
//   その世界の本来のシェーダ・影・空・霧で描かれる。こちらは**どこに貼るか**だけ決める。
//   RT は画面と同じ大きさ（縮めると寄ったときに解像度が変わって見える）。
//
// ⚠ 本編カメラに深度テクスチャが要る（前方向レンダリングで深度パスが 1 枚増える）。
//   線分がどこまで届くかを知らないと「手前に何か写っている」を判定できないため。
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace BellGame
{
    // ★**いちばん最後に走る**。
    //
    //   1 フレームの中の順番：
    //     デモ Update         リスナーを動かす
    //     WorldBounds  (0)    壁から押し出す
    //     デモ LateUpdate (0) カメラ = リスナー ＋ eyeHeight  ← カメラが決まるのはここ
    //     WorldTransition(50) カメラの位置で跨ぎを判定する
    //     ★ここ      (100)   そのカメラで向こうを描き、開口に貼る
    //
    //   既定（0）のままだとデモと同じ順番になり、**どちらが先かを Unity は保証しない**。
    //   先に走った日は 1 フレーム前のカメラ位置で描くことになり、
    //   「たまに」絵が遅れる。カメラを使う物は、カメラが決まった後に走らせる。
    [DefaultExecutionOrder(100)]
    [DisallowMultipleComponent]
    public sealed class WorldPortalStencil : MonoBehaviour
    {
        // ★RT の大きさ。既定は画面と同じ（＝これまでと 1 ミリも変わりません）。
        //
        //   ここは**この方式でいちばん高い部分**です。開口が見えている間、
        //   行き先の世界を画面と同じ解像度・HDR・MSAA でもう一度描いています
        //   ── 実質、描画が 2 倍。
        //
        //   縮めると、寄ったときに粗さが見えます。**どこまで粗くしてよいかは
        //   目で決めること**（実測は参照であって目標ではない）。
        //   1.0 に戻せばいつでも元通りです。
        [Header("負荷")]
        [Range(0.25f, 1f)] public float resolutionScale = 1f;

        // ★MSAA を切る。開口ごしの絵は縁が扉の枠で隠れるので、
        //   画面本体ほど効きません。切ると帯域がまるごと浮きます。
        public bool portalMsaa = true;

        [Header("配線")]
        [Tooltip("この扉の行き先を追う。")]
        public WorldDoor worldDoor;
        [Tooltip("世界を抱えている係。空なら実行時に探す。")]
        public WorldSet worlds;
        [Tooltip("本編のカメラ。空なら Camera.main。")]
        public Camera playerCamera;

        [Header("開口（MakeDoorFrame と揃える）")]
        // ★開口の寸法。**扉の AcousticPortal が持っている値を唯一の答えにする。**
        //
        //   ここは既定の 1.1×2.2（ラボの箱の寸法）のまま置かれていて、誰も設定して
        //   いなかった。本番の扉は 1.4×3.0 なので、**絵の切り抜きだけが小さい**
        //   という状態になっていた（音響側は AcousticPortal の値で正しく計算していた）。
        //
        //   寸法を持つ場所が増えるほど、片方だけ古くなる。§4.3 が見ている開口と
        //   切り抜く矩形は**同じ物**でなければならないので、そちらから貰う。
        [Tooltip("扉の AcousticPortal から自動で貰う。見つからないときだけ下の値を使う。")]
        public bool sizeFromAcousticPortal = true;
        public float apertureWidth = 1.1f;
        public float apertureHeight = 2.2f;

        [Header("描く条件")]
        [Tooltip("この角度以上開いていないと描かない。")]
        [Range(0f, 60f)] public float minAngleDeg = 3f;
        [Tooltip("扉からこれ以上離れたら描かない（常時 1 枚ぶん余計に描くコストの歯止め）。")]
        [Range(4f, 120f)] public float drawDistance = 40f;

        // ★「開口の外に向こうが見える」は、原因が 2 つに割れる。
        //     A) 印（ステンシル）が開口からはみ出している
        //     B) 印は正しいのに、貼っている絵が違う
        //   印をべた塗りにすれば、この 2 つが一発で分かれる。
        [Header("★切り分け用")]
        [Tooltip("印の中をマゼンタで塗る。開口ちょうどに出れば印は正しい（9 キー）。")]
        public bool debugSolidMask;
        [Tooltip("画面左上にポータルカメラの生の絵と数字を出す（8 キー）。")]
        public bool debugShowRawTexture;

        // ★1 回で決着させるための切り分け（F3）。
        //
        //   深度を一切読まないと、印は「扉の開口の輪郭」そのものになるはず。
        //   どこを向いても開口に貼り付いていなければならない。
        //
        //     ・これで合う   → 視線の組み立ては正しい。原因は**深度側**
        //     ・これでもずれる → 原因は**視線側**（基底か画角）
        [Tooltip("深度を読まずに印を付ける。開口の輪郭そのものが出るはず（F3）。")]
        public bool debugIgnoreDepth;

        [Header("診断（読み取り専用）")]
        public string status = "—";
        public bool drawing;
        public WorldId showing = WorldId.None;
        public float lastAngle;

        private Camera _cutCam;
        private Skybox _cutSky;
        private CommandBuffer _cb;
        private Camera _cbOwner;
        private Material _mat;
        private RenderTexture _rt;
        private int _rtW, _rtH;
        private Material _destSky;
        private WorldId _skyOf = WorldId.None;
        private WorldId _requested = WorldId.None;

        // 開口の基準は**扉そのもの**。生成時に扉と同じ姿勢で置いた別の物ではない。
        //   別の物にしていると、片方だけ動いた瞬間に食い違う。
        private Transform Aperture =>
            (worlds != null && worlds.RigDoor != null) ? worlds.RigDoor : transform;

        /// 開口の寸法を、扉の AcousticPortal から貰う。
        ///   §4.3 が見ている開口と、切り抜く矩形を**同じ物**にしておくため。
        private void AdoptApertureSize()
        {
            if (!sizeFromAcousticPortal || worldDoor == null) return;

            // 扉の下（枠ごと）を探す。WorldDoor は枠の子なので、親から辿るのが確実。
            var from = (worldDoor.transform.parent != null)
                     ? worldDoor.transform.parent : worldDoor.transform;
            var portal = from.GetComponentInChildren<AcousticFlow.AcousticPortal>(true);
            if (portal == null || portal.width <= 0.01f || portal.height <= 0.01f) return;

            if (Mathf.Abs(portal.width - apertureWidth) > 0.01f
                || Mathf.Abs(portal.height - apertureHeight) > 0.01f)
                Debug.Log($"[BellGame] 切り抜きの開口を扉から貰った: "
                          + $"{apertureWidth:F2}x{apertureHeight:F2} → "
                          + $"{portal.width:F2}x{portal.height:F2}", this);

            apertureWidth = portal.width;
            apertureHeight = portal.height;
        }

        private void Start()
        {
            if (worlds == null) worlds = WorldSet.Current;
            if (playerCamera == null) playerCamera = Camera.main;
            AdoptApertureSize();

            var sh = Shader.Find("BellGame/PortalStencil");
            if (sh == null)
            {
                Debug.LogError("[BellGame] シェーダ 'BellGame/PortalStencil' が見つからない。"
                    + "Assets/Shaders/PortalStencil.shader が入っているか確認してほしい。", this);
                enabled = false;
                return;
            }
            _mat = new Material(sh) { name = "PortalStencil", hideFlags = HideFlags.DontSave };

            BuildCutCamera();
            AttachBuffer();

            if (playerCamera != null)
            {
                // ★深度は**描くときだけ**要求する（LateUpdate で付け外しする）。
                //   常時 ON にしていたが、深度パスはシーン 1 枚ぶんの描画で、
                //   扉が画面に無い間はまるごと無駄だった。

                // ★本編カメラは行き先のレイヤーを描かない。描くのはポータルカメラだけ。
                //   ここが抜けていると、行き先が扉の外にそのまま見える。
                if (worlds != null && worlds.PreviewLayer > 0)
                {
                    int bit = 1 << worlds.PreviewLayer;
                    if ((playerCamera.cullingMask & bit) != 0)
                    {
                        playerCamera.cullingMask &= ~bit;
                        Debug.Log($"[BellGame] 本編カメラ [{playerCamera.name}] から "
                                  + $"layer {worlds.PreviewLayer} を外した", this);
                    }
                }
            }
        }

        private void BuildCutCamera()
        {
            var go = new GameObject("PortalCamera") { hideFlags = HideFlags.DontSave };
            go.transform.SetParent(transform, false);

            _cutCam = go.AddComponent<Camera>();
            _cutCam.enabled = false;                 // 自分で Render() する（RT へ描くので順番は自由）
            _cutCam.clearFlags = CameraClearFlags.Skybox;
            _cutCam.cullingMask = (worlds != null && worlds.PreviewLayer > 0)
                                ? (1 << worlds.PreviewLayer) : 0;
            _cutCam.useOcclusionCulling = false;

            // ★カメラ単位の空。行き先の空をここに挿す。
            //   RenderSettings.skybox も差し替えるので実際には保険だが、
            //   両方合わせておかないと、差し替えの隙間で現世の空が 1 フレーム出る。
            _cutSky = go.AddComponent<Skybox>();
        }

        // 合成は**本編カメラの上**で走らせる。行き先を描くのとは別の話。
        private void AttachBuffer()
        {
            if (playerCamera == null) return;
            _cb = new CommandBuffer { name = "BellGame/PortalCutout" };
            playerCamera.AddCommandBuffer(CameraEvent.AfterForwardAlpha, _cb);
            _cbOwner = playerCamera;
        }

        private void DetachBuffer()
        {
            if (_cbOwner != null && _cb != null)
                _cbOwner.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha, _cb);
            _cbOwner = null;
        }

        private void OnDisable()
        {
            drawing = false;
            if (_cb != null) _cb.Clear();
        }

        private void OnDestroy()
        {
            DetachBuffer();
            if (_cb != null) _cb.Release();
            if (_mat != null) Destroy(_mat);
            if (_rt != null) { _rt.Release(); Destroy(_rt); }
            if (_destSky != null) Destroy(_destSky);
        }

        // 崩れている絵を**見ながら**切り替えられるようにする。
        //   Inspector だと再生を止めて同じ場所まで歩き直すことになり、
        //   崩れる位置は再現しにくいので 1 つ試すのに毎回やり直しになる。
        private void ReadDebugKeys()
        {
            if (Input.GetKeyDown(KeyCode.Alpha9))
            {
                debugSolidMask = !debugSolidMask;
                Debug.Log($"[BellGame] 印をべた塗り → {(debugSolidMask ? "する" : "しない")}", this);
            }
            if (Input.GetKeyDown(KeyCode.Alpha8))
            {
                debugShowRawTexture = !debugShowRawTexture;
                Debug.Log($"[BellGame] ポータルの生の絵 → {(debugShowRawTexture ? "表示" : "非表示")}", this);
            }
            if (Input.GetKeyDown(KeyCode.F3))
            {
                debugIgnoreDepth = !debugIgnoreDepth;
                Debug.Log("[BellGame] 切り分け: 深度を読まない → "
                    + (debugIgnoreDepth ? "はい（印は開口の輪郭そのものになるはず）" : "いいえ"), this);
            }
        }

        private void LateUpdate()
        {
            ReadDebugKeys();

            if (worlds == null) worlds = WorldSet.Current;
            if (playerCamera == null) playerCamera = Camera.main;
            if (_cbOwner != playerCamera) { DetachBuffer(); AttachBuffer(); }

            // 行き先を先に読み込んでおく（眠らせた状態で）。
            var dest = (worldDoor != null) ? worldDoor.destination : WorldId.None;
            if (worlds != null && dest != WorldId.None && dest != _requested && !worlds.busy)
            {
                _requested = dest;
                worlds.StartCoroutine(worlds.Ensure(dest));
            }

            // ★行き先を扉の向こう側へ置く。**動かしてよいのは眠っている世界だけ。**
            if (worlds != null && dest != WorldId.None && dest != worlds.active
                && worlds.RigDoor != null)
                worlds.PlaceWorld(worlds.Get(dest), flipped: worlds.PreviewFlip);

            drawing = ShouldDraw();

            // ★深度パスは描くときだけ。扉が画面に無い間はシーン 1 枚ぶんまるごと無駄。
            if (playerCamera != null)
            {
                if (drawing) playerCamera.depthTextureMode |= DepthTextureMode.Depth;
                else playerCamera.depthTextureMode &= ~DepthTextureMode.Depth;
            }

            if (!drawing) { if (_cb != null) _cb.Clear(); return; }

            showing = worldDoor.destination;
            RenderDestination();
            BuildCompositeBuffer();
        }

        private bool ShouldDraw()
        {
            if (_cutCam == null || _mat == null) { status = "部品が組み上がっていない"; return false; }
            if (playerCamera == null) { status = "プレイヤーのカメラが見つからない（Camera.main）"; return false; }
            if (worldDoor == null) { status = "扉が繋がっていない（worldDoor が空）"; return false; }
            if (worlds == null) { status = "WorldSet が繋がっていない"; return false; }
            if (worlds.PreviewLayer <= 0) { status = "プレビュー用レイヤーが無い（セットアップ 1）"; return false; }

            if (worldDoor.destination == WorldId.None)
            { status = "扉の行き先が未設定 ── ベルを拾い、閉じた扉の前で Space"; return false; }

            if (!worlds.IsLoaded(worldDoor.destination))
            { status = $"{BellVoices.DisplayName(worldDoor.destination)}を読み込み中"; return false; }

            lastAngle = (worldDoor.door != null) ? worldDoor.door.AngleDeg : 90f;
            if (lastAngle < minAngleDeg) { status = "扉が閉じている（開ければ見える）"; return false; }

            float d = Vector3.Distance(playerCamera.transform.position, Aperture.position);
            if (d > drawDistance) { status = $"扉から遠い（{d:F1}m > {drawDistance}m）"; return false; }

            // ★開口が画面に無いなら、行き先を描かない。
            //
            //   ここが抜けていた。**行き先の世界を丸ごともう一度描く**のがこの方式の
            //   いちばん高い部分なのに、扉が背後にあっても画面外にあっても払っていた。
            //   本番の草原（46×56m・草・葉）では、これがシーン 1 枚ぶんまるごと。
            if (!ApertureOnScreen()) { status = "開口が画面に無い（描かない）"; return false; }

            status = "切り抜き中";
            return true;
        }

        // 開口の矩形が画面に掛かっているか。
        //   ★「全部が目より後ろ」なら確実に描かなくてよい ── 前向きの線分は矩形を横切れない。
        //   前に在るときだけ画面の範囲で判定する。前後が混ざるときは**描く側に倒す**
        //  （目が戸口に入っている場合で、ここを切ると穴が開く）。
        private bool ApertureOnScreen()
        {
            var a = Aperture;
            float hw = apertureWidth * 0.5f, hh = apertureHeight * 0.5f;
            var cam = playerCamera.transform;

            Vector2 min = new Vector2(float.MaxValue, float.MaxValue);
            Vector2 max = new Vector2(float.MinValue, float.MinValue);
            int behind = 0;

            for (int i = 0; i < 4; i++)
            {
                Vector3 p = a.position
                          + a.right * ((i & 1) == 0 ? -hw : hw)
                          + a.up * ((i & 2) == 0 ? -hh : hh);

                if (Vector3.Dot(p - cam.position, cam.forward) <= 0f) { behind++; continue; }

                var v = playerCamera.WorldToViewportPoint(p);
                min = Vector2.Min(min, v); max = Vector2.Max(max, v);
            }

            if (behind == 4) return false;   // 全部が背後。描く必要が無い
            if (behind > 0) return true;     // 前後が混ざる＝戸口の中。切ると穴が開く

            return max.x >= 0f && min.x <= 1f && max.y >= 0f && min.y <= 1f;
        }

        // ★行き先を、その世界の**本来の描かれ方**で RT へ描く。
        //   視点は本編カメラそのもの。行き先は既に扉の裏側の正しい場所に在るので、
        //   写像も別投影も要らない ── ずれる余地が無い。
        private void RenderDestination()
        {
            EnsureTarget();

            var t = playerCamera.transform;
            _cutCam.transform.SetPositionAndRotation(t.position, t.rotation);
            _cutCam.fieldOfView = playerCamera.fieldOfView;
            _cutCam.aspect = playerCamera.aspect;
            _cutCam.nearClipPlane = playerCamera.nearClipPlane;
            _cutCam.farClipPlane = playerCamera.farClipPlane;
            _cutCam.cullingMask = 1 << worlds.PreviewLayer;

            // ★空気を向こうのものへ差し替えて描き、**必ず戻す**。
            //   Built-in RP の空・霧・環境光はグローバルなので、こうするしかない。
            //   Render() は同期呼び出しなので、この挟み方で成立する。
            var here = Snapshot();
            try
            {
                if (_destSky == null || showing != _skyOf)
                {
                    if (_destSky != null) Destroy(_destSky);
                    _destSky = WorldSky.MakeSkybox(showing);
                    _skyOf = showing;
                    _cutSky.material = _destSky;
                }
                WorldSky.Apply(showing, _destSky);

                // ★太陽も差し替える。Skybox/Procedural の太陽は**マテリアルではなく
                //   シーンの光**（RenderSettings.sun）から来るので、指さないと
                //   現世の太陽で向こうの空が焼かれる。
                var sun = worlds.SunOf(showing);
                if (sun != null) RenderSettings.sun = sun;

                // ★行き先の世界の光を、**描くあいだだけ**点ける。
                //
                //   WorldSet は眠っている世界の光を消しています（影付きの平行光を
                //   世界の数だけ持つと、その数だけシャドウマップを描くため）。
                //   要るのはここだけなので、ここで点けて finally で必ず戻します。
                LightsForRender(true);

                _cutCam.targetTexture = _rt;
                _cutCam.Render();
            }
            finally
            {
                LightsForRender(false);
                _cutCam.targetTexture = null;
                Restore(here);
            }
        }

        // 描くあいだだけ点ける光の一覧。付け外しは必ず対で。
        private readonly List<Light> _lit = new List<Light>();

        private void LightsForRender(bool on)
        {
            if (on)
            {
                _lit.Clear();
                var w = (worlds != null) ? worlds.Get(showing) : null;
                if (w == null) return;
                foreach (var l in w.lights)
                {
                    if (l == null || l.enabled) continue;   // 既に点いている物は触らない
                    l.enabled = true;
                    _lit.Add(l);
                }
                return;
            }

            for (int i = 0; i < _lit.Count; i++)
                if (_lit[i] != null) _lit[i].enabled = false;
            _lit.Clear();
        }

        private void BuildCompositeBuffer()
        {
            ApplyDoorParams();
            ApplyCameraBasis();

            _mat.SetTexture("_PortalTex", _rt);
            _mat.SetFloat("_SolidMask", debugSolidMask ? 1f : 0f);
            _mat.SetFloat("_IgnoreDepth", debugIgnoreDepth ? 1f : 0f);

            _cb.Clear();

            // 1. 線分が扉の矩形を横切る画素に印を付ける
            _cb.DrawProcedural(Matrix4x4.identity, _mat, 0, MeshTopology.Triangles, 3);

            // 2. 扉板のところだけ印を消す。
            //    ★奥へ開いた板は、線分が**面を横切った後**に当たるので、
            //      線分の規則では守れない。ここで明示的に外す。
            //      （一度「規則から自動的に外れる」と考えて消し、板が消えた。）
            foreach (var r in DoorRenderers())
                if (r != null && r.enabled) _cb.DrawRenderer(r, _mat, 0, 1);

            // 3. 印の中に行き先の絵を貼る
            _cb.DrawProcedural(Matrix4x4.identity, _mat, 2, MeshTopology.Triangles, 3);
        }

        // 扉板の描画物。板は回るので、そのときの姿勢で拾う。
        //   ★拾うのは**板だけ**。枠や柱は開口の外なので外す必要が無い
        //     （消すと、枠ごしに向こうが見えるべきところまで塗られなくなる）。
        private Renderer[] _doorRenderers;
        private PhysicsDoor _doorOf;

        private Renderer[] DoorRenderers()
        {
            var d = (worldDoor != null) ? worldDoor.door : null;
            if (d == null) return System.Array.Empty<Renderer>();
            if (_doorRenderers == null || _doorOf != d)
            {
                _doorOf = d;
                _doorRenderers = d.GetComponentsInChildren<Renderer>(true);
            }
            return _doorRenderers;
        }

        // 扉の矩形を世界座標で渡す。位置・法線・横・縦・半分の寸法。
        private void ApplyDoorParams()
        {
            var a = Aperture;
            _mat.SetVector("_DoorPos", a.position);
            _mat.SetVector("_DoorNormal", a.forward);
            _mat.SetVector("_DoorRight", a.right);
            _mat.SetVector("_DoorUp", a.up);
            _mat.SetVector("_DoorHalf",
                new Vector4(apertureWidth * 0.5f, apertureHeight * 0.5f, 0f, 0f));
        }

        // ★視線を組み立てるための基底。逆投影行列から戻すより取り違えが起きない
        //   （反転Z・OpenGL・視空間の −Z 前方で符号を間違えやすい）。
        private void ApplyCameraBasis()
        {
            var t = playerCamera.transform;
            float tanY = Mathf.Tan(playerCamera.fieldOfView * 0.5f * Mathf.Deg2Rad);
            _mat.SetVector("_CamRight", t.right);
            _mat.SetVector("_CamUp", t.up);
            _mat.SetVector("_CamFwd", t.forward);
            _mat.SetVector("_TanHalf", new Vector4(tanY * playerCamera.aspect, tanY, 0f, 0f));
        }

        // ★RT は画面と同じ大きさ。縮めると、寄ったときに解像度が変わって見える。
        private void EnsureTarget()
        {
            float s = Mathf.Clamp(resolutionScale, 0.25f, 1f);
            int w = Mathf.Max(16, Mathf.RoundToInt(Screen.width  * s));
            int h = Mathf.Max(16, Mathf.RoundToInt(Screen.height * s));
            int aa = (portalMsaa && QualitySettings.antiAliasing > 0) ? QualitySettings.antiAliasing : 1;

            if (_rt != null && _rtW == w && _rtH == h && _rt.antiAliasing == aa) return;
            if (_rt != null) { _rt.Release(); Destroy(_rt); }
            _rt = new RenderTexture(w, h, 24, RenderTextureFormat.DefaultHDR)
            {
                name = "PortalRT",
                antiAliasing = aa,
            };
            _rt.Create();
            _rtW = w; _rtH = h;
        }

        private void OnGUI()
        {
            if (!debugShowRawTexture) return;

            if (_rt != null)
                GUI.DrawTexture(new Rect(8, 8, 320, 180), _rt, ScaleMode.ScaleToFit, false);

            var a = Aperture;
            var cam = (playerCamera != null) ? playerCamera.transform : null;
            Vector3 local = (cam != null) ? a.InverseTransformPoint(cam.position) : Vector3.zero;

            var sb = new System.Text.StringBuilder();
            sb.AppendLine($"ポータル: {status}");
            sb.AppendLine($"  印の基準 = {a.name} / 世界座標 {a.position}");
            sb.AppendLine($"  扉ローカル = ({local.x:F2}, {local.y:F2}, {local.z:F2})  ← z が奥行き");
            sb.AppendLine($"  開口 = {apertureWidth} x {apertureHeight}"
                          + $" / 立っている側 = {(worlds != null ? worlds.PlayerSide : 0)}");
            sb.AppendLine("  印の付け方 = 線分と矩形の交差（場合分け無し）");

            GUI.Label(new Rect(8, 195, 900, 140), sb.ToString());
        }

        // ── グローバルな描画設定の退避と復帰 ───────────────────────
        private struct Env
        {
            public Material skybox;
            public AmbientMode ambientMode;
            public Color ambSky, ambEq, ambGround;
            public bool fog;
            public FogMode fogMode;
            public Light sun;
            public Color fogColor;
            public float fogDensity;
        }

        private static Env Snapshot() => new Env
        {
            skybox = RenderSettings.skybox,
            ambientMode = RenderSettings.ambientMode,
            ambSky = RenderSettings.ambientSkyColor,
            ambEq = RenderSettings.ambientEquatorColor,
            ambGround = RenderSettings.ambientGroundColor,
            fog = RenderSettings.fog,
            fogMode = RenderSettings.fogMode,
            sun = RenderSettings.sun,
            fogColor = RenderSettings.fogColor,
            fogDensity = RenderSettings.fogDensity,
        };

        private static void Restore(Env e)
        {
            RenderSettings.skybox = e.skybox;
            RenderSettings.ambientMode = e.ambientMode;
            RenderSettings.ambientSkyColor = e.ambSky;
            RenderSettings.ambientEquatorColor = e.ambEq;
            RenderSettings.ambientGroundColor = e.ambGround;
            RenderSettings.fog = e.fog;
            RenderSettings.fogMode = e.fogMode;
            RenderSettings.sun = e.sun;
            RenderSettings.fogColor = e.fogColor;
            RenderSettings.fogDensity = e.fogDensity;
        }
    }
}
