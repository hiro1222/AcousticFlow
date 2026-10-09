// FlowWalker.cs ── 試聴のための歩き（新コア・段 4）。WASD で歩く、右ドラッグで向く、QE で上下、Shift で走る。
//
// ■ 全体の中の位置
//   ゲーム（ここ）→ AcousticWorld（場面を engine へ流す）→ AcousticEngine.dll（音の計算）→ WorldVoice（鳴らす）。
//   この層は「耳をどこへ運ぶか」だけを決める。音の話は 1 行も入っていない。
//   AcousticWorld が毎フレーム、この Transform を聞き手（位置・前・上）として engine へ渡す。
//
// ■ 役割
//   試聴用の移動。歩く速さを実測と揃えてある（1.4 m/s ＝ 回帰テスト [配分] の「歩行の連続性」と同じ速さ）。
//   耳で聞く「歩いたときのぐらつき」と、検査で測っている段差が同じ条件になる。
//
// ■ 中の仕組み
//   Update で 3 つ。1) 右ボタンを押している間だけ視線を回す 2) キーの向きを水平移動に直す 3) 速さを掛けて進む。
//   ★水平の扱い: 入力の向きには **yaw だけの回転**を掛ける。カメラの回転をそのまま使うと、見下ろしたときに
//     前進で床へ潜る（耳が壁の中に入ると見通しが 0 になって音が消える）。
//   ★pitch は ±80° で止める。真上・真下を通すと上方向が反転して視線が裏返る。
//
// ■ 繋がり
//   受ける: Unity の入力（Input）。渡す: 自分の Transform（AcousticWorld が聞き手として読む）。
//
// ■ 退けた書き方
//   ・CharacterController で歩く: 当たり判定が入ると壁際で押し戻され、耳の位置が 1 フレームで跳ぶ。
//     跳ぶと部屋の判定や見通しが不連続になり、試聴で「システムの段差」と区別が付かなくなる。
//   ・Time.fixedDeltaTime（物理の刻み）で動かす: 音は毎フレームの位置で解くので、描画と刻みがずれる。
//
// ■ 壊れる所
//   ・walkSpeed を実測（1.4 m/s）から離すと、耳で聞く段差と回帰の数字が別物になる（比べる意味が消える）。
//   ・lookSpeed を上げすぎると 1 フレームの向きの変化が大きくなり、両耳の差（ITD）の追従が間に合わず「飛ぶ」。
using UnityEngine;

namespace AcousticFlow
{
    public sealed class FlowWalker : MonoBehaviour
    {
        public float walkSpeed = 1.4f;
        public float runSpeed = 3.0f;
        public float lookSpeed = 0.15f;
        private float _yaw, _pitch;

        /// 開始時の向きを yaw/pitch に取り込む。
        ///   これが無いと、Inspector で回しておいた向きが最初の右ドラッグで捨てられて正面へ飛ぶ。
        private void Start() { var e = transform.eulerAngles; _yaw = e.y; _pitch = e.x; }

        /// 毎フレームの視線と移動。
        ///   視線: 右ボタン中だけ、マウスの移動量 ×lookSpeed×10 を yaw/pitch に足す（pitch は ±80° で頭打ち）。
        ///   移動: WASD/QE を足した向きを正規化し、**yaw だけの回転**を掛けて水平面の向きに直してから
        ///         速さ（Shift で走り）× Time.deltaTime を掛けて位置に足す。
        private void Update()
        {
            if (Input.GetMouseButton(1))
            {
                _yaw += Input.GetAxis("Mouse X") * lookSpeed * 10f;
                _pitch = Mathf.Clamp(_pitch - Input.GetAxis("Mouse Y") * lookSpeed * 10f, -80f, 80f);
                transform.rotation = Quaternion.Euler(_pitch, _yaw, 0f);
            }
            float speed = Input.GetKey(KeyCode.LeftShift) ? runSpeed : walkSpeed;
            Vector3 d = Vector3.zero;
            if (Input.GetKey(KeyCode.W)) d += Vector3.forward;
            if (Input.GetKey(KeyCode.S)) d -= Vector3.forward;
            if (Input.GetKey(KeyCode.D)) d += Vector3.right;
            if (Input.GetKey(KeyCode.A)) d -= Vector3.right;
            if (Input.GetKey(KeyCode.E)) d += Vector3.up;
            if (Input.GetKey(KeyCode.Q)) d -= Vector3.up;
            if (d.sqrMagnitude > 0f)
            {
                var flat = Quaternion.Euler(0f, _yaw, 0f);
                transform.position += flat * d.normalized * speed * Time.deltaTime;
            }
        }
    }
}
