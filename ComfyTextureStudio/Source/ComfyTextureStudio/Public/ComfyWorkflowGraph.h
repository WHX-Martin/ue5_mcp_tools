// ComfyTextureStudio · 工作流装载与槽位注入
//
// 核心约定（PRD §4.3）：**C++ 侧只按语义键读写参数，永不硬编码节点 id**。
// 节点地址一律从 Config/Workflows/workflows.manifest.json 读，源图一改只需重新导出
// manifest，代码不用动。

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** manifest 里的一个语义槽位。 */
struct FComfySlotBinding
{
	/** 语义键，如 `dsineFov`。 */
	FString SemanticKey;

	/** 原样地址，如 `35.fov` 或 `{inputNode}.image`。 */
	FString Address;

	/** 声明类型：INT / FLOAT / BOOLEAN / STRING / COMBO。 */
	FString Type;

	/** 运行时值的模式，如 `cts/{jobId}/height`；空表示不由运行时接管。 */
	FString RuntimePattern;

	/** manifest 里记的默认值（仅作参考，图里已有实际值）。 */
	TSharedPtr<FJsonValue> Default;

	/** 把 `{inputNode}` 之类的符号 token 换成真实节点 id 后的地址。 */
	FString ResolvedAddress;
};

/** manifest 里一个阶段的规格。 */
struct FComfyStageSpec
{
	FString StageKey;            // stageA / stageB
	FString DisplayName;
	FString ApiWorkflowFile;

	/** 阶段 B 的输入节点是导出时**新增**的，id 动态 ⇒ 用符号 token 表示。 */
	FString InputNodeToken;      // {inputNode}
	FString InputNodeId;         // 75
	FString InputPlaceholder;    // __CTS_RUNTIME_INPUT__.png

	TMap<FString, FComfySlotBinding> Slots;

	/** 语义键 → 产出该通道的 SaveImage 节点 id。 */
	TMap<FString, FString> OutputNodeIds;
};

class COMFYTEXTURESTUDIO_API FComfyWorkflowGraph
{
public:
	/** 读 manifest，得到两个阶段的槽位表。 */
	static bool LoadManifest(const FString& ManifestPath,
		TMap<FString, FComfyStageSpec>& OutStages, FString& OutError);

	/** 读一张 API 格式图。 */
	bool LoadFromFile(const FString& ApiJsonPath, const FComfyStageSpec& InSpec, FString& OutError);

	// ------------------------------------------------------------- 槽位读写 //
	bool HasSlot(const FString& SemanticKey) const;
	const FComfySlotBinding* FindSlot(const FString& SemanticKey) const;

	/** 按语义键写值，字符串形式，按槽位声明类型自动转换。 */
	bool SetSlotFromString(const FString& SemanticKey, const FString& Value, FString& OutError);

	/** 按语义键写枚举/字符串。 */
	bool SetSlotString(const FString& SemanticKey, const FString& Value, FString& OutError);

	/** 按语义键写数值。 */
	bool SetSlotNumber(const FString& SemanticKey, double Value, FString& OutError);

	/** 按语义键写布尔。 */
	bool SetSlotBool(const FString& SemanticKey, bool Value, FString& OutError);

	/** 读回某个语义槽位的当前值（COMBO 枚举校验用）。 */
	bool GetSlotValueAsString(const FString& SemanticKey, FString& OutValue) const;

	/** 把 `{inputNode}` 换成真实 id。 */
	FString ResolveAddress(const FString& Address) const;

	/**
	 * 把阶段 B 的输入占位符替换成实际上传名。
	 * 返回**占位符被命中的次数**（调用方必须断言 == 1，否则说明图被改过），
	 * 出错返回 -1。
	 */
	int32 ReplaceInputPlaceholder(const FString& UploadedName, FString& OutError);

	// ------------------------------------------------------------------ 其他 //
	/** 序列化成提交用的紧凑 JSON。 */
	FString Serialize() const;

	/** 图里实际存在的节点 id（数字序）。 */
	TArray<FString> GetNodeIds() const;

	/** 节点 id → class_type。 */
	bool GetNodeClassType(const FString& NodeId, FString& OutClassType) const;

	const FComfyStageSpec& GetSpec() const { return Spec; }
	bool IsLoaded() const { return Root.IsValid(); }
	const TSharedPtr<FJsonObject>& GetRoot() const { return Root; }

private:
	bool SetNodeInputString(const FString& Address, const FString& Value, FString& OutError);
	bool SetNodeInputNumber(const FString& Address, double Value, FString& OutError);
	bool SetNodeInputBool(const FString& Address, bool Value, FString& OutError);
	bool GetNodeInputString(const FString& Address, FString& OutValue) const;

	/** 取某个地址所属的 inputs 对象与 widget 名，失败时给出可读原因。 */
	TSharedPtr<FJsonObject> ResolveInputsObject(const FString& Address, FString& OutWidgetName, FString& OutError) const;

	TSharedPtr<FJsonObject> Root;
	FComfyStageSpec Spec;
};
