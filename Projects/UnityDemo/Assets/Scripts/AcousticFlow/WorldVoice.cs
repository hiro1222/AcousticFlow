// WorldVoice.cs ── 新コア（Flow）の音源（段 4）。AudioSource と AF_Voice の橋。
//
// ■ 全体の中の位置
//   場面（ゲーム）→ AcousticWorld → AcousticEngine.dll → **ここ** → Unity の出力。
//   AcousticWorld が「どう聞こえるか」（タップ・方向・尾の量）を engine の Voice に置き、
//   この層は「何の音を流すか」と「返ってきた両耳の波形を出力に書き戻すこと」だけを持つ。
//   音を作る式は 1 つもここに無い（決めごと: 音の計算は音響エンジンだけ、C# は操作だけ）。
//
// ■ 役割
//   AudioSource の音を DLL の Voice に渡し、返ってきた両耳の音で上書きする。配分は AcousticWorld が Voice に置く。
//   このコンポーネントが持つのは「何を鳴らすか」（AudioSource）と Voice の寿命だけ。
//
// ■ 中の仕組み
//   Awake:  Voice を作る（サンプルレートとバッファは Unity から）。HRTF は合成、tailLevel 1、出力の音量は outputGain。
//           AudioSource は 2D（spatialBlend 0。空間化は DLL）、ループ、playOnAwake は切って自分で Play。
//   Start:  AcousticWorld に登録（AcousticWorld が先に OnEnable されているので Instance がある）。
//   OnAudioFilterRead: 入力をモノラルに畳んで AF_VoiceRender、L/R を書き戻す。
//   Attach/SetFdn/SetDirectionBus: AcousticWorld が呼ぶ。器が差し替わったときも SetFdn で追う。
//
// ■ 繋がり
//   受ける: AcousticWorld（emitter 番号、FDN の器、方向バス）。
//   渡す:   AF_Voice*（残す側の DSP）。
//
// ■ 退けた書き方
//   ・VoiceConvolver（旧、600 行）を流用: 旧 API のタップ組み・尾の組み直し・畳み込みの尾が入っている。新コアでは
//     Voice へ置くのは AcousticWorld なので、ここは 120 行で済む。
//
// ■ 壊れる所
//   ・AudioSource にクリップが無いと無音（警告を出す。旧 IrConvolver は無音を差し込んで黙っていた）。
//   ・OnAudioFilterRead は audio thread。ここで Native 以外の Unity API を呼ばない。
using System;
using UnityEngine;

namespace AcousticFlow
{
    [RequireComponent(typeof(AudioSource))]
    public sealed class WorldVoice : MonoBehaviour
    {
        [Tooltip("音源の幅（m）。0 で点。見込み角 2° 以上のとき開口の積分に使う（段 5）")]
        public float radius = 0f;
        [Tooltip("プレイヤーの操作対象（予算の優先度 1 位。段 8）")]
        public bool operated = false;
        [Range(0f, 1f)] public float loudness = 1f;
        [Tooltip("出力の音量（DLL の outputGain）。絶対値は耳で決める")]
        [Range(0f, 2f)] public float outputGain = 0.6f;
        public bool enableHrtf = true;
        // 音源ごとの上書き（2026-09-14）。−1（負）で AcousticWorld の設定に従う。特定の音源だけ受取面や閉じ込めを使うため。
        [Tooltip("この音源の初期反射の出し方。−1 で AcousticWorld に従う / 0 虚像 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網 / 4 虚像の面音源。実行中に変えられる")]
        [Range(-1, 4)] public int earlyModelOverride = -1;
        [Tooltip("この音源の隣の部屋の閉じ込め。負で AcousticWorld に従う / 0..1。実行中に変えられる")]
        [Range(-1f, 1f)] public float adjacentContainOverride = -1f;

        public IntPtr VoiceHandle => _voice;
        public int EmitterId { get; private set; } = -1;
        public float RmsOut { get; private set; }

        private IntPtr _voice, _hrtf;
        private AudioSource _src;
        private float[] _dry, _outL, _outR;
        private volatile bool _ready;
        private int _sampleRate = 48000;
        private bool _wwise;                 // 鳴らす器が Wwise か（Awake で AcousticWorld から決まる）
        public bool UsesWwise => _wwise;

        /// Voice（DLL の音源 1 本ぶんの DSP）を作り、AudioSource をこの経路用に整える。
        ///   サンプルレートとバッファ長は Unity から取る（maxFrames が足りないと OnAudioFilterRead で丸ごと落ちる）。
        ///   tapCrossfadeMs 30 / hrtfCrossfadeMs 12 は、タップと HRTF が差し替わるときに段差を出さない長さ。
        ///   hrtfCrossoverHz 700 は実測 HRTF のとき低域を方向バスに任せる境目（合成 HRTF では効かない）。
        ///   AudioSource は spatialBlend 0（2D）にする。★Unity の空間化を残すと、DLL の両耳化と二重に掛かる。
        private void Awake()
        {
            // 鳴らす器が Wwise なら、Voice は Wwise の標本化周波数と大きめのブロックで作る（Unity の設定ではなく）。
            //   ★AcousticWorld は実行順 −50 なので、この Awake より先に Instance が立っている。
            var world = AcousticWorld.Instance;
            _wwise = world != null && world.UseWwise;
            _sampleRate = _wwise ? world.SampleRate : AudioSettings.outputSampleRate;
            AudioSettings.GetDSPBufferSize(out int bufLen, out _);
            int maxFrames = _wwise ? world.MaxFrames : Mathf.Max(256, bufLen);
            var cfg = new Native.AFVoiceConfig
            {
                sampleRate = _sampleRate, maxFrames = maxFrames, tailSeconds = 1f,
                tapCrossfadeMs = 30f, hrtfCrossfadeMs = 12f, hrtfCrossoverHz = 700f, tailFirstBlock = 64, tailCapBlock = 8192,
            };
            _voice = Native.AF_VoiceCreate(ref cfg);
            if (_voice == IntPtr.Zero) { Debug.LogError("[WorldVoice] Voice を作れませんでした"); return; }
            _hrtf = Native.AF_HrtfCreateSynthetic(_sampleRate);
            Native.AF_VoiceSetHrtf(_voice, _hrtf);
            Native.AF_VoiceSetHrtfEnabled(_voice, enableHrtf ? 1 : 0);
            Native.AF_VoiceSetOutputGain(_voice, outputGain);
            Native.AF_VoiceSetTailLevel(_voice, 1f);
            _dry = new float[cfg.maxFrames]; _outL = new float[cfg.maxFrames]; _outR = new float[cfg.maxFrames];

            _src = GetComponent<AudioSource>();
            _src.spatialBlend = 0f;          // 空間化は DLL
            _src.loop = true;
            _src.playOnAwake = false;
            // Wwise のときは音を Wwise の Event（同じ GameObject の AkAmbient など）が出す。AudioSource は使わない。
            if (!_wwise && _src.clip == null) Debug.LogWarning($"[WorldVoice] {name}: AudioSource にクリップがありません（無音）");
            _ready = true;
        }
        /// 再生を始め、AcousticWorld に自分を登録する（emitter 番号は登録の返しで Attach 経由に入る）。
        ///   ★Start でやるのは、AcousticWorld の OnEnable が先に走って Instance が立っているのを当てにするため。
        private void Start()
        {
            if (!_wwise && _src != null && _src.clip != null && !_src.isPlaying) _src.Play();
            if (AcousticWorld.Instance != null) AcousticWorld.Instance.Register(this);
        }
        /// 場面から外れたら登録を解く（AcousticWorld 側の emitter も外れる）。
        private void OnDisable() { if (AcousticWorld.Instance != null) AcousticWorld.Instance.Unregister(this); }
        /// 器を壊す。★_ready を先に落としてから壊す（audio thread が Render 中の器を掴んだままにしない）。
        private void OnDestroy()
        {
            _ready = false;
            if (_wwise)
            {
                // ★Wwise の音のスレッドが写しの中でまだ握っているかもしれないので、直には壊さず 1 秒後に壊してもらう。
                if (_voice != IntPtr.Zero) { NativeHost.AF_HostRetireVoice(_voice); _voice = IntPtr.Zero; }
                if (_hrtf != IntPtr.Zero) { NativeHost.AF_HostRetireHrtf(_hrtf); _hrtf = IntPtr.Zero; }
                return;
            }
            if (_voice != IntPtr.Zero) { Native.AF_VoiceDestroy(_voice); _voice = IntPtr.Zero; }
            if (_hrtf != IntPtr.Zero) { Native.AF_HrtfDestroy(_hrtf); _hrtf = IntPtr.Zero; }
        }
        /// Inspector で動かせる摘み（出力の音量・HRTF の入切）を毎フレーム押す。
        ///   値の比較をせずに毎回押しているのは、押す費用（数 µs）より「押し忘れで効かない」方が高く付くため。
        private void Update()
        {
            if (_voice == IntPtr.Zero) return;
            Native.AF_VoiceSetOutputGain(_voice, outputGain);
            Native.AF_VoiceSetHrtfEnabled(_voice, enableHrtf ? 1 : 0);
        }

        // ── AcousticWorld から ──
        /// AcousticWorld が呼ぶ。この音源が engine のどの emitter かを覚え、部屋の FDN と方向バスに繋ぐ。
        public void Attach(int emitterId, IntPtr fdn, IntPtr directionBus)
        {
            EmitterId = emitterId;
            SetFdn(fdn);
            if (_voice != IntPtr.Zero) Native.AF_VoiceSetDirectionBus(_voice, directionBus);
        }
        /// 部屋の FDN の器を繋ぎ直す（部屋グラフを組み直すと器が作り直されるので、そのたびに呼ばれる）。
        public void SetFdn(IntPtr fdn) { if (_voice != IntPtr.Zero) Native.AF_VoiceSetFdnMix(_voice, fdn); }
        /// AcousticWorld から借りる HRTF（実測）。Zero なら自前の合成へ戻す。器の寿命は貸す側（Detach で必ず戻す）。
        public void SetSharedHrtf(IntPtr shared)
        {
            if (_voice == IntPtr.Zero) return;
            _sharedHrtf = shared;
            Native.AF_VoiceSetHrtf(_voice, shared != IntPtr.Zero ? shared : _hrtf);
        }
        public bool UsesSharedHrtf => _sharedHrtf != IntPtr.Zero;
        private IntPtr _sharedHrtf = IntPtr.Zero;
        /// 繋ぎを全部外す。借りている HRTF は必ず返す（貸し主が壊した後に使うと落ちる）。
        public void Detach()
        {
            EmitterId = -1;
            if (_voice != IntPtr.Zero)
            {
                Native.AF_VoiceSetFdnMix(_voice, IntPtr.Zero); Native.AF_VoiceSetDirectionBus(_voice, IntPtr.Zero);
                if (_sharedHrtf != IntPtr.Zero) { _sharedHrtf = IntPtr.Zero; Native.AF_VoiceSetHrtf(_voice, _hrtf); }   // 借り物を返す
            }
        }

        /// 音の本体。audio thread で、入力をモノラルに畳んで AF_VoiceRender に渡し、返った L/R で data を上書きする。
        ///   1) 入ってきた全チャンネルを足して ÷ch でモノラルに（素材がステレオでも点音源として扱う）。
        ///   2) frames が確保より多いときは無音にして帰る（配列の外へ書かない。器を作り直すのは audio thread では無理）。
        ///   3) 2ch 以上なら L/R に書き、3ch 目以降は 0（DLL が出すのは両耳の 2 本だけ）。1ch なら平均。
        ///   ★ここで Unity の API（Debug.Log・GetComponent）を呼ばない。audio thread から触ると落ちる。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (_wwise || !_ready || _voice == IntPtr.Zero) { Array.Clear(data, 0, data.Length); return; }   // Wwise のときはプラグインが鳴らす
            int frames = data.Length / channels;
            if (frames > _dry.Length) { Array.Clear(data, 0, data.Length); return; }
            for (int f = 0; f < frames; f++)
            {
                float s = 0f;
                for (int c = 0; c < channels; c++) s += data[f * channels + c];
                _dry[f] = s / channels;
            }
            var m = new Native.AFVoiceMetering();
            Native.AF_VoiceRender(_voice, _dry, frames, _outL, _outR, ref m);
            RmsOut = m.rmsOut;
            if (channels >= 2)
                for (int f = 0; f < frames; f++) { data[f * channels] = _outL[f]; data[f * channels + 1] = _outR[f]; for (int c = 2; c < channels; c++) data[f * channels + c] = 0f; }
            else
                for (int f = 0; f < frames; f++) data[f] = 0.5f * (_outL[f] + _outR[f]);
        }
    }
}
