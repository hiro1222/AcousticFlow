// FlowWalker.cs ── 試聴用の歩き（段 4）。WASD で歩く、右ドラッグで向く、QE で上下、Shift で速く。
//   歩行速度は 1.4 m/s（回帰 [配分] の歩行の連続性と同じ速さ）。音の話は一切入っていない。
using UnityEngine;

namespace AcousticFlow
{
    public sealed class FlowWalker : MonoBehaviour
    {
        public float walkSpeed = 1.4f;
        public float runSpeed = 3.0f;
        public float lookSpeed = 0.15f;
        private float _yaw, _pitch;

        private void Start() { var e = transform.eulerAngles; _yaw = e.y; _pitch = e.x; }

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
