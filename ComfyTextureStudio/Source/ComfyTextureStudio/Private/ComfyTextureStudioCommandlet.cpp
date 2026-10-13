#include "ComfyTextureStudioCommandlet.h"

#include "ComfyTextureJob.h"
#include "ComfyTextureStudio.h"
#include "ComfyTextureStudioSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"

UComfyTextureStudioCommandlet::UComfyTextureStudioCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;   // 插件在工作时要拿编辑器上下文
	LogToConsole = true;
	ShowErrorCount = true;
}

void UComfyTextureStudioCommandlet::PrintUsage()
{
	UE_LOG(LogComfyTextureStudio, Log, TEXT("ComfyTextureStudio · 无 UI 跑完整两阶段"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -run=ComfyTextureStudio -unattended -nopause -nosplash"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -JobId= -Prompt= -Negative= -Seed= -Steps= -Width= -Height= -BatchSize="));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -Lora= -Set=key=value,... -ServerUrl= -OutDir="));
}

int32 UComfyTextureStudioCommandlet::Main(const FString& Params)
{
	// Params 是 -run= 之后的残段；这里要拿完整命令行（-run= 之前的开关也在里面）。
	const FString CmdLine = FCommandLine::Get();
	(void)Params;

	if (FParse::Param(*CmdLine, TEXT("Help")))
	{
		PrintUsage();
		return 0;
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ ComfyTextureStudio · M1 无 UI 全链路 ════"));

	FComfyTextureJobParams JobParams;
	FString ServerUrlOverride;
	FString SetList;

	FParse::Value(*CmdLine, TEXT("JobId="), JobParams.JobId);
	FParse::Value(*CmdLine, TEXT("Prompt="), JobParams.PositivePrompt);
	FParse::Value(*CmdLine, TEXT("Negative="), JobParams.NegativePrompt);
	FParse::Value(*CmdLine, TEXT("Lora="), JobParams.LoraName);
	FParse::Value(*CmdLine, TEXT("Steps="), JobParams.Steps);
	FParse::Value(*CmdLine, TEXT("Width="), JobParams.Width);
	FParse::Value(*CmdLine, TEXT("Height="), JobParams.Height);
	FParse::Value(*CmdLine, TEXT("BatchSize="), JobParams.BatchSize);
	FParse::Value(*CmdLine, TEXT("OutDir="), JobParams.OutputRootAbs);
	FParse::Value(*CmdLine, TEXT("ServerUrl="), ServerUrlOverride);
	FParse::Value(*CmdLine, TEXT("Set="), SetList);

	// -Seed=<int> 或 -Seed=random
	//
	// M0 结论 1：ComfyUI 有**节点级结果缓存**，图和上游参数都不变时重提交直接命中 ——
	// 实测阶段A 从 ~11 s 掉到 1.5 s，用户会以为「重跑了」其实没有。
	// 换种子是最省事的失效手段，所以给一个显式的 random。
	FString SeedText;
	if (FParse::Value(*CmdLine, TEXT("Seed="), SeedText) && !SeedText.IsEmpty())
	{
		if (SeedText.Equals(TEXT("random"), ESearchCase::IgnoreCase))
		{
			JobParams.Seed = FMath::RandRange(0, MAX_int32 - 1);
			UE_LOG(LogComfyTextureStudio, Log, TEXT("使用随机种子 %d（用于避免命中缓存）"), JobParams.Seed);
		}
		else
		{
			JobParams.Seed = FCString::Atoi(*SeedText);
		}
	}

	// -Set=dsineFov=2.5,aoRadius=32
	if (!SetList.IsEmpty())
	{
		TArray<FString> Entries;
		SetList.ParseIntoArray(Entries, TEXT(","), /*InCullEmpty=*/true);
		for (const FString& Entry : Entries)
		{
			FString Key;
			FString Value;
			if (Entry.Split(TEXT("="), &Key, &Value))
			{
				JobParams.SlotOverrides.Add(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
			}
			else
			{
				UE_LOG(LogComfyTextureStudio, Warning, TEXT("忽略无法解析的 -Set 项：%s"), *Entry);
			}
		}
	}

	if (!ServerUrlOverride.IsEmpty())
	{
		// 命令行走一次性进程，改 CDO 是安全的。
		GetMutableDefault<UComfyTextureStudioSettings>()->ServerUrl = ServerUrlOverride;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("服务地址覆盖为 %s"), *ServerUrlOverride);
	}

	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	UE_LOG(LogComfyTextureStudio, Log, TEXT("服务地址 %s · 产物根 %s"),
		*Settings->ServerUrl, *Settings->GetCacheDirAbs());

	// ---- 跑 ----
	FComfyTextureJob Job;
	Job.Start(JobParams);

	while (!Job.IsFinished())
	{
		Job.Tick(0.05f);
	}

	const FComfyJobResult& Result = Job.GetResult();

	// ---- 报告 ----
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("jobId"), Result.JobId);
	Report->SetBoolField(TEXT("success"), Result.bSuccess);
	Report->SetStringField(TEXT("outputDir"), Result.OutputDir);
	Report->SetStringField(TEXT("stageAPromptId"), Result.StageAPromptId);
	Report->SetStringField(TEXT("stageBPromptId"), Result.StageBPromptId);
	Report->SetStringField(TEXT("baseTexture"), Result.BaseTexturePath);
	Report->SetStringField(TEXT("uploadedInput"), Result.UploadedInputName);
	Report->SetNumberField(TEXT("stageASeconds"), Result.StageASeconds);
	Report->SetNumberField(TEXT("stageBSeconds"), Result.StageBSeconds);

	TSharedPtr<FJsonObject> Channels = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Result.ChannelPaths)
	{
		Channels->SetStringField(Pair.Key, Pair.Value);
	}
	Report->SetObjectField(TEXT("channels"), Channels);

	if (Result.Error.IsSet())
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("kind"), LexToString(Result.Error.Kind));
		Error->SetStringField(TEXT("nodeId"), Result.Error.NodeId);
		Error->SetStringField(TEXT("message"), Result.Error.Message);
		Error->SetStringField(TEXT("userText"), Result.Error.UserText());
		Error->SetStringField(TEXT("suggestedAction"), Result.Error.SuggestedAction());
		Error->SetStringField(TEXT("detail"), Result.Error.Detail);
		Report->SetObjectField(TEXT("error"), Error);
	}

	if (!FFileHelper::SaveStringToFile(
		[&Report]()
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Report.ToSharedRef(), Writer);
			return Text;
		}(),
		*FPaths::Combine(Result.OutputDir, TEXT("report.json"))))
	{
		UE_LOG(LogComfyTextureStudio, Warning, TEXT("报告写盘失败：%s"), *Result.OutputDir);
	}

	// ---- 汇总 ----
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ 汇总 ════"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  任务 id     %s"), *Result.JobId);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  产物目录    %s"), *Result.OutputDir);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  阶段A 产物  %s"), *Result.BaseTexturePath);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  耗时        阶段A %.2f s · 阶段B %.2f s"),
		Result.StageASeconds, Result.StageBSeconds);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  通道："));

	int32 Found = 0;
	int32 DeliverableFound = 0;
	for (const FString& Semantic : ComfyChannels::All())
	{
		const FString* Path = Result.ChannelPaths.Find(Semantic);
		const bool bDeliverable = ComfyChannels::IsDeliverable(Semantic);
		if (Path != nullptr)
		{
			++Found;
			if (bDeliverable)
			{
				++DeliverableFound;
			}
		}
		UE_LOG(LogComfyTextureStudio, Log, TEXT("    %-9s %-24s %s"),
			*Semantic,
			Path != nullptr ? *FPaths::GetCleanFilename(*Path) : TEXT("（缺）"),
			bDeliverable ? TEXT("[交付]") : TEXT("[可选，Q3 不导入]"));
	}

	if (Result.bSuccess)
	{
		UE_LOG(LogComfyTextureStudio, Log,
			TEXT("结果        成功 · %d/%d 通道就绪（其中交付通道 %d/%d）"),
			Found, ComfyChannels::All().Num(), DeliverableFound, ComfyChannels::Deliverable().Num());
		return 0;
	}

	UE_LOG(LogComfyTextureStudio, Error, TEXT("结果        失败 · %s"), *Result.Error.ToString());
	UE_LOG(LogComfyTextureStudio, Error, TEXT("  说明      %s"), *Result.Error.UserText());
	UE_LOG(LogComfyTextureStudio, Error, TEXT("  建议      %s"), *Result.Error.SuggestedAction());
	return 1;
}
