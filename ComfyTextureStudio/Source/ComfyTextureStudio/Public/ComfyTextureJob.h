// ComfyTextureStudio · 两阶段任务状态机（PRD §3.2 / §3.3）
//
// 用法（两处共用同一份代码）：
//   · commandlet（M1）：循环 Tick，直到 IsFinished()
//   · 编辑器面板（M2）：FTSTicker 每帧调 Tick
//
// Tick 内部会自动泵 HTTP，所以调用方不需要自己驱动 FHttpManager。

#pragma once

#include "ComfyHttpClient.h"
#include "ComfyTypes.h"
#include "ComfyWorkflowGraph.h"
#include "CoreMinimal.h"

class UComfyTextureStudioSettings;

/** 一次任务的输入参数。留 0 / 空表示沿用 manifest 里的默认值。 */
struct FComfyTextureJobParams
{
	/** 任务 id，也用作产物子目录名。空则自动生成时间戳 + GUID 短码。 */
	FString JobId;

	FString PositivePrompt;
	FString NegativePrompt;
	int32 Seed = -1;
	int32 Steps = 0;
	int32 Width = 0;
	int32 Height = 0;
	int32 BatchSize = 0;

	/** 换 LoRA 的通道（Q7）。空则用 manifest 默认值。 */
	FString LoraName;

	/** 任意语义键覆盖（字符串形式，按槽位声明类型转换）。 */
	TMap<FString, FString> SlotOverrides;

	/** 产物根目录的绝对路径。空则用插件设置里的 CacheDir。 */
	FString OutputRootAbs;
};

/** 一次任务的结果。 */
struct FComfyJobResult
{
	bool bSuccess = false;
	FString JobId;
	FString OutputDir;

	FString StageAPromptId;
	FString StageBPromptId;
	FString BaseTexturePath;      // 阶段 A 的受光贴图
	FString UploadedInputName;    // 上传到 ComfyUI 的名字

	/** 语义键 → 本地路径。6 个通道都会在这里。 */
	TMap<FString, FString> ChannelPaths;

	double StageASeconds = 0.0;
	double StageBSeconds = 0.0;

	int64 VramTotalBytes = 0;
	int64 VramFreeBeforeBytes = 0;

	FComfyJobError Error;

	/** 交付通道（Q3：ORM 合并 ⇒ albedo/normal/height/orm）。 */
	TArray<FString> GetDeliverableChannels() const { return ComfyChannels::Deliverable(); }
};

class COMFYTEXTURESTUDIO_API FComfyTextureJob
{
public:
	FComfyTextureJob();
	~FComfyTextureJob();

	/** 起任务。之后必须持续 Tick。 */
	void Start(const FComfyTextureJobParams& InParams);

	/** 推进状态机（并泵 HTTP）。DeltaSeconds 决定本次泵多久。 */
	void Tick(float DeltaSeconds);

	/** 取消：走 /interrupt + /queue 删除两条路径（PRD §9.3）。 */
	void Cancel();

	bool IsFinished() const;
	EComfyJobState GetState() const { return State; }
	const FString& GetProgressText() const { return ProgressText; }
	const FComfyJobResult& GetResult() const { return Result; }
	FComfyHttpClient& GetHttpClient() { return Http; }

	/** 状态/进度变化（M2 的 UI 接这里刷新）。 */
	TFunction<void(EComfyJobState, const FString&)> OnStateChanged;

	/** 终态回调（Ready / Failed / Cancelled 都会来一次）。 */
	TFunction<void(const FComfyJobResult&)> OnFinished;

private:
	enum class EStep : uint8
	{
		None,
		Prepare,
		PreflightStats,
		PreflightObjectInfo,
		PreflightLocalFiles,
		PreflightEnums,
		StageASubmit,
		StageAPoll,
		StageADownload,
		UploadInput,
		StageBSubmit,
		StageBPoll,
		StageBDownload,
		Complete,
	};

	void Advance(EStep NextStep);
	void SetState(EComfyJobState NewState, const FString& Text);
	void Fail(EComfyErrorKind Kind, const FString& Message, const FString& NodeId, const FString& Detail);

	void Step_Prepare();
	void Step_PreflightStats();
	void Step_PreflightObjectInfo();
	void Step_PreflightLocalFiles();
	void Step_PreflightEnums();
	void Step_StageASubmit();
	void Step_StageAPoll();
	void Step_StageADownload();
	void Step_UploadInput();
	void Step_StageBSubmit();
	void Step_StageBPoll();
	void Step_StageBDownload();
	void Step_Complete();

	/** 用户参数覆盖（只作用于该图确实有的槽位）。 */
	bool ApplyUserOverrides(FComfyWorkflowGraph& Graph, FString& OutError);

	/** 把 runtimeValuePattern 展开成真实值（输出前缀等）。 */
	bool ApplyRuntimePatterns(FComfyWorkflowGraph& Graph, FString& OutError);

	FString ResolvePluginPath(const FString& MaybeRelative) const;
	FString MakeOutputDir() const;
	const FComfyImageRef* FindImageForNode(const TArray<FComfyImageRef>& Images, const FString& NodeId) const;
	void PrepareStageBDownloads();

	const UComfyTextureStudioSettings* Settings = nullptr;

	FComfyHttpClient Http;
	TMap<FString, FComfyStageSpec> StageSpecs;
	FComfyWorkflowGraph GraphA;
	FComfyWorkflowGraph GraphB;
	TSharedPtr<FJsonObject> ObjectInfo;

	EStep Step = EStep::None;
	EComfyJobState State = EComfyJobState::Idle;
	FString ProgressText;
	FComfyJobResult Result;
	FComfyTextureJobParams Params;

	bool bWaiting = false;
	double StartTime = 0.0;
	double StageAStartTime = 0.0;
	double StageBStartTime = 0.0;
	double NextPollTime = 0.0;
	FString CurrentPromptId;

	TArray<FComfyImageRef> PendingImages;
	TArray<FComfyImageRef> PendingDownloads;
	TArray<FString> PendingDownloadSemantics;
	int32 DownloadIndex = 0;
};
