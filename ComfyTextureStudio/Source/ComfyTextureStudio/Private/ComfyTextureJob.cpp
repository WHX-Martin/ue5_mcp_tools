#include "ComfyTextureJob.h"

#include "ComfyTextureStudio.h"
#include "ComfyTextureStudioSettings.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"

namespace
{
	/** 只留文件系统安全字符（JobId 会用作目录名与上传文件名）。 */
	FString SanitizeJobId(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (const TCHAR Ch : In)
		{
			const bool bSafe =
				(Ch >= 'A' && Ch <= 'Z') || (Ch >= 'a' && Ch <= 'z') ||
				(Ch >= '0' && Ch <= '9') || Ch == '-' || Ch == '_';
			Out.AppendChar(bSafe ? Ch : TEXT('_'));
		}
		return Out.IsEmpty() ? FString(TEXT("job")) : Out;
	}
}

FComfyTextureJob::FComfyTextureJob() = default;

FComfyTextureJob::~FComfyTextureJob() = default;

// --------------------------------------------------------------------------- //
// 生命周期
// --------------------------------------------------------------------------- //

void FComfyTextureJob::Start(const FComfyTextureJobParams& InParams)
{
	Settings = GetDefault<UComfyTextureStudioSettings>();

	Params = InParams;
	Params.JobId = SanitizeJobId(Params.JobId.IsEmpty()
		? FString::Printf(TEXT("%s_%s"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6))
		: Params.JobId);

	Result = FComfyJobResult();
	Result.JobId = Params.JobId;
	Result.OutputDir = MakeOutputDir();

	StartTime = FPlatformTime::Seconds();
	Step = EStep::Prepare;
	bWaiting = false;

	SetState(EComfyJobState::Preflight, TEXT("准备：装载工作流与槽位清单"));
}

void FComfyTextureJob::Tick(float DeltaSeconds)
{
	if (IsFinished())
	{
		return;
	}

	// 泵 HTTP。FHttpManager 的完成回调就是在这里面触发的，所以下面每个
	// 「等待中」判断都可能在 Pump 之后失效 —— 顺序不能颠倒。
	Http.Pump(DeltaSeconds);

	if (IsFinished())
	{
		return;
	}

	// 超时只在**真正在跑**的时候计时。停在 PauseForReview 等用户确认不算超时 ——
	// 否则用户切出去喝杯咖啡回来就发现任务"超时失败"了。
	const bool bPausedForReview = (Step == EStep::PauseForReview);
	if (!bPausedForReview && Settings != nullptr &&
		(FPlatformTime::Seconds() - StartTime) > static_cast<double>(Settings->JobTimeoutSeconds))
	{
		Fail(EComfyErrorKind::Timeout,
			FString::Printf(TEXT("整任务超过 %.0f 秒"), Settings->JobTimeoutSeconds),
			FString(), FString());
		return;
	}

	if (bWaiting)
	{
		return;
	}

	if ((Step == EStep::StageAPoll || Step == EStep::StageBPoll) &&
		FPlatformTime::Seconds() < NextPollTime)
	{
		return;
	}

	switch (Step)
	{
	case EStep::Prepare:            Step_Prepare(); break;
	case EStep::PreflightStats:     Step_PreflightStats(); break;
	case EStep::PreflightObjectInfo:Step_PreflightObjectInfo(); break;
	case EStep::PreflightLocalFiles:Step_PreflightLocalFiles(); break;
	case EStep::PreflightEnums:     Step_PreflightEnums(); break;
	case EStep::StageASubmit:       Step_StageASubmit(); break;
	case EStep::StageAPoll:         Step_StageAPoll(); break;
	case EStep::StageADownload:     Step_StageADownload(); break;
	case EStep::PauseForReview:     Step_PauseForReview(); break;
	case EStep::UploadInput:        Step_UploadInput(); break;
	case EStep::StageBSubmit:       Step_StageBSubmit(); break;
	case EStep::StageBPoll:         Step_StageBPoll(); break;
	case EStep::StageBDownload:     Step_StageBDownload(); break;
	case EStep::Complete:           Step_Complete(); break;
	default: break;
	}
}

void FComfyTextureJob::Cancel()
{
	if (IsFinished())
	{
		return;
	}

	Result.Error.Kind = EComfyErrorKind::Cancelled;
	Result.Error.Message = TEXT("用户取消");
	SetState(EComfyJobState::Cancelled, TEXT("已取消"));

	// PRD §9.3：/interrupt 只作用于**当前正在执行**的节点；如果任务还躺在 pending
	// 队列里没轮到，必须再走一次 /queue 把条目删掉。两条路径都要走。
	Http.Interrupt([](bool) {});
	if (!CurrentPromptId.IsEmpty())
	{
		Http.DeleteQueueItem(CurrentPromptId, [](bool) {});
	}
	// 取消请求得真的发出去；commandlet 环境下调用方随后就退出了。
	Http.Pump(0.3f);

	if (OnFinished)
	{
		OnFinished(Result);
	}
}

bool FComfyTextureJob::IsFinished() const
{
	return State == EComfyJobState::Ready
		|| State == EComfyJobState::Done
		|| State == EComfyJobState::Failed
		|| State == EComfyJobState::Cancelled;
}

// --------------------------------------------------------------------------- //
// 小工具
// --------------------------------------------------------------------------- //

void FComfyTextureJob::SetState(EComfyJobState NewState, const FString& Text)
{
	State = NewState;
	ProgressText = Text;
	UE_LOG(LogComfyTextureStudio, Log, TEXT("[%s] %s · %s"),
		*Result.JobId, LexToString(NewState), *Text);
	if (OnStateChanged)
	{
		OnStateChanged(State, ProgressText);
	}
}

void FComfyTextureJob::Advance(EStep NextStep)
{
	Step = NextStep;
	bWaiting = false;
}

void FComfyTextureJob::Fail(EComfyErrorKind Kind, const FString& Message,
	const FString& NodeId, const FString& Detail)
{
	if (IsFinished())
	{
		return;
	}

	Result.Error.Kind = Kind;
	Result.Error.NodeId = NodeId;
	Result.Error.Message = Message;
	Result.Error.Detail = Detail;

	UE_LOG(LogComfyTextureStudio, Error, TEXT("[%s] 任务失败：%s"), *Result.JobId, *Result.Error.ToString());
	UE_LOG(LogComfyTextureStudio, Error, TEXT("  说明：%s"), *Result.Error.UserText());
	UE_LOG(LogComfyTextureStudio, Error, TEXT("  建议：%s"), *Result.Error.SuggestedAction());
	if (!Detail.IsEmpty())
	{
		UE_LOG(LogComfyTextureStudio, Error, TEXT("  详情：%s"), *Detail.Left(1500));
	}

	SetState(EComfyJobState::Failed, Result.Error.ToString());
	if (OnFinished)
	{
		OnFinished(Result);
	}
}

FString FComfyTextureJob::ResolvePluginPath(const FString& MaybeRelative) const
{
	if (FPaths::IsRelative(MaybeRelative))
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ComfyTextureStudio")))
		{
			return FPaths::Combine(Plugin->GetBaseDir(), MaybeRelative);
		}
	}
	return MaybeRelative;
}

FString FComfyTextureJob::MakeOutputDir() const
{
	const FString Root = Params.OutputRootAbs.IsEmpty()
		? (Settings != nullptr ? Settings->GetCacheDirAbs() : FPaths::ProjectSavedDir())
		: Params.OutputRootAbs;
	return FPaths::Combine(Root, Params.JobId);
}

const FComfyImageRef* FComfyTextureJob::FindImageForNode(
	const TArray<FComfyImageRef>& Images, const FString& NodeId) const
{
	return Images.FindByPredicate(
		[&NodeId](const FComfyImageRef& Ref) { return Ref.NodeId == NodeId; });
}

// --------------------------------------------------------------------------- //
// 步骤：准备
// --------------------------------------------------------------------------- //

bool FComfyTextureJob::ApplyUserOverrides(FComfyWorkflowGraph& Graph, FString& OutError)
{
	auto ApplyIfPresent = [&Graph, &OutError](const FString& Key, const FString& Value)
	{
		if (Value.IsEmpty() || !Graph.HasSlot(Key))
		{
			return true;
		}
		return Graph.SetSlotFromString(Key, Value, OutError);
	};

	if (!ApplyIfPresent(TEXT("positive"), Params.PositivePrompt)) { return false; }
	if (!ApplyIfPresent(TEXT("negative"), Params.NegativePrompt)) { return false; }
	if (Params.Seed   >= 0 && !ApplyIfPresent(TEXT("seed"), FString::FromInt(Params.Seed)))     { return false; }
	if (Params.Steps  >  0 && !ApplyIfPresent(TEXT("steps"), FString::FromInt(Params.Steps)))   { return false; }
	if (Params.Width  >  0 && !ApplyIfPresent(TEXT("width"), FString::FromInt(Params.Width)))   { return false; }
	if (Params.Height >  0 && !ApplyIfPresent(TEXT("height"), FString::FromInt(Params.Height))) { return false; }
	if (Params.BatchSize > 0 && !ApplyIfPresent(TEXT("batchSize"), FString::FromInt(Params.BatchSize))) { return false; }
	if (!ApplyIfPresent(TEXT("lora"), Params.LoraName)) { return false; }

	for (const TPair<FString, FString>& Pair : Params.SlotOverrides)
	{
		if (!ApplyIfPresent(Pair.Key, Pair.Value))
		{
			return false;
		}
	}
	return true;
}

bool FComfyTextureJob::ApplyRuntimePatterns(FComfyWorkflowGraph& Graph, FString& OutError)
{
	for (const TPair<FString, FComfySlotBinding>& Pair : Graph.GetSpec().Slots)
	{
		const FComfySlotBinding& Binding = Pair.Value;
		if (Binding.RuntimePattern.IsEmpty())
		{
			continue;
		}
		// 输入图由 ReplaceInputPlaceholder 单独处理：它的真实值要等上传结果，
		// 而且必须校验占位符确实被替换了。
		if (Pair.Key == TEXT("inputImage"))
		{
			continue;
		}

		FString Value = Binding.RuntimePattern;
		Value.ReplaceInline(TEXT("{jobId}"), *Params.JobId, ESearchCase::CaseSensitive);

		if (!Graph.SetSlotString(Pair.Key, Value, OutError))
		{
			return false;
		}
	}
	return true;
}

void FComfyTextureJob::Step_Prepare()
{
	if (Settings == nullptr)
	{
		Fail(EComfyErrorKind::LocalIOFailed, TEXT("插件设置不可用（拿不到 CDO）"), FString(), FString());
		return;
	}

	if (!IFileManager::Get().MakeDirectory(*Result.OutputDir, true))
	{
		Fail(EComfyErrorKind::LocalIOFailed,
			FString::Printf(TEXT("建不了输出目录：%s"), *Result.OutputDir), FString(), FString());
		return;
	}

	FString Error;
	if (!FComfyWorkflowGraph::LoadManifest(ResolvePluginPath(Settings->WorkflowManifestFile), StageSpecs, Error))
	{
		Fail(EComfyErrorKind::LocalIOFailed, Error, FString(), FString());
		return;
	}

	const FComfyStageSpec* SpecA = StageSpecs.Find(TEXT("stageA"));
	const FComfyStageSpec* SpecB = StageSpecs.Find(TEXT("stageB"));
	if (SpecA == nullptr || SpecB == nullptr)
	{
		Fail(EComfyErrorKind::LocalIOFailed, TEXT("槽位清单缺少 stageA 或 stageB"), FString(), FString());
		return;
	}

	if (!GraphA.LoadFromFile(ResolvePluginPath(Settings->StageAWorkflowFile), *SpecA, Error) ||
		!GraphB.LoadFromFile(ResolvePluginPath(Settings->StageBWorkflowFile), *SpecB, Error))
	{
		Fail(EComfyErrorKind::LocalIOFailed, Error, FString(), FString());
		return;
	}

	if (!ApplyUserOverrides(GraphA, Error) || !ApplyUserOverrides(GraphB, Error) ||
		!ApplyRuntimePatterns(GraphA, Error) || !ApplyRuntimePatterns(GraphB, Error))
	{
		Fail(EComfyErrorKind::LocalIOFailed, Error, FString(), FString());
		return;
	}

	Http.Configure(Settings->ServerUrl, Settings->RequestTimeoutSeconds, Settings->VerboseHttpLog);

	UE_LOG(LogComfyTextureStudio, Log, TEXT("[%s] 产物目录 %s"), *Result.JobId, *Result.OutputDir);
	Advance(EStep::PreflightStats);
}

// --------------------------------------------------------------------------- //
// 步骤：预检
// --------------------------------------------------------------------------- //

void FComfyTextureJob::Step_PreflightStats()
{
	SetState(EComfyJobState::Preflight, TEXT("预检：探活 ComfyUI 并读显存"));
	bWaiting = true;

	Http.GetSystemStats([this](const FComfySystemStats& Stats, const FString& Error, bool bTransportFailure)
	{
		bWaiting = false;

		if (bTransportFailure || !Stats.bValid)
		{
			Fail(EComfyErrorKind::NotConnected,
				Error.IsEmpty() ? FString(TEXT("system_stats 没有返回可用设备")) : Error,
				FString(), FString());
			return;
		}

		Result.VramTotalBytes = Stats.VramTotal;
		Result.VramFreeBeforeBytes = Stats.VramFree;

		UE_LOG(LogComfyTextureStudio, Log,
			TEXT("ComfyUI 就绪：%s · 显存 free %.2f / %.2f GiB · RAM free %.2f GiB"),
			*Stats.DeviceName,
			Stats.VramFree / 1073741824.0, Stats.VramTotal / 1073741824.0,
			Stats.RamFree / 1073741824.0);

		// 显存不够就早提示（PRD §9.1 的 R8）—— 这里只告警，不阻断。
		if (Stats.VramTotal > 0 && Stats.VramFree < static_cast<int64>(3.0 * 1073741824.0))
		{
			UE_LOG(LogComfyTextureStudio, Warning,
				TEXT("显存余量偏低（%.2f GiB），大 batch 可能 OOM"), Stats.VramFree / 1073741824.0);
		}

		Advance(EStep::PreflightObjectInfo);
	});
}

void FComfyTextureJob::Step_PreflightObjectInfo()
{
	SetState(EComfyJobState::Preflight, TEXT("预检：拉取 /object_info"));
	bWaiting = true;

	Http.GetObjectInfo([this](bool bOk, const TSharedPtr<FJsonObject>& Info, const FString& Error)
	{
		bWaiting = false;
		if (!bOk || !Info.IsValid())
		{
			Fail(EComfyErrorKind::NotConnected, Error, FString(), FString());
			return;
		}
		ObjectInfo = Info;

		// SeamlessDSINENormal 是整条几何链的入口。它内部 import custom_controlnet_aux.dsine，
		// 所以 controlnet_aux 缺失时它同样注册不上 —— 两种病因在 object_info 上长得一样。
		// 靠文件系统把两者分开，才能给出**可执行**的建议。
		if (!Info->HasField(TEXT("SeamlessDSINENormal")))
		{
			const FString ControlnetAuxDir =
				FPaths::Combine(Settings->GetCustomNodesDir(), TEXT("comfyui_controlnet_aux"));
			if (!IFileManager::Get().DirectoryExists(*ControlnetAuxDir))
			{
				Fail(EComfyErrorKind::MissingControlnetAux,
					FString::Printf(TEXT("找不到 %s，且 object_info 里没有 SeamlessDSINENormal"), *ControlnetAuxDir),
					FString(), FString());
			}
			else
			{
				Fail(EComfyErrorKind::MissingNodePack,
					TEXT("object_info 里没有 SeamlessDSINENormal"),
					FString(),
					FString::Printf(TEXT("目标目录 %s"), *Settings->GetCustomNodesDir()));
			}
			return;
		}

		Advance(EStep::PreflightLocalFiles);
	});
}

void FComfyTextureJob::Step_PreflightLocalFiles()
{
	SetState(EComfyJobState::Preflight, TEXT("预检：DSINE 权重完整性"));

	// 这个包「不会在用户背后下载任何东西」（源码注释原文），所以权重必须自己备好；
	// 而且要校验字节数 —— 镜像曾送出 29 MB 的残件。
	const FString DsinePath = Settings->GetDsineWeightPath();
	const int64 ActualBytes = IFileManager::Get().FileSize(*DsinePath);
	const int64 ExpectedBytes = Settings->DsineWeightExpectedBytes;

	if (ActualBytes == INDEX_NONE)
	{
		Fail(EComfyErrorKind::MissingDsineWeight,
			FString::Printf(TEXT("找不到 DSINE 权重：%s"), *DsinePath), FString(), FString());
		return;
	}
	if (ExpectedBytes > 0 && ActualBytes != ExpectedBytes)
	{
		Fail(EComfyErrorKind::DsineWeightTruncated,
			FString::Printf(TEXT("%s 是 %lld B，应为 %lld B"), *DsinePath, ActualBytes, ExpectedBytes),
			FString(), FString());
		return;
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("DSINE 权重 %lld B（符合预期）"), ActualBytes);
	Advance(EStep::PreflightEnums);
}

void FComfyTextureJob::Step_PreflightEnums()
{
	SetState(EComfyJobState::Preflight, TEXT("预检：枚举值对照实时 /object_info"));

	// manifest 里的 enum 是导出当时的快照，可能已经过时，**不能拿它判失败**。
	// 一律以 ComfyUI 实时返回的候选值为准（PRD §4.4 坑 7）。
	TArray<FString> Problems;
	FString ModelProblem;
	FString OtherProblem;

	auto CheckGraph = [this, &ModelProblem, &OtherProblem](FComfyWorkflowGraph& Graph, const TCHAR* Label)
	{
		for (const TPair<FString, FComfySlotBinding>& Pair : Graph.GetSpec().Slots)
		{
			const FComfySlotBinding& Binding = Pair.Value;
			if (!Binding.Type.Equals(TEXT("COMBO"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			FString NodeId;
			FString Widget;
			if (!Binding.ResolvedAddress.Split(TEXT("."), &NodeId, &Widget))
			{
				continue;
			}

			FString ClassType;
			if (!Graph.GetNodeClassType(NodeId, ClassType))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* ClassObj = nullptr;
			if (!ObjectInfo->TryGetObjectField(ClassType, ClassObj) || ClassObj == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (!(*ClassObj)->TryGetObjectField(TEXT("input"), Input) || Input == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Required = nullptr;
			if (!(*Input)->TryGetObjectField(TEXT("required"), Required) || Required == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonValue> WidgetDef = (*Required)->TryGetField(Widget);
			if (!WidgetDef.IsValid())
			{
				continue;
			}

			// 结构是 [ <候选值数组 或 类型名>, {选项...} ]
			const TArray<TSharedPtr<FJsonValue>>* Entry = nullptr;
			if (!WidgetDef->TryGetArray(Entry) || Entry == nullptr || Entry->Num() == 0)
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
			if (!(*Entry)[0]->TryGetArray(Choices) || Choices == nullptr || Choices->Num() == 0)
			{
				continue; // 不是 COMBO（int/float/string 无候选列表）
			}

			FString Current;
			if (!Graph.GetSlotValueAsString(Pair.Key, Current))
			{
				continue;
			}

			bool bFound = false;
			TArray<FString> ChoiceNames;
			for (const TSharedPtr<FJsonValue>& Choice : *Choices)
			{
				FString Name;
				if (Choice.IsValid())
				{
					Choice->TryGetString(Name);
				}
				if (!Name.IsEmpty())
				{
					ChoiceNames.Add(Name);
					bFound |= (Name == Current);
				}
			}

			if (!bFound && ChoiceNames.Num() > 0)
			{
				const FString Text = FString::Printf(
					TEXT("%s 的 %s（%s）当前值「%s」不在 ComfyUI 可用列表里；可用：%s"),
					Label, *Pair.Key, *ClassType, *Current, *FString::Join(ChoiceNames, TEXT(", ")));

				if (Pair.Key == TEXT("model") || Pair.Key == TEXT("lora"))
				{
					ModelProblem = Text;
				}
				else
				{
					OtherProblem = Text;
				}
			}
		}
	};

	CheckGraph(GraphA, TEXT("阶段A"));
	CheckGraph(GraphB, TEXT("阶段B"));

	if (!ModelProblem.IsEmpty())
	{
		Fail(EComfyErrorKind::MissingModel, ModelProblem, FString(), FString());
		return;
	}
	if (!OtherProblem.IsEmpty())
	{
		Fail(EComfyErrorKind::SubmitValidationFailed, OtherProblem, FString(), FString());
		return;
	}

	Advance(EStep::StageASubmit);
}

// --------------------------------------------------------------------------- //
// 步骤：阶段 A
// --------------------------------------------------------------------------- //

void FComfyTextureJob::Step_StageASubmit()
{
	SetState(EComfyJobState::Submitting, TEXT("提交阶段A（SDXL + LoRA 出受光贴图）"));

	// 留一份实际提交的图，出问题时可以和 ComfyUI 前端对照（也能直接拖进去复现）。
	FFileHelper::SaveStringToFile(GraphA.Serialize(),
		*FPaths::Combine(Result.OutputDir, TEXT("stage_a_api.json")));

	StageAStartTime = FPlatformTime::Seconds();
	bWaiting = true;

	Http.SubmitPrompt(GraphA.Serialize(), Params.JobId, [this](const FComfySubmitResult& Submit)
	{
		bWaiting = false;

		if (Submit.bTransportFailure)
		{
			// 预检刚过就断连 ⇒ 大概率是进程崩了，而不是配置问题。
			Fail(EComfyErrorKind::ServerCrashed, Submit.ErrorText, FString(), FString());
			return;
		}
		if (!Submit.NodeErrorsJson.IsEmpty())
		{
			Fail(EComfyErrorKind::SubmitValidationFailed,
				TEXT("阶段A 提交被拒（node_errors 非空，未入队）"), FString(), Submit.NodeErrorsJson);
			return;
		}
		if (!Submit.bOk)
		{
			Fail(EComfyErrorKind::SubmitValidationFailed,
				FString::Printf(TEXT("阶段A 提交失败：HTTP %d %s"), Submit.HttpStatus, *Submit.ErrorText),
				FString(), FString());
			return;
		}

		CurrentPromptId = Submit.PromptId;
		Result.StageAPromptId = Submit.PromptId;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("阶段A prompt_id = %s"), *Submit.PromptId);

		NextPollTime = FPlatformTime::Seconds() + Settings->PollIntervalSeconds;
		Advance(EStep::StageAPoll);
	});
}

void FComfyTextureJob::Step_StageAPoll()
{
	SetState(EComfyJobState::Running, TEXT("阶段A 执行中"));
	bWaiting = true;

	Http.GetHistory(CurrentPromptId, [this](const FComfyHistoryResult& History)
	{
		bWaiting = false;

		if (History.bTransportFailure)
		{
			// ⚠ 执行期断连 = 疑似 OOM 崩溃。**绝不能归类成「超时」** ——
			// 那会把用户引向「加超时」而不是「降配置」。（PRD §9.1）
			Fail(EComfyErrorKind::ServerCrashed,
				TEXT("阶段A 执行期间与 ComfyUI 的连接中断（疑似显存不足导致进程崩溃）"),
				FString(), History.ErrorText);
			return;
		}

		if (!History.bFound)
		{
			// 未完成的任务不在 /history 里，这是正常现象，继续等。
			NextPollTime = FPlatformTime::Seconds() + Settings->PollIntervalSeconds;
			return;
		}
		if (History.bFailed)
		{
			Fail(EComfyErrorKind::NodeExecutionFailed,
				History.ErrorMessage.IsEmpty() ? FString(TEXT("阶段A 执行报错")) : History.ErrorMessage,
				History.ErrorNodeId, History.ErrorDetailJson);
			return;
		}
		if (!History.bCompleted)
		{
			NextPollTime = FPlatformTime::Seconds() + Settings->PollIntervalSeconds;
			return;
		}

		Result.StageASeconds = FPlatformTime::Seconds() - StageAStartTime;
		PendingImages = History.Images;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("阶段A 完成，耗时 %.2f s，产物 %d 个"),
			Result.StageASeconds, PendingImages.Num());

		Advance(EStep::StageADownload);
	});
}

void FComfyTextureJob::Step_StageADownload()
{
	const FString* BaseNodeId = GraphA.GetSpec().OutputNodeIds.Find(TEXT("baseTexture"));
	if (BaseNodeId == nullptr)
	{
		Fail(EComfyErrorKind::LocalIOFailed, TEXT("manifest 里 stageA 没有 baseTexture 输出节点"),
			FString(), FString());
		return;
	}

	const FComfyImageRef* Image = FindImageForNode(PendingImages, *BaseNodeId);
	if (Image == nullptr)
	{
		Fail(EComfyErrorKind::DownloadFailed,
			FString::Printf(TEXT("阶段A 没有来自节点 %s 的产物"), **BaseNodeId),
			*BaseNodeId, FString());
		return;
	}

	const FString DestDir = FPaths::Combine(Result.OutputDir, TEXT("stage_a"));
	const FComfyImageRef Ref = *Image;

	SetState(EComfyJobState::Downloading, TEXT("下载阶段A 产物"));
	bWaiting = true;

	Http.DownloadImage(Ref, [this, DestDir, Ref](bool bOk, const TArray<uint8>& Bytes, const FString& Error)
	{
		bWaiting = false;
		if (!bOk)
		{
			Fail(EComfyErrorKind::DownloadFailed, Error, Ref.NodeId, Ref.ToQueryString());
			return;
		}

		// 文件名必须用 /history 报的原始名（带 _00001_ 序号），自己拼会拼错。
		const FString Dest = FPaths::Combine(DestDir, Ref.Filename);
		if (!FFileHelper::SaveArrayToFile(Bytes, *Dest))
		{
			Fail(EComfyErrorKind::LocalIOFailed, FString::Printf(TEXT("写不了 %s"), *Dest), FString(), FString());
			return;
		}

		Result.BaseTexturePath = Dest;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("阶段A 产物落盘：%s（%.0f KB）"), *Dest, Bytes.Num() / 1024.0);

		// M2：向导模式（bPauseAfterStageA）下停在这里等用户确认再往下走
		Advance(Params.bPauseAfterStageA ? EStep::PauseForReview : EStep::UploadInput);
	});
}

/**
 * 停在阶段 A 之后，等用户决定「满意就继续 / 换种子重出 / 丢弃」。
 *
 * 这里只设状态、不做任何推进 —— Tick 因为 bWaiting=true 会一直空转。
 * 关键点：这个状态**不算 IsFinished()**，所以超时计时被跳过（见 Tick），
 * 用户挂在这里多久都不会被判失败。关掉面板也不会触发取消（G2 的要求）。
 */
void FComfyTextureJob::Step_PauseForReview()
{
	bWaiting = true;
	SetState(EComfyJobState::StageAReady,
		TEXT("基础贴图已出图。满意就继续生成 PBR，不满意可换种子重出。"));
}

bool FComfyTextureJob::ContinueToStageB()
{
	if (State != EComfyJobState::StageAReady)
	{
		return false;
	}

	// 阶段 B 有自己的完整超时窗口：重新起算，不把用户审阅花掉的时间算进去。
	StartTime = FPlatformTime::Seconds();
	StageBStartTime = StartTime;

	Advance(EStep::UploadInput);
	SetState(EComfyJobState::Submitting, TEXT("上传基础贴图，准备几何反演"));
	return true;
}

// --------------------------------------------------------------------------- //
// 步骤：上传 → 阶段 B
// --------------------------------------------------------------------------- //

void FComfyTextureJob::Step_UploadInput()
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Result.BaseTexturePath))
	{
		Fail(EComfyErrorKind::LocalIOFailed,
			FString::Printf(TEXT("读不了阶段A 产物：%s"), *Result.BaseTexturePath), FString(), FString());
		return;
	}

	const FString RemoteName = FString::Printf(TEXT("cts_%s.png"), *Params.JobId);
	SetState(EComfyJobState::Submitting, TEXT("把阶段A 产物投喂给阶段B（POST /upload/image）"));
	bWaiting = true;

	Http.UploadImage(Bytes, RemoteName,
		[this](bool bOk, const FString& UploadedName, const FString& Error)
	{
		bWaiting = false;
		if (!bOk)
		{
			Fail(EComfyErrorKind::UploadFailed, Error, FString(), FString());
			return;
		}

		Result.UploadedInputName = UploadedName;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("输入图已上传为 input/%s"), *UploadedName);
		Advance(EStep::StageBSubmit);
	});
}

void FComfyTextureJob::Step_StageBSubmit()
{
	FString Error;
	const int32 Hits = GraphB.ReplaceInputPlaceholder(Result.UploadedInputName, Error);
	if (Hits < 0)
	{
		Fail(EComfyErrorKind::LocalIOFailed, Error, FString(), FString());
		return;
	}
	if (Hits != 1)
	{
		// comfy validate 不检查文件存在性 —— 阶段B 带着不存在的占位文件也报 valid=true。
		// 所以「占位符确实被替换过一次」必须由我们自己确认（M0 结论 2）。
		Fail(EComfyErrorKind::LocalIOFailed,
			FString::Printf(TEXT("阶段B 输入占位符命中 %d 次（应为 1），图可能被改动过"), Hits),
			FString(), FString::Printf(TEXT("占位符：%s"), *GraphB.GetSpec().InputPlaceholder));
		return;
	}

	SetState(EComfyJobState::Submitting, TEXT("提交阶段B（几何反演 + 去光照）"));

	FFileHelper::SaveStringToFile(GraphB.Serialize(),
		*FPaths::Combine(Result.OutputDir, TEXT("stage_b_api.json")));

	StageBStartTime = FPlatformTime::Seconds();
	bWaiting = true;

	Http.SubmitPrompt(GraphB.Serialize(), Params.JobId, [this](const FComfySubmitResult& Submit)
	{
		bWaiting = false;

		if (Submit.bTransportFailure)
		{
			Fail(EComfyErrorKind::ServerCrashed, Submit.ErrorText, FString(), FString());
			return;
		}
		if (!Submit.NodeErrorsJson.IsEmpty())
		{
			Fail(EComfyErrorKind::SubmitValidationFailed,
				TEXT("阶段B 提交被拒（node_errors 非空，未入队）"), FString(), Submit.NodeErrorsJson);
			return;
		}
		if (!Submit.bOk)
		{
			Fail(EComfyErrorKind::SubmitValidationFailed,
				FString::Printf(TEXT("阶段B 提交失败：HTTP %d %s"), Submit.HttpStatus, *Submit.ErrorText),
				FString(), FString());
			return;
		}

		CurrentPromptId = Submit.PromptId;
		Result.StageBPromptId = Submit.PromptId;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("阶段B prompt_id = %s"), *Submit.PromptId);

		NextPollTime = FPlatformTime::Seconds() + Settings->PollIntervalSeconds;
		Advance(EStep::StageBPoll);
	});
}

void FComfyTextureJob::Step_StageBPoll()
{
	SetState(EComfyJobState::Running, TEXT("阶段B 执行中"));
	bWaiting = true;

	Http.GetHistory(CurrentPromptId, [this](const FComfyHistoryResult& History)
	{
		bWaiting = false;

		if (History.bTransportFailure)
		{
			Fail(EComfyErrorKind::ServerCrashed,
				TEXT("阶段B 执行期间与 ComfyUI 的连接中断（疑似显存不足导致进程崩溃）"),
				FString(), History.ErrorText);
			return;
		}

		if (!History.bFound)
		{
			NextPollTime = FPlatformTime::Seconds() + Settings->PollIntervalSeconds;
			return;
		}
		if (History.bFailed)
		{
			Fail(EComfyErrorKind::NodeExecutionFailed,
				History.ErrorMessage.IsEmpty() ? FString(TEXT("阶段B 执行报错")) : History.ErrorMessage,
				History.ErrorNodeId, History.ErrorDetailJson);
			return;
		}
		if (!History.bCompleted)
		{
			NextPollTime = FPlatformTime::Seconds() + Settings->PollIntervalSeconds;
			return;
		}

		Result.StageBSeconds = FPlatformTime::Seconds() - StageBStartTime;
		PendingImages = History.Images;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("阶段B 完成，耗时 %.2f s，产物 %d 个"),
			Result.StageBSeconds, PendingImages.Num());

		PrepareStageBDownloads();
	});
}

void FComfyTextureJob::PrepareStageBDownloads()
{
	const TMap<FString, FString>& OutputNodeIds = GraphB.GetSpec().OutputNodeIds;
	for (const FString& Semantic : ComfyChannels::All())
	{
		const FString* NodeId = OutputNodeIds.Find(Semantic);
		if (NodeId == nullptr)
		{
			continue;
		}
		if (const FComfyImageRef* Image = FindImageForNode(PendingImages, *NodeId))
		{
			PendingDownloads.Add(*Image);
			PendingDownloadSemantics.Add(Semantic);
		}
	}

	DownloadIndex = 0;
	Advance(EStep::StageBDownload);
}

void FComfyTextureJob::Step_StageBDownload()
{
	// 全部下载完了 ⇒ 先核对通道齐备，再收尾。
	if (DownloadIndex >= PendingDownloads.Num())
	{
		TArray<FString> Missing;
		for (const FString& Semantic : ComfyChannels::All())
		{
			if (!Result.ChannelPaths.Contains(Semantic))
			{
				Missing.Add(Semantic);
			}
		}
		if (Missing.Num() > 0)
		{
			Fail(EComfyErrorKind::DownloadFailed,
				FString::Printf(TEXT("阶段B 产物缺通道：%s"), *FString::Join(Missing, TEXT(", "))),
				FString(), FString());
			return;
		}
		Advance(EStep::Complete);
		return;
	}

	const FComfyImageRef Ref = PendingDownloads[DownloadIndex];
	const FString Semantic = PendingDownloadSemantics[DownloadIndex];
	const FString DestDir = FPaths::Combine(Result.OutputDir, TEXT("stage_b"));

	SetState(EComfyJobState::Downloading,
		FString::Printf(TEXT("下载 %s（%d/%d）"),
			*ComfyChannels::DisplayName(Semantic), DownloadIndex + 1, PendingDownloads.Num()));
	bWaiting = true;

	Http.DownloadImage(Ref, [this, DestDir, Ref, Semantic](bool bOk, const TArray<uint8>& Bytes, const FString& Error)
	{
		bWaiting = false;
		if (!bOk)
		{
			Fail(EComfyErrorKind::DownloadFailed, Error, Ref.NodeId, Ref.ToQueryString());
			return;
		}

		const FString Dest = FPaths::Combine(DestDir, Ref.Filename);
		if (!FFileHelper::SaveArrayToFile(Bytes, *Dest))
		{
			Fail(EComfyErrorKind::LocalIOFailed, FString::Printf(TEXT("写不了 %s"), *Dest), FString(), FString());
			return;
		}

		Result.ChannelPaths.Add(Semantic, Dest);
		UE_LOG(LogComfyTextureStudio, Log, TEXT("  %-9s → %s（%.0f KB）"),
			*Semantic, *FPaths::GetCleanFilename(Dest), Bytes.Num() / 1024.0);

		++DownloadIndex;
		Advance(EStep::StageBDownload);
	});
}

void FComfyTextureJob::Step_Complete()
{
	Result.bSuccess = true;

	const int32 DeliverableCount = ComfyChannels::Deliverable().Num();
	SetState(EComfyJobState::Ready,
		FString::Printf(TEXT("%d 个交付通道 + %d 个可选通道已就绪"),
			DeliverableCount, Result.ChannelPaths.Num() - DeliverableCount));

	UE_LOG(LogComfyTextureStudio, Log, TEXT("任务完成 · 阶段A %.2f s + 阶段B %.2f s"),
		Result.StageASeconds, Result.StageBSeconds);

	if (OnFinished)
	{
		OnFinished(Result);
	}
}
