// AcousticFlowWorld.h ── Unreal と自前のエンジンの間の司令塔（Unity の AcousticWorld に当たる物）。場面に 1 つ置く。
//
// ■ 全体の中の位置
//   場面（印を付けた箱・扉・UAcousticFlowEmitter・プレイヤーのカメラ）
//     → **ここ**（毎フレーム: 動く箱 → 聞き手 → 音源 → 摘み → AF_WorldUpdate を 1 回 → 各 Voice へ配分 → 台帳へ公開）
//     → Wwise のバス「AcousticFlow」の自前プラグインが、台帳から Voice を引いて鳴らす。
//   ★毎フレームの呼ぶ順は Unity の AcousticWorld と同じ。エンジンの計算は 1 行も変えていない（変わるのは器だけ）。
// ■ 役割
//   ・起動時: 印の付いた箱を集めて世界を作り、部屋グラフを組み、部屋ごとの FDN と方向バスを作る。
//   ・毎フレーム: 上の順に押し、Wwise のプラグインが読む台帳を書く。
// ■ 中の仕組み（座標）
//   エンジンへは m・y 上（AcousticFlowSpace）で渡す。台帳へは **Wwise が見ている座標**（Unreal の cm・Z 上のまま）で渡す。
//   Unreal 版の Wwise は Unreal の座標を変換せずに Wwise へ渡しているので、台帳も同じ座標にしないと位置で引けない。
//   「同じ音源とみなす距離」の単位は AF_HostSetUnitsPerMeter(100) で伝える。
// ■ 繋がり
//   受ける: 箱の印（部品のタグ AF_Wall = 動かない壁、AF_Door = 動く板）、UAcousticFlowEmitter、プレイヤーのカメラ。
//   渡す:   エンジン（AF_World*）、音源の Voice（配分）、台帳（AF_Host*）。
// ■ エリアの表示（AcousticFlowWorldAreas.cpp）
//   Play していないエディタでも、印の付いた箱から下見用のエンジンの世界を組み、部屋グラフを描く:
//   床の色タイル（部屋ごと）・部屋の番号と体積と RT60・戸口の枠・エンジンが見ている箱の輪郭・音源の幅。
//   壁（AF_Wall）が動いたら組み直し（0.25 秒ごとに見る）、扉（AF_Door）は位置だけ送る。Play 中は 8 キーで出す。
// ■ 退けた書き方
//   ・場面の全部の箱を拾う（Unity の既定）: Unreal は床の飾りや見えない当たりも箱なので、何が壁か分からなくなる。印で選ぶ。
//   ・部屋の形をエンジンから多角形で受け取る: そういう関数は無い（部屋は升目の塊）。AF_WorldRoomAt を床の升目で引いて塗る。
// ■ 壊れる所
//   ・扉に AF_Door（動く）の印が無いと、閉じた扉が部屋グラフに焼かれて戸口が消え、部屋が 2 つに割れたまま開かない。
//   ・器（FDN）を作り直したのに音源へ繋ぎ直さないと、音源は壊された器へ送り続ける。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "acoustic_world.h"
#include "acoustic_voice.h"
#include "AcousticFlowWorld.generated.h"

class UAcousticFlowEmitter;
class UStaticMeshComponent;

UCLASS()
class ACOUSTICFLOWUE_API AAcousticFlowWorld : public AActor
{
	GENERATED_BODY()

public:
	AAcousticFlowWorld();

	/// この UWorld の司令塔（無ければ nullptr）。
	static AAcousticFlowWorld* Find(UWorld* World);

	/// Wwise の標本化周波数（Wwise の初期化設定と同じにする）。Voice と器はこれで作る。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow")
	int32 SampleRate = 48000;

	// ── 摘み（Unity の AcousticWorld の既定と同じ値）──
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") int32 RaysPerEmitter = 256;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") int32 EarlyModel = 4;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float PrecedenceDb = 6.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float LateDistancePow = 1.5f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") float AdjacentContain = 1.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") bool bLanePanSplit = false;   // かぶりなし（Unity の既定と同じ）
	// 五成分の重み（エネルギーに掛かる。0.5 で −3 dB）。Unity の weightDirect〜weightTransmit と同じ並び・同じ意味。
	//   ★形（RT60・向き・遅れ）は空間から、量は耳で決める（S2）。Play 中は 9 キーで響き（後期）だけを 0/−3/−6/−9/−12 dB と回せる。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights") float WeightDirect = 1.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights") float WeightEarly = 1.0f;
	/// 響き（後期）は −9 dB（0.126）を既定に（2026-10-01、9 キーで耳で決めた。Unity の weightLate と同じ値）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights") float WeightLate = 0.126f;
	/// 隣の部屋の響き（2026-10-01）。音源が別の部屋にいるときの後期だけに、WeightLate へさらに掛ける（同じ部屋の音源には効かない）。
	///   Play 中は 0 キーで 0/−3/−6/−9/−12 dB を回す。★Unity の AcousticWorld.weightLateAdjacent と同じ値にそろえる。
	///   ★既定 0 dB（1）＝ 2026-10-04 に発注者の原則で戻した（10-01 の −12 dB を取り消し）。エリア（響き）は音源に帰属し、
	///     耳の部屋で量を変えない。中に入ったときに変わるのは包まれ具合（洞窟の口を入った瞬間に響きが 12 dB 跳ねていた）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights") float WeightLateAdjacent = 1.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights") float WeightDiffract = 1.0f;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights") float WeightTransmit = 1.0f;
	/// 影のこもり（2026-10-06、dB、0..24）。遮られた直接の道（透過・回折）だけ、高域を余分に落とす（125 Hz は 0、4 kHz で −この値）。
	///   直接・初期・響きには効かない。Play 中は − キーで 0/3/6/9/12 dB を回す。★Unity の AcousticWorld.shadowMuffleDb と同じ値にそろえる。
	///   ★既定 6 dB は仮（耳で決めるまで）。エンジンの既定は 0 ＝ 物理どおり。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Weights", meta = (ClampMin = "0", ClampMax = "24")) float ShadowMuffleDb = 6.0f;

	/// 耳をキャラクターの頭に置く（三人称の地図）。切ればカメラ（一人称の地図）。向きはどちらもカメラ。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow") bool bListenerAtPawn = false;

	// ── 部屋の割り方（Unity の AcousticWorld の roomSeedRadius / outsideMouth と同じ名前・同じ既定）。組む前に渡す ──
	/// 部屋を口で割る半径（m、既定 0.6）。幅がこの 2 倍に満たない口で部屋が分かれる。洞窟の口（幅 3.5 m）なら 2.1。
	///   ★部屋のいちばん狭い所の半分より大きくすると、その廊下は部屋にならない。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Rooms", meta = (ClampMin = "0")) float RoomSeedRadiusM = 0.6f;
	/// 外への口（既定 切）。入れると、部屋と外の間の口も口になり、外に立っている耳に部屋の響きが口から漏れて聞こえる
	///   （洞窟の前で中の響き）。部屋の響きの長さにも口から逃げる分が入る。切ると、外では部屋の響きが聞こえない。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Rooms") bool bOutsideMouth = false;

	// ── エリアの表示（エディタの下見と、Play 中の 8 キー）──
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Areas") bool bShowAreasInEditor = true;
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Areas") bool bShowAreasInGame = false;
	/// 床を塗る升目（m）。細かいほど部屋の境目が正確だが、引く回数が増える（14 m 四方・0.25 m で 3,600 回）。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Areas", meta = (ClampMin = "0.05")) float AreaCellM = 0.25f;
	/// どの高さの断面で部屋を引くか（m、エンジンの y）。耳の高さ。
	UPROPERTY(EditAnywhere, Category = "AcousticFlow|Areas") float AreaSampleHeightM = 1.6f;

	void Register(UAcousticFlowEmitter* Emitter);
	void Unregister(UAcousticFlowEmitter* Emitter);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;
	/// エディタ（Play していない）でも Tick する ── エリアの下見のため。ゲームの処理は Play 中だけ。
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void BeginDestroy() override;

private:
	struct FBoxEntry { TWeakObjectPtr<UStaticMeshComponent> Comp; int32 Id = -1; bool bDynamic = false; FTransform Last; int32 Preset = 1; };
	struct FAreaRun { int32 Room = -1; float X0 = 0, X1 = 0, Z = 0; };          // 床の升目の横一列の塊（エンジンの座標）
	struct FAreaLabel { FVector Pos; TArray<FString> Lines; FColor Color; };    // 画面に正対して出す文字

	void CollectBoxes();
	void CollectBoxesInto(AF_WorldHandle W, TArray<FBoxEntry>& Out) const;
	void ApplyRoomSettings(AF_WorldHandle W) const;
	// エリアの表示（AcousticFlowWorldAreas.cpp）
	void TickEditorPreview();
	void RebuildPreview(uint32 Hash);
	uint32 WallHash() const;
	void ComputeAreas(AF_WorldHandle W);
	void DrawAreas(AF_WorldHandle W) const;
	void DrawAreaLabels(class UCanvas* Canvas, class APlayerController* PC);
	void EnsureLabelDraw();
	void ReleasePreview();

	AF_WorldHandle Preview = nullptr;                         // エディタの下見用の世界（Play 中の World とは別）
	TArray<FBoxEntry> PreviewBoxes;
	uint32 PreviewHash = 0;
	uint32 PendingHash = 0;                                   // 壁が動いている間は組み直さない（止まって 0.3 秒たったら組む）
	double PendingSince = 0.0;
	double PreviewNextCheck = 0.0;
	TArray<FAreaRun> AreaRuns;
	TArray<float> RoomFloorY;                                 // 部屋ごとの床の高さ（エンジンの y、m）
	TArray<FAreaLabel> AreaLabels;
	float AreaCellUsed = 0.25f;
	FDelegateHandle LabelDrawHandle;
	void CreateShared();
	void RebindFdn();
	void PushKnobs();
	void PublishToHost();

	AF_WorldHandle World = nullptr;
	AF_FdnMixHandle Fdn = nullptr;
	AF_DirectionBusHandle Bus = nullptr;
	AF_HrtfHandle BusHrtf = nullptr;
	TArray<TPair<double, AF_FdnMixHandle>> RetiredFdn;       // 作り直した古い器（1 秒後に壊す）
	TArray<FBoxEntry> Boxes;
	TArray<TWeakObjectPtr<UAcousticFlowEmitter>> Emitters;
	double LastStatusTime = 0.0;                              // 画面の状態を出した時刻
	int32 LastBlocks = 0;                                     // その時のプラグインの累計ブロック数（1 秒あたりの回数を出す）
	bool bListenerCameraOnly = true;                          // 音源の Wwise の聞き手: カメラ 1 人（既定）／既定の全員（7 キー）
	long long LastTotMatched = 0, LastTotMissed = 0;          // 対応付けの累計（前回の表示のとき）
	AF_Vector3 ListenerPos{ 0, 0, 0 };                        // 聞き手（エンジンの座標）。今いる部屋を出す
	AF_Vector3 ListenerFwd{ 0, 0, 1 }, ListenerUp{ 0, 1, 0 }; // 聞き手の向き（録画の記録に書く）
	// 録画（-AFCapture、AcousticFlowWorldCapture.cpp）: エンジンへ渡した物を Saved/Wwise/AFCapture_scene.txt に書く
	void BeginCapture();
	void CaptureEmitter(const UAcousticFlowEmitter* E, int32 Id);
	void CaptureFrame(float Dt);
	void EndCapture();
	void CaptureLine(const FString& Line);
	float CaptureTime() const;
	FArchive* CaptureLog = nullptr;
	uint64 CaptureQpc = 0;
	FString CaptureKnobs;                                     // 前に書いた摘み（変わったときだけ書く）
};
