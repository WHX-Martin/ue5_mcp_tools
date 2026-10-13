#include "ComfyHttpClient.h"

#include "ComfyTextureStudio.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/**
	 * 最小百分号编码。
	 *
	 * 刻意不用引擎的 UrlEncode：那套 API 在 5.x 里搬过位置（FGenericPlatformHttp → FPlatformHttp），
	 * 自己写 20 行反而更稳。保留 `/` 不编码 —— 它在查询串里合法，而 ComfyUI 的 subfolder
	 * 参数本身就是 `cts/<jobId>` 这种带斜杠的形态。
	 */
	FString ComfyUrlEncode(const FString& In)
	{
		static const TCHAR* HexDigits = TEXT("0123456789ABCDEF");
		const FTCHARToUTF8 Convert(*In);
		const uint8* Bytes = reinterpret_cast<const uint8*>(Convert.Get());
		const int32 Len = Convert.Length();

		FString Out;
		Out.Reserve(Len * 3);
		for (int32 i = 0; i < Len; ++i)
		{
			const uint8 C = Bytes[i];
			const bool bSafe =
				(C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') ||
				C == '-' || C == '_' || C == '.' || C == '~' || C == '/';
			if (bSafe)
			{
				Out.AppendChar(static_cast<TCHAR>(C));
			}
			else
			{
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(HexDigits[(C >> 4) & 0xF]);
				Out.AppendChar(HexDigits[C & 0xF]);
			}
		}
		return Out;
	}

	/** 把子对象序列化成紧凑 JSON（错误提示里要原样带出来给用户看）。 */
	FString JsonToString(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return FString();
		}
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	FString Truncate(const FString& In, int32 Max = 700)
	{
		return In.Len() <= Max ? In : In.Left(Max) + TEXT("…");
	}

	/** 从 `messages` 数组里捞 execution_error，并把节点 id / 异常信息拆出来。 */
	void ExtractExecutionError(const TArray<TSharedPtr<FJsonValue>>& Messages,
		FString& OutNodeId, FString& OutMessage, FString& OutDetail)
	{
		for (const TSharedPtr<FJsonValue>& Msg : Messages)
		{
			const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
			if (!Msg.IsValid() || !Msg->TryGetArray(Pair) || Pair == nullptr || Pair->Num() < 2)
			{
				continue;
			}

			FString Tag;
			if (!(*Pair)[0]->TryGetString(Tag) || Tag != TEXT("execution_error"))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!(*Pair)[1]->TryGetObject(Payload) || Payload == nullptr)
			{
				continue;
			}

			(*Payload)->TryGetStringField(TEXT("node_id"), OutNodeId);
			(*Payload)->TryGetStringField(TEXT("exception_message"), OutMessage);
			if (OutMessage.IsEmpty())
			{
				(*Payload)->TryGetStringField(TEXT("exception_type"), OutMessage);
			}
			OutDetail = JsonToString(*Payload);
			return;
		}
	}
}

FString FComfyImageRef::ToQueryString() const
{
	// ⚠ 这三个参数必须原样取自 /history 的 outputs，不能自己拼文件名：
	// SaveImage 落盘时会在前缀后插入 _00001_ 序号。
	//
	// ⚠ Windows 上 history 报的 subfolder 是**反斜杠**（ComfyUI 用 os.sep 拼的，
	// 例如 cts\20261013-105755_600AEC）。一律转成正斜杠再编码：两种写法 /view 都认，
	// 但正斜杠拼出来的 URL 可读、也跨平台一致。真正的坑是**百分号编码必须完整** ——
	// 漏掉 '%' 会把 '\' 编成 "5C"，ComfyUI 于是去找一个叫 cts5Cxxx 的目录，
	// /view 回 404，看起来像「文件不存在」而其实是我们的 URL 坏了。
	FString NormalizedSubfolder = Subfolder;
	NormalizedSubfolder.ReplaceInline(TEXT("\\"), TEXT("/"));

	return FString::Printf(TEXT("filename=%s&subfolder=%s&type=%s"),
		*ComfyUrlEncode(Filename),
		*ComfyUrlEncode(NormalizedSubfolder),
		*ComfyUrlEncode(Type));
}

// --------------------------------------------------------------------------- //
// 骨架
// --------------------------------------------------------------------------- //

FComfyHttpClient::FComfyHttpClient(const FString& InBaseUrl, float InRequestTimeoutSeconds, bool bInVerbose)
{
	Configure(InBaseUrl, InRequestTimeoutSeconds, bInVerbose);
}

void FComfyHttpClient::Configure(const FString& InBaseUrl, float InRequestTimeoutSeconds, bool bInVerbose)
{
	BaseUrl = InBaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}
	RequestTimeoutSeconds = InRequestTimeoutSeconds;
	bVerbose = bInVerbose;
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> FComfyHttpClient::NewRequest(const FString& Verb, const FString& Path)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BaseUrl + Path);
	Request->SetVerb(Verb);
	Request->SetTimeout(FMath::Max(1.0f, RequestTimeoutSeconds));
	return Request;
}

void FComfyHttpClient::LogVerbose(const FString& What, const FString& Body) const
{
	if (bVerbose)
	{
		UE_LOG(LogComfyTextureStudio, Verbose, TEXT("%s\n%s"), *What, *Truncate(Body, 4000));
	}
}

void FComfyHttpClient::Pump(float Seconds)
{
	const double EndTime = FPlatformTime::Seconds() + FMath::Max(0.0f, Seconds);
	do
	{
		// FHttpManager 继承 FTSTickerObjectBase。commandlet 没有引擎 tick 循环，
		// 少了这一行请求会一直「在飞」、回调永远不触发。
		FHttpModule::Get().GetHttpManager().Tick(0.016f);
		FPlatformProcess::Sleep(0.005f);
	}
	while (FPlatformTime::Seconds() < EndTime);
}

// --------------------------------------------------------------------------- //
// GET /system_stats
// --------------------------------------------------------------------------- //

void FComfyHttpClient::GetSystemStats(
	TFunction<void(const FComfySystemStats&, const FString&, bool)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("GET"), TEXT("/system_stats"));
	Request->OnProcessRequestComplete().BindLambda(
		[this, OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FComfySystemStats Stats;
			FString Error;
			bool bTransportFailure = false;

			if (!bConnected || !Response.IsValid())
			{
				bTransportFailure = true;
				Error = FString::Printf(TEXT("无法连接 %s/system_stats"), *BaseUrl);
			}
			else if (Response->GetResponseCode() != 200)
			{
				Error = FString::Printf(TEXT("HTTP %d"), Response->GetResponseCode());
			}
			else
			{
				const FString Body = Response->GetContentAsString();
				LogVerbose(TEXT("GET /system_stats ->"), Body);

				TSharedPtr<FJsonObject> Json;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
				if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
				{
					Error = TEXT("system_stats 返回的不是合法 JSON");
				}
				else
				{
					const TArray<TSharedPtr<FJsonValue>>* Devices = nullptr;
					if (Json->TryGetArrayField(TEXT("devices"), Devices) && Devices != nullptr && Devices->Num() > 0)
					{
						const TSharedPtr<FJsonObject> Device = (*Devices)[0]->AsObject();
						if (Device.IsValid())
						{
							double Value = 0.0;
							Stats.bValid = true;
							Device->TryGetStringField(TEXT("name"), Stats.DeviceName);
							if (Device->TryGetNumberField(TEXT("vram_free"), Value))  { Stats.VramFree  = static_cast<int64>(Value); }
							if (Device->TryGetNumberField(TEXT("vram_total"), Value)) { Stats.VramTotal = static_cast<int64>(Value); }

							const TSharedPtr<FJsonObject>* System = nullptr;
							if (Json->TryGetObjectField(TEXT("system"), System) && System != nullptr)
							{
								if ((*System)->TryGetNumberField(TEXT("ram_free"), Value))  { Stats.RamFree  = static_cast<int64>(Value); }
								if ((*System)->TryGetNumberField(TEXT("ram_total"), Value)) { Stats.RamTotal = static_cast<int64>(Value); }
							}
						}
					}
					if (!Stats.bValid)
					{
						Error = TEXT("system_stats 里没有可用的 devices 条目");
					}
				}
			}

			OnDone(Stats, Error, bTransportFailure);
		});

	Request->ProcessRequest();
}

// --------------------------------------------------------------------------- //
// GET /object_info
// --------------------------------------------------------------------------- //

void FComfyHttpClient::GetObjectInfo(
	TFunction<void(bool, const TSharedPtr<FJsonObject>&, const FString&)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("GET"), TEXT("/object_info"));
	Request->OnProcessRequestComplete().BindLambda(
		[this, OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnDone(false, nullptr, FString::Printf(TEXT("无法连接 %s/object_info"), *BaseUrl));
				return;
			}
			if (Response->GetResponseCode() != 200)
			{
				OnDone(false, nullptr, FString::Printf(TEXT("object_info HTTP %d"), Response->GetResponseCode()));
				return;
			}

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				OnDone(false, nullptr, TEXT("object_info 返回的不是合法 JSON"));
				return;
			}

			UE_LOG(LogComfyTextureStudio, Log, TEXT("object_info：%d 个节点类"), Json->Values.Num());
			OnDone(true, Json, FString());
		});

	Request->ProcessRequest();
}

// --------------------------------------------------------------------------- //
// POST /prompt
// --------------------------------------------------------------------------- //

void FComfyHttpClient::SubmitPrompt(const FString& PromptGraphJson, const FString& ClientId,
	TFunction<void(const FComfySubmitResult&)> OnDone)
{
	const FString Body = FString::Printf(TEXT("{\"prompt\":%s,\"client_id\":\"%s\"}"),
		*PromptGraphJson, *ClientId);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("POST"), TEXT("/prompt"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda(
		[this, OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FComfySubmitResult Result;
			if (!bConnected || !Response.IsValid())
			{
				Result.bTransportFailure = true;
				Result.ErrorText = TEXT("POST /prompt 连接失败");
				OnDone(Result);
				return;
			}

			Result.HttpStatus = Response->GetResponseCode();
			const FString BodyText = Response->GetContentAsString();
			LogVerbose(TEXT("POST /prompt ->"), BodyText);

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyText);
			if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
			{
				Json->TryGetStringField(TEXT("prompt_id"), Result.PromptId);

				// node_errors 非空 = 校验失败，任务**没有**入队（PRD §4.1）
				const TSharedPtr<FJsonObject>* NodeErrors = nullptr;
				if (Json->TryGetObjectField(TEXT("node_errors"), NodeErrors) &&
					NodeErrors != nullptr && (*NodeErrors)->Values.Num() > 0)
				{
					Result.NodeErrorsJson = JsonToString(*NodeErrors);
				}
				if (Result.PromptId.IsEmpty())
				{
					Json->TryGetStringField(TEXT("error"), Result.ErrorText);
				}
			}
			else
			{
				Result.ErrorText = Truncate(BodyText, 400);
			}

			Result.bOk = (Result.HttpStatus == 200) && !Result.PromptId.IsEmpty() && Result.NodeErrorsJson.IsEmpty();
			OnDone(Result);
		});

	Request->ProcessRequest();
}

// --------------------------------------------------------------------------- //
// GET /history/{prompt_id}
// --------------------------------------------------------------------------- //

void FComfyHttpClient::GetHistory(const FString& PromptId, TFunction<void(const FComfyHistoryResult&)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		NewRequest(TEXT("GET"), FString::Printf(TEXT("/history/%s"), *PromptId));

	Request->OnProcessRequestComplete().BindLambda(
		[this, OnDone, PromptId](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FComfyHistoryResult Result;
			if (!bConnected || !Response.IsValid())
			{
				Result.bTransportFailure = true;
				Result.ErrorText = TEXT("GET /history 连接失败");
				OnDone(Result);
				return;
			}

			Result.HttpStatus = Response->GetResponseCode();
			if (Result.HttpStatus != 200)
			{
				Result.ErrorText = FString::Printf(TEXT("HTTP %d"), Result.HttpStatus);
				OnDone(Result);
				return;
			}

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				Result.ErrorText = TEXT("history 返回的不是合法 JSON");
				OnDone(Result);
				return;
			}

			// 未完成的任务不在 history 里，此时响应是 {}，不算错误。
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Json->TryGetObjectField(PromptId, Entry) || Entry == nullptr)
			{
				OnDone(Result);
				return;
			}
			Result.bFound = true;

			const TSharedPtr<FJsonObject>* Status = nullptr;
			if ((*Entry)->TryGetObjectField(TEXT("status"), Status) && Status != nullptr)
			{
				(*Status)->TryGetBoolField(TEXT("completed"), Result.bCompleted);
				(*Status)->TryGetStringField(TEXT("status_str"), Result.StatusStr);
				Result.bFailed = (Result.StatusStr == TEXT("error"));

				const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
				if ((*Status)->TryGetArrayField(TEXT("messages"), Messages) && Messages != nullptr)
				{
					ExtractExecutionError(*Messages, Result.ErrorNodeId, Result.ErrorMessage, Result.ErrorDetailJson);
				}
			}

			const TSharedPtr<FJsonObject>* Outputs = nullptr;
			if ((*Entry)->TryGetObjectField(TEXT("outputs"), Outputs) && Outputs != nullptr)
			{
				for (const auto& Pair : (*Outputs)->Values)
				{
					const TSharedPtr<FJsonObject> NodeOutput = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
					if (!NodeOutput.IsValid())
					{
						continue;
					}
					const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
					if (!NodeOutput->TryGetArrayField(TEXT("images"), Images) || Images == nullptr)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& ImageValue : *Images)
					{
						const TSharedPtr<FJsonObject> ImageObj = ImageValue.IsValid() ? ImageValue->AsObject() : nullptr;
						if (!ImageObj.IsValid())
						{
							continue;
						}
						FComfyImageRef Ref;
						Ref.NodeId = ComfyJsonKeyToString(Pair.Key);
						ImageObj->TryGetStringField(TEXT("filename"), Ref.Filename);
						ImageObj->TryGetStringField(TEXT("subfolder"), Ref.Subfolder);
						if (!ImageObj->TryGetStringField(TEXT("type"), Ref.Type) || Ref.Type.IsEmpty())
						{
							Ref.Type = TEXT("output");
						}
						if (!Ref.Filename.IsEmpty())
						{
							Result.Images.Add(MoveTemp(Ref));
						}
					}
				}
			}

			OnDone(Result);
		});

	Request->ProcessRequest();
}

// --------------------------------------------------------------------------- //
// GET /view
// --------------------------------------------------------------------------- //

void FComfyHttpClient::DownloadImage(const FComfyImageRef& Image,
	TFunction<void(bool, const TArray<uint8>&, const FString&)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		NewRequest(TEXT("GET"), FString::Printf(TEXT("/view?%s"), *Image.ToQueryString()));

	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnDone(false, TArray<uint8>(), TEXT("下载连接失败"));
				return;
			}
			if (Response->GetResponseCode() != 200)
			{
				OnDone(false, TArray<uint8>(),
					FString::Printf(TEXT("GET /view HTTP %d"), Response->GetResponseCode()));
				return;
			}
			OnDone(true, Response->GetContent(), FString());
		});

	Request->ProcessRequest();
}

// --------------------------------------------------------------------------- //
// POST /upload/image（multipart）
// --------------------------------------------------------------------------- //

void FComfyHttpClient::UploadImage(const TArray<uint8>& PngBytes, const FString& RemoteName,
	TFunction<void(bool, const FString&, const FString&)> OnDone)
{
	const FString Boundary = FString::Printf(TEXT("----CTS%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// 字段顺序与 M0 已验证的 Python 版一致：image → type → overwrite
	TArray<uint8> Body;
	auto AppendText = [&Body](const FString& Text)
	{
		const FTCHARToUTF8 Convert(*Text);
		Body.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
	};

	AppendText(FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendText(FString::Printf(
		TEXT("Content-Disposition: form-data; name=\"image\"; filename=\"%s\"\r\n"), *RemoteName));
	AppendText(TEXT("Content-Type: image/png\r\n\r\n"));
	Body.Append(PngBytes);
	AppendText(TEXT("\r\n"));
	AppendText(FString::Printf(
		TEXT("--%s\r\nContent-Disposition: form-data; name=\"type\"\r\n\r\ninput\r\n"), *Boundary));
	AppendText(FString::Printf(
		TEXT("--%s\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n"), *Boundary));
	AppendText(FString::Printf(TEXT("--%s--\r\n"), *Boundary));

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("POST"), TEXT("/upload/image"));
	Request->SetHeader(TEXT("Content-Type"),
		FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
	Request->SetContent(MoveTemp(Body));

	Request->OnProcessRequestComplete().BindLambda(
		[this, OnDone, RemoteName](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnDone(false, FString(), TEXT("上传连接失败"));
				return;
			}
			if (Response->GetResponseCode() != 200)
			{
				OnDone(false, FString(),
					FString::Printf(TEXT("upload HTTP %d"), Response->GetResponseCode()));
				return;
			}

			const FString BodyText = Response->GetContentAsString();
			LogVerbose(TEXT("POST /upload/image ->"), BodyText);

			FString UploadedName = RemoteName;
			FString Subfolder;
			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyText);
			if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
			{
				Json->TryGetStringField(TEXT("name"), UploadedName);
				Json->TryGetStringField(TEXT("subfolder"), Subfolder);
				if (UploadedName.IsEmpty())
				{
					UploadedName = RemoteName;
				}
			}

			// LoadImage 里要填的是「相对 input 目录的名字」，带 subfolder 前缀。
			const FString FullName = Subfolder.IsEmpty()
				? UploadedName
				: FString::Printf(TEXT("%s/%s"), *Subfolder, *UploadedName);
			OnDone(true, FullName, FString());
		});

	Request->ProcessRequest();
}

// --------------------------------------------------------------------------- //
// POST /interrupt、POST /queue（删除）、GET /queue
// --------------------------------------------------------------------------- //

void FComfyHttpClient::Interrupt(TFunction<void(bool)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("POST"), TEXT("/interrupt"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(TEXT("{}"));
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			OnDone(bConnected && Response.IsValid() && Response->GetResponseCode() == 200);
		});
	Request->ProcessRequest();
}

void FComfyHttpClient::DeleteQueueItem(const FString& PromptId, TFunction<void(bool)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("POST"), TEXT("/queue"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(FString::Printf(TEXT("{\"delete\":[\"%s\"]}"), *PromptId));
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			OnDone(bConnected && Response.IsValid() && Response->GetResponseCode() == 200);
		});
	Request->ProcessRequest();
}

void FComfyHttpClient::GetQueue(
	TFunction<void(bool, const TSharedPtr<FJsonObject>&, const FString&)> OnDone)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = NewRequest(TEXT("GET"), TEXT("/queue"));
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnDone(false, nullptr, TEXT("GET /queue 连接失败"));
				return;
			}
			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				OnDone(false, nullptr, TEXT("queue 返回的不是合法 JSON"));
				return;
			}
			OnDone(true, Json, FString());
		});
	Request->ProcessRequest();
}
