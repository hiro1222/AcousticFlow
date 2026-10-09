// CollArea.cs
// 空間スコープで「忠実度」と「素材」を切り替える範囲ボックス（エリア法）。
// 1つのコンポーネントに独立した2機能を持たせる（どちらか片方だけ使ってもよい）:
//   ・忠実度LOD: provideFidelity が ON のエリアに「重なる」Collider は実形状(OBB=回転反映)。
//                どのエリアにも重ならない Collider は bounds を軸並行(AABB)に丸めて軽量化。
//   ・エリア素材: provideMaterial が ON のエリアに「中心が入る」Collider に、このゾーンの
//                既定マテリアルを与える。AcousticSurface があればそちらが優先。
// 範囲は同じ GameObject の Collider.bounds（無ければ position×lossyScale の箱）で表す。
//
// 判定セマンティクスを意図的に分けている:
//   忠実度 = bounds 重なり（境界をまたぐ壁も取りこぼさない・保守的）
//   素材   = 中心包含（1オブジェクト=1ゾーンの所有者を一意に決める）
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public sealed class CollArea : MonoBehaviour
    {
        [Header("忠実度LOD")]
        [Tooltip("ON: この範囲に重なる占有物を高忠実度で扱う。メッシュがあれば実形状(三角形)、" +
                 "箱なら回転反映(OBB)。範囲外はメッシュも箱に丸める(AABB)。")]
        public bool provideFidelity = true;

        [Header("エリア素材")]
        [Tooltip("ON: この範囲に中心が入る Collider に、下の材質をゾーン既定として与える。")]
        public bool provideMaterial = false;

        [Tooltip("このゾーンの既定マテリアル（provideMaterial が ON のとき有効）。")]
        public AcousticMaterialPreset material = AcousticMaterialPreset.Default;

        [Tooltip("素材エリアが重なったとき、値が大きいエリアの材質が勝つ。")]
        public int priority = 0;

        // 範囲のワールドAABB。Collider があればその bounds、無ければ position×lossyScale。
        public Bounds WorldBounds
        {
            get
            {
                var col = GetComponent<Collider>();
                if (col != null) return col.bounds;
                return new Bounds(transform.position, AbsScale(transform.lossyScale));
            }
        }

        private static Vector3 AbsScale(Vector3 s)
            => new Vector3(Mathf.Abs(s.x), Mathf.Abs(s.y), Mathf.Abs(s.z));

        private void OnDrawGizmosSelected()
        {
            var b = WorldBounds;
            // 忠実度=シアン / 素材=マゼンタ / 両方=白っぽく。視認用。
            Gizmos.color = (provideFidelity && provideMaterial) ? new Color(0.8f, 0.8f, 0.9f, 1f)
                         : provideFidelity ? Color.cyan
                         : provideMaterial ? Color.magenta
                         : Color.gray;
            Gizmos.DrawWireCube(b.center, b.size);
        }
    }
}
