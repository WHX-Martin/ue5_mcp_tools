// ComfyTextureStudio · 主面板（PRD §7.2 单页 / 左右分栏 / 三态推进）
//
// 三个区块就是向导的三步：
//   ① 生成基础贴图（提示词 → SDXL+LoRA → 审阅）
//   ② 生成 PBR 全套（确认后跑几何反演）
//   ③ 导入 UE（命名 → 导入 4 张 + 建材质 + 实例）
//
// UI 只负责「收集参数、驱动状态机、显示结果」。所有网络与资产操作都在
// FComfyTextureJob 与 FComfyTextureImportService 里，这里不自己发请求。

#pragma once

#include "ComfyHttpClient.h"
#include "ComfyTextureJob.h"
#include "ComfyImportTypes.h"
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UTexture2D;
struct FSlateDynamicImageBrush;   // 注意是 struct，不是 class（不一致会触发 C4099）
struct FComfyOutputAssetSpec;

/** 面板里的一次导入结果（供 §7.4 的报告区展示）。 */
struct FComfyPanelImportState
{
	bool bRan = false;
	bool bSuccess = false;
	FString ErrorText;
	FComfyImportReport Report;
	FString ReportText;
};

class SComfyTextureStudioPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SComfyTextureStudioPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SComfyTextureStudioPanel();

private:
	// ------------------------------------------------------------------ 布局 //
	TSharedRef<SWidget> BuildHeaderRow();
	TSharedRef<SWidget> BuildStageASection();
	TSharedRef<SWidget> BuildPreviewColumn();
	TSharedRef<SWidget> BuildStageBSection();
	TSharedRef<SWidget> BuildImportSection();

	TSharedRef<SWidget> BuildLabeledRow(const FText& Label, TSharedRef<SWidget> Content, const FText& Tooltip = FText());
	TSharedRef<SWidget> BuildChannelThumb(const FString& Semantic);
	TSharedRef<SWidget> BuildTiledPreview(const FString& PngPath, float TileSize);

	// ------------------------------------------------------------ 任务控制 //
	void StartStageA(bool bNewSeed);
	void RequestContinueToStageB();
	void RequestCancel();
	void RequestImport();
	void DiscardStageAResult();

	/** 面板打开时异步拉一次 /object_info，填充 LoRA / 采样器 / 调度枚举。 */
	void ProbeServer();

	void OnJobStateChanged(EComfyJobState NewState, const FString& Text);
	void OnJobFinished(const FComfyJobResult& InResult);

	// -------------------------------------------------------------- 状态查询 //
	EActiveTimerReturnType OnActiveTimer(double InCurrentTime, float InDeltaTime);

	bool IsJobRunning() const;
	EComfyJobState GetJobState() const { return LastState; }
	bool CanGenerate() const;
	bool CanReviewStageA() const;
	bool CanContinueToStageB() const;
	bool CanImport() const;
	bool CanDiscardStageA() const;

	FText GetConnectionText() const;
	FSlateColor GetConnectionColor() const;
	FText GetStatusText() const;
	FSlateColor GetStatusColor() const;
	TOptional<float> GetProgressFraction() const;
	FText GetProgressOverlay() const;
	FText GetStageAInfoText() const;
	FText GetImportHintText() const;
	FText GetResolvedNamePreview() const;

	// ---------------------------------------------------------------- 缩略图 //
	const FSlateBrush* GetPreviewBrush(const FString& PngPath);
	void ReleasePreviewTextures();

	// -------------------------------------------------------------- 选项读写 //
	UTexture2D* LoadTransientTexture(const FString& PngPath);

	// ------------------------------------------------------------- 通道路径 //
	FString GetChannelPngPath(const FString& Semantic) const;

	/** 组装导入请求（含命名解析）。失败时返回 false 并填 OutError。 */
	bool BuildImportRequest(FComfyImportRequest& OutRequest, FString& OutError);

	/** 从 /object_info 里取某个 COMBO 输入的可选值。 */
	void CacheEnumFromObjectInfo(const TSharedPtr<FJsonObject>& ObjectInfo,
		const FString& NodeClass, const FString& InputName, TArray<TSharedPtr<FString>>& OutValues);

	// ----------------------------------------------------------------- 数据 //
	TUniquePtr<FComfyTextureJob> Job;
	TUniquePtr<FComfyHttpClient> ProbeClient;
	TSharedPtr<FActiveTimerHandle> ActiveTimerHandle;

	EComfyJobState LastState = EComfyJobState::Idle;
	FString StatusText;
	FComfyJobResult LastResult;
	FComfyPanelImportState ImportState;

	/** /object_info 探测结果（决定连接指示灯与生成按钮是否可点）。 */
	bool bServerReachable = false;
	FString ServerProbeText;

	/** 任务开始时刻，仅用于状态栏里的"已耗时"。 */
	double JobStartTime = 0.0;

	/** 阶段 A 的历史候选（PRD §7.2 的 #1~#4）。只留路径，缩略图按需加载。 */
	TArray<FString> StageAHistory;
	/** 当前在预览区展示的那张（默认取最新）。 */
	FString FocusedPreviewPath;

	/** 6 个通道的本地 PNG 路径（阶段 B 完成后填充）。 */
	TMap<FString, FString> ChannelPngPaths;

	/** 透明贴图缓存（key = PNG 绝对路径）。 */
	TMap<FString, TObjectPtr<UTexture2D>> TextureCache;
	TMap<FString, TSharedPtr<FSlateDynamicImageBrush>> BrushCache;

	// ---- 阶段 A 参数 ----
	FString MaterialDescription = TEXT("coarse pebbly sand, beige, weathered, scattered small stones");
	FString NegativePrompt;
	FString PositivePrompt;
	int32 Seed = 20261013;
	int32 Steps = 30;
	float CFG = 6.5f;
	int32 Width = 1024;
	int32 Height = 1024;
	bool bLockSquare = true;
	FString LoraName;
	float LoraStrengthModel = 1.0f;
	float LoraStrengthClip = 1.0f;

	TSharedPtr<FString> SelectedSampler;
	TSharedPtr<FString> SelectedScheduler;

	TArray<TSharedPtr<FString>> SamplerOptions;
	TArray<TSharedPtr<FString>> SchedulerOptions;
	TArray<TSharedPtr<FString>> LoraOptions;

	// ---- 阶段 B 参数 ----
	bool bUseHeightParallax = true;

	// ---- 阶段 ③ 命名与选项 ----
	bool bUseTemplateNaming = true;
	FString ManualName = TEXT("Sand_Coarse_01");
	FString TemplateMaterial = TEXT("Sand");
	FString TemplateGrain = TEXT("Coarse");
	bool bImportOrphanMaps = false;
	bool bCreateMaterialInstance = true;
	bool bFlipGreenChannel = true;
	float BumpDepth = 0.02f;

	/** manifest 里的通道规格（属性表的来源，只读）。 */
	TMap<FString, FComfyOutputAssetSpec> ChannelSpecs;
	FString SpecLoadError;
};
