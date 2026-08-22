// BellCallResponse.cs
// 草案書 §3「呼びかけて、返事を聴く」と §4.5「応答遅延を RT60 から導出する」の実装。
//
//   1. プレイヤーがベルを鳴らす（既定 E キー）
//   2. RT60/3 + baseDelay だけ置いて、世界のベルが応答する
//   3. 応答の音色・遅延・定位・残響から方向と距離を推測する
//
// ★サウンドシステムには一切触らない。
//   このコンポーネントがエンジンから読むのは AcousticFlowSceneDemo.Status.RoomRt60 だけで、
//   それは既に public として公開されている値（部屋ごとの Sabine を占有割合で混ぜた RT60）。
//   鳴らす経路も既存のまま ── AudioSource の出力を VoiceConvolver が畳み込む。
//
// ★ベルの音は資産を持たずにその場で合成する。理由は 2 つ:
//   ・デモの音源が市販楽曲で、配布できない（仕様書 §7.6）
//   ・ベルは草案 §2 が言うとおり理想的なテスト信号で、合成なら
//     「鋭いアタック（先行音効果で直接音の定位が立つ）」と
//     「長い減衰（部屋の尾がそのまま聞こえる）」を狙って作れる
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class BellCallResponse : MonoBehaviour
    {
        [Header("配線")]
        [Tooltip("プレイヤー（リスナー）の Transform。到達判定に使う。")]
        public Transform listener;
        [Tooltip("手持ちのベルの AudioSource。VoiceConvolver が載っている音源オブジェクト。")]
        public AudioSource playerBell;
        [Tooltip("世界のベルの AudioSource。同上。")]
        public AudioSource worldBell;

        [Header("操作")]
        // ★Space はデモ側と衝突する。
        //   AcousticFlowSceneDemo が Space を「音源を1点に重ねるトグル」に使っているので、
        //   押すと世界のベルが元配置の重心へ飛ぶ。対策は 2 つ入れてある:
        //     1) 世界のベルに PinPosition を付けて LateUpdate で位置を書き戻す
        //     2) ベルを鳴らすのを 1 フレーム遅らせる（デモが動かした位置がエンジンへ
        //        渡ってしまう 1 フレームを、音が出る前にやり過ごす）
        //   気になるなら E など空いているキーに変えてよい。
        public KeyCode callKey = KeyCode.Space;
        [Tooltip("連打を防ぐ。0 で無制限（草案 §9 の未決定事項なので、まずは緩めに置く）。")]
        [Range(0f, 3f)] public float cooldown = 0.6f;

        [Header("応答遅延（距離で決まる）")]
        // 返しは常に **1 回**。遅延だけが距離で変わる。
        //   遠い（farDistance 以遠）→ farDelay（数秒）。呼びかけと返事がはっきり分かれる
        //   近い（nearDistance 以内）→ nearDelay（0.1s 程度）。ほぼ即座に返る
        //
        // ⚠ 決めごと #1（同じ問いに 2 つの答えを持たせない）には触れている。
        //   距離は物理側も答えている（直接音のレベル・早期反射の密度・残響/直接比）ので、
        //   遅延はその 2 つ目の答えになる。承知のうえでこう決めた、という位置づけ。
        //   物理側だけに戻したいときは farDelay と nearDelay を同じ値にすればよい。
        [Tooltip("この距離より遠ければ farDelay で頭打ち。")]
        [Range(5f, 60f)] public float farDistance = 30f;
        [Tooltip("この距離より近ければ nearDelay で頭打ち。")]
        [Range(0.5f, 15f)] public float nearDistance = 3f;
        [Tooltip("遠いときの応答遅延(秒)。")]
        [Range(0.3f, 6f)] public float farDelay = 3.0f;
        [Tooltip("近いときの応答遅延(秒)。")]
        [Range(0f, 1f)] public float nearDelay = 0.1f;
        [Tooltip("距離→遅延の曲がり。1=線形 / 1未満=遠側で詰まる / 1超=近側で詰まる。")]
        [Range(0.3f, 3f)] public float delayCurve = 1.0f;
        [Tooltip("RT60/3 をどれだけ上乗せするか。0=距離だけ（既定）。\n"
                 + "1 にすると草案 §4.5 どおり『自分の残響が終わってから返る』が混ざる。")]
        [Range(0f, 1f)] public float rt60Weight = 0f;

        // ★全部を物理に任せる（発注者の決定）。既定 ON。
        //
        //   ON  … 鳴らしたら世界のベルが**即座に**鳴る。距離・方向・空間・遅れは
        //         すべてエンジンが解いた経路が語ります。上の遅延の設定は効きません。
        //   OFF … 従来どおり、距離と RT60 から応答遅延を作ります（A/B 比較用）。
        //
        // ⚠ 置き換えではなく**切り替え**にしてあります。OFF に戻せば元の挙動です。
        [Tooltip("ON（既定）: 返事は即座。距離も方向も空間も物理が語る。OFF: 従来の応答遅延。")]
        public bool physicalOnly = true;
        [Range(0f, 1f)] public float minDelay = 0.05f;
        [Range(1f, 8f)] public float maxDelay = 6.0f;

        [Header("ベルの音（合成）")]
        // ★2 つのベルは**もともと同じ 1 つの鐘**。世界に落ちているのはその倍音 1 本。
        //   音色で区別できてしまうと、プレイヤーは響きではなく音色で当ててしまう。
        //   同じ鐘の一部に揃えることで、手がかりを「遅延・定位・残響」に絞れる（草案 §3）。
        // ★基音は世界によらず 1 つ（BellVoices.BaseHz）。ベルは元々ひとつだったので。
        //   変えると「どの音が返ってきたか」ではなく「どの音で呼んだか」で
        //   場所を当てられる余地が出る。手がかりは遅延・定位・残響だけに絞る。
        //
        // ★世界ごとの区別は**基音ではなく倍音**が付ける。
        //   断片は同じベルの倍音 1 本なので、音高は違うが**同じ鐘の一部に聞こえる**。
        //   勝手な基音を並べたときの「別の楽器が 2 つ」にならない。
        //   ステージ内の「どこにあるか」は音色ではなく遅延と定位が伝えるので、
        //   倍音が違っても探索の手がかりは増えない。
        [Tooltip("この世界（記録用）。")]
        public WorldId world = WorldId.None;

        // ★この世界に落ちているベルは、**この世界の鍵ではなく次の世界の鍵**。
        //   「いろんな世界に散らばった"次の世界への"ベル」という設定どおり。
        //   その世界自身の鍵をその世界に置くと、まだ行っていない世界へ永遠に行けなくなる
        //   （鶏と卵）。
        //
        // ★声は unlocks 側で決める。断片の声＝それが開く世界の声。
        //   結果として、**まだ見たことのない世界の声を、この世界で聞くことになる**。
        //   §0 の「奥を想像する」が、扉の前だけでなく探索中にも効く。
        [Tooltip("ここで見つけたベルが開く世界。声もここから決まる。")]
        public WorldId unlocks = WorldId.None;
        [Tooltip("減衰の長さ(秒)。長いほど部屋の尾が乗る余地が増える。")]
        [Range(0.5f, 6f)] public float bellSeconds = 3.0f;
        // 返しが 1 回になったので重なりは減ったが、遅延が最大 6s あるぶん
        // 「返事が来る前にもう一度呼ぶ」は普通に起きる。多少の余裕は残しておく。
        [Range(0f, 1f)] public float bellVolume = 0.7f;

        [Header("★聴き比べ用：世界のベルを持続音にする")]
        // ベル（過渡音）は定位と遅延を確かめるのに向くが、**音色の変化は分かりにくい**。
        // こもり・コムフィルタ・粒感・残響の量を聴くには持続音のほうがよい
        //（デモの AddConvolver も同じ理由で既定を楽曲にしている。DEV_LOG E-3
        //  「テスト信号を目的で使い分ける」）。
        //
        // ここにクリップを入れると、世界のベルは**鳴らしっぱなし**になり、
        // 呼びかけへの応答（PlayOneShot）は止まる。歩き回って響きの変化だけを聴く用。
        //
        // ⚠ 入れるクリップが市販楽曲なら**配布物に含められない**（仕様書 §7.6）。
        //   聴き取りが済んだら空に戻すこと。`*.wav` はコミットしない。
        [Tooltip("空ならベルの合成音で応答する。入れるとその音を鳴らしっぱなしにする。")]
        public AudioClip worldBellLoopClip;

        [Header("★仮：デバッグ用の近道")]
        // 扉の奥や音の漏れを確かめたいだけのときに、探索を毎回やり直すのは無駄。
        //   このキー 1 つで「断片を拾った ＋ 扉をその世界へ繋いだ」状態まで飛ぶ。
        //
        // ★通る道は本番と同じにしてある（Acquire を呼ぶ）。
        //   状態だけ別に作ると、デバッグでは動くのに本番で動かない、が起きる。
        //   扉の行き先だけは TrySetDestination の「近くで・閉じているとき」条件を飛ばす。
        // ⚠ 出荷前に外すこと。
        [Tooltip("押すと断片を取得し、扉を行き先へ繋ぐ。デバッグ用。")]
        public KeyCode debugGrantKey = KeyCode.Alpha7;

        [Header("到達判定")]
        [Tooltip("この距離まで寄って鳴らすと『見つけた』とみなす。")]
        [Range(0.5f, 5f)] public float arriveRadius = 2.0f;

        [Header("診断（読み取り専用）")]
        public float lastRt60;          // 呼びかけた瞬間の RoomRt60
        public float lastDelay;         // そこから導かれた応答遅延
        public float distanceToBell;    // 世界のベルまでの実距離（答え合わせ用）
        public bool found;

        [Header("進行（扉と手持ちのベル）")]
        [Tooltip("集めたベル。扉の前で選んだベルを鳴らすと行き先が変わる。")]
        public PlayerBells bells;
        [Tooltip("この世界の扉。閉じているときに近くで鳴らすと行き先を設定できる。")]
        public WorldDoor worldDoor;

        private AudioClip _playerClip, _worldClip;
        private readonly AudioClip[] _heldClips = new AudioClip[8];
        private int _sr = 48000;
        private float _nextCallTime;
        private int _callFrame = -1;    // Space を受けたフレーム（1 フレーム遅らせて鳴らす）

        // 予約された返し。1 回の呼びかけにつき 1 本だが、遅延が長いので
        // 「返事が来る前にもう一度呼ぶ」ぶんだけ同時に走りうる。
        // オーディオスレッドには触らないので、素朴な配列で足りる。
        private const int kMaxPending = 8;
        private readonly float[] _pendingAt = new float[kMaxPending];
        private int _pendingCount;

        private void Awake()
        {
            _sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;
            // 世界に落ちている断片＝**自分のベルの倍音 1 本**。
            //   元が同じものなので、拾って重なれば溶ける（別の鐘が 2 つ、にならない）。
            _worldClip = MakeFragmentClip("Bell_World", unlocks, bellSeconds, _sr);
            // 集めた断片の声。扉の前で鳴らして行き先を選ぶ用。
            foreach (WorldId id in System.Enum.GetValues(typeof(WorldId)))
            {
                if (id == WorldId.None || BellVoices.Partial(id) <= 0f) continue;
                _heldClips[(int)id] = MakeFragmentClip("Bell_" + id, id, bellSeconds, _sr);
            }

            // ★無音をループ再生させておく。
            //   Unity は OnAudioFilterRead を「その GameObject の AudioSource が鳴っている間」しか
            //   呼ばない。鳴り止むと畳み込みの尾まで切れてしまうので、無音を流し続けて
            //   フィルタ経路を常時開けておき、ベル自体は PlayOneShot で重ねる。
            KeepAlive(playerBell, _sr);

            // 世界のベルは、持続音が指定されていればそれをループ、無ければ無音＋PlayOneShot。
            if (worldBellLoopClip != null && worldBell != null)
            {
                worldBell.clip = worldBellLoopClip;
                worldBell.loop = true;
                worldBell.playOnAwake = true;
                worldBell.volume = 1f;
                worldBell.Play();
            }
            else
            {
                KeepAlive(worldBell, _sr);
            }
        }

        // ★手持ちのベルを組むのは Start。Awake ではない。
        //   持ち越した断片は PlayerBells が Awake で拾い直すので、
        //   こちらも Awake だと**順番次第で前の世界の倍音が乗らない**。
        private void Start()
        {
            RebuildPlayerClip();
        }

        /// 手持ちのベルを、いま持っている断片で組み直す。拾うたびに呼ぶ。
        public void RebuildPlayerClip()
        {
            _playerClip = MakePlayerClip("Bell_Player", bells != null ? bells.held : null,
                                         bellSeconds, _sr);
        }

        private static void KeepAlive(AudioSource src, int sr)
        {
            if (src == null) return;
            var silence = AudioClip.Create("Silence", sr / 10, 1, sr, false);
            silence.SetData(new float[sr / 10], 0);
            src.clip = silence;
            src.loop = true;
            src.playOnAwake = true;
            src.volume = 1f;
            src.Play();
        }

        private void Update()
        {
            if (listener != null && worldBell != null)
                distanceToBell = Vector3.Distance(listener.position, worldBell.transform.position);

            // ★受け付けたフレームでは鳴らさず、次のフレームまで待つ。
            //   Space を押した瞬間はデモ側の ApplySourceLayout が音源を動かしており、
            //   その位置がそのままエンジンへ渡ってしまう（PinPosition が押さえ返すのは
            //   LateUpdate なので間に合わない）。音を出すのを 1 フレームずらせば、
            //   ベルの立ち上がりは正しい幾何のもとで鳴る。16ms なので体感できない。
            // デバッグの近道。取得 → 扉を接続 まで一息に。
            if (Input.GetKeyDown(debugGrantKey) || Input.GetKeyDown(KeyCode.Keypad7))
            {
                if (!found) Acquire();
                if (worldDoor != null && unlocks != WorldId.None)
                {
                    worldDoor.destination = unlocks;
                    if (bells != null)
                    {
                        int i = bells.held.IndexOf(unlocks);
                        if (i >= 0) bells.selected = i;      // 持ち替えも合わせておく
                    }
                }
                Debug.LogWarning($"[BellGame] デバッグ: 断片を取得し、扉を"
                                 + $"{BellVoices.DisplayName(unlocks)}へ繋いだ。"
                                 + "（5/6 か掴みで扉を開ければ奥が見える）");
            }

            if (Input.GetKeyDown(callKey) && Time.time >= _nextCallTime)
            {
                _nextCallTime = Time.time + cooldown;
                _callFrame = Time.frameCount;
            }
            if (_callFrame >= 0 && Time.frameCount > _callFrame)
            {
                _callFrame = -1;
                Call();
            }

            // 予約された返しのうち、時刻が来たものを鳴らして詰める。
            //   持続音モードのときは応答を鳴らさない（既に鳴りっぱなしなので重ねる意味がない）。
            for (int i = _pendingCount - 1; i >= 0; i--)
            {
                if (Time.time < _pendingAt[i]) continue;
                if (worldBell != null && worldBellLoopClip == null)
                    worldBell.PlayOneShot(_worldClip, bellVolume);
                _pendingCount--;
                _pendingAt[i] = _pendingAt[_pendingCount];
            }
        }

        // ★鳴らす動作は 1 つだが、**拾う前と後で鳴る物が入れ替わる。**
        //
        //   拾う前 … 手持ちのベルは欠けていて**鳴らない**。呼びかけると、
        //             遠くに落ちている断片が共鳴して鳴る。空間の情報は
        //             「経路」（遅延・到来方向・こもり）からしか得られない
        //   拾った後 … 断片が戻って**自分のベルが鳴る**。音源が自分の位置に来るので、
        //             **周りの空間が分かるようになる**
        //
        //   ＝ 手に入るのは行き先の鍵だけでなく、**聴くための道具**でもある。
        //   「本来の力を取り戻す」が、テキストではなく操作の変化として出る。
        private void Call()
        {
            // 扉の前で閉じているときは、選んでいる断片を鳴らして行き先を決める。
            //   これは拾った後にしか起きない（持っていないと選べない）ので、
            //   常に自分のベルが鳴る側になる。
            WorldId pick = (bells != null) ? bells.Selected : WorldId.None;
            bool selectingDestination =
                worldDoor != null && pick != WorldId.None &&
                worldDoor.playerIsNear && worldDoor.IsClosed;

            if (selectingDestination)
            {
                var sel = _heldClips[(int)pick] ?? _playerClip;
                if (playerBell != null) playerBell.PlayOneShot(sel, bellVolume);
                worldDoor.TrySetDestination(pick);
                _nextCallTime = Time.time + cooldown;
                return;
            }

            if (found)
            {
                // 拾った後。自分のベルが手元で鳴る。遅延は無い（手に持っているので）。
                if (playerBell != null) playerBell.PlayOneShot(_playerClip, bellVolume);
                return;
            }

            // ── ここから下は「まだ拾っていない」場合だけ ──
            // 遠さ 0..1。nearDistance 以内で 0、farDistance 以遠で 1。
            float t = Mathf.Clamp01(Mathf.InverseLerp(nearDistance, farDistance, distanceToBell));
            float distDelay = Mathf.Lerp(nearDelay, farDelay, Mathf.Pow(t, delayCurve));

            // §4.5 の RT60 由来ぶん。既定は 0（距離だけ）。
            //   1 にすると草案どおり「自分の残響が終わってから返る」が混ざる。
            //   屋外（部屋 0 個）では RoomRt60 が 0 に落ちるので寄与しない。それで正しい。
            lastRt60 = AcousticFlowSceneDemo.Status.RoomRt60;
            float rtDelay = (lastRt60 / 3f) * rt60Weight;

            lastDelay = Mathf.Clamp(distDelay + rtDelay, minDelay, maxDelay);

            // ★全部を物理に任せる（発注者の決定・2026-08-22）。
            //
            //   鳴らしたら**世界のベルは即座に鳴る**。距離も方向も空間も、
            //   エンジンが解いた経路がそのまま語ります:
            //
            //       距離   … 音量（距離減衰）と遮蔽
            //       方向   … 定位
            //       空間   … 残響
            //       遅れ   … **音が実際に飛ぶ時間**（タップの遅延。30m で 0.09 秒）
            //
            // ⚠ 上の約束事の遅延は、距離を**二重に語って**いました。
            //   30m で 3.0 秒は物理の 33 倍で、音量も遮蔽も同じことを言っています。
            //   同じ問いに答えが二つある状態でした（決めごと #1）。
            //
            // ★遊び方もこれで決まります ──
            //   歩きながら定期的に鳴らし、**少しでも聞こえたらそちらへ進む**。
            //   返事を待つのではなく、返事の**大きさと向き**を頼りにします。
            //
            // ⚠ 古い作りは消していません。physicalOnly を切れば戻ります
            //   （上の nearDelay/farDelay/rt60Weight がそのまま効きます）。
            if (physicalOnly) lastDelay = 0f;

            // 返しは 1 回だけ。前の呼びかけの返事がまだ来ていなくても消さずに積む
            //   （遠距離だと 3 秒待つので、待っている間にもう一度呼ぶのは普通の操作）。
            if (_pendingCount < kMaxPending)
                _pendingAt[_pendingCount++] = Time.time + lastDelay;

            if (listener != null && worldBell != null && distanceToBell <= arriveRadius)
            {
                Debug.Log($"[BellGame] 断片を手に入れた（距離 {distanceToBell:F2}m / "
                          + $"RT60 {lastRt60:F2}s / 応答遅延 {lastDelay:F2}s）"
                          + $" → {BellVoices.DisplayName(unlocks)}への扉が開けるようになった");
                Acquire();
            }
        }

        /// 断片を手に入れる。到達判定からも、デバッグキーからも通る唯一の口。
        private void Acquire()
        {
            found = true;
            if (bells != null) bells.Collect(unlocks);

            // ★戻った倍音が、次に鳴らしたときから自分のベルに乗る。
            //   進行の表示はこれだけ。数字も UI も要らない。
            RebuildPlayerClip();

            // 断片は自分のベルへ戻る。世界に落ちていた音源は黙る。
            //   ★音源が 1 本減るので、そのぶんフレームコストも下がる
            //     （実測でコストは「遮蔽された音源数」で効く）。
            _pendingCount = 0;
            if (worldBell != null) { worldBell.Stop(); worldBell.enabled = false; }
        }

        // ══ ベルの合成 ══════════════════════════════════════════════
        //
        // 撞かれた鐘に似た非調和倍音の重ね合わせ。
        //   倍音比は整数倍ではない（1.19 の短三度、2.55 など）。これが「鐘らしさ」の正体で、
        //   同時に 125Hz〜4kHz の 6 帯域へ満遍なく乗ってくれるので、
        //   帯域ごとの遮蔽・回折・吸音の差がそのまま音色差として聞こえる。
        //   高い倍音ほど速く減衰させる（現実の鐘と同じ。遠いほど鈍く聞こえる手がかりになる）。
        //
        // ★ここが世界観と直結している。
        //   世界に散っている断片＝**この並びから抜き出した倍音 1 本**。
        //   拾うたびに自分のベルへ戻り、鳴らしたときの倍音が 1 本ずつ増える。
        //   「本来の力を取り戻す」がテキストではなく**音そのもの**として出る。
        internal static readonly float[] FullRatios = { 0.50f, 1.00f, 1.19f, 1.50f, 2.00f, 2.55f, 3.01f, 4.10f };
        internal static readonly float[] FullAmps = { 0.35f, 1.00f, 0.70f, 0.50f, 0.45f, 0.30f, 0.22f, 0.15f };
        internal static readonly float[] FullDecays = { 0.70f, 1.20f, 1.80f, 2.20f, 3.00f, 4.00f, 5.00f, 7.00f };

        /// 欠けていない完全なベル。全部集め終わった姿。
        public static AudioClip MakeBellClip(string name, float f0, float seconds, int sr)
            => Build(name, f0, FullRatios, FullAmps, FullDecays, seconds, sr);

        /// 世界に落ちている断片。倍音 1 本だけが鳴る。
        public static AudioClip MakeFragmentClip(string name, WorldId id, float seconds, int sr)
        {
            float r = BellVoices.Partial(id);
            if (r <= 0f) return MakeBellClip(name, BellVoices.BaseHz, seconds, sr);
            return Build(name, BellVoices.BaseHz,
                         new[] { r },
                         new[] { BellVoices.PartialAmp(id) },
                         new[] { BellVoices.PartialDecay(id) },
                         seconds, sr);
        }

        /// いま持っている断片だけで鳴る、欠けたベル。芯（ハムと基音）は常にある。
        public static AudioClip MakePlayerClip(string name, System.Collections.Generic.IList<WorldId> held,
                                               float seconds, int sr)
        {
            var ratio = new System.Collections.Generic.List<float>(BellVoices.CoreRatios);
            var amp = new System.Collections.Generic.List<float>(BellVoices.CoreAmps);
            var decay = new System.Collections.Generic.List<float>(BellVoices.CoreDecays);
            if (held != null)
            {
                for (int i = 0; i < held.Count; i++)
                {
                    float r = BellVoices.Partial(held[i]);
                    if (r <= 0f) continue;
                    ratio.Add(r);
                    amp.Add(BellVoices.PartialAmp(held[i]));
                    decay.Add(BellVoices.PartialDecay(held[i]));
                }
            }
            return Build(name, BellVoices.BaseHz, ratio.ToArray(), amp.ToArray(), decay.ToArray(), seconds, sr);
        }

        private static AudioClip Build(string name, float f0, float[] ratio, float[] amp, float[] decay,
                                       float seconds, int sr)
        {
            int n = Mathf.Max(1, Mathf.RoundToInt(seconds * sr));
            var data = new float[n];

            // 倍音の本数が変わっても**音量は変えない**。
            //   減るのは「厚み」であって「大きさ」ではない。ここを normalize しないと、
            //   欠けたベルが単に小さくなり、距離の手がかり（レベル）に進行度が混ざってしまう。
            float norm = 0f;
            for (int p = 0; p < amp.Length; p++) norm += amp[p];
            norm = 1f / Mathf.Max(1e-6f, norm);

            for (int i = 0; i < n; i++)
            {
                float t = i / (float)sr;
                float s = 0f;
                for (int p = 0; p < ratio.Length; p++)
                {
                    float w = 2f * Mathf.PI * f0 * ratio[p];
                    s += amp[p] * Mathf.Exp(-decay[p] * t) * Mathf.Sin(w * t);
                }
                // 2ms だけ立ち上がりをなます。0 サンプル目からいきなり振幅を出すと
                // デジタルなクリックが乗り、それ自体が定位の手がかりになってしまう。
                float attack = Mathf.Min(1f, t / 0.002f);
                data[i] = s * norm * attack;
            }

            AddStrike(data, sr);

            var clip = AudioClip.Create(name, n, 1, sr, false);
            clip.SetData(data, 0);
            return clip;
        }

        // ★打点の当たり。**倍音の本数と無関係に常に乗せる。**
        //
        //   倍音を抜くと帯域が痩せる。欠けたベルは低域しか持たなくなるので、
        //   HRTF も帯域ごとの遮蔽差も効かなくなり、序盤が探索不能になりかねない
        //   （草案 §2 が「ベルは 6 帯域にきれいに乗る理想的なテスト信号」としているのは
        //     倍音が揃っている前提）。
        //   撞いた瞬間の接触音は広帯域なので、これを常設すれば
        //   **定位の手がかりは最初から確保しつつ、響きの豊かさだけが育つ**。
        //   実際の鐘も撞点は広帯域なので、演出ではなく物理として正しい。
        private static void AddStrike(float[] data, int sr)
        {
            const float strikeAmp = 0.14f;
            const float strikeDecay = 240f;      // e 折り 4ms 程度
            int len = Mathf.Min(data.Length, Mathf.RoundToInt(sr * 0.04f));
            uint rng = 0x9E3779B9u;
            float lp = 0f;
            for (int i = 0; i < len; i++)
            {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                float w = (rng / 4294967295f) * 2f - 1f;
                lp += 0.45f * (w - lp);          // 一極ローパス。いちばん硬い所だけ落とす
                float t = i / (float)sr;
                float ramp = Mathf.Min(1f, t / 0.0003f);
                data[i] += strikeAmp * lp * Mathf.Exp(-strikeDecay * t) * ramp;
            }
        }
    }
}
