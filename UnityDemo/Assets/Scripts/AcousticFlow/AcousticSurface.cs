// AcousticSurface.cs
// 個別オブジェクトの音響マテリアルを明示指定するコンポーネント。
// 材質は外見から自動で決まらない（音響特性）ので、必要な壁にこれを付けて指定する。
// 解決の優先順位は「AcousticSurface（明示）> CollArea のエリア素材 > グローバル既定」。
// これを付けないオブジェクトはエリア素材かグローバル既定にフォールバックする。
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public sealed class AcousticSurface : MonoBehaviour
    {
        [Tooltip("この面の音響マテリアル。エリア素材より優先される。")]
        public AcousticMaterialPreset material = AcousticMaterialPreset.Default;

        public AcousticMaterial Resolve() => AcousticMaterial.FromPreset(material);
    }
}
