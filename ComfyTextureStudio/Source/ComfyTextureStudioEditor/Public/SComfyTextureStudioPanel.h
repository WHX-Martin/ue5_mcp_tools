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
#include "ComfyPromptScaffold.h"
#include "ComfyTextureJob.h"
#include "ComfyImportTypes.h"
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FComfyPreviewTexture;
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
	/** 取预览资源；文件读不到且缓存里也没有时返回 nullptr。 */
	TSharedPtr<FComfyPreviewTexture> GetPreviewTexture(const FString& PngPath);
	const FSlateBrush* GetPreviewBrush(const FString& PngPath);
	void ReleasePreviewTextures();

	/**
	 * 在独立窗口里放大查看某张 PNG。
	 *
	 * 面板里的预览最大只有 150px，而贴图是 1024² —— 接缝与噪点根本看不出来。
	 * **每次调用都新建一个窗口**（不复用）：并排对比 albedo / normal 时正需要这个。
	 *
	 * @param bStartTiled 打开时直接进 2×2 平铺模式（点面板上的平铺图时用）
	 */
	void OpenImageViewer(const FString& PngPath, const FString& Title, bool bStartTiled = false);

	// -------------------------------------------------------------- 选项读写 //
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

	/**
	 * 预览资源缓存（key = PNG 绝对路径）。
	 *
	 * 用 FComfyPreviewTexture 而不是「裸 UTexture2D + 裸 brush」：它自带
	 * AddToRoot/RemoveFromRoot 与 brush 的强引用，这份资源可以**安全地共享给查看器窗口**
	 * （引用计数保证窗口活着贴图就活着）。
	 *
	 * 共享是必需的，不只是省事：导入完成后中转 PNG 会被删掉
	 * （Settings::PreserveDownloadedPng 默认 false），查看器若重新读盘就会打不开 ——
	 * 2026-10-13 用户报的「关掉预览窗口后再点没反应」正是这个。
	 */
	TMap<FString, TSharedPtr<FComfyPreviewTexture>> PreviewCache;

	// ---- 阶段 A 参数 ----
	/**
	 * 正向提示词的编辑状态（固定骨架 + 可编辑的描述）。
	 *
	 * 逻辑全在 ComfyPrompt::FComfyPromptEditor 里，**不在这里手写拼接** ——
	 * 2026-10-13 的事故就是"每次编辑都从完整提示词反推骨架"，描述被清空的瞬间
	 * 骨架被吞掉且永久丢失。那段逻辑原来藏在面板的匿名命名空间里，
	 * 既没法单测也不能回归，所以现在抽出去配 `-TestPromptScaffold` 负向测试。
	 */
	ComfyPrompt::FComfyPromptEditor PromptEditor;
	FString NegativePrompt;

	/** manifest 里的采样器/调度默认值 —— 探测拿到真实枚举后要回退到它，而不是列表第 0 项。 */
	FString SamplerDefault = TEXT("dpmpp_2m");
	FString SchedulerDefault = TEXT("karras");

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
