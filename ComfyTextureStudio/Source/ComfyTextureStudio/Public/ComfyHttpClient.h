// ComfyTextureStudio · ComfyUI REST 客户端（PRD §4.1）
//
// 线程模型（PRD §3.3）：请求与回调都由引擎的 FHttpManager 驱动。在编辑器里由引擎
// 的 tick 循环自动推进；在 commandlet 里**没有 tick 循环**，所以必须显式调 Pump()
// 去驱动 FHttpManager，否则请求永远不会完成、回调永不触发。
//
// 本类不持有任何 UObject，回调里也不要碰 UObject（那是 UE 插件最常见的崩溃来源）。

#pragma once

#include "ComfyTypes.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IHttpRequest.h"

/** /system_stats 的关键字段（启动时先看显存，不够就提示）。 */
struct FComfySystemStats
{
	bool bValid = false;
	FString DeviceName;
	int64 VramFree = 0;
	int64 VramTotal = 0;
	int64 RamFree = 0;
	int64 RamTotal = 0;
};

/** /prompt 的结果。 */
struct FComfySubmitResult
{
	bool bOk = false;
	bool bTransportFailure = false; // 连接层失败（区分于参数被拒）
	FString PromptId;
	FString NodeErrorsJson;         // 非空 = 校验失败，**未入队**
	FString ErrorText;
	int32 HttpStatus = 0;
};

/** /history/{id} 的结果。 */
struct FComfyHistoryResult
{
	bool bFound = false;      // history 里有这个 prompt_id 的条目
	bool bCompleted = false;  // status.completed
	bool bFailed = false;     // status.status_str == "error"
	bool bTransportFailure = false;
	FString StatusStr;
	TArray<FComfyImageRef> Images;

	// 失败详情
	FString ErrorNodeId;
	FString ErrorMessage;
	FString ErrorDetailJson;

	int32 HttpStatus = 0;
	FString ErrorText;
};

class COMFYTEXTURESTUDIO_API FComfyHttpClient
{
public:
	FComfyHttpClient();
	FComfyHttpClient(const FString& InBaseUrl, float InRequestTimeoutSeconds = 60.0f, bool bInVerbose = false);
	~FComfyHttpClient();

	// 请求是异步的，飞行中可能有回调等着打回来。一旦本对象被移动/拷贝，
	// 旧对象的令牌就得作废 —— 所以显式处理这两个特殊成员。
	FComfyHttpClient(FComfyHttpClient&& Other);
	FComfyHttpClient(const FComfyHttpClient&) = delete;
	FComfyHttpClient& operator=(const FComfyHttpClient&) = delete;

	void Configure(const FString& InBaseUrl, float InRequestTimeoutSeconds, bool bInVerbose);

	/**
	 * 驱动引擎 HTTP 管理器若干秒。
	 * FHttpManager 继承 FTSTickerObjectBase：没有引擎 tick 时（commandlet），
	 * 必须手工 Tick 才能让请求推进、让完成回调触发。
	 */
	void Pump(float Seconds);

	// ---------------------------------------------------------------- 端点 //
	void GetSystemStats(TFunction<void(const FComfySystemStats&, const FString& /*Error*/, bool /*bTransportFailure*/)> OnDone);

	void GetObjectInfo(TFunction<void(bool bOk, const TSharedPtr<FJsonObject>& /*ObjectInfo*/, const FString& /*Error*/)> OnDone);

	void SubmitPrompt(const FString& PromptGraphJson, const FString& ClientId,
		TFunction<void(const FComfySubmitResult&)> OnDone);

	void GetHistory(const FString& PromptId, TFunction<void(const FComfyHistoryResult&)> OnDone);

	void DownloadImage(const FComfyImageRef& Image,
		TFunction<void(bool bOk, const TArray<uint8>& /*Bytes*/, const FString& /*Error*/)> OnDone);

	/** multipart 上传（字段名/顺序照 M0 已验证的形态来）。返回可用作 LoadImage 的名字。 */
	void UploadImage(const TArray<uint8>& PngBytes, const FString& RemoteName,
		TFunction<void(bool bOk, const FString& /*UploadedName*/, const FString& /*Error*/)> OnDone);

	void Interrupt(TFunction<void(bool bOk)> OnDone);

	/** POST /queue {"delete":[pid]} —— 取消时若任务还在 pending，需要把队列条目删掉（PRD §9.3）。 */
	void DeleteQueueItem(const FString& PromptId, TFunction<void(bool bOk)> OnDone);

	void GetQueue(TFunction<void(bool bOk, const TSharedPtr<FJsonObject>&, const FString& /*Error*/)> OnDone);

	bool IsVerbose() const { return bVerbose; }
	const FString& GetBaseUrl() const { return BaseUrl; }

private:
	/**
	 * 存活令牌。
	 *
	 * 为什么必须有：`FHttpModule` 的请求是异步的，完成回调由引擎在稍后触发。
	 * 如果本对象在请求飞行期间就被销毁（典型场景：用户点了「重新探测」马上关掉面板，
	 * 或任务被丢弃），裸捕获的 `this` 就变成野指针 —— 表现是
	 * `EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff` 这种"读空指针"崩溃，
	 * 而且发生在 HTTP 回调里，堆栈完全指不到业务代码。
	 *
	 * 每个回调都拿一份这个令牌，开头先确认它还在且未被作废。
	 * 令牌用 TSharedPtr 持有，所以即使客户端对象已经没了，令牌本身还活着 ——
	 * 回调因此能安全地读到 `bAlive == false` 并安静返回。
	 */
	struct FLiveness
	{
		bool bAlive = true;
	};
	TSharedPtr<FLiveness> Liveness;

	/** 飞行中的请求（析构时统一取消，免得关掉面板后还在后台等超时）。 */
	TArray<FHttpRequestPtr> PendingRequests;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> NewRequest(const FString& Verb, const FString& Path);
	void LogVerbose(const FString& What, const FString& Body) const;

	FString BaseUrl = TEXT("http://127.0.0.1:8000");
	float RequestTimeoutSeconds = 60.0f;
	bool bVerbose = false;
};
