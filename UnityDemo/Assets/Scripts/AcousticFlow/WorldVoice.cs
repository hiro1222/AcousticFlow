// WorldVoice.cs ── 新コア（Flow）の音源（段 4）。AudioSource と AF_Voice の橋。
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

        public IntPtr VoiceHandle => _voice;
        public int EmitterId { get; private set; } = -1;
        public float RmsOut { get; private set; }

        private IntPtr _voice, _hrtf;
        private AudioSource _src;
        private float[] _dry, _outL, _outR;
        private volatile bool _ready;
        private int _sampleRate = 48000;

        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            AudioSettings.GetDSPBufferSize(out int bufLen, out _);
            var cfg = new Native.AFVoiceConfig
            {
                sampleRate = _sampleRate, maxFrames = Mathf.Max(256, bufLen), tailSeconds = 1f,
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
            if (_src.clip == null) Debug.LogWarning($"[WorldVoice] {name}: AudioSource にクリップがありません（無音）");
            _ready = true;
        }
        private void Start()
        {
            if (_src != null && _src.clip != null && !_src.isPlaying) _src.Play();
            if (AcousticWorld.Instance != null) AcousticWorld.Instance.Register(this);
        }
        private void OnDisable() { if (AcousticWorld.Instance != null) AcousticWorld.Instance.Unregister(this); }
        private void OnDestroy()
        {
            _ready = false;
            if (_voice != IntPtr.Zero) { Native.AF_VoiceDestroy(_voice); _voice = IntPtr.Zero; }
            if (_hrtf != IntPtr.Zero) { Native.AF_HrtfDestroy(_hrtf); _hrtf = IntPtr.Zero; }
        }
        private void Update()
        {
            if (_voice == IntPtr.Zero) return;
            Native.AF_VoiceSetOutputGain(_voice, outputGain);
            Native.AF_VoiceSetHrtfEnabled(_voice, enableHrtf ? 1 : 0);
        }

        // ── AcousticWorld から ──
        public void Attach(int emitterId, IntPtr fdn, IntPtr directionBus)
        {
            EmitterId = emitterId;
            SetFdn(fdn);
            if (_voice != IntPtr.Zero) Native.AF_VoiceSetDirectionBus(_voice, directionBus);
        }
        public void SetFdn(IntPtr fdn) { if (_voice != IntPtr.Zero) Native.AF_VoiceSetFdnMix(_voice, fdn); }
        public void Detach()
        {
            EmitterId = -1;
            if (_voice != IntPtr.Zero) { Native.AF_VoiceSetFdnMix(_voice, IntPtr.Zero); Native.AF_VoiceSetDirectionBus(_voice, IntPtr.Zero); }
        }

        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (!_ready || _voice == IntPtr.Zero) { Array.Clear(data, 0, data.Length); return; }
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
