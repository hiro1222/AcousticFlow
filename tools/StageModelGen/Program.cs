// Program.cs
using System.Collections.Generic;
using System.Linq;
// BellGame のステージの「見た目用メッシュ」を OBJ で書き出す。
//
// ★音響には一切使わない。
//   コライダーを付けずにシーンへ置くこと。AcousticFlowSceneDemo.CollectOccluders が
//   拾うのは BoxCollider と MeshCollider だけなので、コライダーが無いメッシュは
//   部屋グラフにもエコグラムのレイにも入らない。
//   音響は BellGameStages が作るブロックアウトの箱（BoxCollider）が担当し続ける。
//   → 仕様書 §7.6「メッシュ壁は跨ぎ判定が使えず幻・崖が残りうる」を踏まずに済む。
//
// ★寸法は BellGameStages と一致させてある。
//   ここの数字を変えると見た目と音響がズレる。音響側の寸法は
//   RT60・臨界距離・部屋の割れ方を決めているので、勝手に動かさないこと。
//
// 使い方:
//   dotnet run --project tools/StageModelGen -- <出力ディレクトリ>
using StageModelGen;

const float DoorW = 1.4f, DoorH = 3.0f, DoorT = 0.062f;   // 扉。全世界で共通
// ★川の断面。**GrasslandTerrain の同名の定数と一致させること。**
//   RiverOutR は「土手が終わって野原に戻る距離」で、
//   草を剥ぐ帯（InstancedGrassField.riverClear）と土手の材質の帯も**この値**にする。
//   3 つが同じでないと「草は消えているのに地面は草色」の帯が出る。
const float RiverBedR = 3.2f, RiverTopR = 6.4f, RiverOutR = 9.5f;
const float RiverBedY = 0.10f, RiverCrestY = 1.15f;
string outDir = args.Length > 0 ? args[0] : "UnityDemo/Assets/Models";
Directory.CreateDirectory(outDir);

WriteMtl(Path.Combine(outDir, "StageModels.mtl"));
// ★丘と川の土手を 1 枚に統合した。`Stage1_Mound.obj` は使わない（生成器は残す）。
BuildGrassField(Path.Combine(outDir, "Stage1_Field.obj"));
BuildGrassHills(Path.Combine(outDir, "Stage1_Hills.obj"));
BuildTree(Path.Combine(outDir, "Prop_Tree.obj"));
BuildRiver(Path.Combine(outDir, "Stage1_River.obj"));
BuildTemple(Path.Combine(outDir, "Stage2_Temple.obj"));
// ★洞窟は棄却（発注者の決定・2026-08-22）。生成器は残す（番号が確定するまで消さない）。
// ステージ 4（洞窟を落としたので 3 番目）：雪山。
//   ★この世界だけ絵の要請が逆 ── **輪郭が溶けること。稜線だけが読める。**
//   寸法は BellGameStages.Stage4Snow のブロックアウトと一致。
BuildSnowField(Path.Combine(outDir, "Snow_Field.obj"), 4101);
BuildSnowRidge(Path.Combine(outDir, "Snow_Ridge1.obj"), 40f, 6.0f, 3.2f, true,  4201);
BuildSnowRidge(Path.Combine(outDir, "Snow_Ridge2.obj"), 30f, 8.0f, 3.6f, false, 4202);
BuildSnowRidge(Path.Combine(outDir, "Snow_Ridge3.obj"), 26f, 5.0f, 2.8f, true,  4203);
BuildSnowRange(Path.Combine(outDir, "Snow_Range.obj"), 4301);
// ステージ 2：無人の現代都市（2026-08-22 の配置。連絡板 M12 の座標表と一致）
//   段（横ライン）だけ揃え、幅と高さは振ってある。建物間は最低 5m。
//   ★塊は内部を作らない ── 遮蔽と反射だけが仕事。部屋になるのは B1 とドームの 2 つだけ。
BuildCityMass(Path.Combine(outDir, "City_A4.obj"),   8f, 20f, 27f, 3104);
BuildCityMass(Path.Combine(outDir, "City_A2.obj"),  18f, 18f, 18f, 3103);
BuildCityMass(Path.Combine(outDir, "City_A3.obj"),   8f, 18f, 27f, 3105);
BuildCityMass(Path.Combine(outDir, "City_B4.obj"),   9f, 20f, 21f, 3204);
BuildCityMass(Path.Combine(outDir, "City_B2.obj"),  18f, 14f, 15f, 3203);
BuildCityMass(Path.Combine(outDir, "City_B3.obj"),   8f, 14f, 21f, 3205);
BuildCityMass(Path.Combine(outDir, "City_C1.obj"),  18f, 18f, 18f, 3301);
BuildCityMass(Path.Combine(outDir, "City_C2.obj"),   8f, 18f, 27f, 3302);
// A1: 囲いだけ・上が空（入れる／響かない）。内寸 = 外寸 - 壁厚 0.35×2
BuildCityLot(Path.Combine(outDir, "City_A1.obj"), 15.3f, 17.3f, 4.5f, 0, 3101);
// B1: 屋根あり（入れる／響く・囮）。開口は +X
BuildCityBlock(Path.Combine(outDir, "City_B1.obj"), 13.3f, 13.3f, 6.0f, 3, 18f, 3201);
// ドーム。ベルはこの中。開口は南 1 つだけ
BuildCityDome(Path.Combine(outDir, "City_Dome.obj"), 7f, 6f, 3401);
BuildCityPark(Path.Combine(outDir, "City_Park.obj"), 3501);
// ★瓦礫と折れた鉄骨は作らない ── 無人だが**荒れてはいない**都市（発注者の決定）。
//   生成器は残す（番号が確定するまで消さない）。
BuildCitySkyline(Path.Combine(outDir, "City_Skyline.obj"), 2401);
BuildCityRoads(Path.Combine(outDir, "City_Roads.obj"), 2501);
// ステージ 0：白い部屋（プロローグ）。★閉じた部屋にして響かせる（下の理由）。
BuildWhiteRoom(Path.Combine(outDir, "Stage0_WhiteRoom.obj"), 8f, 8f, 4.2f, 2801);
// ★扉は**全世界で共通の 1 種類**（発注者の指示・参考画像あり）。
//   これで石の門 BuildGrasslandShrine と崩壊都市の躯体 BuildCityDoorway は使わない。
//   生成器は残す（番号が確定するまで消さない ── M0 と同じ扱い）。
BuildWhiteDoorFrame(Path.Combine(outDir, "Prop_Door.obj"), 2601);
BuildWhiteDoorSlab(Path.Combine(outDir, "Prop_DoorSlab.obj"), 2701);
BuildBellProp(Path.Combine(outDir, "Prop_Bell.obj"));
BuildBrazierProp(Path.Combine(outDir, "Prop_Brazier.obj"));

// ★扉の寸法。トップレベル文の並びの中に置くこと（return より後ろだと到達不能の警告）。
Console.WriteLine("完了。コライダーを付けずに (0,0,0) へ置くこと。");
return 0;

// ───────────────────────────────────────────────────────────────
// ステージ 2：神殿
//   BellGameStages.Stage2Temple の寸法:
//     外殻 内寸 20×24m・高さ 8m・厚み 0.4m、中心 (0,0,2)
//       → 内側 x -10..10 / z -10..14 / y 0..8
//       → 壁の外面 x ±10.4 / z -10.4, 14.4
//     入口   -Z 壁の中央に 1.1×3.0m
//     仕切り z=-4（-4.2..-3.8）、中央に 1.1×3.0m
//     列柱   x=±6、z=0,4,8,12 の 8 本。1.0×1.0×8.0m
//     屋根   y 8.0..8.4
// ───────────────────────────────────────────────────────────────
static void BuildTemple(string path)
{
    const float T = 0.4f;      // 壁厚
    const float H = 8f;        // 内法高さ
    const float XI = 10f;      // 内側 x
    const float XO = 10.4f;    // 外面 x
    const float ZI0 = -10f, ZI1 = 14f;      // 内側 z
    const float ZO0 = -10.4f, ZO1 = 14.4f;  // 外面 z
    const float DW = 1.1f, DH = 3.0f;       // 開口
    const float PZ = -4f;                   // 仕切りの中心 z

    var m = new ObjMesh();

    // ── 壁 ──────────────────────────────────────────────
    m.Group("Walls", "Stone_Wall");

    // -Z（入口のある面）。開口を避けて左袖・右袖・まぐさに割る。
    WallWithOpeningX(m, XO, ZO0, ZO0 + T, 0f, H, 0f, DW, DH);
    // +Z（奥）
    m.AddBox(new V3(-XO, 0, ZO1 - T), new V3(XO, H, ZO1));
    // -X / +X
    m.AddBox(new V3(-XO, 0, ZI0), new V3(-XI, H, ZI1));
    m.AddBox(new V3(XI, 0, ZI0), new V3(XO, H, ZI1));

    // 仕切り（前室と主室の境）。ここを抜けた瞬間に響きが入れ替わる。
    WallWithOpeningX(m, XO, PZ - T * 0.5f, PZ + T * 0.5f, 0f, H, 0f, DW, DH);

    // 基壇の水切りと軒。外側へ張り出すだけの飾りで、内法は変えない。
    m.Group("Plinth", "Stone_Wall");
    Ring(m, XO, ZO0, ZO1, 0.22f, 0f, 0.45f);
    m.Group("Cornice", "Stone_Wall");
    Ring(m, XO, ZO0, ZO1, 0.32f, 7.30f, H);

    // ── 列柱 ────────────────────────────────────────────
    m.Group("Columns", "Stone_Column");
    for (int i = 0; i < 4; i++)
    {
        float cz = i * 4f;
        Column(m, -6f, cz, 0f, H);
        Column(m, 6f, cz, 0f, H);
    }

    // ── 屋根 ────────────────────────────────────────────
    // 平らな部分は音響側の屋根（y 8.0..8.4）と同じ。その上の切妻は完全に飾り。
    m.Group("Roof", "Stone_Roof");
    m.AddBox(new V3(-XO, H, ZO0), new V3(XO, H + T, ZO1));
    Gable(m, XO + 0.5f, ZO0 - 0.5f, ZO1 + 0.5f, H + T, 2.6f);

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount} / 面 {m.Faces.Count}");
}

// X 方向に伸びる壁（z0..z1 の厚み）に、x=cx を中心とした幅 w・高さ h の開口を空ける。
static void WallWithOpeningX(ObjMesh m, float xHalf, float z0, float z1,
                             float y0, float y1, float cx, float w, float h)
{
    float gl = cx - w * 0.5f, gr = cx + w * 0.5f;
    if (gl > -xHalf) m.AddBox(new V3(-xHalf, y0, z0), new V3(gl, y1, z1));
    if (gr < xHalf) m.AddBox(new V3(gr, y0, z0), new V3(xHalf, y1, z1));
    if (h < y1) m.AddBox(new V3(gl, y0 + h, z0), new V3(gr, y1, z1));   // まぐさ
}

// 外周をぐるりと囲む帯（水切り・軒）。out だけ外へ張り出す。
static void Ring(ObjMesh m, float xHalf, float z0, float z1, float outset, float y0, float y1)
{
    float x = xHalf + outset;
    float a = z0 - outset, b = z1 + outset;
    m.AddBox(new V3(-x, y0, a), new V3(x, y1, z0));           // -Z 側
    m.AddBox(new V3(-x, y0, z1), new V3(x, y1, b));           // +Z 側
    m.AddBox(new V3(-x, y0, z0), new V3(-xHalf, y1, z1));     // -X 側
    m.AddBox(new V3(xHalf, y0, z0), new V3(x, y1, z1));       // +X 側
}

// ドリス式に寄せた円柱。音響側は 1.0×1.0m の角柱なので、直径 1.0 で内接させる。
//   （角柱の四隅だけ音響のほうが太い。見た目と音響の差はここだけ）
static void Column(ObjMesh m, float cx, float cz, float y0, float y1)
{
    const int Flutes = 20;
    float h = y1 - y0;

    // 台座（方形）と柱礎（円盤）
    m.AddBox(new V3(cx - 0.62f, y0, cz - 0.62f), new V3(cx + 0.62f, y0 + 0.16f, cz + 0.62f));
    Prism(m, cx, cz, y0 + 0.16f, y0 + 0.38f, 0.58f, 0.52f, Flutes * 2, 0f);

    // 柱身。溝彫り＋エンタシス（下 1/3 が最も太い）。
    float sh0 = y0 + 0.38f, sh1 = y0 + h - 0.62f;
    FlutedShaft(m, cx, cz, sh0, sh1, 0.50f, 0.435f, Flutes, 0.035f);

    // 柱頭（エキノス＝逆円錐台）とアバクス（方形の板）
    Prism(m, cx, cz, sh1, sh1 + 0.34f, 0.435f, 0.60f, Flutes * 2, 0f);
    m.AddBox(new V3(cx - 0.66f, sh1 + 0.34f, cz - 0.66f), new V3(cx + 0.66f, y1, cz + 0.66f));
}

// 円錐台（溝なし）。
static void Prism(ObjMesh m, float cx, float cz, float y0, float y1,
                  float r0, float r1, int segs, float flute)
{
    for (int s = 0; s < segs; s++)
    {
        float a0 = s / (float)segs * MathF.Tau;
        float a1 = (s + 1) / (float)segs * MathF.Tau;
        V3 b0 = new(cx + MathF.Cos(a0) * r0, y0, cz + MathF.Sin(a0) * r0);
        V3 b1 = new(cx + MathF.Cos(a1) * r0, y0, cz + MathF.Sin(a1) * r0);
        V3 t0 = new(cx + MathF.Cos(a0) * r1, y1, cz + MathF.Sin(a0) * r1);
        V3 t1 = new(cx + MathF.Cos(a1) * r1, y1, cz + MathF.Sin(a1) * r1);
        m.AddPolygon(b0, t0, t1, b1);   // 外向き（cross(up, tangent) = +radial）
    }
}

// 溝彫りの柱身。稜（arris）と溝底を交互に置くだけで、影が縦に走る。
static void FlutedShaft(ObjMesh m, float cx, float cz, float y0, float y1,
                        float r0, float r1, int flutes, float depth, int Rings = 10)
{
    // Rings は縦の分割数。エンタシスの膨らみが 14mm しかないので、
    // 小さい柱は 5 で足りる（10 のままだと三角形の半分を無駄に使う）。
    int segs = flutes * 2;
    var ring = new V3[Rings + 1][];
    for (int i = 0; i <= Rings; i++)
    {
        float t = i / (float)Rings;
        // エンタシス。直線に細らせると痩せて見えるので、正弦でわずかに膨らませる。
        float r = r0 + (r1 - r0) * t + MathF.Sin(t * MathF.PI) * 0.014f;
        float y = y0 + (y1 - y0) * t;
        ring[i] = new V3[segs];
        for (int s = 0; s < segs; s++)
        {
            float a = s / (float)segs * MathF.Tau;
            float rr = (s % 2 == 0) ? r : r - depth;
            ring[i][s] = new V3(cx + MathF.Cos(a) * rr, y, cz + MathF.Sin(a) * rr);
        }
    }
    for (int i = 0; i < Rings; i++)
        for (int s = 0; s < segs; s++)
        {
            int s2 = (s + 1) % segs;
            m.AddPolygon(ring[i][s], ring[i + 1][s], ring[i + 1][s2], ring[i][s2]);
        }
}

// 切妻屋根。完全に飾りで、音響側の屋根より上にしか置かない。
static void Gable(ObjMesh m, float xHalf, float z0, float z1, float y0, float rise)
{
    V3 a = new(-xHalf, y0, z0), b = new(xHalf, y0, z0);
    V3 c = new(xHalf, y0, z1), d = new(-xHalf, y0, z1);
    V3 r0 = new(0, y0 + rise, z0), r1 = new(0, y0 + rise, z1);

    m.AddPolygon(a, r0, r1, d);   // -X 側の斜面
    m.AddPolygon(b, c, r1, r0);   // +X 側の斜面
    m.AddPolygon(a, b, r0);       // -Z のペディメント
    m.AddPolygon(d, r1, c);       // +Z のペディメント
    m.AddPolygon(a, d, c, b);     // 下面（-Y）
}

// ───────────────────────────────────────────────────────────────
// ステージ 3：洞窟
//   BellGameStages.Stage3Cave の寸法:
//     外殻 内寸 24×40m・高さ 4.5m・厚み 0.5m、中心 (0,0,6)
//       → 内側 x -12..12 / z -14..26 / y 0..4.5
//     入口   -Z 壁の中央に 0.9×2.2m
//     仕切り z = -6 / 4 / 14、開口の x は -8 / +8 / -8（食い違い）
//
// ★見た目は内側の面をノイズで**内向きに**押し出して岩肌にする。
//   箱の外へはみ出さないので、音響（箱）との差は「部屋が少し狭く見える」だけ。
//   縁は動かさない（隣の面との継ぎ目が開くため）。
// ───────────────────────────────────────────────────────────────
static void BuildCave(string path)
{
    const float XI = 12f, H = 4.5f, Z0 = -14f, Z1 = 26f;
    const float GW = 0.9f, GH = 2.2f;
    const float Cell = 0.8f;

    var m = new ObjMesh();
    m.Group("CaveRock", "Cave_Rock");

    V3 X = new(1, 0, 0), Y = new(0, 1, 0), Z = new(0, 0, 1);

    // 床（少しだけ。歩く面が荒れすぎると見づらい）
    DisplacedPlane(m, new V3(-XI, 0, Z0), Z, X, Z1 - Z0, XI * 2, Cell, 0.22f, 0.16f);
    // 天井
    DisplacedPlane(m, new V3(-XI, H, Z0), X, Z, XI * 2, Z1 - Z0, Cell, 0.55f, 0.14f);
    // 側壁
    DisplacedPlane(m, new V3(-XI, 0, Z0), Y, Z, H, Z1 - Z0, Cell, 0.45f, 0.17f);
    DisplacedPlane(m, new V3(XI, 0, Z0), Z, Y, Z1 - Z0, H, Cell, 0.45f, 0.17f);
    // 奥（+Z）
    DisplacedPlane(m, new V3(-XI, 0, Z1), Y, X, H, XI * 2, Cell, 0.45f, 0.17f);
    // 入口のある面（-Z）。開口を空ける。
    DisplacedPlane(m, new V3(-XI, 0, Z0), X, Y, XI * 2, H, Cell, 0.45f, 0.17f,
                   c => MathF.Abs(c.X) < GW * 0.6f && c.Y < GH * 1.05f);

    // 仕切り 3 枚。開口の x を振ってあるので、視覚的にも直線が通らない。
    foreach ((float pz, float ox) in new[] { (-6f, -8f), (4f, 8f), (14f, -8f) })
    {
        bool Skip(V3 c) => MathF.Abs(c.X - ox) < GW * 0.6f && c.Y < GH * 1.05f;
        // -Z を向く面 / +Z を向く面
        DisplacedPlane(m, new V3(-12.5f, 0, pz - 0.25f), Y, X, H, 25f, 0.5f, 0.30f, 0.22f, Skip);
        DisplacedPlane(m, new V3(-12.5f, 0, pz + 0.25f), X, Y, 25f, H, 0.5f, 0.30f, 0.22f, Skip);
        // 開口の見付け（穴の厚みを塞ぐ）
        float hw = GW * 0.6f, ht = GH * 1.05f;
        m.AddBox(new V3(ox - hw - 0.12f, 0, pz - 0.25f), new V3(ox - hw, ht, pz + 0.25f));
        m.AddBox(new V3(ox + hw, 0, pz - 0.25f), new V3(ox + hw + 0.12f, ht, pz + 0.25f));
        m.AddBox(new V3(ox - hw, ht, pz - 0.25f), new V3(ox + hw, ht + 0.12f, pz + 0.25f));
    }

    // 岩塊。音響側の箱（Rock_1..3）に合わせた位置。
    m.Group("CaveBoulder", "Cave_Rock");
    Boulder(m, new V3(4f, 1.4f, -10f), new V3(3f, 2.8f, 2.5f), 0.28f);
    Boulder(m, new V3(-6f, 1.2f, 0f), new V3(2.5f, 2.4f, 3f), 0.26f);
    Boulder(m, new V3(2f, 1.6f, 10f), new V3(3.5f, 3.2f, 2f), 0.30f);

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount} / 面 {m.Faces.Count}");
}

// ───────────────────────────────────────────────────────────────
// ステージ 4：雪山
//   地面 110×110m（上面 y=0）
//   稜線 Ridge_1 (-10,3,-12) 40×6×1.5 / Ridge_2 (14,4,6) 1.5×8×30
//        Ridge_3 (-4,2.5,24) 26×5×1.5
//   露岩 Rock_1 (6,1,-20) 3×2×3 / Rock_2 (-14,1.2,4) 2.5×2.4×2.5
//
// ★稜線は「音響の箱より広い裾」を持たせる。吹き溜まりの見た目になるうえ、
//   稜（回折が起きる縁）は箱の上辺と一致したままになる。
//   裾が広がっても音響は箱のままなので、回折の稜線は動かない。
// ───────────────────────────────────────────────────────────────
// ───────────────────────────────────────────────────────────────
// ステージ 4（洞窟の棄却で 3 番目になる）：雪山
//
//   ★この世界だけ**絵の要請が逆**（WORLD_VISUAL §3-4）。
//     「**輪郭が溶けること。**白い面に白い雪で形が読みにくい。**稜線だけが読める**」
//     ── 音で頼れるものが稜線の回折だけ、というのと同じ構図。
//     他の世界でやってきた「シルエットを読ませる」を、ここでは**逆に使う。**
//
//   ★だから作り込まない。
//     雪原に細かい起伏を刻むと、形が読めてしまって要請に反する。
//     長い波長のうねりだけにして、**面としては情報を持たせない。**
//
//   ★読ませるものは 3 つだけ:
//       稜線 ── 空を背にした縁。雪庇（せっぴ）を付けると線が締まる
//       露岩 ── 一面の白の中で**唯一の暗い物**。距離の基準になる
//       樹氷 ── 縦の線。大きさの手がかり
//
//   ★寸法は `BellGameStages.Stage4Snow` のブロックアウトと一致させてある。
//     Ridge_1 (-10,3,-12) 40×6×1.5 ／ Ridge_2 (14,4,6) 1.5×8×30 ／ Ridge_3 (-4,2.5,24) 26×5×1.5
//     Rock_1 (6,1,-20) 3×2×3 ／ Rock_2 (-14,1.2,4) 2.5×2.4×2.5
// ───────────────────────────────────────────────────────────────
static void BuildSnowField(string path, int seed)
{
    const float E = 42f;          // 雪原の半径（地面 64×80 を包む）
    const float Cell = 1.6f;      // 粗くてよい。細かくすると形が読めてしまう
    var r = new Rng(seed);
    var m = new ObjMesh();

    m.Group("SnowGround", "Snow_Ground");
    int n = (int)MathF.Round(E * 2f / Cell);
    int quads = Grid(m, n, E, Cell, H, (x, z) => true);

    // ── 露岩。**一面の白の中で唯一の暗い物** ────────────────
    //   ブロックアウトの箱と同じ位置・寸法。距離の基準はこれだけになる。
    m.Group("SnowRock", "Snow_Rock");
    Boulder(m, new V3(6f, 1f, -20f), new V3(3f, 2f, 3f), 0.22f);
    Boulder(m, new V3(-14f, 1.2f, 4f), new V3(2.5f, 2.4f, 2.5f), 0.20f);

    // ── 樹氷。**縦の線が大きさを教える** ──────────────────
    //   稜線の内側に散らす。歩く道は空けておく（-Z から +Z へ抜ける中央）。
    m.Group("SnowTree", "Snow_Tree");
    for (int i = 0; i < 26; i++)
    {
        float tx = r.Range(-30f, 30f), tz = r.Range(-30f, 34f);
        if (MathF.Abs(tx) < 5f) continue;                   // 中央の通り道は空ける
        if (MathF.Abs(tz + 12f) < 3f || MathF.Abs(tz - 24f) < 3f) continue;  // 稜線の上には立てない
        FrostTree(m, tx, H(tx, tz), tz, r.Range(2.2f, 4.6f), ref r);
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 1100, 800, 34f, 16f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（区画 {quads} / 半径 {E:0}m / "
                    + "うねりだけ＝形を読ませない）");

    // 雪原の高さ。**長い波長だけ。**吹き溜まりの縞を弱く重ねる。
    static float H(float x, float z)
    {
        // 大きなうねり。歩く面なので振幅は控えめ。
        float y = Noise.Fbm(new V3(x, 0f, z), 0.018f, 2) * 1.1f;
        // ★吹き溜まりの縞。風向き（+X 寄り）に伸ばした細かい波。
        //   面に「向き」を与えるが、形は教えない ── まさにこの世界が欲しい情報量。
        y += MathF.Sin((x * 0.28f + z * 0.09f)) * 0.055f;
        return y;
    }
}

// 稜線 1 本。**雪庇（せっぴ）を片側へ張り出させる。**
//   ★平らな三角柱だと、白い面が白い空に溶けて線が出ない。
//     庇が張り出すと、その下に影が落ちて**縁が締まる** ── この世界で唯一読める線。
//   a→b が稜線の走る向き。h が高さ、w が裾の半幅。
static void BuildSnowRidge(string path, float len, float h, float w, bool cornRight, int seed)
{
    var r = new Rng(seed);
    var m = new ObjMesh();
    const int Seg = 24;
    const float Corn = 1.6f;      // 雪庇の張り出し

    m.Group("SnowRidge", "Snow_Ridge");
    for (int i = 0; i < Seg; i++)
    {
        float t0 = i / (float)Seg, t1 = (i + 1) / (float)Seg;
        float z0 = (t0 - 0.5f) * len, z1 = (t1 - 0.5f) * len;
        // 稜線の高さは端で落とす。真っ直ぐな棟だと壁に見える。
        float e0 = MathF.Sin(t0 * MathF.PI), e1 = MathF.Sin(t1 * MathF.PI);
        float h0 = h * (0.35f + 0.65f * e0) + r.Range(-0.12f, 0.12f);
        float h1 = h * (0.35f + 0.65f * e1) + r.Range(-0.12f, 0.12f);
        // 棟の位置も少し蛇行させる。直線だと人工物に見える。
        float c0 = MathF.Sin(t0 * 5.1f) * 0.7f, c1 = MathF.Sin(t1 * 5.1f) * 0.7f;
        // ★断面を作ってから帯を張る。**左右で別々に面を組まない。**
        //   前は cornRight の false 側だけ手で書いていて、棟と裾を跨ぐ面を張っていた
        //   （法線を測って発覚。72 面が下向き）。s で x を反転すれば順序は同じ。
        float sgn = cornRight ? 1f : -1f;
        V3 Pt(float c, float yy, float zz, float dx) => new V3(c + sgn * dx, yy, zz);
        //   風上の裾 → 棟 → 雪庇の先 → 風下の裾
        V3 fw0 = Pt(c0, 0f, z0, -w), fw1 = Pt(c1, 0f, z1, -w);
        V3 tp0 = Pt(c0, h0, z0, 0f), tp1 = Pt(c1, h1, z1, 0f);
        V3 cn0 = Pt(c0, h0 - 0.28f, z0, Corn), cn1 = Pt(c1, h1 - 0.28f, z1, Corn);
        V3 le0 = Pt(c0, 0f, z0, w), le1 = Pt(c1, 0f, z1, w);

        // ★x を反転すると面の裏表も入れ替わるので、巻き方向も逆にする。
        void Band(V3 lo0, V3 lo1, V3 hi1, V3 hi0)
        {
            if (sgn > 0f) m.AddPolygon(lo0, lo1, hi1, hi0);
            else m.AddPolygon(hi0, hi1, lo1, lo0);
        }
        Band(fw0, fw1, tp1, tp0);      // 風上の斜面
        Band(tp0, tp1, cn1, cn0);      // 雪庇の上面
        Band(cn0, cn1, le1, le0);      // 庇の下（影が溜まる ＝ 縁が締まる）
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（長さ {len:0.#}m / 高さ {h:0.#}m / "
                    + $"雪庇は{(cornRight ? "+X" : "-X")}側）");
}

// 樹氷 1 本。細い縦の塊を積む。**形ではなく「縦の線がある」ことだけが仕事。**
static void FrostTree(ObjMesh m, float x, float y, float z, float h, ref Rng r)
{
    int n = 5;
    float rr = 0.34f;
    for (int i = 0; i < n; i++)
    {
        float t0 = i / (float)n, t1 = (i + 1) / (float)n;
        float r0 = rr * (1f - t0 * 0.72f) * r.Range(0.85f, 1.2f);
        float r1 = rr * (1f - t1 * 0.72f) * r.Range(0.85f, 1.2f);
        float y0 = y + h * t0, y1 = y + h * t1;
        float ox = r.Range(-0.06f, 0.06f), oz = r.Range(-0.06f, 0.06f);
        for (int k = 0; k < 6; k++)
        {
            float a0 = MathF.Tau * k / 6, a1 = MathF.Tau * (k + 1) / 6;
            m.AddPolygon(
                new V3(x + MathF.Cos(a0) * r0, y0, z + MathF.Sin(a0) * r0),
                new V3(x + MathF.Cos(a1) * r0, y0, z + MathF.Sin(a1) * r0),
                new V3(x + ox + MathF.Cos(a1) * r1, y1, z + oz + MathF.Sin(a1) * r1),
                new V3(x + ox + MathF.Cos(a0) * r1, y1, z + oz + MathF.Sin(a0) * r1));
        }
        x += ox; z += oz;
    }
}

// 遠景の山脈。**板ポリを 3 層。**触れない・音響に入らない。
//   ★夜空を背にした黒い稜線だけが仕事。細部は霧（14→70m）で消える。
static void BuildSnowRange(string path, int seed)
{
    var r = new Rng(seed);
    var m = new ObjMesh();
    m.Group("SnowFar", "Snow_Far");

    float[] dist = { 90f, 130f, 180f };
    float[] high = { 26f, 44f, 66f };
    for (int layer = 0; layer < 3; layer++)
    {
        float d = dist[layer], hmax = high[layer];
        // 山並みの折れ線を 1 周ぶん作る。
        int n = 40;
        for (int i = 0; i < n; i++)
        {
            float a0 = MathF.Tau * i / n, a1 = MathF.Tau * (i + 1) / n;
            float y0 = hmax * (0.35f + 0.65f * MathF.Abs(Noise.Fbm(
                new V3(MathF.Cos(a0) * 30f, layer * 17f, MathF.Sin(a0) * 30f), 0.06f, 3)));
            float y1 = hmax * (0.35f + 0.65f * MathF.Abs(Noise.Fbm(
                new V3(MathF.Cos(a1) * 30f, layer * 17f, MathF.Sin(a1) * 30f), 0.06f, 3)));
            V3 A = new(MathF.Cos(a0) * d, -4f, MathF.Sin(a0) * d);
            V3 B = new(MathF.Cos(a1) * d, -4f, MathF.Sin(a1) * d);
            V3 C = new(MathF.Cos(a1) * d, y1, MathF.Sin(a1) * d);
            V3 D = new(MathF.Cos(a0) * d, y0, MathF.Sin(a0) * d);
            m.AddPolygon(A, B, C, D);
        }
    }
    _ = r;
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（遠景の山脈 3 層 / 90・130・180m）");
}

static void BuildSnow(string path)
{
    var m = new ObjMesh();

    m.Group("SnowGround", "Snow_Ground");
    V3 X = new(1, 0, 0), Z = new(0, 0, 1);
    // 雪原。長い波長のうねりだけ。歩く面なので振幅は控えめ。
    DisplacedPlane(m, new V3(-55, 0, -55), Z, X, 110f, 110f, 2.5f, 0.55f, 0.021f);

    m.Group("SnowRidge", "Snow_Ridge");
    SnowRidge(m, new V3(-30f, 0f, -12.75f), new V3(10f, 6f, -11.25f), true);
    SnowRidge(m, new V3(13.25f, 0f, -9f), new V3(14.75f, 8f, 21f), false);
    SnowRidge(m, new V3(-17f, 0f, 23.25f), new V3(9f, 5f, 24.75f), true);

    m.Group("SnowRock", "Snow_Rock");
    Boulder(m, new V3(6f, 1f, -20f), new V3(3f, 2f, 3f), 0.22f);
    Boulder(m, new V3(-14f, 1.2f, 4f), new V3(2.5f, 2.4f, 2.5f), 0.20f);

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount} / 面 {m.Faces.Count}");
}

// 草原ほか全ステージに置く扉。BellGameStages.MakeDoorFrame と同じ寸法。
//   原点に作るので、各ステージが ExitDoor の位置へ置く。
static void BuildDoorProp(string path)
{
    const float W = 1.1f, H = 2.2f, T = 0.25f;
    var m = new ObjMesh();

    m.Group("DoorFrame", "Stone_Wall");
    // 柱を少しすぼめる（下太・上細）。
    Taper(m, new V3(-(W * 0.5f + T * 0.5f), 0, 0), H, T * 1.15f, T * 0.9f);
    Taper(m, new V3(W * 0.5f + T * 0.5f, 0, 0), H, T * 1.15f, T * 0.9f);
    // まぐさ。前後に少し出す。
    m.AddBox(new V3(-(W * 0.5f + T * 1.1f), H, -T * 0.7f),
             new V3(W * 0.5f + T * 1.1f, H + T, T * 0.7f));

    // 敷居。すずめの戸締まりの扉のように、野に一枚だけ立っている絵にしたいので、
    // 枠の足元を石の板で受ける（地面に刺さっているように見せない）。
    m.AddBox(new V3(-(W * 0.5f + T * 1.4f), -0.06f, -T * 1.1f),
             new V3(W * 0.5f + T * 1.4f, 0.03f, T * 1.1f));

    // ★板は入れない。物理で蝶番まわりに回る別オブジェクト（Prop_DoorSlab.obj）になるため。
    //   枠に固定した板を描くと、開いた扉と二重に見える。

    m.Write(path, "StageModels.mtl");
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}");
}

// ベル。**手持ちの鈴**。世界に在るほうも同じ物が浮いて回っている。
//   ★原点は**鐘の胴の中心**。各ステージは音源の座標へそのまま置く
//     （音源は y=1.2 に置いてあるので、見えている物と音の出どころが一致する）。
//   ★取っ手は**平たい輪**にしてある。挽き物の握りだと回転軸まわりに対称で、
//     ゆっくり回しても回っていることが分からない。輪なら向きが読める。
static void BuildBellProp(string path)
{
    var m = new ObjMesh();
    const int Seg = 24;
    const float Mouth = -0.10f;     // 口
    const float Crown = 0.08f;      // 冠（胴の中心が原点に来る高さ）

    // 手持ちの寸法。口径 0.15m ほど。
    var outer = new (float t, float r)[]
    {
        (0.00f, 0.075f), (0.06f, 0.074f), (0.14f, 0.067f), (0.26f, 0.057f),
        (0.40f, 0.050f), (0.55f, 0.045f), (0.70f, 0.041f), (0.80f, 0.039f),
        (0.87f, 0.032f), (0.94f, 0.023f), (1.00f, 0.019f),
    };

    m.Group("Bell", "Bell_Bronze");
    float h = Crown - Mouth;

    var oPts = new V3[outer.Length][];
    var iPts = new V3[outer.Length][];
    for (int k = 0; k < outer.Length; k++)
    {
        float y = Mouth + outer[k].t * h;
        float thick = 0.010f * (1f - outer[k].t) + 0.004f * outer[k].t;
        oPts[k] = CircleRing(0f, y, 0f, outer[k].r, Seg);
        iPts[k] = CircleRing(0f, y, 0f, MathF.Max(0.004f, outer[k].r - thick), Seg);
    }
    for (int k = 0; k < outer.Length - 1; k++)
        for (int s = 0; s < Seg; s++)
        {
            int s2 = (s + 1) % Seg;
            m.AddPolygon(oPts[k][s], oPts[k + 1][s], oPts[k + 1][s2], oPts[k][s2]);   // 外
            m.AddPolygon(iPts[k][s], iPts[k][s2], iPts[k + 1][s2], iPts[k + 1][s]);   // 内
        }
    for (int s = 0; s < Seg; s++)   // 口の縁
    {
        int s2 = (s + 1) % Seg;
        m.AddPolygon(oPts[0][s], oPts[0][s2], iPts[0][s2], iPts[0][s]);
    }
    for (int s = 0; s < Seg; s++)   // 冠の天面
    {
        int s2 = (s + 1) % Seg;
        int last = outer.Length - 1;
        m.AddPolygon(oPts[last][s], oPts[last][s2], iPts[last][s2], iPts[last][s]);
    }

    // 肩の飾り帯。回っていることが分かるよう、等間隔に刻みを入れる。
    m.Group("BellBand", "Bell_Bronze");
    float bandY = Mouth + h * 0.72f;
    for (int i = 0; i < 8; i++)
    {
        float a = MathF.Tau * i / 8f;
        float r = 0.043f;
        m.AddBox(new V3(MathF.Cos(a) * r - 0.006f, bandY - 0.010f, MathF.Sin(a) * r - 0.006f),
                 new V3(MathF.Cos(a) * r + 0.006f, bandY + 0.010f, MathF.Sin(a) * r + 0.006f));
    }

    // 首と、平たい輪の取っ手。輪は XY 面に置く＝Y 軸まわりに非対称なので回転が読める。
    m.Group("BellHandle", "Bell_Bronze");
    Prism(m, 0f, 0f, Crown, Crown + 0.022f, 0.019f, 0.014f, 12, 0f);
    const float LoopR = 0.046f, TubeR = 0.0085f;
    float loopY = Crown + 0.022f + LoopR;
    for (int a = 0; a < 20; a++)
    {
        float a0 = MathF.Tau * a / 20f, a1 = MathF.Tau * (a + 1) / 20f;
        for (int s = 0; s < 8; s++)
        {
            float b0 = MathF.Tau * s / 8f, b1 = MathF.Tau * (s + 1) / 8f;
            V3 P(float arc, float tube) => new(
                MathF.Cos(arc) * (LoopR + TubeR * MathF.Cos(tube)),
                loopY + MathF.Sin(arc) * (LoopR + TubeR * MathF.Cos(tube)),
                TubeR * MathF.Sin(tube));
            m.AddPolygon(P(a0, b0), P(a1, b0), P(a1, b1), P(a0, b1));
        }
    }

    // 舌。
    m.Group("BellClapper", "Bell_Bronze");
    m.AddBox(new V3(-0.005f, Mouth + 0.045f, -0.005f), new V3(0.005f, Crown - 0.012f, 0.005f));
    Blob(m, new V3(0f, Mouth + 0.038f, 0f), 0.020f, 12, 8);

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 640, 700, 24f, 12f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}");
}

// 扉の板。框（かまち）組みの木扉。
//   ★原点は**板の中心**。物理側は無スケールの GameObject に BoxCollider を持たせ、
//     これを子として付ける（スケールした transform に子を付けると潰れるため）。
//   ★蝶番は −X 側。取っ手は自由端の +X 側に付ける。
static void BuildDoorSlabProp(string path)
{
    const float W = 1.1f, H = 2.2f, T = 0.08f;
    const float Stile = 0.14f;   // 縦框
    const float Rail = 0.16f;    // 横桟
    const float PanelT = 0.030f; // 鏡板の落ち込み

    var m = new ObjMesh();
    m.Group("DoorSlab", "Wood_Door");

    float x0 = -W * 0.5f, x1 = W * 0.5f;
    float y0 = -H * 0.5f, y1 = H * 0.5f;
    float z0 = -T * 0.5f, z1 = T * 0.5f;
    float midY = y0 + H * 0.46f;   // 中桟。やや下寄りが落ち着く

    // 縦框 2 本
    m.AddBox(new V3(x0, y0, z0), new V3(x0 + Stile, y1, z1));
    m.AddBox(new V3(x1 - Stile, y0, z0), new V3(x1, y1, z1));
    // 横桟 3 本（上・中・下）
    m.AddBox(new V3(x0 + Stile, y1 - Rail, z0), new V3(x1 - Stile, y1, z1));
    m.AddBox(new V3(x0 + Stile, midY - Rail * 0.5f, z0), new V3(x1 - Stile, midY + Rail * 0.5f, z1));
    m.AddBox(new V3(x0 + Stile, y0, z0), new V3(x1 - Stile, y0 + Rail * 1.3f, z1));

    // 鏡板 2 枚（框より薄く、少し奥へ落とす）
    m.AddBox(new V3(x0 + Stile, midY + Rail * 0.5f, z0 + PanelT),
             new V3(x1 - Stile, y1 - Rail, z1 - PanelT));
    m.AddBox(new V3(x0 + Stile, y0 + Rail * 1.3f, z0 + PanelT),
             new V3(x1 - Stile, midY - Rail * 0.5f, z1 - PanelT));

    // 取っ手。自由端（+X）側。前後に出す。
    m.Group("DoorKnob", "Bell_Bronze");
    float kx = x1 - Stile * 0.5f;
    Blob(m, new V3(kx, midY + 0.16f, z1 + 0.045f), 0.036f, 12, 8);
    Blob(m, new V3(kx, midY + 0.16f, z0 - 0.045f), 0.036f, 12, 8);
    m.AddBox(new V3(kx - 0.012f, midY + 0.148f, z0 - 0.05f),
             new V3(kx + 0.012f, midY + 0.172f, z1 + 0.05f));

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 520, 760, 28f, 8f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}");
}

// 火鉢。神殿と洞窟の灯り。
//   ★見た目だけ。コライダーを付けないので音響には入らない。
//   屋根で閉じた空間は開口 1.1m しか光が入らず、置かないと中が見えない。
//   原点は地面（y=0）。
static void BuildBrazierProp(string path)
{
    var m = new ObjMesh();

    m.Group("BrazierStand", "Stone_Wall");
    // 三脚。内へ傾いた 3 本。
    for (int i = 0; i < 3; i++)
    {
        float a = MathF.Tau * i / 3f;
        float bx = MathF.Cos(a) * 0.26f, bz = MathF.Sin(a) * 0.26f;
        float tx = MathF.Cos(a) * 0.10f, tz = MathF.Sin(a) * 0.10f;
        LegQuad(m, new V3(bx, 0f, bz), new V3(tx, 0.72f, tz), 0.055f);
    }
    // 台座と、鉢の受け。
    m.AddBox(new V3(-0.30f, 0f, -0.30f), new V3(0.30f, 0.06f, 0.30f));
    Prism(m, 0f, 0f, 0.72f, 0.80f, 0.12f, 0.20f, 16, 0f);

    // 鉢。外面・内面・縁。
    m.Group("BrazierBowl", "Stone_Wall");
    var oOut = new[] { (0.80f, 0.20f), (0.88f, 0.28f), (0.98f, 0.33f) };
    var oIn = new[] { (0.82f, 0.15f), (0.89f, 0.24f), (0.98f, 0.30f) };
    for (int k = 0; k < oOut.Length - 1; k++)
    {
        Prism(m, 0f, 0f, oOut[k].Item1, oOut[k + 1].Item1, oOut[k].Item2, oOut[k + 1].Item2, 16, 0f);
        Prism(m, 0f, 0f, oIn[k + 1].Item1, oIn[k].Item1, oIn[k + 1].Item2, oIn[k].Item2, 16, 0f);
    }
    var rimO = CircleRing(0f, 0.98f, 0f, 0.33f, 16);
    var rimI = CircleRing(0f, 0.98f, 0f, 0.30f, 16);
    for (int s = 0; s < 16; s++)
    {
        int s2 = (s + 1) % 16;
        m.AddPolygon(rimO[s], rimO[s2], rimI[s2], rimI[s]);
    }

    // 炭。自己発光のマテリアルを当てる。
    m.Group("BrazierCoals", "Fire_Coals");
    Blob(m, new V3(0f, 0.90f, 0f), 0.19f, 14, 8);

    m.Write(path, "StageModels.mtl");
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}");
}

// ───────────────────────────────────────────────────────────────
// 部品
// ───────────────────────────────────────────────────────────────

// 傾いた角柱の脚（下 a から上 b へ）。
static void LegQuad(ObjMesh m, V3 a, V3 b, float r)
{
    V3 axis = (b - a).Normalized;
    V3 side = V3.Cross(axis, new V3(0, 0, 1)).Normalized;
    if (side.Length < 0.5f) side = V3.Cross(axis, new V3(1, 0, 0)).Normalized;
    V3 up = V3.Cross(side, axis).Normalized;
    for (int s = 0; s < 4; s++)
    {
        float t0 = MathF.Tau * s / 4f, t1 = MathF.Tau * (s + 1) / 4f;
        V3 D(float t) => side * (MathF.Cos(t) * r) + up * (MathF.Sin(t) * r);
        m.AddPolygon(a + D(t0), b + D(t0), b + D(t1), a + D(t1));
    }
}

// 水平な円環の頂点列。
static V3[] CircleRing(float cx, float y, float cz, float r, int seg)
{
    var a = new V3[seg];
    for (int s = 0; s < seg; s++)
    {
        float th = MathF.Tau * s / seg;
        a[s] = new V3(cx + MathF.Cos(th) * r, y, cz + MathF.Sin(th) * r);
    }
    return a;
}

// 単純な球。
static void Blob(ObjMesh m, V3 c, float r, int seg, int ring)
{
    var pts = new V3[ring + 1][];
    for (int i = 0; i <= ring; i++)
    {
        float phi = MathF.PI * i / ring;
        pts[i] = new V3[seg];
        for (int s = 0; s < seg; s++)
        {
            float th = MathF.Tau * s / seg;
            pts[i][s] = new V3(c.X + MathF.Sin(phi) * MathF.Cos(th) * r,
                               c.Y + MathF.Cos(phi) * r,
                               c.Z + MathF.Sin(phi) * MathF.Sin(th) * r);
        }
    }
    for (int i = 0; i < ring; i++)
        for (int s = 0; s < seg; s++)
        {
            int s2 = (s + 1) % seg;
            m.AddPolygon(pts[i][s], pts[i][s2], pts[i + 1][s2], pts[i + 1][s]);
        }
}

// 矩形の面を cell 刻みに割り、法線方向へ fBm で押し出す。
//   u, v は面内の単位ベクトル。**cross(u,v) が押し出したい向き**になるよう渡すこと。
//   skip に渡すのは四角形の中心。true を返した区画は出さない（＝開口）。
static void DisplacedPlane(ObjMesh m, V3 origin, V3 u, V3 v, float uLen, float vLen,
                           float cell, float amp, float freq, Func<V3, bool> skip = null)
{
    int nu = Math.Max(1, (int)MathF.Round(uLen / cell));
    int nv = Math.Max(1, (int)MathF.Round(vLen / cell));
    V3 n = V3.Cross(u, v).Normalized;

    var pts = new V3[nu + 1, nv + 1];
    for (int i = 0; i <= nu; i++)
        for (int j = 0; j <= nv; j++)
        {
            V3 p = origin + u * (uLen * i / nu) + v * (vLen * j / nv);
            // 縁は動かさない。隣の面と継ぎ目が開くのを防ぐ。
            float e = MathF.Min(MathF.Min(i, nu - i) / (float)nu,
                                MathF.Min(j, nv - j) / (float)nv);
            float taper = Math.Clamp(e * 7f, 0f, 1f);
            pts[i, j] = p + n * (Noise.Fbm(p, freq) * amp * taper);
        }

    for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++)
        {
            if (skip != null)
            {
                V3 c = origin + u * (uLen * (i + 0.5f) / nu) + v * (vLen * (j + 0.5f) / nv);
                if (skip(c)) continue;
            }
            m.AddPolygon(pts[i, j], pts[i + 1, j], pts[i + 1, j + 1], pts[i, j + 1]);
        }
}

// 雪の稜線。音響の箱（min..max）の上辺に稜を合わせ、裾だけ外へ広げる。
static void SnowRidge(ObjMesh m, V3 min, V3 max, bool alongX)
{
    float h = max.Y - min.Y;
    float skirt = h * 0.65f;                       // 吹き溜まりの広がり
    float lo = alongX ? min.X : min.Z, hi = alongX ? max.X : max.Z;
    float wc = alongX ? (min.Z + max.Z) * 0.5f : (min.X + max.X) * 0.5f;
    float hw = (alongX ? (max.Z - min.Z) : (max.X - min.X)) * 0.5f + skirt;

    int nl = Math.Max(2, (int)MathF.Round((hi - lo) / 1.6f));
    const int Nw = 14;
    var pts = new V3[nl + 1, Nw + 1];
    for (int i = 0; i <= nl; i++)
    {
        float l = lo + (hi - lo) * i / nl;
        for (int j = 0; j <= Nw; j++)
        {
            float t = j / (float)Nw * 2f - 1f;                 // -1..1
            float w = wc + t * hw;
            // 稜を中心にした山なり。端で 0、中心で h。
            float y = h * MathF.Pow(MathF.Max(0f, 1f - MathF.Abs(t)), 1.5f);
            V3 p = alongX ? new V3(l, y, w) : new V3(w, y, l);
            // 稜の高さは崩さず、斜面だけ荒らす（回折の縁を動かさないため）。
            float rough = (1f - MathF.Abs(t)) * MathF.Abs(t) * 2f;
            p = new V3(p.X, p.Y + Noise.Fbm(p, 0.09f) * 0.30f * rough, p.Z);
            if (i == 0 || i == nl) p = new V3(p.X, p.Y * 0.35f, p.Z);   // 端は寝かせる
            pts[i, j] = p;
        }
    }
    for (int i = 0; i < nl; i++)
        for (int j = 0; j < Nw; j++)
        {
            // 上向きに巻く。
            if (alongX) m.AddPolygon(pts[i, j], pts[i, j + 1], pts[i + 1, j + 1], pts[i + 1, j]);
            else m.AddPolygon(pts[i, j], pts[i + 1, j], pts[i + 1, j + 1], pts[i, j + 1]);
        }
}

// 岩塊。球を寸法へ潰してノイズで崩す。
static void Boulder(ObjMesh m, V3 center, V3 size, float amp)
{
    const int Seg = 18, Ring = 10;
    var pts = new V3[Ring + 1, Seg];
    for (int i = 0; i <= Ring; i++)
    {
        float phi = MathF.PI * i / Ring;
        for (int s = 0; s < Seg; s++)
        {
            float th = MathF.Tau * s / Seg;
            var dir = new V3(MathF.Sin(phi) * MathF.Cos(th), MathF.Cos(phi), MathF.Sin(phi) * MathF.Sin(th));
            var p = new V3(center.X + dir.X * size.X * 0.5f,
                           center.Y + dir.Y * size.Y * 0.5f,
                           center.Z + dir.Z * size.Z * 0.5f);
            float d = 1f + Noise.Fbm(p, 0.55f) * amp;
            pts[i, s] = new V3(center.X + (p.X - center.X) * d,
                               MathF.Max(0f, center.Y + (p.Y - center.Y) * d),
                               center.Z + (p.Z - center.Z) * d);
        }
    }
    for (int i = 0; i < Ring; i++)
        for (int s = 0; s < Seg; s++)
        {
            int s2 = (s + 1) % Seg;
            m.AddPolygon(pts[i, s], pts[i, s2], pts[i + 1, s2], pts[i + 1, s]);
        }
}

// 下太・上細の角柱。
static void Taper(ObjMesh m, V3 basePos, float h, float w0, float w1)
{
    for (int s = 0; s < 4; s++)
    {
        float a0 = MathF.Tau * s / 4f + MathF.PI / 4f;
        float a1 = MathF.Tau * (s + 1) / 4f + MathF.PI / 4f;
        float r0 = w0 * 0.7071f, r1 = w1 * 0.7071f;
        V3 b0 = new(basePos.X + MathF.Cos(a0) * r0, basePos.Y, basePos.Z + MathF.Sin(a0) * r0);
        V3 b1 = new(basePos.X + MathF.Cos(a1) * r0, basePos.Y, basePos.Z + MathF.Sin(a1) * r0);
        V3 t0 = new(basePos.X + MathF.Cos(a0) * r1, basePos.Y + h, basePos.Z + MathF.Sin(a0) * r1);
        V3 t1 = new(basePos.X + MathF.Cos(a1) * r1, basePos.Y + h, basePos.Z + MathF.Sin(a1) * r1);
        m.AddPolygon(b0, t0, t1, b1);
    }
}

// ───────────────────────────────────────────────────────────────
// ステージ 2：崩壊都市
//   BellGameStages.Stage2Ruins のブロックアウトと寸法を一致させてある。
//     Building(内寸 ix×iz, 高さ h, 壁厚 t=0.35, 戸口 1.1×2.4)
//     壁は ±Z が x 方向に inner.x+2t、±X が z 方向に inner.y。外寸 =(ix+0.7)×(iz+0.7)
//     屋根は y h..h+t
//
//   ★この世界で一番大事な絵の要請（WORLD_VISUAL.md §3-2）:
//     **屋根の有無が遠目に読めること。**
//     響く建物＝閉じている、響かない建物＝上が抜けて空が見える。
//     だから対比はシルエットで作る ── 屋根ありは**天端が水平に切り揃った**パラペット、
//     屋根なしは**天端がぎざぎざ**で鉄筋が突き出る。霧 80m 越しでも輪郭なら読める。
//
//   ★崩れは h から**上へ足すだけ**にしてある。**下へ削ってはいけない。**
//     音響の壁は y=0..h まで詰まっているので、天端を削ると
//     「向こうが見えているのに音が抜けてこない」嘘になる。
//     上へ足すぶんには「見た目より少し早く音が回り込む」だけで、こちらは気づかれない。
//
//   ★窓は貫通させない。壁面を内側へ 0.10m 彫った**行き止まりの窪み**にしてある。
//     穴を開けると「抜けて見えるのに音は通らない」── この世界の芯を壊す。
//     暗く見せるのは陰影ではなく**材質**（窪みの底だけ City_Deep）。
// ───────────────────────────────────────────────────────────────
// ステージ 2：**無人の現代都市**（2026-08-21 発注者の指示で崩壊都市から変更）
//
//   ★寸法は `BellGameStages.Stage2Ruins` のブロックアウトのまま使う。
//     Building(内寸 ix×iz, 高さ h, 壁厚 t=0.35, 戸口 1.4×3.0)
//     壁は ±Z が x 方向に inner.x+2t、±X が z 方向に inner.y。外寸 =(ix+0.7)×(iz+0.7)
//
//   ★**屋根の対比は捨てていない。**
//     WORLD_VISUAL §3-2 の芯「屋根の有無が遠目に読めること」は、
//     現代都市でもそのまま成立する ── 建物は屋根がある（＝響く）、
//     **壁に囲まれた駐車場・空き地は上が抜けている**（＝響かない）。
//     エンジンの規則（`2e63c46`：部屋には天井が要る）を作り替える必要はない。
//     フィクションだけ差し替わった。
//
//   ★崩れは作らない。無人だが**荒れてはいない**都市（発注者の決定）。
//     「静かで幻想的」を壊すので、瓦礫・鉄筋・折れた鉄骨は使わない。
//
//   ★上へ伸ばすのは前と同じ。音響の箱は h までで、見た目だけ高くする
//     （部屋グラフの格子が上限 4M を超えるため。C7）。
// ───────────────────────────────────────────────────────────────

// 無傷のビル。1 階が店先、上階は窓の格子、陸屋根にパラペットと設備。
static void BuildCityBlock(string path, float ix, float iz, float h, int doorSide,
                           float hVis, int seed)
{
    const float T = 0.35f;
    const float DW = 1.4f, DH = 3.0f;   // 戸口（扉の寸法と同じ）
    const float Relief = 0.10f;
    const float Pier = 0.44f;           // 1 階の柱型
    const float Band = 0.30f;           // 階の見切り

    float hx = ix * 0.5f, hz = iz * 0.5f;
    float ox = hx + T, oz = hz + T;
    var r = new Rng(seed);
    var m = new ObjMesh();

    V3 Pt(int side, float s, float y, float d) => side switch
    {
        0 => new V3(s, y, -oz + d),
        1 => new V3(s, y, oz - d),
        2 => new V3(-ox + d, y, s),
        _ => new V3(ox - d, y, s),
    };
    float Span(int side) => side < 2 ? ox : hz;

    void Box(int side, float sA, float sB, float yA, float yB, float dA, float dB)
    {
        if (sB - sA < 0.02f || yB - yA < 0.02f) return;
        var p = Pt(side, sA, yA, dA);
        var q = Pt(side, sB, yB, dB);
        m.AddBox(new V3(MathF.Min(p.X, q.X), MathF.Min(p.Y, q.Y), MathF.Min(p.Z, q.Z)),
                 new V3(MathF.Max(p.X, q.X), MathF.Max(p.Y, q.Y), MathF.Max(p.Z, q.Z)));
    }
    // 戸口を避ける。ブロックアウトが袖・袖・まぐさに割っているのと同じ割り方。
    void Emit(int side, float sA, float sB, float yA, float yB, float dA, float dB)
    {
        if (side != doorSide || sB <= -DW * 0.5f || sA >= DW * 0.5f || yA >= DH)
        { Box(side, sA, sB, yA, yB, dA, dB); return; }
        if (sA < -DW * 0.5f) Box(side, sA, MathF.Min(sB, -DW * 0.5f), yA, yB, dA, dB);
        if (sB > DW * 0.5f) Box(side, MathF.Max(sA, DW * 0.5f), sB, yA, yB, dA, dB);
        if (yB > DH) Box(side, MathF.Max(sA, -DW * 0.5f), MathF.Min(sB, DW * 0.5f),
                         DH, yB, dA, dB);
    }

    // ── 1 階。**店先。**窪みを深くして、開口が暗い穴に見えるようにする ──
    //   ★WORLD_VISUAL の「響く建物＝開口が暗い穴」はここが担う。
    float g1 = MathF.Min(h, DH + 0.55f);      // 1 階の高さ（まぐさの少し上まで）
    m.Group("CityDeep", "City_Deep");
    for (int side = 0; side < 4; side++)
        Emit(side, -Span(side), Span(side), 0f, g1, Relief * 1.8f, T);

    m.Group("CityFrame", "City_Concrete");
    for (int side = 0; side < 4; side++)
    {
        float sp = Span(side);
        // 柱型。店先の間口を割る。
        int cols = Math.Max(2, (int)MathF.Round(sp * 2f / 3.4f));
        for (int i = 0; i <= cols; i++)
        {
            float sv = -sp + sp * 2f * i / cols;
            Emit(side, MathF.Max(-sp, sv - Pier * 0.5f),
                       MathF.Min(sp, sv + Pier * 0.5f), 0f, g1, 0f, Relief * 1.8f);
        }
        // 幅木と、1 階と 2 階の見切り（庇）。
        Emit(side, -sp, sp, 0f, 0.22f, 0f, Relief * 1.8f);
        Box(side, -sp, sp, g1 - Band, g1 + 0.10f, -0.10f, T);
    }

    // ── 2 階から上。**窓はシェーダの格子が塗る**ので素の箱でよい ──
    if (hVis > g1 + 1f)
    {
        m.Group("CityTower", "City_Tower");
        float yb = g1 + 0.10f;
        float ySet = yb + (hVis - yb) * 0.68f;
        float sx = ox - 0.55f, sz = oz - 0.55f;
        void Shell(float x0, float x1, float z0, float z1, float y0, float y1)
        {
            if (y1 - y0 < 0.05f) return;
            m.AddBox(new V3(x0, y0, z0), new V3(x1, y1, z0 + T));
            m.AddBox(new V3(x0, y0, z1 - T), new V3(x1, y1, z1));
            m.AddBox(new V3(x0, y0, z0 + T), new V3(x0 + T, y1, z1 - T));
            m.AddBox(new V3(x1 - T, y0, z0 + T), new V3(x1, y1, z1 - T));
        }
        Shell(-ox, ox, -oz, oz, yb, ySet);
        m.AddBox(new V3(-ox, ySet, -oz), new V3(ox, ySet + 0.22f, oz));   // 段の床
        Shell(-sx, sx, -sz, sz, ySet + 0.22f, hVis);
        m.AddBox(new V3(-sx, hVis - 0.22f, -sz), new V3(sx, hVis, sz));   // 陸屋根

        // ── 屋上。**天端が水平に切り揃うことが「屋根がある」の証** ──
        m.Group("CityFrame", "City_Concrete");
        void Parapet(float x0, float x1, float z0, float z1, float y, float ph)
        {
            const float Pw = 0.24f;
            m.AddBox(new V3(x0, y, z0), new V3(x1, y + ph, z0 + Pw));
            m.AddBox(new V3(x0, y, z1 - Pw), new V3(x1, y + ph, z1));
            m.AddBox(new V3(x0, y, z0 + Pw), new V3(x0 + Pw, y + ph, z1 - Pw));
            m.AddBox(new V3(x1 - Pw, y, z0 + Pw), new V3(x1, y + ph, z1 - Pw));
        }
        Parapet(-ox, ox, -oz, oz, ySet + 0.22f, 0.42f);
        Parapet(-sx, sx, -sz, sz, hVis, 0.55f);

        // 屋上の設備（高置水槽・室外機）。**輪郭に小さな凹凸を作るのが仕事。**
        //   平らな天端だけだと積み木に見える。
        m.Group("CityMetal", "City_Metal");
        float tw = MathF.Min(2.2f, sx * 0.5f);
        m.AddBox(new V3(-tw, hVis + 0.55f, -tw * 0.7f),
                 new V3(tw * 0.2f, hVis + 2.3f, tw * 0.5f));               // 水槽の脚と胴
        for (int i = 0; i < 3; i++)
        {
            float bx = r.Range(sx * 0.1f, sx - 1.2f);
            float bz = r.Range(-sz + 0.8f, sz - 1.2f);
            m.AddBox(new V3(bx, hVis, bz), new V3(bx + 0.9f, hVis + 0.7f, bz + 0.7f));
        }
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（外寸 {ix + 0.7f:0.#}×{iz + 0.7f:0.#}m / 音響 {h:0.#}m → 見た目 {hVis:0.#}m / 屋根あり）");
}

// 入れないビルの塊。**内部を作らない。**
//   ★街を詰めるために 8 棟置くが、全部を「入れる建物」にすると
//     部屋が 8 つ立って音響が重くなるうえ、探索の対象が散る。
//     遮蔽と反射だけが仕事なので、外皮だけでよい。
//   ★寸法は**外寸**で受ける（ブロックアウトの箱と同じ数字）。
static void BuildCityMass(string path, float w, float d, float hVis, int seed)
{
    const float T = 0.35f;
    const float Relief = 0.18f;         // 1 階の窪み。ここが暗い穴に見える
    const float Pier = 0.44f;
    float hx = w * 0.5f, hz = d * 0.5f;
    var r = new Rng(seed);
    var m = new ObjMesh();

    float g1 = 4.0f;                    // 1 階の高さ
    // ── 1 階。店先の窪み ──
    m.Group("CityDeep", "City_Deep");
    m.AddBox(new V3(-hx + Relief, 0f, -hz + Relief), new V3(hx - Relief, g1, hz - Relief));

    m.Group("CityFrame", "City_Concrete");
    // 柱型を 4 面に回す。間口を割って、のっぺりした箱に見せない。
    for (int side = 0; side < 4; side++)
    {
        bool alongX = side < 2;
        float sign = (side % 2 == 0) ? -1f : 1f;
        float sp = alongX ? hx : hz;
        int cols = Math.Max(2, (int)MathF.Round(sp * 2f / 3.4f));
        for (int i = 0; i <= cols; i++)
        {
            float s = -sp + sp * 2f * i / cols;
            float a = MathF.Max(-sp, s - Pier * 0.5f), b = MathF.Min(sp, s + Pier * 0.5f);
            if (alongX)
                m.AddBox(new V3(a, 0f, sign > 0 ? hz - Relief : -hz),
                         new V3(b, g1, sign > 0 ? hz : -hz + Relief));
            else
                m.AddBox(new V3(sign > 0 ? hx - Relief : -hx, 0f, a),
                         new V3(sign > 0 ? hx : -hx + Relief, g1, b));
        }
    }
    // 幅木と庇。1 階と上階の見切り。
    m.AddBox(new V3(-hx, 0f, -hz), new V3(hx, 0.22f, hz));
    m.AddBox(new V3(-hx - 0.10f, g1 - 0.30f, -hz - 0.10f), new V3(hx + 0.10f, g1 + 0.10f, hz + 0.10f));

    // ── 上階。**窓はシェーダの格子が塗る**ので素の塊でよい ──
    m.Group("CityTower", "City_Tower");
    float yb = g1 + 0.10f;
    float ySet = yb + (hVis - yb) * 0.70f;
    float sx = hx - 0.55f, sz = hz - 0.55f;
    m.AddBox(new V3(-hx, yb, -hz), new V3(hx, ySet, hz));
    m.AddBox(new V3(-hx, ySet, -hz), new V3(hx, ySet + 0.22f, hz));      // 段の床
    m.AddBox(new V3(-sx, ySet + 0.22f, -sz), new V3(sx, hVis, sz));

    // ── 屋上。**天端が水平に切り揃うことが「屋根がある」の証** ──
    m.Group("CityFrame", "City_Concrete");
    void Parapet(float x0, float x1, float z0, float z1, float y, float ph)
    {
        const float Pw = 0.24f;
        m.AddBox(new V3(x0, y, z0), new V3(x1, y + ph, z0 + Pw));
        m.AddBox(new V3(x0, y, z1 - Pw), new V3(x1, y + ph, z1));
        m.AddBox(new V3(x0, y, z0 + Pw), new V3(x0 + Pw, y + ph, z1 - Pw));
        m.AddBox(new V3(x1 - Pw, y, z0 + Pw), new V3(x1, y + ph, z1 - Pw));
    }
    Parapet(-hx, hx, -hz, hz, ySet + 0.22f, 0.42f);
    Parapet(-sx, sx, -sz, sz, hVis, 0.55f);

    // 屋上の設備。輪郭に小さな凹凸を作るのが仕事（平らだと積み木に見える）。
    m.Group("CityMetal", "City_Metal");
    float tw = MathF.Min(2.0f, sx * 0.4f);
    m.AddBox(new V3(-tw, hVis + 0.55f, -tw * 0.7f), new V3(tw * 0.2f, hVis + 2.2f, tw * 0.5f));
    for (int i = 0; i < 3; i++)
    {
        float bx = r.Range(sx * 0.1f, sx - 1.2f);
        float bz = r.Range(-sz + 0.8f, sz - 1.2f);
        m.AddBox(new V3(bx, hVis, bz), new V3(bx + 0.9f, hVis + 0.7f, bz + 0.7f));
    }
    _ = T;

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（外寸 {w:0.#}×{d:0.#}m / 見た目 {hVis:0.#}m / 入れない塊）");
}

// 公園の地面。x -35..-13 / z 24..44。**開けた場所**なので建物は置かない。
//   ★舗装より 8cm 上げて縁石を回す。都市の中で「ここだけ地面が違う」が
//     遠目に読めるのが仕事。音響には触らない（コライダーを付けない）。
static void BuildCityPark(string path, int seed)
{
    // ★1.7 倍に広げた（発注者の指示）。440 → 756 m²。
    //   南の縁 z=17 は、段2 の建物（z 2..16）から **1m 空けてある** ── 接着させない。
    const float X0 = -35f, X1 = -7f, Z0 = 17f, Z1 = 44f;
    const float Y = 0.09f, Kerb = 0.5f;
    var r = new Rng(seed);
    var m = new ObjMesh();

    m.Group("ParkGrass", "Park_Grass");
    m.AddPolygon(new V3(X0 + Kerb, Y, Z0 + Kerb), new V3(X0 + Kerb, Y, Z1 - Kerb),
                 new V3(X1 - Kerb, Y, Z1 - Kerb), new V3(X1 - Kerb, Y, Z0 + Kerb));
    m.Group("ParkKerb", "City_Concrete");
    m.AddBox(new V3(X0, 0f, Z0), new V3(X1, Y, Z0 + Kerb));
    m.AddBox(new V3(X0, 0f, Z1 - Kerb), new V3(X1, Y, Z1));
    m.AddBox(new V3(X0, 0f, Z0 + Kerb), new V3(X0 + Kerb, Y, Z1 - Kerb));
    m.AddBox(new V3(X1 - Kerb, 0f, Z0 + Kerb), new V3(X1, Y, Z1 - Kerb));
    // ドームへの敷石。南の開口へ真っ直ぐ延ばす（歩く先が目で分かる）。
    m.Group("ParkPath", "City_Walk");
    // ドームは中心 (-21, 30.5)。開口は南なので、東の通りから入って南へ回る道にする。
    m.AddPolygon(new V3(-22.6f, Y + 0.01f, Z0), new V3(-22.6f, Y + 0.01f, 23.5f),
                 new V3(-19.4f, Y + 0.01f, 23.5f), new V3(-19.4f, Y + 0.01f, Z0));
    _ = r;
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（{X1 - X0:0.#}×{Z1 - Z0:0.#}m の公園）");
}

// 公園のドーム。**ベルはこの中。**
//   ★開口は南（-Z）に 1 つだけ。§4.3 の崩しをここが引き継ぐ ──
//     外に漏れる音はベルからではなく**開口から**来るので、
//     南から聞こえるのに公園への入口は東、という嘘が成立する。
//   ★半径 7m の半球 = 718m³。現行 BldgB(720m³) とほぼ同じなので、
//     RT60 1.5s / 臨界距離 1.26m の狙いがそのまま移せる。
//   ★音響側は箱で近似してもらう（多角形の筒 ＋ 天板）。
//     ここが出すのは見た目だけ。**焦点効果はエンジンが模擬しない。**
//
//   ★作り直しの要点（2026-08-22）
//     前は「腰壁 ＋ つるっとした殻」だけで、丸い塊にしか見えなかった。
//     ドームらしさは曲面ではなく**構造**が出す:
//       1. 基壇 ── 地面に直に置くと生えているように見える
//       2. 稜（リブ）── 経線に沿った太い骨。**大きさの手がかり**になる
//       3. 頂塔 ── 天辺の小さな塔。輪郭で「ドーム」と即座に読める
//       4. 入口の枠 ── 曲面に穴を開けただけだと、破れて見える
static void BuildCityDome(string path, float r, float h, int seed)
{
    const int Seg = 48;                 // 周の分割（リブが 8 本なので 48 = 8×6）
    const int Ring = 10;                // 高さの分割
    const int Ribs = 8;                 // 稜の本数
    const float T = 0.30f;              // 殻の厚み
    const float DW = 1.6f, DH = 3.2f;   // 開口
    const float Base = 1.2f;            // 腰壁
    const float Plinth = 0.35f;         // 基壇の高さ

    var rng = new Rng(seed);
    var m = new ObjMesh();

    // ★巻き方向。**この関数の面はすべてここを通す。**
    //   球面を経線・緯線で刻んで素直な順（下→隣→上隣→上）に並べると、
    //   法線が**内側**を向く ── OBJ の法線を実測して確認した
    //   （基壇・稜・頂塔は 100% 内向き、殻は表裏が入れ替わっていた）。
    //   外から見ると面が無いように見え、透けて見える。逆順にして外へ向ける。
    void Face(V3 a, V3 b, V3 c, V3 d) => m.AddPolygon(d, c, b, a);
    void Tri(V3 a, V3 b, V3 c) => m.AddPolygon(c, b, a);

    float halfA = MathF.Asin(MathF.Min(0.95f, DW * 0.5f / r));
    float a0 = -MathF.PI * 0.5f - halfA, a1 = -MathF.PI * 0.5f + halfA;
    bool InDoor(float a)
    {
        float t = MathF.Atan2(MathF.Sin(a), MathF.Cos(a));
        return t > a0 && t < a1;
    }
    // 輪郭。腰壁から上は四分楕円。
    float RAt(float t) => r * MathF.Cos(t * MathF.PI * 0.5f);
    float YAt(float t) => Base + (h - Base) * MathF.Sin(t * MathF.PI * 0.5f);

    // ── ① 基壇。2 段の低い円盤 ──────────────────────
    m.Group("DomePlinth", "City_Concrete");
    void Disc(float rad, float y0, float y1)
    {
        for (int i = 0; i < Seg; i++)
        {
            float aa = MathF.Tau * i / Seg, ab = MathF.Tau * (i + 1) / Seg;
            V3 A = new(MathF.Cos(aa) * rad, y0, MathF.Sin(aa) * rad);
            V3 B = new(MathF.Cos(ab) * rad, y0, MathF.Sin(ab) * rad);
            V3 C = new(MathF.Cos(ab) * rad, y1, MathF.Sin(ab) * rad);
            V3 D = new(MathF.Cos(aa) * rad, y1, MathF.Sin(aa) * rad);
            Face(A, B, C, D);                                  // 側面
            Tri(new V3(0f, y1, 0f), D, C);                    // 天面
        }
    }
    Disc(r + 1.5f, 0f, Plinth * 0.5f);
    Disc(r + 0.6f, Plinth * 0.5f, Plinth);

    // ── ② 殻 ────────────────────────────────────────
    m.Group("DomeShell", "City_Concrete");
    for (int i = 0; i < Seg; i++)
    {
        float aa = MathF.Tau * i / Seg, ab = MathF.Tau * (i + 1) / Seg;
        float ca = MathF.Cos(aa), sa = MathF.Sin(aa), cb = MathF.Cos(ab), sb = MathF.Sin(ab);
        bool door = InDoor((aa + ab) * 0.5f);
        float floorY = Plinth;

        // 腰壁
        float y0 = door ? DH : floorY;
        if (y0 < Base)
        {
            Face(new V3(ca * r, y0, sa * r), new V3(cb * r, y0, sb * r),
                         new V3(cb * r, Base, sb * r), new V3(ca * r, Base, sa * r));
            float ri = r - T;
            Face(new V3(ca * ri, Base, sa * ri), new V3(cb * ri, Base, sb * ri),
                         new V3(cb * ri, y0, sb * ri), new V3(ca * ri, y0, sa * ri));
        }
        // 曲面
        for (int k = 0; k < Ring; k++)
        {
            float t0 = k / (float)Ring, t1 = (k + 1) / (float)Ring;
            float y0d = YAt(t0), y1d = YAt(t1);
            if (door && y1d <= DH) continue;
            float r0 = RAt(t0), r1 = RAt(t1);
            Face(new V3(ca * r0, y0d, sa * r0), new V3(cb * r0, y0d, sb * r0),
                         new V3(cb * r1, y1d, sb * r1), new V3(ca * r1, y1d, sa * r1));
            float e = T * 0.85f;
            Face(new V3(ca * (r1 - e), y1d, sa * (r1 - e)), new V3(cb * (r1 - e), y1d, sb * (r1 - e)),
                         new V3(cb * (r0 - e), y0d, sb * (r0 - e)), new V3(ca * (r0 - e), y0d, sa * (r0 - e)));
        }
    }

    // ── ③ 稜（リブ）。**大きさの手がかりはこれが作る** ──
    //   経線に沿って太い骨を通す。つるっとした殻は、遠くから見ると
    //   大きさが分からない ── 骨が 8 本あれば「1 本ぶんの幅」で測れる。
    m.Group("DomeRib", "City_Deep");
    for (int b = 0; b < Ribs; b++)
    {
        float a = MathF.Tau * b / Ribs + MathF.PI * 0.5f;   // 開口を避けて配る
        if (InDoor(a)) continue;
        float ca = MathF.Cos(a), sa = MathF.Sin(a);
        float wa = 0.20f / r;                                // 骨の半幅（角度）
        float c0 = MathF.Cos(a - wa), s0 = MathF.Sin(a - wa);
        float c1 = MathF.Cos(a + wa), s1 = MathF.Sin(a + wa);
        for (int k = 0; k < Ring; k++)
        {
            float t0 = k / (float)Ring, t1 = (k + 1) / (float)Ring;
            float y0 = YAt(t0), y1 = YAt(t1);
            float r0 = RAt(t0) + 0.16f, r1 = RAt(t1) + 0.16f;   // 殻の外へ出す
            Face(new V3(c0 * r0, y0, s0 * r0), new V3(c1 * r0, y0, s1 * r0),
                         new V3(c1 * r1, y1, s1 * r1), new V3(c0 * r1, y1, s0 * r1));
            // 骨の側面。厚みが見えないと帯を貼っただけに見える。
            Face(new V3(c0 * (r0 - 0.16f), y0, s0 * (r0 - 0.16f)), new V3(c0 * r0, y0, s0 * r0),
                         new V3(c0 * r1, y1, s0 * r1), new V3(c0 * (r1 - 0.16f), y1, s0 * (r1 - 0.16f)));
            Face(new V3(c1 * r1, y1, s1 * r1), new V3(c1 * r0, y0, s1 * r0),
                         new V3(c1 * (r0 - 0.16f), y0, s1 * (r0 - 0.16f)), new V3(c1 * (r1 - 0.16f), y1, s1 * (r1 - 0.16f)));
        }
        // 腰壁の付け柱。骨が地面まで降りると、支えている感じが出る。
        Face(new V3(c0 * (r + 0.16f), Plinth, s0 * (r + 0.16f)), new V3(c1 * (r + 0.16f), Plinth, s1 * (r + 0.16f)),
                     new V3(c1 * (r + 0.16f), Base, s1 * (r + 0.16f)), new V3(c0 * (r + 0.16f), Base, s0 * (r + 0.16f)));
    }

    // ── ④ 頂塔。**輪郭で「ドーム」と読ませる** ────────
    m.Group("DomeLantern", "City_Concrete");
    float lr = 1.25f, ly = h;
    Disc2(lr, ly, ly + 1.6f);
    Disc2(lr + 0.28f, ly + 1.6f, ly + 1.85f);           // 笠
    // 頂の小さな丸屋根
    for (int k = 0; k < 4; k++)
    {
        float t0 = k / 4f, t1 = (k + 1) / 4f;
        float rr0 = lr * MathF.Cos(t0 * MathF.PI * 0.5f), rr1 = lr * MathF.Cos(t1 * MathF.PI * 0.5f);
        float yy0 = ly + 1.85f + 1.0f * MathF.Sin(t0 * MathF.PI * 0.5f);
        float yy1 = ly + 1.85f + 1.0f * MathF.Sin(t1 * MathF.PI * 0.5f);
        for (int i = 0; i < 16; i++)
        {
            float aa = MathF.Tau * i / 16, ab = MathF.Tau * (i + 1) / 16;
            Face(new V3(MathF.Cos(aa) * rr0, yy0, MathF.Sin(aa) * rr0),
                         new V3(MathF.Cos(ab) * rr0, yy0, MathF.Sin(ab) * rr0),
                         new V3(MathF.Cos(ab) * rr1, yy1, MathF.Sin(ab) * rr1),
                         new V3(MathF.Cos(aa) * rr1, yy1, MathF.Sin(aa) * rr1));
        }
    }
    void Disc2(float rad, float y0, float y1)
    {
        for (int i = 0; i < 16; i++)
        {
            float aa = MathF.Tau * i / 16, ab = MathF.Tau * (i + 1) / 16;
            Face(new V3(MathF.Cos(aa) * rad, y0, MathF.Sin(aa) * rad),
                         new V3(MathF.Cos(ab) * rad, y0, MathF.Sin(ab) * rad),
                         new V3(MathF.Cos(ab) * rad, y1, MathF.Sin(ab) * rad),
                         new V3(MathF.Cos(aa) * rad, y1, MathF.Sin(aa) * rad));
        }
    }

    // ── ⑤ 入口の枠。曲面に穴を開けただけだと**破れて見える** ──
    m.Group("DomeTrim", "City_Deep");
    foreach (float a in new[] { a0, a1 })
    {
        float c = MathF.Cos(a), s = MathF.Sin(a);
        float ro = r + 0.22f, ri = r - T - 0.06f;
        m.AddBox(new V3(MathF.Min(c * ro, c * ri) - 0.07f, Plinth, MathF.Min(s * ro, s * ri) - 0.07f),
                 new V3(MathF.Max(c * ro, c * ri) + 0.07f, DH + 0.28f, MathF.Max(s * ro, s * ri) + 0.07f));
    }
    // まぐさ
    m.AddBox(new V3(-DW * 0.5f - 0.3f, DH, -r - 0.25f), new V3(DW * 0.5f + 0.3f, DH + 0.28f, -r + T + 0.1f));
    _ = rng;

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（半径 {r:0.#}m / 高さ {h:0.#}m ＋頂塔 {2.85f:0.#}m / 容積 {2.0 / 3.0 * MathF.PI * r * r * r:0} m³ / 稜 {Ribs} 本 / 開口は南）");
}


// 壁に囲まれた駐車場・空き地。**上が抜けている＝響かない。**
//   ★これが「屋根なし」の現代都市版。壁は立っているのに部屋にならないので、
//     エンジンの規則をそのまま使いつつ、絵は無傷の都市のままでいられる。
//   ★天端は水平に切り揃えるが、**低い。**ビルと並べたとき、
//     「上が空へ抜けている」が一目で分かる高さに留める。
static void BuildCityLot(string path, float ix, float iz, float h, int doorSide, int seed)
{
    const float T = 0.35f;
    const float DW = 1.4f, DH = 3.0f;
    float hx = ix * 0.5f, hz = iz * 0.5f;
    float ox = hx + T, oz = hz + T;
    var r = new Rng(seed);
    var m = new ObjMesh();

    V3 Pt(int side, float s, float y, float d) => side switch
    {
        0 => new V3(s, y, -oz + d),
        1 => new V3(s, y, oz - d),
        2 => new V3(-ox + d, y, s),
        _ => new V3(ox - d, y, s),
    };
    float Span(int side) => side < 2 ? ox : hz;
    void Box(int side, float sA, float sB, float yA, float yB, float dA, float dB)
    {
        if (sB - sA < 0.02f || yB - yA < 0.02f) return;
        var p = Pt(side, sA, yA, dA);
        var q = Pt(side, sB, yB, dB);
        m.AddBox(new V3(MathF.Min(p.X, q.X), MathF.Min(p.Y, q.Y), MathF.Min(p.Z, q.Z)),
                 new V3(MathF.Max(p.X, q.X), MathF.Max(p.Y, q.Y), MathF.Max(p.Z, q.Z)));
    }
    void Emit(int side, float sA, float sB, float yA, float yB, float dA, float dB)
    {
        if (side != doorSide || sB <= -DW * 0.5f || sA >= DW * 0.5f || yA >= DH)
        { Box(side, sA, sB, yA, yB, dA, dB); return; }
        if (sA < -DW * 0.5f) Box(side, sA, MathF.Min(sB, -DW * 0.5f), yA, yB, dA, dB);
        if (sB > DW * 0.5f) Box(side, MathF.Max(sA, DW * 0.5f), sB, yA, yB, dA, dB);
        if (yB > DH) Box(side, MathF.Max(sA, -DW * 0.5f), MathF.Min(sB, DW * 0.5f), DH, yB, dA, dB);
    }

    // 囲いの壁。音響の箱と同じ高さまで。
    m.Group("CityFrame", "City_Concrete");
    for (int side = 0; side < 4; side++)
    {
        float sp = Span(side);
        Emit(side, -sp, sp, 0f, h, 0f, T);
        // 笠木（天端の帯）。**水平の線が通ると「壁」に見える。**
        //   ぎざぎざにすると廃墟に戻ってしまう。
        Box(side, -sp, sp, h, h + 0.14f, -0.06f, T + 0.06f);
    }
    // 壁の目地。縦の線を等間隔に入れて、コンクリの型枠に見せる。
    m.Group("CityDeep", "City_Deep");
    for (int side = 0; side < 4; side++)
    {
        float sp = Span(side);
        for (float s = -sp + 1.8f; s < sp - 0.4f; s += 1.8f)
            Emit(side, s, s + 0.06f, 0.2f, h - 0.2f, 0f, 0.04f);
    }

    // 天端の手すり（パイプ）。**空を背に細い線が出るので、上が抜けているのが読める。**
    m.Group("CityMetal", "City_Metal");
    for (int side = 0; side < 4; side++)
    {
        float sp = Span(side);
        foreach (float ry in new[] { h + 0.55f, h + 1.00f })
            Box(side, -sp, sp, ry, ry + 0.05f, T * 0.5f - 0.03f, T * 0.5f + 0.02f);
        for (float s = -sp; s <= sp - 0.1f; s += 2.4f)
            Box(side, s, s + 0.06f, h + 0.14f, h + 1.05f, T * 0.5f - 0.03f, T * 0.5f + 0.03f);
    }
    // 車止め。地面に低い塊を並べる。無人だが荒れていない、を出す小物。
    m.Group("CityRubble", "City_Rubble");
    for (float x = -hx + 1.6f; x < hx - 1.2f; x += 2.6f)
        for (float z = -hz + 2.2f; z < hz - 1.4f; z += 5.2f)
            m.AddBox(new V3(x, 0f, z), new V3(x + 1.6f, 0.14f, z + 0.18f));
    _ = r;

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（外寸 {ix + 0.7f:0.#}×{iz + 0.7f:0.#}m / 囲い {h:0.#}m / **上は空** ＝ 響かない）");
}

static void BuildRuin(string path, float ix, float iz, float h, int doorSide,
                      bool makeRoof, float hVis, int seed)
{
    const float T = 0.35f;              // 壁厚（ブロックアウトと一致）
    const float DW = 1.1f, DH = 2.4f;   // 戸口（同上）
    const float Relief = 0.10f;         // 枠と窪みの段差。**内側へ彫る**（外へ出すと箱をはみ出す）
    const float Mullion = 0.44f;        // 縦枠
    const float Band = 0.34f;           // 横梁

    float hx = ix * 0.5f, hz = iz * 0.5f;
    float ox = hx + T, oz = hz + T;     // 外面
    var r = new Rng(seed);
    var m = new ObjMesh();

    int rows = Math.Clamp((int)MathF.Round(h / 2.6f), 1, 3);

    // 面 side の (span, 高さ, 外面からの深さ) を世界座標へ。
    //   side 0=-Z 1=+Z 2=-X 3=+X。深さ d は外面で 0、内側へ増える。
    V3 Pt(int side, float s, float y, float d) => side switch
    {
        0 => new V3(s, y, -oz + d),
        1 => new V3(s, y, oz - d),
        2 => new V3(-ox + d, y, s),
        _ => new V3(ox - d, y, s),
    };
    // ±Z の壁は角まで回り込む。±X の壁はその内側に挟まる（ブロックアウトと同じ）。
    float Span(int side) => side < 2 ? ox : hz;

    void Box(int side, float sA, float sB, float yA, float yB, float dA, float dB)
    {
        if (sB - sA < 0.02f || yB - yA < 0.02f) return;
        var p = Pt(side, sA, yA, dA);
        var q = Pt(side, sB, yB, dB);
        m.AddBox(new V3(MathF.Min(p.X, q.X), MathF.Min(p.Y, q.Y), MathF.Min(p.Z, q.Z)),
                 new V3(MathF.Max(p.X, q.X), MathF.Max(p.Y, q.Y), MathF.Max(p.Z, q.Z)));
    }

    // 戸口の矩形を避けて置く。ブロックアウトが袖・袖・まぐさに割っているのと同じ割り方。
    void Emit(int side, float sA, float sB, float yA, float yB, float dA, float dB)
    {
        if (side != doorSide || sB <= -DW * 0.5f || sA >= DW * 0.5f || yA >= DH)
        { Box(side, sA, sB, yA, yB, dA, dB); return; }
        if (sA < -DW * 0.5f) Box(side, sA, MathF.Min(sB, -DW * 0.5f), yA, yB, dA, dB);
        if (sB > DW * 0.5f) Box(side, MathF.Max(sA, DW * 0.5f), sB, yA, yB, dA, dB);
        if (yB > DH) Box(side, MathF.Max(sA, -DW * 0.5f), MathF.Min(sB, DW * 0.5f),
                         DH, yB, dA, dB);
    }

    // ── 窪みの底（＝壁の芯）。暗い材質にして、これが窓に見える ──
    m.Group("CityDeep", "City_Deep");
    for (int side = 0; side < 4; side++)
        Emit(side, -Span(side), Span(side), 0f, h, Relief, T);

    // ── 躯体の枠。縦の柱と横の梁（WORLD_VISUAL のシルエット指定）──
    m.Group("CityFrame", "City_Concrete");
    for (int side = 0; side < 4; side++)
    {
        float sp = Span(side);
        int cols = Math.Max(2, (int)MathF.Round(sp * 2f / 2.6f));
        for (int i = 0; i <= cols; i++)
        {
            float sv = -sp + sp * 2f * i / cols;
            Emit(side, MathF.Max(-sp, sv - Mullion * 0.5f),
                       MathF.Min(sp, sv + Mullion * 0.5f), 0f, h, 0f, Relief);
        }
        for (int j = 0; j <= rows; j++)
        {
            float yh = h * j / rows;
            Emit(side, -sp, sp, MathF.Max(0f, yh - Band * 0.5f),
                                MathF.Min(h, yh + Band * 0.5f), 0f, Relief);
        }
    }

    if (makeRoof)
    {
        // 屋根スラブ。ブロックアウトの箱と同じ y h..h+T。
        m.Group("CityDeep", "City_Deep");
        m.AddBox(new V3(-ox, h, -oz), new V3(ox, h + T, oz));
        // パラペット。**コライダーより上なので飾り。**
        //   天端を水平に切り揃えるのが仕事 ── 屋根が残っていることを遠目に言う。
        m.Group("CityFrame", "City_Concrete");
        const float Pw = 0.28f, Ph = 0.46f;
        m.AddBox(new V3(-ox, h + T, -oz), new V3(ox, h + T + Ph, -oz + Pw));
        m.AddBox(new V3(-ox, h + T, oz - Pw), new V3(ox, h + T + Ph, oz));
        m.AddBox(new V3(-ox, h + T, -oz + Pw), new V3(-ox + Pw, h + T + Ph, oz - Pw));
        m.AddBox(new V3(ox - Pw, h + T, -oz + Pw), new V3(ox, h + T + Ph, oz - Pw));
    }
    else
    {
        // ── 崩れた天端。h から**上へ足すだけ**（下へ削らない。上のコメント参照）──
        m.Group("CityFrame", "City_Concrete");
        for (int side = 0; side < 4; side++)
        {
            float sp = Span(side);
            for (float s = -sp; s < sp - 0.05f;)
            {
                float w = MathF.Min(r.Range(0.55f, 1.30f), sp - s);
                float up = r.Range(0f, 0.62f);
                if (up > 0.06f) Box(side, s, s + w, h, h + up, 0f, T);
                s += w;
            }
        }
        // 折れた鉄筋。細い縦の線が空を背に立つ＝「上が抜けている」の駄目押し。
        m.Group("CityMetal", "City_Metal");
        for (int side = 0; side < 4; side++)
        {
            float sp = Span(side);
            int n = (int)(sp * 1.1f);
            for (int i = 0; i < n; i++)
            {
                float s = r.Range(-sp + 0.2f, sp - 0.25f);
                float d = r.Range(0.06f, T - 0.11f);
                Box(side, s, s + 0.05f, h, h + r.Range(0.22f, 0.95f), d, d + 0.05f);
            }
        }
        // 割れた床。**水平の段**（同じくシルエット指定）。頭上 2.4m より上に置く。
        m.Group("CityDeep", "City_Deep");
        float ys = MathF.Max(2.45f, h * 0.58f);
        for (float x = -hx; x < hx - 0.05f;)
        {
            float w = MathF.Min(r.Range(0.7f, 1.6f), hx - x);
            float reach = r.Range(0.15f, 0.55f) * iz;   // 折れた縁がぎざぎざになる
            m.AddBox(new V3(x, ys, -hz), new V3(x + w, ys + 0.22f, -hz + reach));
            x += w;
        }
    }

    // ── ここから上は**音響の箱より上**。純粋な飾り ────────────────
    //
    //   ★高さ h は音響の寸法で、RT60（720m³ → 1.5s）と臨界距離を決めている。
    //     部屋グラフの格子も全コライダーの AABB に張られるので、
    //     コライダーを 20m まで伸ばすと 200×84×244 ≒ 4.1M ボクセルで
    //     上限 4M を超えて**セルが粗くなる**（既知の C7）。だから箱は伸ばさない。
    //
    //   ★残る食い違いは「屋根の上を回り込む回折が、見た目より低い所で起きる」1 点だけ。
    //     音源もリスナーも地上 1.2〜1.6m なので直達経路は上層を通らない。
    //     「見えているのに聞こえない」に比べれば、桁違いに気づかれにくい。
    //
    //   ★窓はここでは作らない。シェーダの `_WindowAmount` が
    //     ワールド座標の格子で塗る（三角形 0 枚）。だから上層は素の箱でよい。
    void Shell(float x0, float x1, float z0, float z1, float y0, float y1, float wt)
    {
        if (y1 - y0 < 0.05f) return;
        m.AddBox(new V3(x0, y0, z0), new V3(x1, y1, z0 + wt));
        m.AddBox(new V3(x0, y0, z1 - wt), new V3(x1, y1, z1));
        m.AddBox(new V3(x0, y0, z0 + wt), new V3(x0 + wt, y1, z1 - wt));
        m.AddBox(new V3(x1 - wt, y0, z0 + wt), new V3(x1, y1, z1 - wt));
    }
    void Parapet(float x0, float x1, float z0, float z1, float y, float ph)
    {
        const float Pw = 0.26f;
        m.AddBox(new V3(x0, y, z0), new V3(x1, y + ph, z0 + Pw));
        m.AddBox(new V3(x0, y, z1 - Pw), new V3(x1, y + ph, z1));
        m.AddBox(new V3(x0, y, z0 + Pw), new V3(x0 + Pw, y + ph, z1 - Pw));
        m.AddBox(new V3(x1 - Pw, y, z0 + Pw), new V3(x1, y + ph, z1 - Pw));
    }

    if (makeRoof && hVis > h + T + 2f)
    {
        m.Group("CityTower", "City_Tower");
        float yb = h + T;
        // セットバック。上へ行くほど細らせないと、ただの長い箱に見える。
        float ySet = yb + (hVis - yb) * 0.62f;
        float sx = ox - 0.80f, sz = oz - 0.80f;
        Shell(-ox, ox, -oz, oz, yb, ySet, T);
        m.AddBox(new V3(-ox, ySet, -oz), new V3(ox, ySet + 0.26f, oz));   // 段の床
        Shell(-sx, sx, -sz, sz, ySet + 0.26f, hVis, T);
        m.AddBox(new V3(-sx, hVis - 0.26f, -sz), new V3(sx, hVis, sz));   // 屋上
        m.Group("CityFrame", "City_Concrete");
        Parapet(-ox, ox, -oz, oz, ySet + 0.26f, 0.42f);   // 段のところの手すり
        Parapet(-sx, sx, -sz, sz, hVis, 0.52f);           // 天端。**ここが水平に切り揃う**
    }
    else if (!makeRoof && hVis > h + 2f)
    {
        // ── 崩れ残った躯体。**2 面だけ**立ち上げる ────────────────
        //   ★4 面とも立てない。立てると「高い箱の中なのに響かない」になって、
        //     この世界の芯（屋根が無い＝響かない）と食い違う。
        //     2 面だけ残して残りを抜けば、空が見えて音が抜けるのが目で分かる。
        m.Group("CityTower", "City_Tower");
        int keepA = (doorSide + 1) % 4, keepB = (doorSide + 2) % 4;
        foreach (int side in new[] { keepA, keepB })
        {
            float sp = Span(side);
            for (float s = -sp; s < sp - 0.05f;)
            {
                float w = MathF.Min(r.Range(1.1f, 2.6f), sp - s);
                float topY = r.Range(h + 1.2f, hVis);
                Box(side, s, s + w, h, topY, 0f, T);
                s += w;
            }
        }
        // 折れた床が作る**水平の段**。抜けた側から見えるので、これが崩壊を語る。
        m.Group("CityDeep", "City_Deep");
        for (float y = h + 2.8f; y < hVis - 1f; y += 3.1f)
            for (float x = -hx; x < hx - 0.05f;)
            {
                float w = MathF.Min(r.Range(0.9f, 2.2f), hx - x);
                if (r.Next() < 0.42f) { x += w; continue; }        // 抜け落ちた区画
                float reach = r.Range(0.2f, 0.7f) * iz;
                m.AddBox(new V3(x, y, -hz), new V3(x + w, y + 0.22f, -hz + reach));
                x += w;
            }
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}" +
                      $"（外寸 {ix + 0.7f:0.#}×{iz + 0.7f:0.#}m / 音響 {h:0.#}m → 見た目 {hVis:0.#}m / " +
                      $"{(makeRoof ? "屋根あり" : "屋根なし")}）");
}

// 道路。**ステージの外を通す。**（2026-08-21 発注者の図で確定）
//   ステージ = x -35..35 / z -36..44。道はその外側、西と南に L 字で回る。
//
//   ★中を通さない理由は絵と仕組みの両方。
//     絵: 街区の**内側**に立っている、という場所が決まる。道が見えるのに行けないので、
//         「世界は続いているが、行けるのはここまで」が柵を見せずに伝わる（§7.2）。
//     仕組み: 道がステージ内を横切らなくなったので、**建物を街路から退ける必要が消えた。**
//         ブロックアウトの BldgA〜D をそのまま使える（連絡板 M9 の移動依頼はここで取り下げ）。
//
//   ★歩けない場所なので**コライダーは付けない**（配置側で false）。
//     見えるが行けない、は歩行の境界（LayoutWalker の矩形）が受け持つ。
static void BuildCityRoads(string path, int seed)
{
    // ステージの外周。ここより外が道。
    const float SX = 35f, SZ0 = -36f, SZ1 = 44f;
    const float Walk = 2.2f;            // 歩道
    const float Half = 4.0f;            // 車道の半幅（8m）
    const float YRoad = 0.02f, YWalk = 0.07f, YLine = 0.034f;

    // 車道の中心線。ステージの縁から 歩道 + 車道半幅 だけ外へ。
    const float WestX = -SX - Walk - Half;          // 西の道の中心 x
    const float SouthZ = SZ0 - Walk - Half;         // 南の道の中心 z
    // 道の長さ。霧が 80m で終わるので、そこまで届けば足りる。
    const float EndZ = 62f, EndX = 58f;

    var r = new Rng(seed);
    var m = new ObjMesh();

    void Quad(float x0, float x1, float z0, float z1, float y)
    {
        if (x1 - x0 < 0.03f || z1 - z0 < 0.03f) return;
        m.AddPolygon(new V3(x0, y, z0), new V3(x0, y, z1),
                     new V3(x1, y, z1), new V3(x1, y, z0));
    }

    // ── 車道 ─────────────────────────────────────────
    //   西の道は南の道の手前で止め、交差点は 1 枚で敷く（二重に敷くと z 争い）。
    m.Group("CityRoad", "City_Road");
    float cz0 = SouthZ - Half, cz1 = SouthZ + Half;   // 交差点の z 範囲
    float cx0 = WestX - Half, cx1 = WestX + Half;     // 同 x 範囲
    Quad(cx0, cx1, cz0, cz1, YRoad);                  // 交差点
    Quad(cx0, cx1, cz1, EndZ, YRoad);                 // 西の道（交差点より北）
    Quad(cx0, cx1, -EndZ, cz0, YRoad);                // 同（南）
    Quad(cx1, EndX, cz0, cz1, YRoad);                 // 南の道（交差点より東）
    Quad(-EndX, cx0, cz0, cz1, YRoad);                // 同（西）

    // ── 歩道。ステージ側と反対側に 1 本ずつ ──────────────
    m.Group("CityWalk", "City_Walk");
    void Kerb(float x0, float x1, float z0, float z1, bool alongZ, float inner)
    {
        Quad(x0, x1, z0, z1, YWalk);
        // 縁石の立ち上がり。車道側の辺に立てる。
        if (alongZ)
            m.AddPolygon(new V3(inner, YRoad, z0), new V3(inner, YWalk, z0),
                         new V3(inner, YWalk, z1), new V3(inner, YRoad, z1));
        else
            m.AddPolygon(new V3(x0, YRoad, inner), new V3(x1, YRoad, inner),
                         new V3(x1, YWalk, inner), new V3(x0, YWalk, inner));
    }
    // 西の道：ステージ側（東）と反対側（西）
    Kerb(cx1, cx1 + Walk, cz1, EndZ, true, cx1);
    Kerb(cx0 - Walk, cx0, cz1, EndZ, true, cx0);
    Kerb(cx1, cx1 + Walk, -EndZ, cz0, true, cx1);
    Kerb(cx0 - Walk, cx0, -EndZ, cz0, true, cx0);
    // 南の道：ステージ側（北）と反対側（南）
    Kerb(cx1, EndX, cz1, cz1 + Walk, false, cz1);
    Kerb(cx1, EndX, cz0 - Walk, cz0, false, cz0);
    Kerb(-EndX, cx0, cz1, cz1 + Walk, false, cz1);
    Kerb(-EndX, cx0, cz0 - Walk, cz0, false, cz0);

    // ── 中央線。**黄色。★世界で唯一の暖色**なので褪せた塗料に留める ──
    m.Group("CityLineY", "City_LineYellow");
    void Dashes(bool alongZ)
    {
        const float W = 0.10f;
        float a = alongZ ? -EndZ : -EndX, b = alongZ ? EndZ : EndX;
        for (float u = a; u < b; u += 5.0f)
        {
            float u2 = MathF.Min(u + 2.8f, b);
            float pa = alongZ ? cz0 : cx0, pb = alongZ ? cz1 : cx1;
            if (u2 > pa && u < pb) continue;          // 交差点では消える
            if (r.Next() < 0.24f) continue;           // ところどころ剥げている
            if (alongZ) Quad(WestX - W, WestX + W, u, u2, YLine);
            else Quad(u, u2, SouthZ - W, SouthZ + W, YLine);
        }
    }
    Dashes(true);
    Dashes(false);

    // ── 横断歩道。交差点の 4 辺 ────────────────────────
    m.Group("CityLine", "City_Line");
    void Zebra(bool alongZ, float at, float span0, float span1)
    {
        const float Bw = 0.45f, Gap = 0.42f, Depth = 2.0f;
        for (float s = span0; s < span1 - Bw; s += Bw + Gap)
        {
            if (r.Next() < 0.13f) continue;
            if (alongZ) Quad(s, s + Bw, at - Depth * 0.5f, at + Depth * 0.5f, YLine);
            else Quad(at - Depth * 0.5f, at + Depth * 0.5f, s, s + Bw, YLine);
        }
    }
    Zebra(true, cz1 - 1.2f, cx0, cx1);     // 交差点の北
    Zebra(true, cz0 + 1.2f, cx0, cx1);     // 南
    Zebra(false, cx1 - 1.2f, cz0, cz1);    // 東
    Zebra(false, cx0 + 1.2f, cz0, cz1);    // 西

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（**ステージの外**。西 x={WestX:0.#} / 南 z={SouthZ:0.#} / 車道 {Half * 2:0.#}m）");
}



// ── 白い部屋（プロローグ）───────────────────────────────────────
//   発注者の決定（案 B）: 一番最初は真っ白な何もない部屋と扉から始まり、
//   最初から持っているベルを鳴らすと草原へ入る。
//
//   ★**閉じた部屋にしてある。**「無限の白い虚空」にはしない。
//     草原は `部屋 0・開口 0・尾なし` なので、ここも無響にすると
//     **二つが同じ音になり、冒頭が何も教えない。**
//     天井があれば部屋になる（`2e63c46`）ので、壁と天井を立てて響かせる。
//     目が「閉じている」と言い、耳も「閉じている」と言う ── それが揃っていることが大事。
//
//   ★だから教える順番はこうなる:
//       白い部屋 = 閉じた場所は響く → 草原 = 開けた場所は返らない
//       → 崩壊都市 = 返す建物と返さない建物がある
//     一段ごとに区別が 1 つずつ増える。
//
//   ★色を持たせない。白 1 色に近づけるが、**真っ白にはしない** ──
//     セル調で面が全部同じ値だと、壁と床と天井の境目が消えて奥行きが死ぬ。
//     面ごとにごくわずかな差を付ける。
static void BuildWhiteRoom(string path, float w, float d, float h, int seed)
{
    const float T = 0.30f;                       // 壁厚
    var m = new ObjMesh();
    float hw = w * 0.5f, hd = d * 0.5f;

    m.Group("WhiteFloor", "White_Floor");
    m.AddBox(new V3(-hw - T, -T, -hd - T), new V3(hw + T, 0f, hd + T));

    m.Group("WhiteWall", "White_Wall");
    m.AddBox(new V3(-hw - T, 0f, -hd - T), new V3(hw + T, h, -hd));
    m.AddBox(new V3(-hw - T, 0f, hd), new V3(hw + T, h, hd + T));
    m.AddBox(new V3(-hw - T, 0f, -hd), new V3(-hw, h, hd));
    m.AddBox(new V3(hw, 0f, -hd), new V3(hw + T, h, hd));

    m.Group("WhiteCeil", "White_Ceil");
    m.AddBox(new V3(-hw - T, h, -hd - T), new V3(hw + T, h + T, hd + T));

    // 幅木（はばき）。床と壁の境に細い線を入れる。
    //   ★これが無いと、白い面どうしの境目が本当に見えなくなる。
    m.Group("WhiteTrim", "White_Trim");
    const float Sk = 0.09f, Sp = 0.018f;
    m.AddBox(new V3(-hw, 0f, -hd), new V3(hw, Sk, -hd + Sp));
    m.AddBox(new V3(-hw, 0f, hd - Sp), new V3(hw, Sk, hd));
    m.AddBox(new V3(-hw, 0f, -hd), new V3(-hw + Sp, Sk, hd));
    m.AddBox(new V3(hw - Sp, 0f, -hd), new V3(hw, Sk, hd));

    _ = seed;
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（内寸 {w:0.#}×{d:0.#}×{h:0.#}m = {w * d * h:0} m³ / 閉じている）");
}

// ── 扉。**全世界で共通の 1 種類。** ───────────────────────────────
//
//   白く塗られた鏡板の木戸と、細い枠。足元に崩れた煉瓦、枠に蔦。
//   「どこにも属さない普通の扉が、属さない場所に立っている」という絵。
//
//   ★寸法は 1.1×2.2m。**`MakeDoorFrame` の現行の当たり判定とちょうど同じ。**
//     草原の石の門に合わせて 1.6×3.0 へ広げていたが、門ごと廃止したので戻した。
//     参考画像の扉は住宅の扉で、人の背丈に対して普通の大きさ ──
//     3m の扉は「普通の扉」に見えず、記念碑になってしまう。
//     通り幅も 1.1m あり、プレイヤーの直径 0.70m の **1.57 倍**なので
//     「プレイヤー 1.5 倍くらいの通り幅」という指示は満たしている。
//
//   ★これで石の門（`BuildGrasslandShrine`）と崩壊都市の躯体（`BuildCityDoorway`）は
//     使わなくなる。生成器は残す（番号が確定するまで消さない ── M0 と同じ扱い）。

// ★寸法は 2 度動いている。1.1×2.2（住宅の扉）→ 発注者から「もう少しでかく」→ **1.4×3.0**。
//   単葉としては広いが、開けた場所（草原・交差点）に 1 枚で立つので
//   住宅寸法だと小さく見える、という判断。枠も一回り太くした。
//   ⚠ `MakeDoorFrame` の当たり判定は 1.1×2.2 のまま。**食い違っている**（連絡板 M8）。
// 扉の板。原点は板の中心、蝶番は -X 側（草原の板と同じ約束）。
//   6 枚の鏡板。框（かまち）を全厚の箱で組み、その間を薄い芯板で埋める ──
//   薄い芯板は**両面から同じだけ凹む**ので、裏返しても鏡板に見える。
static void BuildWhiteDoorSlab(string path, int seed)
{
    const float W = DoorW, H = DoorH, T = DoorT;
    const float Stile = 0.135f;      // 縦框
    const float Muntin = 0.105f;     // 中桟（縦）
    const float RailB = 0.200f;      // 下框
    const float RailM = 0.120f;      // 中框
    const float RailT = 0.110f;      // 上框
    const float Recess = 0.014f;     // 鏡板の落ち込み（片面あたり）
    const float Bevel = 0.028f;      // 鏡板の縁の傾斜

    var r = new Rng(seed);
    var m = new ObjMesh();
    float x0 = -W * 0.5f, x1 = W * 0.5f, y0 = -H * 0.5f, y1 = H * 0.5f;
    float tc = T * 0.5f - Recess;    // 芯板の半厚

    // 鏡板の 3 段。参考画像の割り（上が短く・中が一番長く・下が中くらい）。
    float inner = H - RailB - RailM * 2f - RailT;
    float pT = inner * 0.26f, pM = inner * 0.41f, pB = inner * 0.33f;
    float yb0 = y0 + RailB, yb1 = yb0 + pB;
    float ym0 = yb1 + RailM, ym1 = ym0 + pM;
    float yt0 = ym1 + RailM, yt1 = yt0 + pT;

    // ── 芯板。框より薄いので、框の内側が段になる ──
    m.Group("DoorPanel", "Door_Wood");
    m.AddBox(new V3(x0, y0, -tc), new V3(x1, y1, tc));

    // ── 框。全厚で組む ──
    m.Group("DoorStile", "Door_Wood");
    void Rail(float a0, float a1, float b0, float b1)
        => m.AddBox(new V3(a0, b0, -T * 0.5f), new V3(a1, b1, T * 0.5f));
    Rail(x0, x0 + Stile, y0, y1);                 // 左の縦框（蝶番側）
    Rail(x1 - Stile, x1, y0, y1);                 // 右の縦框
    Rail(x0, x1, y0, y0 + RailB);                 // 下框
    Rail(x0, x1, yb1, ym0);                       // 中框（下）
    Rail(x0, x1, ym1, yt0);                       // 中框（上）
    Rail(x0, x1, yt1, y1);                        // 上框
    float mx = -Muntin * 0.5f;
    Rail(mx, mx + Muntin, yb0, yb1);              // 中桟 3 本
    Rail(mx, mx + Muntin, ym0, ym1);
    Rail(mx, mx + Muntin, yt0, yt1);

    // ── 鏡板の縁の傾斜。框の内側の角から芯板の面へ落とす ──
    //   これが無いと段が直角に切れて、板を貼っただけに見える。
    void Bevels(float a0, float a1, float b0, float b1)
    {
        foreach (int s in new[] { -1, 1 })
        {
            float zf = s * T * 0.5f, zi = s * tc;   // 框の面 → 芯板の面
            float c0 = a0 + Bevel, c1 = a1 - Bevel, d0 = b0 + Bevel, d1 = b1 - Bevel;
            if (c1 <= c0 || d1 <= d0) continue;
            V3 O(float x, float y) => new V3(x, y, zf);   // 框の面（外周）
            V3 I(float x, float y) => new V3(x, y, zi);   // 芯板の面（内周）
            var quads = new[]
            {
                (O(a0, b0), O(a1, b0), I(c1, d0), I(c0, d0)),   // 下
                (O(a1, b1), O(a0, b1), I(c0, d1), I(c1, d1)),   // 上
                (O(a0, b1), O(a0, b0), I(c0, d0), I(c0, d1)),   // 左
                (O(a1, b0), O(a1, b1), I(c1, d1), I(c1, d0)),   // 右
            };
            foreach (var q in quads)
            {
                if (s > 0) m.AddPolygon(q.Item1, q.Item2, q.Item3, q.Item4);
                else m.AddPolygon(q.Item4, q.Item3, q.Item2, q.Item1);
            }
        }
    }
    foreach (var (b0, b1) in new[] { (yb0, yb1), (ym0, ym1), (yt0, yt1) })
    {
        Bevels(x0 + Stile, mx, b0, b1);
        Bevels(mx + Muntin, x1 - Stile, b0, b1);
    }

    // ── 取っ手。開く側（+X）の座金と握り玉 ──
    m.Group("DoorBrass", "Brass_Old");
    float hy = y0 + H * 0.44f;
    foreach (int s in new[] { -1, 1 })
    {
        float zf = s * T * 0.5f;
        m.AddBox(new V3(x1 - Stile - 0.005f, hy - 0.105f, MathF.Min(zf, zf + s * 0.008f)),
                 new V3(x1 - 0.022f, hy + 0.105f, MathF.Max(zf, zf + s * 0.008f)));
        m.AddBox(new V3(x1 - 0.088f, hy - 0.028f, MathF.Min(zf + s * 0.008f, zf + s * 0.055f)),
                 new V3(x1 - 0.032f, hy + 0.028f, MathF.Max(zf + s * 0.008f, zf + s * 0.055f)));
    }
    // 蝶番。**-X 側**（回転の軸）。
    foreach (float y in new[] { y0 + 0.26f, y1 - 0.28f })
        m.AddBox(new V3(x0 - 0.006f, y - 0.055f, -T * 0.5f - 0.010f),
                 new V3(x0 + 0.055f, y + 0.055f, T * 0.5f + 0.010f));

    _ = r;
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（{W:0.##}×{H:0.##}×{T:0.###}m / 鏡板 6 枚 / 蝶番 -X）");
}

// 扉の枠。開口 1.1×2.2m。**世界を問わず同じ。**
//   足元の煉瓦と蔦は別グループにしてあるので、要らない世界では消せる。
static void BuildWhiteDoorFrame(string path, int seed)
{
    const float W = DoorW, H = DoorH;
    const float Jamb = 0.165f;       // 枠の見付（正面から見た幅）
    const float Depth = 0.28f;       // 枠の奥行き
    const float Head = 0.195f;       // 上枠
    const float Case_ = 0.100f;      // ケーシング（枠に回す化粧縁）の見付
    const float CaseP = 0.028f;      // その出っ張り

    var r = new Rng(seed);
    var m = new ObjMesh();
    float x0 = -W * 0.5f - Jamb, x1 = W * 0.5f + Jamb;
    float zd = Depth * 0.5f;

    m.Group("DoorFrame", "Door_WoodDark");
    m.AddBox(new V3(x0, 0f, -zd), new V3(-W * 0.5f, H + Head, zd));      // 左の縦枠
    m.AddBox(new V3(W * 0.5f, 0f, -zd), new V3(x1, H + Head, zd));       // 右の縦枠
    m.AddBox(new V3(-W * 0.5f, H, -zd), new V3(W * 0.5f, H + Head, zd)); // 上枠
    // 沓摺（くつずり）。ごく低くする ── 高いと足が引っかかって見える。
    m.AddBox(new V3(x0, 0f, -zd), new V3(x1, 0.035f, zd));

    // ケーシング。枠の縁に回す化粧縁。**これが無いと板を立てただけに見える。**
    // ケーシング（枠に回す化粧縁）。**中まで詰めた solid で作る。**
    //   ★前は前後の面に薄板を 2 枚貼るだけで、枠の外側へ 0.1m 張り出しているのに
    //     その間（奥行き 0.28m）が空洞だった ── **横から見ると 2 枚の刃と隙間**になる。
    //     化粧縁は「面に貼る板」ではなく「枠を一回り太くする材」として作るのが正しい。
    m.Group("DoorCase", "Door_WoodDark");
    {
        float cz = zd + CaseP;                 // 前後へ 0.028m ずつ出す（面の見切りになる）
        float top = H + Head + Case_;
        m.AddBox(new V3(x0 - Case_, 0f, -cz), new V3(x0, top, cz));          // 左の縦縁
        m.AddBox(new V3(x1, 0f, -cz), new V3(x1 + Case_, top, cz));          // 右の縦縁
        m.AddBox(new V3(x0 - Case_, H + Head, -cz), new V3(x1 + Case_, top, cz));  // 上の横縁
    }

    // ★足元の煉瓦と蔦は**撤去した**（発注者の指示・2026-08-21）。扉と枠だけ。
    //   参考画像には両方あったが、置いてみると「廃墟の一部」に見えて
    //   「どこにも属さない扉」から遠のく。撤去は Rng を使わなくなるだけなので、
    //   戻したくなったら群を 2 つ足せばよい（材質 Brick_Old / Ivy_Leaf は .mtl に残してある）。
    _ = r;
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（開口 {W:0.##}×{H:0.##}m / 全高 {H + Head + Case_:0.##}m / 扉と枠だけ）");
}

// 崩壊都市の扉口。**開口は草原の門と同じ 1.6×3.0m。**
//   ★草原は石を積んだ門。こちらはコンクリの躯体が 1 枚だけ残った、という形にする。
//     世界が違えば作りが違うのは当然で、開口の寸法だけ揃っていればよい
//     （音響の開口測定は開口の寸法で決まる）。
//   ★敷居は付けない。道路のど真ん中に立つので、段を上がるのは不自然。
static void BuildCityDoorway(string path, int seed)
{
    const float OpenW = 1.6f, OpenH = 3.0f;
    const float WallT = 0.45f, HalfW = 2.70f, TopY = 4.60f;
    var r = new Rng(seed);
    var m = new ObjMesh();
    float z0 = -WallT * 0.5f, z1 = WallT * 0.5f;
    float ow = OpenW * 0.5f;

    m.Group("CityFrame", "City_Concrete");
    m.AddBox(new V3(-HalfW, 0f, z0), new V3(-ow, TopY, z1));     // 左の袖
    m.AddBox(new V3(ow, 0f, z0), new V3(HalfW, TopY, z1));       // 右の袖
    m.AddBox(new V3(-ow, OpenH, z0), new V3(ow, TopY, z1));      // まぐさ
    // 開口の縁を厚くする。枠が立つと「扉である」と読める。
    const float Rev = 0.10f;
    m.AddBox(new V3(-ow - 0.16f, 0f, z0 - Rev), new V3(-ow, OpenH + 0.16f, z1 + Rev));
    m.AddBox(new V3(ow, 0f, z0 - Rev), new V3(ow + 0.16f, OpenH + 0.16f, z1 + Rev));
    m.AddBox(new V3(-ow - 0.16f, OpenH, z0 - Rev), new V3(ow + 0.16f, OpenH + 0.16f, z1 + Rev));

    // 崩れた天端。上へだけ足す。
    for (float x = -HalfW; x < HalfW - 0.05f;)
    {
        float w = MathF.Min(r.Range(0.4f, 0.95f), HalfW - x);
        float up = r.Range(0f, 0.5f);
        if (up > 0.05f) m.AddBox(new V3(x, TopY, z0), new V3(x + w, TopY + up, z1));
        x += w;
    }
    m.Group("CityMetal", "City_Metal");
    for (int i = 0; i < 7; i++)
    {
        float x = r.Range(-HalfW + 0.1f, HalfW - 0.15f);
        float z = r.Range(z0 + 0.06f, z1 - 0.11f);
        m.AddBox(new V3(x, TopY, z), new V3(x + 0.05f, TopY + r.Range(0.25f, 0.9f), z + 0.05f));
    }
    // 足元の瓦礫。躯体が崩れて落ちた分。
    m.Group("CityRubble", "City_Rubble");
    for (int i = 0; i < 14; i++)
    {
        float x = r.Range(-HalfW - 0.5f, HalfW);
        if (MathF.Abs(x) < ow + 0.3f) continue;             // 通り道は塞がない
        float s = r.Range(0.16f, 0.44f);
        float z = r.Range(z0 - 0.5f, z1 + 0.2f);
        m.AddBox(new V3(x, 0f, z), new V3(x + s, r.Range(0.1f, 0.42f), z + s));
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                    + $"（全幅 {HalfW * 2:0.#}m / 開口 {OpenW:0.#}×{OpenH:0.#}m / 敷居なし）");
}

// 木の扉。**草原の石造りと同じ 1.6×3.0×0.08m。**原点は板の中心、蝶番は -X 側。
//   ★草原以外の三世界で共通に使う。少しくたびれた板戸。
//     反った板の隙間・打ち直した釘・鉄の帯金具で「ぼろ目」を出す。
//     板を 1 枚ずつ前後にずらすのが効く ── 平らな 1 枚板だと新品に見える。
static void BuildWornDoorSlab(string path, int seed)
{
    const float W = 1.6f, H = 3.0f, T = 0.08f;
    var r = new Rng(seed);
    var m = new ObjMesh();
    float x0 = -W * 0.5f, x1 = W * 0.5f, y0 = -H * 0.5f, y1 = H * 0.5f;

    // ── 縦板。1 枚ずつ幅と反りを変える ──────────────────
    m.Group("DoorPlank", "Wood_Worn");
    for (float x = x0; x < x1 - 0.01f;)
    {
        float w = MathF.Min(r.Range(0.16f, 0.27f), x1 - x);
        float bow = r.Range(-0.010f, 0.012f);               // 反り。前後にずれる
        float gap = r.Range(0.004f, 0.011f);                // 板の隙間
        float top = y1 - r.Range(0f, 0.05f);                // 上端が欠けている板もある
        m.AddBox(new V3(x, y0, -T * 0.5f + bow),
                 new V3(x + w - gap, top, T * 0.5f + bow));
        x += w;
    }
    // ── 帯金具。横に 2 本と、斜めの筋交い ────────────────
    m.Group("DoorIron", "Iron_Rust");
    float[] bands = { y0 + 0.32f, y1 - 0.42f };
    foreach (float by in bands)
        m.AddBox(new V3(x0 + 0.02f, by - 0.045f, T * 0.5f - 0.004f),
                 new V3(x1 - 0.02f, by + 0.045f, T * 0.5f + 0.018f));
    // 斜めの筋交い。2 本の帯を結ぶ。
    {
        var a = new V3(x0 + 0.08f, bands[0], T * 0.5f + 0.006f);
        var b = new V3(x1 - 0.08f, bands[1], T * 0.5f + 0.006f);
        var u = (b - a).Normalized;
        var n = new V3(-u.Y, u.X, 0f) * 0.042f;
        var d = new V3(0f, 0f, 0.011f);
        m.AddPolygon(a - n + d, b - n + d, b + n + d, a + n + d);
        m.AddPolygon(a - n - d, a + n - d, b + n - d, b - n - d);
        m.AddPolygon(a - n - d, b - n - d, b - n + d, a - n + d);
        m.AddPolygon(a + n + d, b + n + d, b + n - d, a + n - d);
    }
    // 蝶番。**-X 側**（回転の軸はここ）。
    foreach (float hy in new[] { y0 + 0.34f, y1 - 0.44f })
        m.AddBox(new V3(x0 - 0.035f, hy - 0.075f, -T * 0.5f - 0.014f),
                 new V3(x0 + 0.28f, hy + 0.075f, T * 0.5f + 0.014f));
    // 取っ手。開く側（+X）。
    m.AddBox(new V3(x1 - 0.24f, -0.03f, T * 0.5f), new V3(x1 - 0.09f, 0.03f, T * 0.5f + 0.10f));

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（{W:0.#}×{H:0.#}×{T:0.##}m / 蝶番 -X）");
}


// 遠景のビル群。**触れない・音響に一切入らない。**
//   WORLD_VISUAL.md §3-2 の「描く: 奥のビル群（板ポリ 3〜4 層で視差）」の代わり。
//   板の書き割りではなく素の箱で出す ── 1 棟 12 三角形なので、
//   視差の効く立体のまま置いても板より安い。
//
//   ★霧が 80m で終わるので、70m の物は 9 割方 霧の色になる。
//     つまりここは**シルエットの濃さ**だけを作る仕事で、細部は要らない。
//     足元は霧に沈むので y=-4 まで下ろして、地面が切れているのを隠す。
static void BuildCitySkyline(string path, int seed)
{
    var r = new Rng(seed);
    var m = new ObjMesh();
    m.Group("CityFar", "City_Far");
    // ★遠景が乗る地面。**道路の外にはもう地面が無い**ので、
    //   これが無いと遠景のビルが宙に浮いた足元を見せる。
    //   y=-0.02 に置く ── ステージの地面(0) と道路(0.02) より下なので、
    //   重なる所は向こうが勝ち、外側だけが見える。
    m.AddPolygon(new V3(-150f, -0.02f, -150f), new V3(-150f, -0.02f, 150f),
                 new V3(150f, -0.02f, 150f), new V3(150f, -0.02f, -150f));

    // ★道路の**外**に出すこと。道は x ±58 / z ±62 まで敷いてあるので、
    //   輪を 48m から始めると**足元が道路の板に埋まる**（実際そうなっていた）。
    //   68m から始めれば、歩ける端（±32/±37）から 36m ＝ 霧が 3 割掛かる距離に立つ。
    var center = new V3(0f, 0f, 4f);
    float[] rings = { 68f, 86f, 106f };
    int[] counts = { 30, 34, 38 };

    for (int k = 0; k < rings.Length; k++)
    {
        for (int i = 0; i < counts[k]; i++)
        {
            float a = MathF.Tau * (i + r.Range(-0.35f, 0.35f)) / counts[k];
            float rad = rings[k] + r.Range(-5f, 7f);
            float cx = center.X + MathF.Cos(a) * rad;
            float cz = center.Z + MathF.Sin(a) * rad * 1.12f;   // 少し縦長にして四角い世界に馴染ませる
            float w = r.Range(6f, 13f), d = r.Range(6f, 13f);
            // 奥の層ほど高く。手前が低いと層が重なって見えず、視差が出ない。
            float hh = r.Range(12f, 26f) + k * 7f;
            // 上を折る。全部平らな天端だと墓石が並んで見える。
            m.AddBox(new V3(cx - w * 0.5f, -4f, cz - d * 0.5f),
                     new V3(cx + w * 0.5f, hh, cz + d * 0.5f));
            if (r.Next() < 0.45f)
            {
                float sw = w * r.Range(0.35f, 0.62f), sd = d * r.Range(0.35f, 0.62f);
                m.AddBox(new V3(cx - sw * 0.5f, hh, cz - sd * 0.5f),
                         new V3(cx + sw * 0.5f, hh + r.Range(2f, 9f), cz + sd * 0.5f));
            }
        }
    }
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（遠景 {counts.Sum()} 棟 / 3 層）");
}

// 崩れた壁の断片。ブロックアウトの Rubble_* の箱（w×h×d、底は y=0）に合わせる。
//   天端は**上へだけ**崩す。足元の瓦礫も箱の内側に収める。
static void BuildCitySlab(string path, float w, float h, float d, int seed)
{
    var r = new Rng(seed);
    var m = new ObjMesh();
    float hw = w * 0.5f, hd = d * 0.5f;

    m.Group("CityFrame", "City_Concrete");
    m.AddBox(new V3(-hw, 0f, -hd), new V3(hw, h, hd));
    for (float x = -hw; x < hw - 0.05f;)
    {
        float sw = MathF.Min(r.Range(0.4f, 0.9f), hw - x);
        float up = r.Range(0f, 0.5f);
        if (up > 0.05f) m.AddBox(new V3(x, h, -hd), new V3(x + sw, h + up, hd));
        x += sw;
    }
    m.Group("CityMetal", "City_Metal");
    for (int i = 0; i < (int)(w * 1.2f); i++)
    {
        float x = r.Range(-hw + 0.1f, hw - 0.15f);
        float z = r.Range(-hd + 0.05f, hd - 0.1f);
        m.AddBox(new V3(x, h, z), new V3(x + 0.05f, h + r.Range(0.2f, 0.8f), z + 0.05f));
    }
    // 足元の瓦礫。箱からはみ出さない範囲で盛る。
    m.Group("CityRubble", "City_Rubble");
    for (int i = 0; i < (int)(w * 2.5f); i++)
    {
        float x = r.Range(-hw, hw - 0.4f);
        float z = r.Range(-hd, hd - 0.2f);
        float s = r.Range(0.18f, 0.46f);
        m.AddBox(new V3(x, 0f, z), new V3(x + s, r.Range(0.14f, 0.5f), z + MathF.Min(s, d)));
    }
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（{w:0.#}×{h:0.#}×{d:0.#}m）");
}

// 折れた鉄骨。ブロックアウトの Beam_* は 0.4×h×0.4 の柱。
//   ★「折れた鉄骨の斜め線」はシルエット指定の 3 つ目。
//     斜めの部分は h より**上**に出す ── 箱の外へ出るのは上だけにする。
static void BuildCityBeam(string path, float h, int seed)
{
    var r = new Rng(seed);
    var m = new ObjMesh();
    const float A = 0.2f;                 // 箱の半分（0.4 角）

    // H 形鋼。フランジ 2 枚とウェブ 1 枚。0.4 角に収める。
    m.Group("CityMetal", "City_Metal");
    m.AddBox(new V3(-A, 0f, -A), new V3(A, h, -A + 0.07f));
    m.AddBox(new V3(-A, 0f, A - 0.07f), new V3(A, h, A));
    m.AddBox(new V3(-0.035f, 0f, -A), new V3(0.035f, h, A));

    // 折れて傾いだ先。h から上へ 4 節、少しずつ倒す。
    V3 c = new(0f, h, 0f);
    float ang = 0f;
    for (int i = 0; i < 4; i++)
    {
        ang += r.Range(0.12f, 0.34f);
        var next = c + new V3(MathF.Sin(ang) * 0.42f, MathF.Cos(ang) * 0.42f, 0f);
        var u = (next - c).Normalized;
        var side = new V3(-u.Z, 0f, u.X);
        if (side.Length < 0.001f) side = new V3(0f, 0f, 1f);
        side = side.Normalized * 0.05f;
        var up = V3.Cross(u, side.Normalized) * 0.05f;
        m.AddPolygon(c - side - up, c + side - up, next + side - up, next - side - up);
        m.AddPolygon(next - side + up, next + side + up, c + side + up, c - side + up);
        m.AddPolygon(c - side - up, next - side - up, next - side + up, c - side + up);
        m.AddPolygon(next + side - up, c + side - up, c + side + up, next + side + up);
        c = next;
    }
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"));
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（0.4×{h:0.#}×0.4m + 折れた先）");
}

static void WriteMtl(string path)
{
    File.WriteAllText(path, string.Join('\n', new[]
    {
        "# BellGame ステージの見た目用マテリアル（手続き生成）",
        "# テクスチャは付けていない。色だけ。Unity 側で差し替える前提。",
        "",
        "newmtl Stone_Wall",
        "Kd 0.66 0.63 0.57",
        "Ks 0.05 0.05 0.05",
        "Ns 12",
        "",
        "newmtl Stone_Column",
        "Kd 0.74 0.71 0.65",
        "Ks 0.08 0.08 0.08",
        "Ns 18",
        "",
        "newmtl Stone_Roof",
        "Kd 0.50 0.47 0.44",
        "Ks 0.04 0.04 0.04",
        "Ns 8",
        "",
        "# ── 崩壊都市。WORLD_VISUAL.md §2 の導出パレットから。**新しい色を足さない** ──",
        "# ── 白い部屋（プロローグ）。真っ白にはしない ──",
        "newmtl White_Floor",
        "Kd 0.80 0.81 0.82",
        "Ks 0.05 0.05 0.05",
        "Ns 14",
        "",
        "newmtl White_Wall",
        "Kd 0.87 0.88 0.88",
        "Ks 0.04 0.04 0.04",
        "Ns 12",
        "",
        "newmtl White_Ceil",
        "Kd 0.91 0.92 0.93",
        "Ks 0.03 0.03 0.03",
        "Ns 10",
        "",
        "newmtl White_Trim",
        "Kd 0.72 0.74 0.76",
        "Ks 0.06 0.06 0.06",
        "Ns 18",
        "",
        "# ── 扉（全世界共通）──",
        "newmtl Door_Wood",         // 板と框。木の茶
        "Kd 0.48 0.36 0.24",
        "Ks 0.05 0.05 0.05",
        "Ns 14",
        "",
        "newmtl Door_WoodDark",     // 枠とケーシング。板より一段暗くして輪郭を出す
        "Kd 0.38 0.28 0.19",
        "Ks 0.05 0.05 0.05",
        "Ns 14",
        "",
        "newmtl Brass_Old",         // 真鍮の取っ手と蝶番
        "Kd 0.66 0.55 0.29",
        "Ks 0.22 0.20 0.14",
        "Ns 34",
        "",
        "newmtl Brick_Old",         // 足元の崩れた煉瓦
        "Kd 0.47 0.30 0.25",
        "Ks 0.02 0.02 0.02",
        "Ns 6",
        "",
        "newmtl Ivy_Leaf",
        "Kd 0.40 0.50 0.34",
        "Ks 0.04 0.04 0.04",
        "Ns 10",
        "",
        "newmtl Park_Grass",        // 公園の芝
        "Kd 0.40 0.50 0.36",
        "Ks 0.03 0.03 0.03",
        "Ns 8",
        "",
        "newmtl City_Concrete",     // ハイライト面と中間色の間
        "Kd 0.49 0.55 0.61",
        "Ks 0.03 0.03 0.03",
        "Ns 8",
        "",
        "newmtl City_Deep",         // 中間色。窪みの底と屋根裏
        "Kd 0.34 0.41 0.48",
        "Ks 0.02 0.02 0.02",
        "Ns 6",
        "",
        "newmtl City_Tower",        // 上層。窓はシェーダの格子が塗る
        "Kd 0.46 0.52 0.58",
        "Ks 0.03 0.03 0.03",
        "Ns 8",
        "",
        "newmtl City_Metal",        // 影・遠景シルエット #36434F
        "Kd 0.21 0.26 0.31",
        "Ks 0.10 0.10 0.10",
        "Ns 22",
        "",
        "newmtl City_Road",         // 車道。地面より暗く沈める
        "Kd 0.18 0.22 0.25",
        "Ks 0.02 0.02 0.02",
        "Ns 5",
        "",
        "newmtl City_Walk",         // 歩道
        "Kd 0.29 0.34 0.38",
        "Ks 0.02 0.02 0.02",
        "Ns 5",
        "",
        "newmtl City_LineYellow",   // 中央線。**唯一の暖色**。褪せた塗料に留める
        "Kd 0.56 0.50 0.30",
        "Ks 0.01 0.01 0.01",
        "Ns 3",
        "",
        "newmtl Wood_Worn",         // くたびれた板戸
        "Kd 0.40 0.34 0.27",
        "Ks 0.03 0.03 0.03",
        "Ns 8",
        "",
        "newmtl Iron_Rust",         // 帯金具・蝶番
        "Kd 0.26 0.24 0.22",
        "Ks 0.09 0.09 0.09",
        "Ns 20",
        "",
        "newmtl City_Line",         // 剥げかけた白線
        "Kd 0.45 0.50 0.55",
        "Ks 0.01 0.01 0.01",
        "Ns 3",
        "",
        "newmtl City_Far",          // 遠景。霧に沈むシルエット #36434F
        "Kd 0.21 0.26 0.31",
        "Ks 0.00 0.00 0.00",
        "Ns 2",
        "",
        "newmtl City_Rubble",
        "Kd 0.37 0.45 0.50",
        "Ks 0.02 0.02 0.02",
        "Ns 5",
        "",
        "newmtl Cave_Rock",
        "Kd 0.42 0.40 0.38",
        "Ks 0.03 0.03 0.03",
        "Ns 6",
        "",
        "# ── 雪山。★純白にしない（WORLD_VISUAL: 白は #DAE3F6 基調）──",
        "newmtl Snow_Tree",         // 樹氷。雪原よりわずかに青く
        "Kd 0.80 0.84 0.92",
        "Ks 0.05 0.05 0.05",
        "Ns 12",
        "",
        "newmtl Snow_Far",          // 遠景の山脈。夜空を背にした暗い稜線
        "Kd 0.25 0.28 0.38",
        "Ks 0.00 0.00 0.00",
        "Ns 2",
        "",
        "newmtl Snow_Ground",
        "Kd 0.88 0.90 0.94",
        "Ks 0.06 0.06 0.06",
        "Ns 10",
        "",
        "newmtl Snow_Ridge",
        "Kd 0.84 0.87 0.92",
        "Ks 0.05 0.05 0.05",
        "Ns 10",
        "",
        "newmtl Snow_Rock",
        "Kd 0.38 0.36 0.34",
        "Ks 0.03 0.03 0.03",
        "Ns 6",
        "",
        "newmtl Wood_Door",
        "Kd 0.42 0.29 0.17",
        "Ks 0.03 0.03 0.03",
        "Ns 6",
        "",
        "newmtl Wood_Beam",
        "Kd 0.34 0.24 0.16",
        "Ks 0.03 0.03 0.03",
        "Ns 5",
        "",
        "newmtl Fire_Coals",
        "Kd 0.32 0.09 0.03",
        "Ks 0.02 0.02 0.02",
        "Ns 6",
        "",
        "newmtl Bell_Bronze",
        "Kd 0.55 0.41 0.19",
        "Ks 0.60 0.50 0.30",
        "Ns 48",
        "",
        // 草原。**色を 1 色にしない** ── 同じ緑で埋めると草地ではなく緑の絨毯になる。
        // 明るい緑・濃い緑を株群ごとに振り、枯草と花で差し色を入れている。
        //
        // ★`newmtl` を書き忘れると、Unity は**既定の白**でマテリアルを作る。
        //   OBJ の usemtl と 1 対 1 で揃っているか、生成のたびに確かめること
        //   （扉の丘が白一色になっていたのがこれ）。
        "newmtl Grass_Ground",
        "Kd 0.43 0.61 0.27",
        "Ks 0.03 0.03 0.03",
        "Ns 8",
        "",
        "newmtl Grass_Blade",
        "Kd 0.44 0.56 0.24",
        "Ks 0.04 0.04 0.04",
        "Ns 8",
        "",
        "newmtl Grass_Blade2",
        "Kd 0.27 0.40 0.18",
        "Ks 0.04 0.04 0.04",
        "Ns 8",
        "",
        "newmtl Grass_Dry",
        "Kd 0.63 0.56 0.31",
        "Ks 0.04 0.04 0.04",
        "Ns 8",
        "",
        "newmtl Grass_Flower",
        "Kd 0.90 0.88 0.72",
        "Ks 0.06 0.06 0.06",
        "Ns 12",
        "",
        "newmtl Shrine_Stone",
        "Kd 0.60 0.64 0.68",
        "Ks 0.05 0.05 0.05",
        "Ns 12",
        "",
        "newmtl Shrine_Column",
        "Kd 0.68 0.71 0.74",
        "Ks 0.06 0.06 0.06",
        "Ns 16",
        "",
        "newmtl Shrine_Step",
        "Kd 0.53 0.56 0.58",
        "Ks 0.04 0.04 0.04",
        "Ns 10",
        "",
        "newmtl Shrine_Deep",
        "Kd 0.44 0.48 0.53",
        "Ks 0.06 0.06 0.06",
        "Ns 14",
        "",
        "newmtl Door_Aqua",
        "Kd 0.62 0.82 0.88",
        "Ks 0.20 0.24 0.26",
        "Ns 40",
        "",
        "newmtl River_Water",
        "Kd 0.28 0.52 0.62",
        "Ks 0.55 0.60 0.62",
        "Ns 60",
        "",
        "newmtl River_Bank",
        "Kd 0.63 0.61 0.53",
        "Ks 0.03 0.03 0.03",
        "Ns 6",
        "",
        "newmtl Grass_Ground_Dark",
        "Kd 0.20 0.27 0.14",
        "Ks 0.02 0.02 0.02",
        "Ns 6",
        "",
        "newmtl Tree_Leaf",
        "Kd 0.30 0.44 0.20",
        "Ks 0.04 0.04 0.04",
        "Ns 8",
        "",
        "newmtl Grass_Shrub",
        "Kd 0.21 0.32 0.15",
        "Ks 0.03 0.03 0.03",
        "Ns 6",
        "",
        "newmtl Grass_Rock",
        "Kd 0.46 0.45 0.42",
        "Ks 0.03 0.03 0.03",
        "Ns 6",
        "",
    }));
}

#pragma warning disable CS8321   // 呼んでいないローカル関数（下の理由で残してある）
// ───────────────────────────────────────────────────────────────
// ステージ 1：草原の草木
//
// ★2026-08-20 いまは呼んでいない。
//   下草・株・穂・花はすべて GPU インスタンシング（`InstancedGrassField`）が持つ。
//   実体メッシュで出していた株 1,853 個が 44,500 三角形あり、しかも
//   **周りが全部揺れている中でそこだけ固まって見えていた**（発注者の指摘）。
//   撒き方の設計（株群・潤い・裸地の斑）は残してあるので、
//   穂や花をインスタンシングの層として足すときはここを参照すること。
// ───────────────────────────────────────────────────────────────
// ステージ 1：草原
//   BellGameStages.Stage1Grassland の寸法:
//     音響用の地面 中心 (0,-0.5,0) / 50×1×60m → 上面 y=0、x -25..25 / z -30..30
//     見た目の地面 Ground_Visual 220×220m、上面 y=-0.03（3cm 下げてある）
//     プレイヤー開始 (0,1.6,-27) / ベル (0,1.2,-10) / 扉 (0,0.7,0)  ← Z 軸上に一直線
//
// ★背の高い遮蔽物を置かない（草案 §4.3「遮蔽物がなく、地面もそこそこ反射する」基準の世界）。
//   音響側は箱を 1 つも置いていない（部屋 0・開口 0）ので、腰より高い物を生やすと
//   **見た目が音と食い違う**。実測の最高点 0.836m（穂）＝ 耳 1.6m の 52%。茂みは 0.70m。
//
// ★ベルの在り処を指す飾りを置かない。避けるのは浮いているベルと重なる 2.0m だけ。
//   株群の間隔より小さいので、空き地としては読めない。
//
// ── 作りかた ──
// **株を一様に撒かない。** 等間隔に同じ株を置くと、草原ではなく点描になる（前版がそれ）。
// 草原らしさは 1 本 1 本の出来ではなく **疎密・背丈・色のムラ**が作るので、
//   1. 2.2m 間隔で「株群」を置き、群ごとに fBm から *潤い* を引く
//   2. 潤いが背丈・株数・枯れ具合・花の出方をまとめて決める
//   3. 別のノイズで *裸地* の斑を抜く（踏み分けと乾きのつもり）
//   4. 群ごとに色味（明るい緑／濃い緑）を振り、隣り合う群でムラが出るようにする
// という順で積む。高さも 4 段（下草 0.24m / 株 0.45m / 穂 0.62m + 穂先 / 茂み 0.70m）に分けてある。
// ───────────────────────────────────────────────────────────────
static void BuildGrassland(string path)
{
    // 散らす範囲は円。ボーダー（半径 30m）の内側いっぱいまで。暗い帯には掛けない。
    float B = GrassBorderRadius() - 0.4f;          // ボーダーの内側いっぱいまで
    float X0 = -B, X1 = B, Z0 = -B, Z1 = B;
    const float Y = -0.05f;         // 根元は 5cm 埋める（見た目の地面が 3cm 下がっているため）

    var bell = new V3(0f, 0f, -10f);
    var door = new V3(0f, 0f, 0f);
    var spawn = new V3(0f, 0f, -27f);

    var m = new ObjMesh();
    // (位置, 種類, 向き, 大きさ, 種)。種類ごとにまとめて出す（材質＝群を跨がせないため）。
    var spots = new List<(V3 p, int kind, float yaw, float sc, int seed)>();
    int nextSeed = 1;

    void Put(V3 p, int kind, float yaw, float sc) => spots.Add((p, kind, yaw, sc, nextSeed++));

    // ── 株群を置く ──────────────────────────────────────
    const float ClumpCell = 1.65f;
    int nx = (int)((X1 - X0) / ClumpCell), nz = (int)((Z1 - Z0) / ClumpCell);
    int clumps = 0;
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++)
        {
            var r = new Rng((i * 73856093) ^ (j * 19349663));
            float cx = X0 + ClumpCell * (i + r.Range(0.05f, 0.95f));
            float cz = Z0 + ClumpCell * (j + r.Range(0.05f, 0.95f));
            var c = new V3(cx, Y, cz);

            // 円形。ボーダーの内側だけに撒く。縁の 2m だけ薄くして、暗い帯との境を硬くしない。
            float rc = MathF.Sqrt(cx * cx + cz * cz);
            if (rc > B) continue;
            float edge = Math.Clamp((B - rc) / 2f, 0f, 1f);

            // 潤い。長波長（周期 ≒ 36m）なので、歩くと草地の表情がゆっくり移り変わる。
            float wet = Math.Clamp(0.52f + Noise.Fbm(c, 0.028f) * 1.15f, 0f, 1f);
            // ★裸地の斑は作らない。「地べたが見えないくらい一面に」という指示なので、
            //   潤いは**密度ではなく**背丈・枯れ具合・花の出方にだけ効かせる。
            float dens = (0.75f + wet * 0.25f) * edge;
            if (r.Next() > dens) continue;

            if (Flat(c, door) < 1.4f) continue;      // 敷居は空ける（跨ぐ所なので）
            if (RiverDistance(cx, cz) < 3.4f) continue;   // 川の水面と土手には生やさない
            clumps++;

            bool dark = r.Next() < 0.45f;            // 群の色味。隣とムラになる
            float rad = 0.45f + dens * 0.6f;
            float tall = 0.75f + wet * 0.5f;         // 潤っている群ほど背が高い

            // 下草。数で地面を埋める層。
            int nc = 4 + (int)(dens * 6f + r.Next() * 2f);
            for (int k = 0; k < nc; k++)
            {
                float a = r.Range(0f, MathF.Tau), d = rad * MathF.Sqrt(r.Next());
                Put(new V3(cx + MathF.Cos(a) * d, Y, cz + MathF.Sin(a) * d),
                    dark ? 1 : 0, r.Range(0f, MathF.Tau), tall * r.Range(0.8f, 1.2f));
            }

            // 中背の株。輪郭を作る層。乾いた群では枯草に振り替える。
            int nt = 1 + (int)(dens * 2.0f + r.Next() * 1.2f);
            for (int k = 0; k < nt; k++)
            {
                float a = r.Range(0f, MathF.Tau), d = rad * 0.85f * MathF.Sqrt(r.Next());
                int kind = r.Next() > wet ? 4 : (dark ? 3 : 2);
                Put(new V3(cx + MathF.Cos(a) * d, Y, cz + MathF.Sin(a) * d),
                    kind, r.Range(0f, MathF.Tau), tall * r.Range(0.85f, 1.25f));
            }

            // 穂。空に対する細い縦線で、遠景の「草原」はほぼこれが作る。
            if (r.Next() < 0.26f + (1f - wet) * 0.35f)
            {
                int ns = 1 + (int)(r.Next() * 3f);
                for (int k = 0; k < ns; k++)
                {
                    float a = r.Range(0f, MathF.Tau), d = rad * r.Next();
                    Put(new V3(cx + MathF.Cos(a) * d, Y, cz + MathF.Sin(a) * d),
                        5, r.Range(0f, MathF.Tau), r.Range(0.85f, 1.2f));
                }
            }

            // 花。潤った群だけ。点数は少なくてよく、あると一気に野原になる。
            if (r.Next() < 0.14f * wet)
            {
                int nf = 2 + (int)(r.Next() * 4f);
                for (int k = 0; k < nf; k++)
                {
                    float a = r.Range(0f, MathF.Tau), d = rad * 0.9f * MathF.Sqrt(r.Next());
                    Put(new V3(cx + MathF.Cos(a) * d, Y, cz + MathF.Sin(a) * d),
                        6, r.Range(0f, MathF.Tau), r.Range(0.8f, 1.15f));
                }
            }
        }

    // ── 茂みと小石 ──────────────────────────────────────
    var sr = new Rng(9112026);
    var big = new List<V3>();
    for (int tries = 0; tries < 900 && big.Count < 18; tries++)
    {
        var p = new V3(sr.Range(X0, X1), Y, sr.Range(Z0, Z1));
        if (Flat(p, new V3(0f, Y, 0f)) > B - 3f) continue;
        if (Flat(p, bell) < 2.0f || Flat(p, door) < 2.5f || Flat(p, spawn) < 2.5f) continue;
        if (big.Exists(q => Flat(p, q) < 3.6f)) continue;
        Put(p, 7, sr.Range(0f, MathF.Tau), sr.Range(0.8f, 1.25f));
        big.Add(p);
    }
    var pr = new Rng(3141592);
    var rocks = new List<V3>();
    for (int tries = 0; tries < 1200 && rocks.Count < 55; tries++)
    {
        var p = new V3(pr.Range(X0, X1), Y, pr.Range(Z0, Z1));
        if (Flat(p, new V3(0f, Y, 0f)) > B - 3f) continue;
        if (Flat(p, bell) < 2.0f || Flat(p, door) < 1.6f) continue;
        if (rocks.Exists(q => Flat(p, q) < 1.5f) || big.Exists(q => Flat(p, q) < 1.2f)) continue;
        Put(p, 8, pr.Range(0f, MathF.Tau), pr.Range(0.7f, 1.3f));
        rocks.Add(p);
    }

    // ── 種類ごとにまとめて出す ──────────────────────────
    // 材質は「群 → usemtl」で決まるので、種類を跨いで交互に出すと群が細切れになる。
    void Emit(string group, string material, int kind, bool uv = true)
    {
        m.Group(group, material, uv);
        foreach (var s in spots)
        {
            if (s.kind != kind) continue;
            var r = new Rng(s.seed);
            switch (kind)
            {
                case 0:
                case 1: Tuft(m, s.p, s.yaw, 0.24f * s.sc, 4, 1, ref r); break;
                case 2:
                case 3: Tuft(m, s.p, s.yaw, 0.45f * s.sc, 4, 2, ref r); break;
                case 4: Tuft(m, s.p, s.yaw, 0.52f * s.sc, 4, 2, ref r); break;
                case 5: SeedStalk(m, s.p, s.yaw, 0.62f * s.sc, ref r); break;
                case 6: Flower(m, s.p, s.yaw, 0.34f * s.sc, ref r); break;
                case 7: Shrub(m, s.p, 0.62f * s.sc, 0.70f * s.sc, ref r); break;
                case 8: Pebble(m, s.p, s.sc, ref r); break;
            }
        }
    }

    // ★下草は出さない。GPU インスタンシング（InstancedGrassField）が持つ。
    //   ここに残していた 6,500 株が OBJ の重さのほとんどだった。
    Emit("GrassTufts", "Grass_Blade", 2, false);
    Emit("GrassTuftsDark", "Grass_Blade2", 3, false);
    Emit("GrassDry", "Grass_Dry", 4, false);
    Emit("GrassStalks", "Grass_Dry", 5, false);
    Emit("GrassFlowers", "Grass_Flower", 6, false);
    // ★茂みと小石は出さない。インスタンシングの草が密になった今、
    //   揺れない塊が転がっているだけに見えて邪魔だった（発注者の指摘）。
    //   置き場所の計算は残してあるので、要るときは Emit を戻すだけでよい。

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 1400, 1000, 38f, 11f);
    int under = spots.Count(s => s.kind <= 1);
    int tufts = spots.Count(s => s.kind == 2 || s.kind == 3 || s.kind == 4);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount} / 面 {m.Faces.Count}"
                      + $"（株群 {clumps} / 下草 {under}=インスタンシング側 / 株 {tufts}"
                      + $" / 穂 {spots.Count(s => s.kind == 5)} / 花 {spots.Count(s => s.kind == 6)}"
                      + $" / 茂み {big.Count}・小石 {rocks.Count} は出さない）");
}

#pragma warning restore CS8321

// 水平距離（y は見ない）。
static float Flat(V3 a, V3 b) => MathF.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Z - b.Z) * (a.Z - b.Z));

// 草株。葉を放射に散らす。segs で反りの滑らかさ（下草は 1、中背は 2）。
static void Tuft(ObjMesh m, V3 c, float yaw, float h, int blades, int segs, ref Rng r)
{
    for (int i = 0; i < blades; i++)
    {
        float a = yaw + MathF.Tau * i / blades + r.Range(-0.6f, 0.6f);
        float d = r.Range(0f, 0.045f);
        var b = new V3(c.X + MathF.Cos(a) * d, c.Y, c.Z + MathF.Sin(a) * d);
        Blade(m, b, a, h * r.Range(0.7f, 1.3f), h * r.Range(0.045f, 0.075f),
              r.Range(0.35f, 1.15f), segs, r.Range(-0.35f, 0.35f));
    }
}

// 草の葉 1 枚。yaw の向きへ反りながら細る。
//   ★両面に出す。Standard は裏面を描かないので、片面だと回り込んだ瞬間に消える
//     （音源を探して歩き回る遊びなので必ず起きる）。
//   ★twist で葉面をひねる。平面のままだと平坦シェーディングで全部同じ明るさになり、
//     板を並べただけに見える。
static void Blade(ObjMesh m, V3 b, float yaw, float h, float w, float bend, int segs, float twist)
{
    V3 prev = b;
    float wPrev = w;
    for (int s = 0; s < segs; s++)
    {
        float t0 = s / (float)segs, t1 = (s + 1) / (float)segs;
        V3 sideP = new(-MathF.Sin(yaw + twist * t0), 0f, MathF.Cos(yaw + twist * t0));

        if (s == segs - 1)
        {
            // 先端は 1 点に絞る。
            AddDouble(m, prev - sideP * wPrev, prev + sideP * wPrev, Spine(b, yaw, h, bend, 1f));
        }
        else
        {
            V3 side1 = new(-MathF.Sin(yaw + twist * t1), 0f, MathF.Cos(yaw + twist * t1));
            V3 next = Spine(b, yaw, h, bend, t1);
            float wNext = w * (1f - t1 * 0.85f);
            AddDouble(m, prev - sideP * wPrev, prev + sideP * wPrev,
                      next + side1 * wNext, next - side1 * wNext);
            prev = next;
            wPrev = wNext;
        }
    }
}

// 葉の背骨。t=0 で根元、t=1 で先端。反るほど先が垂れる。
static V3 Spine(V3 b, float yaw, float h, float bend, float t)
{
    float lat = bend * h * 0.62f * t * t;                 // 横への出はゆっくり効かせる
    return new V3(b.X + MathF.Cos(yaw) * lat,
                  b.Y + h * t * (1f - 0.28f * bend * t),
                  b.Z + MathF.Sin(yaw) * lat);
}

// 穂。細い茎の上に紡錘の穂を載せる。遠景の縦線はこれが作る。
static void SeedStalk(ObjMesh m, V3 b, float yaw, float h, ref Rng r)
{
    float bend = r.Range(0.15f, 0.5f);
    Blade(m, b, yaw, h, h * 0.012f, bend, 2, 0f);

    V3 top = Spine(b, yaw, h, bend, 1f);
    float len = h * r.Range(0.16f, 0.26f), w = len * r.Range(0.16f, 0.24f);
    V3 up = new(MathF.Cos(yaw) * bend * 0.25f, 1f, MathF.Sin(yaw) * bend * 0.25f);

    // 十字に 2 枚。どの向きから見ても紡錘に見える。
    for (int q = 0; q < 2; q++)
    {
        V3 sd = q == 0
            ? new V3(-MathF.Sin(yaw), 0f, MathF.Cos(yaw))
            : new V3(MathF.Cos(yaw), 0f, MathF.Sin(yaw));
        V3 mid = top + up * (len * 0.45f);
        AddDouble(m, top, mid + sd * w, top + up * len, mid - sd * w);
    }
}

// 花。細い茎と、平たい花冠。3 枚の花弁で足りる（小さいので形は読めない）。
static void Flower(ObjMesh m, V3 b, float yaw, float h, ref Rng r)
{
    float bend = r.Range(0.1f, 0.4f);
    Blade(m, b, yaw, h, h * 0.012f, bend, 2, 0f);

    V3 top = Spine(b, yaw, h, bend, 1f);
    float pr = h * r.Range(0.09f, 0.14f);
    for (int i = 0; i < 3; i++)
    {
        float a0 = yaw + MathF.Tau * i / 3f, a1 = yaw + MathF.Tau * (i + 1) / 3f;
        AddDouble(m, top,
                  top + new V3(MathF.Cos(a0) * pr, pr * 0.25f, MathF.Sin(a0) * pr),
                  top + new V3(MathF.Cos(a1) * pr, pr * 0.25f, MathF.Sin(a1) * pr));
    }
}

// 表裏の 2 面として出す。
static void AddDouble(ObjMesh m, params V3[] p)
{
    m.AddPolygon(p);
    var q = (V3[])p.Clone();
    Array.Reverse(q);
    m.AddPolygon(q);
}

// 低い茂み。塊をいくつか重ねるだけ。**高さは 0.7m を超えさせない。**
static void Shrub(ObjMesh m, V3 c, float rad, float h, ref Rng r)
{
    int n = 4 + (int)(r.Next() * 3f);
    for (int i = 0; i < n; i++)
    {
        float a = MathF.Tau * i / n + r.Range(-0.4f, 0.4f);
        float d = r.Range(0f, rad * 0.55f);
        float s = r.Range(0.55f, 0.95f);
        var p = new V3(c.X + MathF.Cos(a) * d, c.Y + r.Range(h * 0.28f, h * 0.48f),
                       c.Z + MathF.Sin(a) * d);
        Lobe(m, p, new V3(rad * s * 1.6f, h * 0.70f * s, rad * s * 1.6f), 0.32f, 2.2f, 8, 4, c.Y);
    }
}

// 小石。
static void Pebble(ObjMesh m, V3 c, float sc, ref Rng r)
{
    float w = sc * r.Range(0.16f, 0.46f), h = sc * r.Range(0.10f, 0.26f);
    Lobe(m, new V3(c.X, c.Y + h * 0.42f, c.Z),
         new V3(w, h, w * r.Range(0.7f, 1.3f)), 0.30f, 2.0f, 8, 4, c.Y);
}

// 塊。Boulder の粗い版（Boulder は 18×10 = 360 三角形あり、小石や葉叢には重すぎる）。
//   yFloor より下へは潰さない ── 地面へめり込ませずに座らせるため。
static void Lobe(ObjMesh m, V3 c, V3 size, float amp, float freq, int seg, int ring, float yFloor)
{
    var pts = new V3[ring + 1, seg];
    for (int i = 0; i <= ring; i++)
    {
        float phi = MathF.PI * i / ring;
        for (int s = 0; s < seg; s++)
        {
            float th = MathF.Tau * s / seg;
            var dir = new V3(MathF.Sin(phi) * MathF.Cos(th), MathF.Cos(phi), MathF.Sin(phi) * MathF.Sin(th));
            var p = new V3(c.X + dir.X * size.X * 0.5f,
                           c.Y + dir.Y * size.Y * 0.5f,
                           c.Z + dir.Z * size.Z * 0.5f);
            float d = 1f + Noise.Fbm(p, freq) * amp;
            pts[i, s] = new V3(c.X + (p.X - c.X) * d,
                               MathF.Max(yFloor, c.Y + (p.Y - c.Y) * d),
                               c.Z + (p.Z - c.Z) * d);
        }
    }
    for (int i = 0; i < ring; i++)
        for (int s = 0; s < seg; s++)
        {
            int s2 = (s + 1) % seg;
            m.AddPolygon(pts[i, s], pts[i, s2], pts[i + 1, s2], pts[i + 1, s]);
        }
}

// ───────────────────────────────────────────────────────────────
// ステージ 1：草原の地形
//   ★これは「見た目だけ」では済まない可能性がある形。
//     扉まわりの盛り上がりは**プレイヤーが歩いて登る**ので、コライダーが要る。
//     付けるなら MeshCollider。§7.6 の「メッシュ壁は跨ぎ判定が使えない」は
//     **部屋の**跨ぎ判定の話で、草原は屋根が無く部屋が 0 個なので成立しない、と読んでいる。
//     ── 未確認。サウンドシステムに B4 の実測を頼むこと。
//
//   WORLD_SETTING §7.2:
//     「草原だけは壁で囲んではいけない。縁に垂直な壁を立てると早期反射が返り、
//       基準世界の役目が壊れる。→ 緩やかな丘で閉じる」
//
//   寸法（扉を中央に置く前提の再中心化後）:
//     音響の地面  中心 (0,-0.5,0) / 50×60m → 上面 y=0、x -25..25 / z -30..30
//     扉 (0,0,0) / ベル (-6,1.2,-8) / プレイヤー開始 (-22,1.6,-29)
//
//   高さは 2 つの山の足し算:
//     扉まわり … 半径 3m は平ら(1.2m)、そこから 10m で 0 へ。傾斜 約 7°
//     縁の丘   … 遊ぶ矩形の外へ 9m で 3.6m、さらに 10m で 4.2m
//
//   ★内側の平らな部分は出さない（h <= 0.02 の区画を捨てる）。
//     既存の Ground_Visual が y=-0.03 に居るので、重ねると Z ファイティングになる。
// ───────────────────────────────────────────────────────────────
// ★2 ファイルに割ってある。**片方にしかコライダーを付けないため。**
//   扉の丘   … プレイヤーが登るので MeshCollider が要る。範囲が狭いので格子に響かない
//   縁の丘   … 付けない。付けると外接が 92×102m になり、格子が
//              368×37×408 = 555 万ボクセル ＝ 上限 400 万を超えて **セルが 0.375m へ降格**する
//              （C7 と同じ罠）。プレイヤーは WorldBounds に押し戻されて丘には届かない。
// 草原の遊べる範囲の半径。**地形・草・配置スクリプトで同じ値を使うこと。**
static float GrassBorderRadius() => 30f;

// ───────────────────────────────────────────────────────────────
// ステージ 1：草原の地形（円形）
//
//   ★世界は円。中心 (0,0) に扉が立ち、半径 30m までが遊べる範囲。
//     ボーダーの外は**地面の色を落とす**（`Grass_Ground_Dark`）ので、
//     押し戻しが始まる位置が目で分かる。§7.2 が禁じているのは
//     「見えない壁の手触り」であって、境界を見せること自体はその趣旨に沿う。
//
//   半径の内訳:
//     0        扉が立つ平場（半径 3m・高さ 1.2m ちょうど）
//     ..10     扉の丘の斜面（傾斜 約 7°）
//     ..30     平ら。遊べる範囲。明るい地面
//     ..33     平らな**暗い縁**。ここから先は行けないと分かる帯
//     ..42     縁の丘が 3.6m までせり上がる
//     ..52     さらに 4.2m へ緩く転がる
//
//   ★2 ファイルに割ってある。**片方にしかコライダーを付けないため。**
//     扉の丘 … MeshCollider。プレイヤーが登るので要る。範囲が狭く格子に響かない
//     縁の丘 … 付けない。付けると外接 104×104m ＝ 格子 416×37×416 = 640 万で
//              上限 400 万を超え、セルが 0.375m へ降格する（C7 と同じ罠）
// ───────────────────────────────────────────────────────────────
// ── 川の断面。**生成器と `GrasslandTerrain.RiverBerm` は同じ式でなければならない。**
//    片方だけ変えると、草が土手の上に浮く／水面から生える。
//
//    ★掘り下げてはいけない。音響の地面（`Ground` の箱）の上面が y=0 なので、
//      掘ると「水の上を歩く」ことになる。だから**土手を上げて相対的に窪ませる。**
//      河床 0.10m ＞ 0 なので、箱の天面と z 争いも起きない。
//
//    ★前回（土手 0.55m・幅 3.0m）は見えなかった。今回変えたのは 2 つ:
//        1. 天端を 1.00m へ。水面との差が 0.78m あれば、20m 先からでも
//           **両側の土手が明るい 2 本の帯**として読める（水面そのものは見えなくてよい）
//        2. **草を剥ぐ帯（riverClear）と土手の材質の帯を 7.5m でぴったり揃えた。**
//           前は除外 7.4m に対し土手の見た目が 6m しかなく、はみ出した 1.4m が
//           「川でもないのに草が剥げている」に見えていた
static float RiverBerm(float d)
{
    if (d <= RiverBedR) return RiverBedY;
    if (d >= RiverOutR) return 0f;
    if (d <= RiverTopR)
    {
        float t = (d - RiverBedR) / (RiverTopR - RiverBedR);
        return RiverBedY + (RiverCrestY - RiverBedY) * t * t * (3f - 2f * t);
    }
    float u = (d - RiverTopR) / (RiverOutR - RiverTopR);
    return RiverCrestY * (1f - u * u * (3f - 2f * u));
}

// ステージ 1：草原の野原。丘と川の土手を 1 枚のメッシュで持つ。
//   ★平らな区画は出さない（`Grid` の keep が false で高さ 0 の区画を捨てる）。
//     残りは音響の箱の天面（y=0）がそのまま見える。継ぎ目が出ないよう全体を 12mm 持ち上げる。
//   ★丘と川は**足さずに max を取る。** 川の土手の外側の裾が丘の斜面に入り込むので、
//     足すと合流点が盛り上がる。どちらも縁で 0 になるので max なら滑らかに繋がる。
static void BuildGrassField(string path)
{
    const float MoundR = 10f, Flat = 3f, MoundH = 0.7f, Cell = 0.8f;
    const float E = 33f;          // ボーダー 30m + 少し
    const float Lift = 0.012f;    // 箱の天面との z 争いを避ける

    var m = new ObjMesh();
    int quads = 0;

    // 草の面と土手の面で材質を分ける。**明るい帯が出ることが川を見せる仕掛けの本体。**
    m.Group("GrassField", "Grass_Ground");
    quads += Grid(m, (int)MathF.Round(E * 2f / Cell), E, Cell, H,
                  (x, z) => !IsBank(x, z), onlyKeep: true, dropFlat: true);
    m.Group("RiverBank", "River_Bank");
    quads += Grid(m, (int)MathF.Round(E * 2f / Cell), E, Cell, H,
                  (x, z) => IsBank(x, z), onlyKeep: true);

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 1100, 800, 38f, 18f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（区画 {quads} / 丘 {MoundH:0.00}m / "
                    + $"土手の天端 {RiverCrestY:0.00}m / 半径 {E:0}m）");

    // 土手の帯。**`InstancedGrassField.riverClear` と同じ `Out`(9.5m)。距離だけで判定する。**
    //   ★半径の条件（r>13 など）を足してはいけない。riverClear は距離しか見ないので、
    //     条件を足したぶんだけ「草は消えているのに地面は草色」の帯ができる ──
    //     これが前回の「川でもないのに草が剥げている」の正体。
    static bool IsBank(float x, float z) => RiverDistance(x, z) < RiverOutR;

    static float H(float x, float z)
    {
        float r = MathF.Sqrt(x * x + z * z);
        float mound = 0f;
        if (r < MoundR)
        {
            float t = Math.Clamp((MoundR - r) / (MoundR - Flat), 0f, 1f);
            mound = MoundH * t * t * (3f - 2f * t);
        }
        float rd = RiverDistance(x, z);
        float y = MathF.Max(mound, RiverBerm(rd));
        if (y <= 0.02f) return Lift;

        // ★平場と河床はノイズを切る。ベルが立つ所と水面が波打つと埋まる／浮く。
        float amp = MathF.Min(0.55f, y * 0.16f)
                  * Math.Clamp((r - Flat) / 2f, 0f, 1f)
                  * Math.Clamp((rd - RiverBedR) / 1.5f, 0f, 1f);
        return y + Noise.Fbm(new V3(x, 0f, z), 0.035f, 3) * amp + Lift;
    }
}

static void BuildGrassMound(string path)
{
    const float R = 10f, Flat = 3f, H = 0.7f, Cell = 0.6f;
    const float E = R + 1f;

    var m = new ObjMesh();
    m.Group("GrassMound", "Grass_Ground");
    int n = (int)MathF.Round(E * 2f / Cell);
    int quads = Grid(m, n, E, Cell, Height, (x, z) => true);

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 1100, 800, 38f, 18f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（区画 {quads} / 頂上 {H:0.00}m / 半径 {R:0}m）");

    static float Height(float x, float z)
    {
        float r = MathF.Sqrt(x * x + z * z);
        if (r >= R) return 0f;
        float t = Math.Clamp((R - r) / (R - Flat), 0f, 1f);
        float y = H * t * t * (3f - 2f * t);
        // ★頂上の平場はノイズを切る。扉がここに立つので、土台が波打つと埋まる／浮く。
        float amp = MathF.Min(0.55f, y * 0.16f) * Math.Clamp((r - Flat) / 2f, 0f, 1f);
        return y + Noise.Fbm(new V3(x, 0f, z), 0.035f, 3) * amp;
    }
}

static void BuildGrassHills(string path)
{
    const float Apron = 3f, Rise = 9f, Crest = 3.6f, Roll = 10f, Outer = 4.2f, Cell = 1.0f;
    float B = GrassBorderRadius();
    float E = B + Apron + Rise + Roll;

    var m = new ObjMesh();
    m.Group("GrassHills", "Grass_Ground_Dark");
    int n = (int)MathF.Round(E * 2f / Cell);
    // ★高さではなく**半径**で出す区画を決める。平らな暗い縁（高さ 0）も要るため。
    int quads = Grid(m, n, E, Cell, Height,
                     (x, z) => { float r = MathF.Sqrt(x * x + z * z); return r >= B && r <= E; });

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 1400, 1000, 38f, 14f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}（区画 {quads} / ボーダー {B:0}m"
                      + $" / 暗い縁 {Apron:0}m / 稜線 {Crest:0.0}m）");

    float Height(float x, float z)
    {
        float d = MathF.Sqrt(x * x + z * z) - B - Apron;
        // ★川はここでも要る。縁の丘は r>33 を受け持つが、川はそこを突っ切って
        //   ボーダーの外まで続く。ここに入れないと、川が縁で唐突に途切れる。
        float berm = RiverBerm(RiverDistance(x, z));
        if (d <= 0f) return berm;            // 平らな暗い縁（川があればその分だけ）
        float y;
        if (d <= Rise)
        {
            float t = d / Rise;
            y = Crest * t * t * (3f - 2f * t);
        }
        else
        {
            float t = Math.Clamp((d - Rise) / Roll, 0f, 1f);
            y = Crest + (Outer - Crest) * t * t * (3f - 2f * t);
        }
        y = MathF.Max(y, berm);            // 丘と川は足さずに max（BuildGrassField と同じ）
        float amp = MathF.Min(0.55f, y * 0.16f) * Math.Clamp(y / 0.6f, 0f, 1f)
                  * Math.Clamp((RiverDistance(x, z) - 3.2f) / 1.5f, 0f, 1f);
        return y + Noise.Fbm(new V3(x, 0f, z), 0.035f, 3) * amp;
    }
}

// 正方の升目を高さ関数で持ち上げて張る。keep が false の区画は出さない。
//   高さが全隅 0 の区画も、keep が true なら出す（平らな帯を作るため）。
static int Grid(ObjMesh m, int n, float extent, float cell,
                Func<float, float, float> height, Func<float, float, bool> keep,
                bool onlyKeep = false, bool dropFlat = false)
{
    var h = new float[n + 1, n + 1];
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= n; j++)
            h[i, j] = height(-extent + cell * i, -extent + cell * j);

    int quads = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
        {
            float x0 = -extent + cell * i, z0 = -extent + cell * j;
            float x1 = x0 + cell, z1 = z0 + cell;
            bool k = keep(x0 + cell * 0.5f, z0 + cell * 0.5f);
            // ★keep が false は「**強制しない**」であって「出さない」ではない。
            //   高さがあれば結局出るので、材質で 2 回走査すると**同じ面が 2 枚**重なる
            //   （実際そうなって、土手の 1,286 面すべてに草の面が乗っていた）。
            //   onlyKeep で本当に捨て、dropFlat で「拾う側でも平らなら捨てる」を足す。
            // onlyKeep: 条件を満たす区画だけ出す（材質で 2 枚に割るとき用）。
            if (onlyKeep && !k) continue;
            if (!k || dropFlat)
            {
                float hi = MathF.Max(MathF.Max(h[i, j], h[i + 1, j]),
                                     MathF.Max(h[i, j + 1], h[i + 1, j + 1]));
                if (hi <= 0.02f) continue;
            }
            // 上向きに巻く（cross(+X, +Z) が -Y なので、+Z → +X の順）。
            m.AddPolygon(new V3(x0, h[i, j], z0), new V3(x0, h[i, j + 1], z1),
                         new V3(x1, h[i + 1, j + 1], z1), new V3(x1, h[i + 1, j], z0));
            quads++;
        }
    return quads;
}

// ───────────────────────────────────────────────────────────────
// 普通の木 1 本（草原の右手）。原点＝根元、+Y が上。
//
//   ★幹だけがコライダーを持つ想定。葉には付けない ──
//     葉は音をほとんど遮らないし、樹冠に箱を付けると巨大な遮蔽物になる。
//     ゲームシステム側に「幹に 0.8×0.8m の BoxCollider」を依頼すること。
//   ★木は屋根を作らないので部屋にはならない（`2e63c46`：天井が要る）。
//
//   樹冠は葉を 1 枚ずつ出さず、**丸い塊を数個**にしてある。
//   平坦シェーディングの大きな面のほうが、段の少ない陰影に乗る。
// ───────────────────────────────────────────────────────────────
static void BuildTree(string path)
{
    const float H = 10.5f;         // 幹の足場の高さ（樹冠が上に乗るので全高は 12m 台になる）
    const float TrunkR = 0.70f;    // 根元の半径（直径 1.4m）
    var m = new ObjMesh();
    var r = new Rng(77042026);

    // ── 幹 ──────────────────────────────────────────────
    // ★まっすぐな円柱は棒に見える。3 つで崩す:
    //   ① 芯を S 字に曲げる。一方向へ傾けるだけだと「倒れかけた棒」になる
    //   ② 太さを単調に細らせない。節の高さで膨らませる
    //   ③ 断面の歪みを**縦に長く相関させる**＝樹皮の溝が上下に走る
    //      （前は縦の周波数が高すぎて、ただのざらつきにしかなっていなかった）
    m.Group("TreeTrunk", "Wood_Beam");
    const int Seg = 14, Rings = 14;
    var trunk = new V3[Rings + 1];
    var rad = new float[Rings + 1];
    for (int i = 0; i <= Rings; i++)
    {
        float t = i / (float)Rings;
        // ① S 字。下で −X へ張り出し、上で戻しながら +Z へ抜ける。
        float bx = MathF.Sin(t * 2.6f) * 0.55f - t * 0.30f;
        float bz = MathF.Sin(t * 1.7f + 0.9f) * 0.34f - 0.27f;
        trunk[i] = new V3(bx, H * 0.62f * t, bz);

        // ② 太さ。根張り（下 18%）＋ 節の膨らみ 2 箇所。
        float taper = 0.52f + 0.48f * MathF.Pow(1f - t, 0.62f);
        float flare = t < 0.18f ? MathF.Pow((0.18f - t) / 0.18f, 1.6f) * 0.85f : 0f;
        float knot = MathF.Exp(-MathF.Pow((t - 0.34f) / 0.07f, 2f)) * 0.13f
                   + MathF.Exp(-MathF.Pow((t - 0.68f) / 0.06f, 2f)) * 0.10f;
        rad[i] = TrunkR * (taper + flare + knot);
    }

    for (int i = 0; i < Rings; i++)
        for (int s = 0; s < Seg; s++)
        {
            int s2 = (s + 1) % Seg;
            m.AddPolygon(TrunkPt(i, s), TrunkPt(i + 1, s), TrunkPt(i + 1, s2), TrunkPt(i, s2));
        }

    // ── 板根 ────────────────────────────────────────────
    // 根元から 5 枚、外へ張り出させる。**断面の丸さを崩すのはここが一番効く。**
    // 円柱の裾がそのまま地面に刺さっていると、どれだけ曲げても棒に見える。
    m.Group("TreeRoot", "Wood_Beam");
    for (int k = 0; k < 5; k++)
    {
        float a = MathF.Tau * k / 5f + 0.4f;
        float reach = r.Range(0.9f, 1.5f), up = r.Range(0.7f, 1.25f);
        V3 dir = new(MathF.Cos(a), 0f, MathF.Sin(a));
        V3 side = new(-dir.Z, 0f, dir.X);
        float w = r.Range(0.14f, 0.24f);

        V3 rTop = trunk[0] + dir * (rad[0] * 0.7f) + new V3(0f, up, 0f);
        V3 toe = trunk[0] + dir * (rad[0] + reach) + new V3(0f, 0.02f, 0f);
        V3 baseL = trunk[0] + dir * (rad[0] * 0.5f) - side * w;
        V3 baseR = trunk[0] + dir * (rad[0] * 0.5f) + side * w;
        V3 topL = rTop - side * (w * 0.55f), topR = rTop + side * (w * 0.55f);

        // 両側の面（外向き）と、上面。裏返しも出しておく（薄いので裏から見える）。
        m.AddPolygon(baseL, toe, topL);
        m.AddPolygon(topL, toe, baseL);
        m.AddPolygon(baseR, topR, toe);
        m.AddPolygon(toe, topR, baseR);
        m.AddPolygon(topL, toe, topR);
        m.AddPolygon(topR, toe, topL);
    }
    m.Group("TreeTrunk", "Wood_Beam");

    // ── 枝 ──────────────────────────────────────────────
    V3 fork = trunk[Rings];
    var tips = new V3[6];
    for (int k = 0; k < 6; k++)
    {
        float a = MathF.Tau * k / 6f + r.Range(-0.35f, 0.35f);
        float len = r.Range(3.6f, 5.4f);          // 横へ大きく張り出す
        var tip = fork + new V3(MathF.Cos(a) * len, r.Range(0.7f, 1.7f), MathF.Sin(a) * len);
        tips[k] = tip;
        LegQuad(m, fork, tip, TrunkR * r.Range(0.30f, 0.42f));
    }

    // ── 樹冠は作らない ──────────────────────────────────
    // 葉は `InstancedCanopy`（GPU インスタンシング）が持つ。
    // 塊で出すと団子に見えるうえ、風で揺らせない。ここは幹と枝だけ。
    // 枝先の座標は下に書き出しておく（配置スクリプトが葉の房の中心に使う）。
    m.Group("TreeTwig", "Wood_Beam");
    foreach (V3 tip in tips)
    {
        int nb = 3 + (int)(r.Next() * 3f);
        for (int b = 0; b < nb; b++)
        {
            float a = r.Range(0f, MathF.Tau);
            float len = r.Range(1.6f, 2.8f);
            var t2 = tip + new V3(MathF.Cos(a) * len, r.Range(0.3f, 1.1f), MathF.Sin(a) * len);
            LegQuad(m, tip, t2, TrunkR * r.Range(0.12f, 0.20f));
        }
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 900, 1100, 30f, 8f);
    float top = 0f;
    foreach (var f in m.Faces) foreach (V3 p in f.poly) if (p.Y > top) top = p.Y;
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                      + $"（全高 {top:0.00}m / 幹の根元 直径 {TrunkR * 2f:0.00}m）");

    // 幹の表面の 1 点。③ 樹皮の溝 ── 角度方向は細かく、**縦方向は粗く**相関させる。
    //   縦の周波数を下げる（t * 1.6）ことで、溝が上下に continuous に走る。
    V3 TrunkPt(int i, int s)
    {
        float a = MathF.Tau * s / Seg;
        float t = i / (float)Rings;
        float groove = Noise.Fbm(new V3(MathF.Cos(a) * 4f, t * 1.6f, MathF.Sin(a) * 4f), 0.7f, 2);
        float rr = rad[i] * (1f + groove * 0.22f);
        return trunk[i] + new V3(MathF.Cos(a) * rr, 0f, MathF.Sin(a) * rr);
    }
}

// ───────────────────────────────────────────────────────────────
// ステージ 1：左手奥の川
//
//   ★中心線は草の生成とも共有する（`RiverDistance`）。共有しないと草が水面から生える。
//
//   ★浅瀬にしてある。音響の地面は y=0 の平らな箱で、川はその上に載るだけ。
//     深い溝を掘ると「見えている水面より上を歩く」ことになるし、
//     溝にコライダーを付けると外接が伸びて格子に効く。
//     渡れる浅い流れなら、地面の箱を 1mm も触らずに済む。
//
//   高さの取り方（★効くのは**音響用の地面**の上面 y=0。見た目の地面 -0.03 ではない）:
//     水面   y = +0.03  … 音響の箱より 3cm 上。下げると箱に埋まって見えない
//     岸     y = +0.16  … 水際から 1.4m で持ち上がる。低い土手
//
//   ★UV は流れ方向に沿って張る（`AddPolygon(p, uv)`）。
//     v が川下へ進むので、マテリアル側で v をスクロールさせれば流れて見える。
//     自動投影のままだと世界軸に貼られて、曲がったところで流れが横に走る。
// ───────────────────────────────────────────────────────────────

// 川の中心線の制御点。両端はボーダー(30m)の外へ出しておく（流れ込んで流れ去る形）。
static V3[] RiverControl() => new[]
{
    // ★川は**左（-X）**。2026-08-21 確定。
    //   ここまで 3 回位置を取り違えた原因は「Unity の OBJ 読み込みが X を反転する」ことで、
    //   それは `ObjWriter` 側で打ち消した（そちらの★を参照）。
    //   打ち消した今、**ここの符号がそのまま画面の左右**になる。-X が左。
    //   z=-25 付近だけ外へ振ってあるのは、木を (14.5,-25) に置いたときの名残ではなく
    //   川筋の形として残している（緩い蛇行）。
    new V3(  -8f, 0f,  48f),
    new V3( -12f, 0f,  34f),
    new V3( -16f, 0f,  18f),
    new V3( -19f, 0f,   2f),
    new V3( -22f, 0f, -12f),
    new V3( -26f, 0f, -25f),
    new V3( -18.5f, 0f, -37.5f),
    new V3( -10f, 0f, -48f),
};

// 中心線を折れ線に落とす。トップレベルには静的な入れ物が置けないので毎回作る
// （制御点 8 個・分割 16 なので安い）。
static V3[] RiverPath()
{
    if (RiverCache.Path != null) return RiverCache.Path;
    var c = RiverControl();
    var pts = new List<V3>();
    for (int i = 1; i < c.Length - 2; i++)
        for (int s = 0; s < 16; s++)
            pts.Add(CatmullRom(c[i - 1], c[i], c[i + 1], c[i + 2], s / 16f));
    pts.Add(c[c.Length - 2]);
    return RiverCache.Path = pts.ToArray();
}

static V3 CatmullRom(V3 a, V3 b, V3 c, V3 d, float t)
{
    float t2 = t * t, t3 = t2 * t;
    return (b * 2f + (c - a) * t + (a * 2f - b * 5f + c * 4f - d) * t2
            + (b * 3f - a - c * 3f + d) * t3) * 0.5f;
}

/// 中心線までの水平距離。草を川に生やさないために共有する。
static float RiverDistance(float x, float z)
{
    var p = RiverPath();
    float best = float.MaxValue;
    for (int i = 0; i < p.Length - 1; i++)
    {
        float ax = p[i].X, az = p[i].Z, bx = p[i + 1].X, bz = p[i + 1].Z;
        float dx = bx - ax, dz = bz - az;
        float len2 = dx * dx + dz * dz;
        float t = len2 < 1e-6f ? 0f : Math.Clamp(((x - ax) * dx + (z - az) * dz) / len2, 0f, 1f);
        float qx = ax + dx * t - x, qz = az + dz * t - z;
        float d = MathF.Sqrt(qx * qx + qz * qz);
        if (d < best) best = d;
    }
    return best;
}

static void BuildRiver(string path)
{
    // ★音響用の地面（`Ground` の箱）の上面が y=0 なので、その**上**へ出すこと。
    //   見た目の地面（Ground_Visual・上面 -0.03）に合わせて -0.02 にしていたら、
    //   音響の箱に丸ごと埋まって一度も見えていなかった。
    // ★段差を作る。水面と地面の差が 13cm しかないと、目線 1.6m からは平地に見える。
    //   音響の地面（y=0 の平らな箱）があるので**掘り下げられない**
    //   ── 掘ると水の上を歩くことになる。だから**土手を上げる**。
    //   土手には既にコライダーが付いているので、歩いて越えられる（10 度の坂）。
    // ★水面は野原メッシュの河床（0.10m）の上。土手の天端 1.00m との差が 0.78m。
    // ★水位を 0.22 → 0.40m へ（発注者の指示）。河床 0.10m なので水深 0.30m。
    // ★水面の幅は**固定**にして、汀線は**地形との交差**で決めさせる。
    //   幅を揺らすと、水面の縁が土手の斜面から浮いて板に見える。
    //   広めに敷いて土手に埋めれば、地形のノイズがそのまま自然な汀線になる。
    const float WaterY = 0.40f;
    // ★土手を広く取る。草が 0.35m あるので、水面（y=+0.03）は横から見ると草に隠れる。
    //   川が「在る」と分かるのは**開けた帯**のほうで、水面そのものではない。
        const float TileM = 3f;           // テクスチャ 1 枚あたりの長さ(m)

    var p = RiverPath();
    var m = new ObjMesh();
    var r = new Rng(51720260);

    // 各点での接線・法線・川幅を先に作る。
    int n = p.Length;
    var side = new V3[n];
    var half = new float[n];
    var along = new float[n];
    for (int i = 0; i < n; i++)
    {
        V3 a = p[Math.Max(0, i - 1)], b = p[Math.Min(n - 1, i + 1)];
        V3 tan = new V3(b.X - a.X, 0f, b.Z - a.Z).Normalized;
        side[i] = new V3(-tan.Z, 0f, tan.X);
        // 川幅は 2.6〜4.2m で緩く揺らす。一定だと水路に見える。
        // 川幅は 4.6〜7.4m。細いと帯にしか見えないので、前より広くした。
        // 幅は固定。土手に埋まった分は見えないので、汀線は地形が決める。
        half[i] = 5.2f;
        if (i > 0)
        {
            float dx = p[i].X - p[i - 1].X, dz = p[i].Z - p[i - 1].Z;
            along[i] = along[i - 1] + MathF.Sqrt(dx * dx + dz * dz);
        }
    }

    // ── 水面 ────────────────────────────────────────────
    // 横に 3 分割して、中央をわずかに下げる（縁が浅く見える）。
    m.Group("RiverWater", "River_Water");
    for (int i = 0; i < n - 1; i++)
        for (int k = 0; k < 3; k++)
        {
            float u0 = k / 3f * 2f - 1f, u1 = (k + 1) / 3f * 2f - 1f;   // -1..1
            V3 a0 = Edge(i, u0), a1 = Edge(i, u1);
            V3 b0 = Edge(i + 1, u0), b1 = Edge(i + 1, u1);
            var uv = new[]
            {
                ((u0 + 1f) * 0.5f, along[i] / TileM), ((u0 + 1f) * 0.5f, along[i + 1] / TileM),
                ((u1 + 1f) * 0.5f, along[i + 1] / TileM), ((u1 + 1f) * 0.5f, along[i] / TileM),
            };
            // ★巻き方向。**a0→b0→b1→a1 だと法線が真下を向く。**
            //   接線 × 横方向 = -Y になるため。裏面は描かれないので、
            //   この向きだと水面が**一度も見えない**（実際そうなっていた）。
            //   逆順にして上を向かせる。UV も同じ順に入れ替える。
            m.AddPolygon(new[] { a1, b1, b0, a0 }, new[] { uv[3], uv[2], uv[1], uv[0] });
        }

    // ── 岸（両側） ──────────────────────────────────────
    // ★土手のジオメトリはここでは作らない。`BuildGrassField` が地形として持つ。
    //   前は川メッシュに土手を付けていたが、地形と二重になって段差が出る。
    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 900, 1300, 20f, 34f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                      + $"（長さ {along[n - 1]:0.0}m / 幅 {half[0] * 2f:0.0}m 前後 / 水面 {WaterY:0.00}m）");

    // 中心から u（-1..1）だけ横へ寄った水際の点。
    V3 Edge(int i, float u) => new V3(p[i].X + side[i].X * half[i] * u, WaterY,
                                      p[i].Z + side[i].Z * half[i] * u);

}

// ───────────────────────────────────────────────────────────────
// 草原の扉 ── 祭壇のような石の門
//
//   ★草原専用。他の世界は `Prop_Door.obj` / `Prop_DoorSlab.obj` のまま。
//   ★設定画は両開きだが、**板は 1 枚**にしてある（発注者の指示）。
//     物理で回るのは `Prop_DoorSlab_Grassland.obj` 側で、寸法の契約は現行と同じ:
//       原点＝板の中心 / 1.1 × 2.2 × 0.08m / 蝶番は −X / 取っ手は +X
//
//   原点は**敷居の中央・基壇の上面**。正面は −Z。
//   配置側で 1.2m（扉の丘の頂上）へ上げ、プレイヤーのほうへ向ける。
//
//   高さの取り方:
//     0.00        基壇の上面（＝丘の頂上）
//     0.00..0.72  石段 4 段（蹴上 0.18m）。登って扉の前に立つ
//     0.72..2.92  開口 1.1 × 2.2m ＝ 板とちょうど同じ
//     2.92..3.47  半円アーチ（飾り。開口は矩形のまま）
//     3.47..3.95  帯
//     3.95..      崩れた頂部
// ───────────────────────────────────────────────────────────────
static void BuildGrasslandShrine(string path)
{
    const float Sill = 0.72f;              // 敷居の高さ（石段の上）
    // ★開口を広げた（1.1 → 1.6m）。プレイヤーの当たり判定は直径 0.70m なので **2.3 倍**。
    //   ⚠ 板の寸法も一緒に変わる。ゲームシステム側の物理ボディの BoxCollider が
    //     1.1×2.2×0.08m 前提なので、**そちらの変更が要る**（板に起票）。
    // ★開口は 1.6 × 3.0m。**高さは開口そのもので稼ぐ。**
    //   前は開口 2.2m の上に石を 4.7m 積んで背を出したので、門ではなく
    //   「穴の空いた壁」に見えていた。開口が全高の 3 割しかないとそうなる。
    const float OpenW = 1.6f, OpenH = 3.0f;
    const float WallT = 0.70f;             // 壁の厚み
    const float HalfW = 3.40f;             // 全体の半幅（全幅 6.8m）
    const float ArchR = 0.80f;             // アーチ内半径（開口の半分）

    var m = new ObjMesh();
    var r = new Rng(20260820);
    float top = Sill + OpenH;              // 2.92

    // ── 石段（基壇）──────────────────────────────────────
    // 手前（−Z）へ 4 段。上ほど狭くして、登る先が絞られて見えるようにする。
    //
    // ★横幅は壁（全幅 5.2m）より外へ出す。基壇が壁より細いと、門が板に見える。
    // ★前へ寄せる。壁の裏側は誰も行かないので、後ろに伸ばしても無駄になる。
    // ★どの段も底を y=-0.6 まで下ろす。丘の平場は半径 3m しかないので、
    //   広い段の外側は斜面に載る。底を深くしておかないと角が浮く。
    m.Group("ShrineSteps", "Shrine_Step");
    for (int i = 0; i < 4; i++)
    {
        float y1 = Sill - 0.18f * i;
        float w = 3.2f + 0.52f * i;                 // 下の段ほど広い（最下段 全幅 9.5m）
        // ★土台だけ Z を -0.35 ずらす（壁・アーチ・柱は動かさない）。
        const float Shift = -0.35f;
        float z0 = -0.9f * (i + 1) + Shift, z1 = 0.9f + Shift;
        m.AddBox(new V3(-w, -0.6f, z0), new V3(w, y1, z1));
    }

    // ── 壁（積み石） ────────────────────────────────────
    // 一枚板にすると石に見えないので、段（コース）に割って目地を彫る。
    m.Group("ShrineWall", "Shrine_Stone");
    const float Course = 0.34f;
    float lintel = top + 0.26f;                              // まぐさの上端
    // ★アーチの上に「屋階」を積んで背を出す。開口の高さ（2.2m）は板の寸法契約なので
    //   触れない ── 高さは**開口より上**で稼ぐ。
    const float Attic = 0.35f;                               // アーチと帯のあいだの積み石（薄く）
    float band = lintel + ArchR + 0.38f + Attic;             // 帯（軒）の下端

    // ★まぐさから帯までを埋めること。ここを空けると、アーチ（半径 0.93m）の
    //   外側が幅いっぱい・高さ 1.2m の穴になる（実際に空いていた）。
    //   アーチは**開いた穴ではなく壁に彫った飾り**なので、後ろは塞がっているのが正しい。
    Wall(Sill, top, withOpening: true);                      // 開口の左右
    // ★まぐさの段の**袖**も積むこと。まぐさは開口の幅しか埋めないので、
    //   ここを飛ばすと開口の左右に高さ 0.26m の穴が残る（下から 9 段目の上に空いていた）。
    Wall(top, lintel, withOpening: true);
    m.AddBox(new V3(-OpenW * 0.5f, top, -WallT * 0.5f),
             new V3(OpenW * 0.5f, lintel, WallT * 0.5f));    // まぐさ
    Wall(lintel, band, withOpening: false);                  // アーチが載る壁（全幅）

    // アーチの内側を少し彫り込む（ティンパヌム）。奥へ落とすと影が溜まって、
    // 平らな壁のままより「アーチが在る」ことが読める。
    m.Group("ShrineTympanum", "Shrine_Deep");
    for (int i = 0; i < 18; i++)
    {
        float a0 = MathF.PI * i / 18, a1 = MathF.PI * (i + 1) / 18;
        V3 P(float a) => new V3(-MathF.Cos(a) * ArchR, lintel + MathF.Sin(a) * ArchR,
                                -WallT * 0.5f + 0.06f);
        m.AddPolygon(new V3(0f, lintel, -WallT * 0.5f + 0.06f), P(a0), P(a1));
    }

    // ── アーチ（迫石） ──────────────────────────────────
    //   壁石と同じく **1 石ずつ面取りして目地を出す。**
    //   前は面取り無しの六面体だったので、24 個に割ってあっても継ぎ目が見えず、
    //   のっぺりした帯にしか見えなかった。
    m.Group("ShrineArch", "Shrine_Stone");
    const int Vou = 15;                    // 分割を増やすほどアーチが丸くなる
    {
        float ri = ArchR, ro = ArchR + 0.38f;
        float zf = -WallT * 0.5f - 0.05f, zb = WallT * 0.5f + 0.05f;
        // 角度 a・半径 rr・奥行 z の 1 点。a=0 が左の迫元、a=π が右。
        V3 P(float a, float rr, float z) =>
            new V3(-MathF.Cos(a) * rr, lintel + MathF.Sin(a) * rr, z);

        const float Bev = 0.026f;
        float ba = Bev / ri;               // 面取り幅を角度に直す（内周の弧長で測る）

        // (半径, 奥行) を断面にして**角度方向へ**押し出す。壁石の x を a に読み替えただけ。
        //   接線 × 半径方向 = 奥行 の順が右手系なので、巻き方向はそのまま通る。
        var thin = BevelOct(ri, ro, zf, zb, Bev, Bev);
        var full = BevelOct(ri, ro, zf, zb, Bev, 0f);

        for (int i = 0; i < Vou; i++)
        {
            float a0 = MathF.PI * i / Vou, a1 = MathF.PI * (i + 1) / Vou;
            var angs = new[] { a0, a0 + ba, a1 - ba, a1 };
            var rings = new[] { thin, full, full, thin };

            for (int k = 0; k < 3; k++)
            {
                var (ra, rb) = (rings[k], rings[k + 1]);
                float aa = angs[k], ab = angs[k + 1];
                for (int t = 0; t < 8; t++)
                {
                    int t2 = (t + 1) % 8;
                    m.AddPolygon(P(aa, ra[t].u, ra[t].v), P(aa, ra[t2].u, ra[t2].v),
                                 P(ab, rb[t2].u, rb[t2].v), P(ab, rb[t].u, rb[t].v));
                }
            }
            // 迫面（隣の楔石と接する面）。ここが見えるから目地になる。
            var cA = new V3[8];
            var cB = new V3[8];
            for (int t = 0; t < 8; t++)
            {
                cA[t] = P(a0, thin[7 - t].u, thin[7 - t].v);
                cB[t] = P(a1, thin[t].u, thin[t].v);
            }
            m.AddPolygon(cA);
            m.AddPolygon(cB);
        }
    }

    // ── 付柱（左右） ────────────────────────────────────
    // 設定画の柱。溝を数本だけ彫る（本式の 20 溝は近寄らないと分からない）。
    m.Group("ShrineColumn", "Shrine_Column");
    for (int s = -1; s <= 1; s += 2)
    {
        float cx = s * 2.10f;
        // 丸柱にする。角柱だと稜が立って硬い。
        float cz = -WallT * 0.5f - 0.34f;   // 壁から 0.34m 前へ。溶けると柱に見えない
        Prism(m, cx, cz, Sill - 0.10f, Sill + 0.16f, 0.40f, 0.34f, 24, 0f);   // 柱礎
        // 柱も背に合わせて伸ばす。開口の高さで止めると、上の石積みだけが伸びて頭でっかちになる。
        float capY = lintel + ArchR + 0.30f;
        FlutedShaft(m, cx, cz, Sill + 0.16f, capY, 0.34f, 0.29f, 16, 0.024f, 5);
        Prism(m, cx, cz, capY, capY + 0.18f, 0.29f, 0.40f, 24, 0f);           // 柱頭（エキノス）
        m.AddBox(new V3(cx - 0.42f, capY + 0.18f, cz - 0.42f),
                 new V3(cx + 0.42f, capY + 0.36f, cz + 0.42f));                // アバクス
    }

    // ── 蔓の茎（葉が絡む骨組みだけ） ────────────────────
    m.Group("ShrineVine", "Wood_Beam");
    for (int i = 0; i < 4; i++)
    {
        float s = i < 2 ? -1f : 1f;
        float x = s * r.Range(2.0f, HalfW);
        var a = new V3(x, Sill - 0.1f, -WallT * 0.5f - 0.04f);
        float y = a.Y;
        while (y < band - 0.3f)
        {
            float dy = r.Range(0.3f, 0.6f);
            var b = new V3(a.X + r.Range(-0.28f, 0.28f), y + dy, -WallT * 0.5f - r.Range(0.02f, 0.09f));
            LegQuad(m, a, b, 0.022f);
            a = b;
            y += dy;
        }
    }

    // ── 帯と、崩れた頂部 ────────────────────────────────
    m.Group("ShrineCornice", "Shrine_Stone");
    // 軒。背が高くなったぶん厚く・張り出しも大きく（上が薄いと頭でっかちに見えない代わりに貧相になる）。
    m.AddBox(new V3(-HalfW - 0.20f, band, -WallT * 0.5f - 0.24f),
             new V3(HalfW + 0.20f, band + 0.30f, WallT * 0.5f + 0.24f));
    m.AddBox(new V3(-HalfW - 0.10f, band + 0.30f, -WallT * 0.5f - 0.14f),
             new V3(HalfW + 0.10f, band + 0.48f, WallT * 0.5f + 0.14f));

    // 崩れ。石を不揃いに残して、抜けを作る。
    m.Group("ShrineRubble", "Shrine_Step");
    // ★等幅・等高に並べると城の狭間（クレネル）になる。実際そう見えていた。
    //   幅も高さも大きく振り、左へ行くほど崩れが進む勾配を付けて、
    //   「割れて残った」形にする。奥行きも 1 石ずつ変える。
    float bx = -HalfW;
    while (bx < HalfW - 0.1f)
    {
        float w = r.Range(0.16f, 0.92f);
        // 左ほど低く、右ほど残っている（一方向に崩れた形）。
        float lean = Math.Clamp((bx + HalfW) / (HalfW * 2f), 0f, 1f);
        float h = r.Range(0.06f, 0.40f) + lean * r.Range(0.12f, 0.70f);
        float zf = -WallT * 0.5f + r.Range(0f, 0.12f);
        float zb = WallT * 0.5f - r.Range(0f, 0.12f);
        if (r.Next() > 0.10f)
        {
            m.AddBox(new V3(bx, band + 0.48f, zf), new V3(bx + w, band + 0.48f + h, zb));
            // たまに 2 段目を半分ずらして載せる。1 段だと横一列に見える。
            if (r.Next() < 0.28f && h > 0.3f)
            {
                float w2 = w * r.Range(0.35f, 0.7f);
                float ox = r.Range(0f, w - w2);
                m.AddBox(new V3(bx + ox, band + 0.48f + h, zf + 0.05f),
                         new V3(bx + ox + w2, band + 0.48f + h + r.Range(0.14f, 0.45f), zb - 0.05f));
            }
        }
        bx += w + r.Range(0f, 0.07f);
    }

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 1100, 1000, 18f, 6f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                      + $"（全幅 {HalfW * 2f:0.0}m / 開口 {OpenW:0.0}×{OpenH:0.0}m / 敷居 {Sill:0.00}m）");

    // y0..y1 を段に割って積む。withOpening のときだけ開口（±OpenW/2）を空ける。
    // ★段の高さを揃えない。等間隔だと目地が横縞になり、石積みではなく
    //   サイディングに見える（実際そう見えていた）。段ごとに 0.7〜1.35 倍で振る。
    void Wall(float y0, float y1, bool withOpening)
    {
        int n = Math.Max(1, (int)MathF.Round((y1 - y0) / Course));
        var hs = new float[n];
        float sum = 0f;
        for (int i = 0; i < n; i++) { hs[i] = r.Range(0.7f, 1.35f); sum += hs[i]; }
        float y = y0;
        for (int i = 0; i < n; i++)
        {
            float ch = (y1 - y0) * hs[i] / sum;
            bool offset = (i & 1) == 1;
            if (withOpening)
            {
                Courses(-HalfW, -OpenW * 0.5f, y, ch, offset);
                Courses(OpenW * 0.5f, HalfW, y, ch, offset);
            }
            else Courses(-HalfW, HalfW, y, ch, offset);
            y += ch;
        }
    }

    // 1 コース分の積み石を x0..x1 に並べる。目地を彫るため 1 石ずつ奥行きを変える。
    void Courses(float x0, float x1, float y, float h, bool offset)
    {
        float x = x0;
        if (offset) x -= 0.24f;
        while (x < x1 - 0.02f)
        {
            float w = MathF.Min(r.Range(0.82f, 1.34f), x1 - x);
            if (w < 0.08f) break;
            // 石ごとに前へ出したり引っ込めたりする。全部同じ深さだと線が揃って縞に見える。
            // ★前後のずれは控えめに。大きいと段の横線がぶれて、積んだ石に見えない。
            float inset = r.Range(-0.005f, 0.009f);
            // ★段の高さは h ちょうど。**削ってはいけない。**
            //   前は天端を 12〜42mm 下げていて、そこに**壁の厚みを貫く水平の隙間**が
            //   できていた（壁が 1 層の箱なので、目地がそのまま穴になる）。
            //   目地は隙間ではなく**面取り**で出す。
            Stone(MathF.Max(x0, x), x + w, y, y + h,
                  -WallT * 0.5f + inset, WallT * 0.5f - inset, r.Range(0.020f, 0.030f));
            x += w;
        }
    }

    // 縁を面取りした積み石。断面の八角形を x 方向へ押し出し、**両端も絞る**。
    //
    //   隣の石とは x0/x1 の面で接したまま、角に V 字の谷ができるので、
    //   **貫通する隙間を作らずに目地が見える**。
    //   端を絞らないと x 方向の端が平らなまま隣とベタ付きになり、
    //   横目地だけあって**縦目地が出ない**（実際そうなっていた）。
    //
    //   x 方向に 4 枚の輪を並べる: x0(絞) → x0+bx(素) → x1-bx(素) → x1(絞)
    void Stone(float x0, float x1, float y0, float y1, float z0, float z1, float b)
    {
        b = MathF.Min(b, MathF.Min(y1 - y0, z1 - z0) * 0.30f);
        float bx = MathF.Min(b, (x1 - x0) * 0.30f);

        var thin = BevelOct(y0, y1, z0, z1, b, bx);   // 両端の絞った輪
        var full = BevelOct(y0, y1, z0, z1, b, 0f);   // 中ほどの輪
        var xs = new[] { x0, x0 + bx, x1 - bx, x1 };
        var rings = new[] { thin, full, full, thin };

        for (int k = 0; k < 3; k++)
        {
            var (ra, rb) = (rings[k], rings[k + 1]);
            float xa = xs[k], xb = xs[k + 1];
            for (int i = 0; i < 8; i++)
            {
                int j = (i + 1) % 8;
                m.AddPolygon(new V3(xa, ra[i].u, ra[i].v), new V3(xa, ra[j].u, ra[j].v),
                             new V3(xb, rb[j].u, rb[j].v), new V3(xb, rb[i].u, rb[i].v));
            }
        }

        // 木口。壁の中で隣と接するので見えないことが多いが、**開口の側面と壁の端では見える。**
        //
        // ★巻き方向に注意。`AddPolygon` は最初の 3 点から法線を出すので、
        //   順序を間違えると裏面になり、Cull Back で消える ── そうなると
        //   横から見たとき石の中が透けて、**面取りの帯だけが板のように残る**（実際そうなっていた）。
        //   x0 側は −X を向くので **逆順**、x1 側は +X なので **そのまま**。
        var capA = new V3[8];
        var capB = new V3[8];
        for (int i = 0; i < 8; i++)
        {
            capA[i] = new V3(x0, thin[7 - i].u, thin[7 - i].v);
            capB[i] = new V3(x1, thin[i].u, thin[i].v);
        }
        m.AddPolygon(capA);
        m.AddPolygon(capB);
    }
}

// 面取りした八角断面。u=(y や 半径) v=(z や 奥行) の 2 軸を b だけ角取りし、
// さらに d だけ全周を内側へ寄せる（押し出しの両端を絞るのに使う）。
//   ★点の並びはこの順を崩さないこと。Stone / Voussoir の側面と木口の
//     巻き方向がこの順序に乗っている。逆にすると裏面になって面が消える。
static (float u, float v)[] BevelOct(float u0, float u1, float v0, float v1, float b, float d)
{
    u0 += d; u1 -= d; v0 += d; v1 -= d;
    return new (float u, float v)[]
    {
        (u0, v0 + b), (u0 + b, v0), (u1 - b, v0), (u1, v0 + b),
        (u1, v1 - b), (u1 - b, v1), (u0 + b, v1), (u0, v1 - b),
    };
}

// 8 点の六面体。角柱を傾けたいとき用（アーチの楔石など）。
//   f0..f3 が手前の面（この順で外向き）、b0..b3 が奥。
static void Hexa(ObjMesh m, V3 f0, V3 f1, V3 f2, V3 f3, V3 b0, V3 b1, V3 b2, V3 b3)
{
    m.AddPolygon(f0, f1, f2, f3);
    m.AddPolygon(b3, b2, b1, b0);
    m.AddPolygon(f0, f3, b3, b0);
    m.AddPolygon(f1, b1, b2, f2);
    m.AddPolygon(f0, b0, b1, f1);
    m.AddPolygon(f3, f2, b2, b3);
}

// ───────────────────────────────────────────────────────────────
// 草原の扉の板（1 枚）
//   ★寸法の契約は現行と同じ。原点＝板の中心 / 1.1 × 2.2 × 0.08m / 蝶番 −X / 取っ手 +X
//   ★設定画の「水色 → 白 でグラデーション」は **UV の v** に載せてある。
//     鏡板の表裏だけ UV を明示して、v が 0（下＝水色）から 1（上＝白）へ進む。
//     マテリアル側に縦のグラデーションを貼れば、そのまま設定画の色になる。
// ───────────────────────────────────────────────────────────────
static void BuildGrasslandDoorSlab(string path)
{
    // ★門の開口に合わせて 1.1 → 1.6m。**ゲームシステム側の当たり判定も同じ寸法へ**。
    const float W = 1.6f, H = 3.0f, T = 0.08f;
    const float Stile = 0.14f;
    float x0 = -W * 0.5f, x1 = W * 0.5f, y0 = -H * 0.5f, y1 = H * 0.5f;
    float z0 = -T * 0.5f, z1 = T * 0.5f;

    var m = new ObjMesh();

    // ── 枠 ──────────────────────────────────────────────
    m.Group("DoorFrame", "Shrine_Stone");
    m.AddBox(new V3(x0, y0, z0), new V3(x0 + Stile, y1, z1));
    m.AddBox(new V3(x1 - Stile, y0, z0), new V3(x1, y1, z1));
    m.AddBox(new V3(x0 + Stile, y1 - Stile, z0), new V3(x1 - Stile, y1, z1));
    m.AddBox(new V3(x0 + Stile, y0, z0), new V3(x1 - Stile, y0 + Stile, z1));

    // ── 鏡板（グラデーションを載せる面） ────────────────
    m.Group("DoorPanel", "Door_Aqua");
    float px0 = x0 + Stile, px1 = x1 - Stile, py0 = y0 + Stile, py1 = y1 - Stile;
    float pz = T * 0.5f - 0.012f;
    (float u, float v) Uv(float x, float y) => ((x - px0) / (px1 - px0), (y - py0) / (py1 - py0));

    // 表（+Z）と裏（−Z）。v が下から上へ 0→1。
    m.AddPolygon(new[] { new V3(px0, py0, pz), new V3(px1, py0, pz),
                         new V3(px1, py1, pz), new V3(px0, py1, pz) },
                 new[] { Uv(px0, py0), Uv(px1, py0), Uv(px1, py1), Uv(px0, py1) });
    m.AddPolygon(new[] { new V3(px1, py0, -pz), new V3(px0, py0, -pz),
                         new V3(px0, py1, -pz), new V3(px1, py1, -pz) },
                 new[] { Uv(px1, py0), Uv(px0, py0), Uv(px0, py1), Uv(px1, py1) });
    // 鏡板の胴（framing の内側の見付け）
    m.AddBox(new V3(px0, py0, -pz), new V3(px1, py1, pz));

    // ── 渦の彫り ────────────────────────────────────────
    // 設定画の波打つ意匠。細い帯を S 字に 3 本、表側へ浮かせる。
    m.Group("DoorRelief", "Shrine_Stone");
    for (int k = 0; k < 3; k++)
    {
        float phase = k * 0.8f;
        const int Seg = 22;
        for (int i = 0; i < Seg; i++)
        {
            float t0 = i / (float)Seg, t1 = (i + 1) / (float)Seg;
            float ya = py0 + (py1 - py0) * t0, yb = py0 + (py1 - py0) * t1;
            float xa = MathF.Sin(t0 * MathF.Tau + phase) * (W * 0.22f);
            float xb = MathF.Sin(t1 * MathF.Tau + phase) * (W * 0.22f);
            const float Rib = 0.022f;
            m.AddPolygon(new V3(xa - Rib, ya, pz + 0.010f), new V3(xb - Rib, yb, pz + 0.010f),
                         new V3(xb + Rib, yb, pz + 0.010f), new V3(xa + Rib, ya, pz + 0.010f));
        }
    }

    // ── 取っ手（自由端＝+X 側） ─────────────────────────
    m.Group("DoorKnob", "Bell_Bronze");
    float kx = x1 - Stile * 0.6f, ky = -0.05f;
    Blob(m, new V3(kx, ky, z1 + 0.045f), 0.036f, 12, 8);
    Blob(m, new V3(kx, ky, z0 - 0.045f), 0.036f, 12, 8);
    m.AddBox(new V3(kx - 0.012f, ky - 0.012f, z0 - 0.05f), new V3(kx + 0.012f, ky + 0.012f, z1 + 0.05f));

    m.Write(path, "StageModels.mtl");
    Preview.WriteSvg(m.Faces, Path.ChangeExtension(path, ".preview.svg"), 700, 1100, 24f, 6f);
    Console.WriteLine($"{path}  三角形 {m.TriangleCount}"
                      + $"（{W:0.0} × {H:0.0} × {T:0.00}m・原点＝中心・蝶番 −X）");
}

// ★格子から 7 万回呼ばれる川筋を覚えておく入れ物。
//   トップレベル プログラムではファイル直下に static フィールドを置けず、
//   型宣言もローカル関数より後（＝ファイル末尾）にしか置けない。
static class RiverCache { public static V3[] Path; }
