// AcousticZone.cs
// 音響LODストリーミング用の「ゾーン」。リスナーがこの範囲に入ると、このゾーンの
// メッシュ occluder（BVH）が有効になり、出ると無効になる（BVHは保持＝再構築なし）。
//   ・範囲   : 同じ GameObject の Collider.bounds（無ければ position×lossyScale の箱）。
//   ・メッシュ: meshes（子の MeshFilter 含む）。空なら自分の子から集める。
// エリアを「入りたい所」に置く運用。各ゾーンは1本の統合BVHとして登録され、
// リスナー在否で active を切替える（重いソルブは有効ゾーンだけ舐める）。
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public sealed class AcousticZone : MonoBehaviour
    {
        [Tooltip("このゾーンに属するメッシュ（子の MeshFilter 含む）。空なら自分の子から集める。")]
        public Transform[] meshes;

        [Tooltip("このゾーンのメッシュ材質。")]
        public AcousticMaterialPreset material = AcousticMaterialPreset.Default;

        [Tooltip("出るときは範囲をこのぶん外側まで許容してから無効化（境界チラつき防止）。メートル。")]
        public float hysteresis = 1.5f;

        [Tooltip("最初から有効にしておく（リスナーがゾーン内で開始する場合など）。")]
        public bool startActive = false;

        // メッシュの供給元（明示 or 自分の子）。
        public Transform[] MeshRoots =>
            (meshes != null && meshes.Length > 0) ? meshes : new[] { transform };

        // 範囲のワールドAABB。Collider があればその bounds、無ければ position×lossyScale。
        public Bounds WorldBounds
        {
            get
            {
                var col = GetComponent<Collider>();
                if (col != null) return col.bounds;
                return new Bounds(transform.position, Abs(transform.lossyScale));
            }
        }

        private static Vector3 Abs(Vector3 s)
            => new Vector3(Mathf.Abs(s.x), Mathf.Abs(s.y), Mathf.Abs(s.z));

        private void OnDrawGizmosSelected()
        {
            var b = WorldBounds;
            Gizmos.color = new Color(0.3f, 1f, 0.6f, 1f);  // ゾーン=緑
            Gizmos.DrawWireCube(b.center, b.size);
        }
    }
}
