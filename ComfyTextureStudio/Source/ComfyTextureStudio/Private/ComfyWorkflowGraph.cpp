#include "ComfyWorkflowGraph.h"

#include "ComfyTextureStudio.h"
#include "ComfyTypes.h"   // ComfyJsonKeyToString：FJsonObject 键类型的兼容层
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}

		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();

		case EJson::Number:
		{
			double Number = 0.0;
			Value->TryGetNumber(Number);
			// 节点 id / 整数参数写成 "35" 而不是 "35.000000"
			return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
				? FString::Printf(TEXT("%lld"), static_cast<int64>(Number))
				: FString::Printf(TEXT("%g"), Number);
		}

		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");

		case EJson::Null:
			return FString();

		default:
			return Value->AsString();
		}
	}

	/** 把语义槽位的 `type` 字段归一到 4 类，决定怎么写 JSON。 */
	enum class ESlotType { Int, Float, Bool, String };

	ESlotType ParseSlotType(const FString& Type)
	{
		if (Type.Equals(TEXT("INT"), ESearchCase::IgnoreCase))
		{
			return ESlotType::Int;
		}
		if (Type.Equals(TEXT("FLOAT"), ESearchCase::IgnoreCase))
		{
			return ESlotType::Float;
		}
		if (Type.Equals(TEXT("BOOLEAN"), ESearchCase::IgnoreCase))
		{
			return ESlotType::Bool;
		}
		return ESlotType::String;
	}

	bool ParseBoolText(const FString& Text)
	{
		return Text.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("1"))
			|| Text.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
	}

	/** 数字序排序节点 id（"9" 在 "10" 前面）。 */
	void SortNodeIds(TArray<FString>& Ids)
	{
		Ids.Sort([](const FString& A, const FString& B)
		{
			const bool bNumA = A.IsNumeric();
			const bool bNumB = B.IsNumeric();
			if (bNumA && bNumB)
			{
				return FCString::Atoi(*A) < FCString::Atoi(*B);
			}
			if (bNumA != bNumB)
			{
				return bNumA; // 数字 id 排前面
			}
			return A < B;
		});
	}
}

// --------------------------------------------------------------------------- //
// manifest
// --------------------------------------------------------------------------- //

bool FComfyWorkflowGraph::LoadManifest(const FString& ManifestPath,
	TMap<FString, FComfyStageSpec>& OutStages, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ManifestPath))
	{
		OutError = FString::Printf(TEXT("读不到槽位清单：%s"), *ManifestPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("槽位清单不是合法 JSON：%s"), *ManifestPath);
		return false;
	}

	const TSharedPtr<FJsonObject>* Stages = nullptr;
	if (!Root->TryGetObjectField(TEXT("stages"), Stages) || Stages == nullptr)
	{
		OutError = TEXT("槽位清单缺少 stages 字段");
		return false;
	}

	const TCHAR* StageKeys[] = { TEXT("stageA"), TEXT("stageB") };
	for (const TCHAR* StageKey : StageKeys)
	{
		const TSharedPtr<FJsonObject>* StageObj = nullptr;
		if (!(*Stages)->TryGetObjectField(StageKey, StageObj) || StageObj == nullptr)
		{
			OutError = FString::Printf(TEXT("槽位清单缺少 stages.%s"), StageKey);
			return false;
		}

		FComfyStageSpec Spec;
		Spec.StageKey = StageKey;
		(*StageObj)->TryGetStringField(TEXT("displayName"), Spec.DisplayName);
		(*StageObj)->TryGetStringField(TEXT("apiWorkflowFile"), Spec.ApiWorkflowFile);
		(*StageObj)->TryGetStringField(TEXT("inputNodeToken"), Spec.InputNodeToken);
		(*StageObj)->TryGetStringField(TEXT("inputNodeId"), Spec.InputNodeId);
		(*StageObj)->TryGetStringField(TEXT("inputPlaceholder"), Spec.InputPlaceholder);

		const TSharedPtr<FJsonObject>* Slots = nullptr;
		if ((*StageObj)->TryGetObjectField(TEXT("slots"), Slots) && Slots != nullptr)
		{
			for (const auto& Pair : (*Slots)->Values)
			{
				const TSharedPtr<FJsonObject> SlotObj = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (!SlotObj.IsValid())
				{
					continue;
				}

				const FString SemanticKey = ComfyJsonKeyToString(Pair.Key);

				FComfySlotBinding Binding;
				Binding.SemanticKey = SemanticKey;
				SlotObj->TryGetStringField(TEXT("address"), Binding.Address);
				SlotObj->TryGetStringField(TEXT("type"), Binding.Type);
				SlotObj->TryGetStringField(TEXT("runtimeValuePattern"), Binding.RuntimePattern);
				Binding.Default = SlotObj->TryGetField(TEXT("default"));
				Spec.Slots.Add(SemanticKey, MoveTemp(Binding));
			}
		}

		const TSharedPtr<FJsonObject>* Outputs = nullptr;
		if ((*StageObj)->TryGetObjectField(TEXT("outputNodeIds"), Outputs) && Outputs != nullptr)
		{
			for (const auto& Pair : (*Outputs)->Values)
			{
				const FString NodeId = JsonValueToString(Pair.Value);
				if (!NodeId.IsEmpty())
				{
					Spec.OutputNodeIds.Add(ComfyJsonKeyToString(Pair.Key), NodeId);
				}
			}
		}

		// ---- outputAssets：PRD §6.3 属性表的单一来源 ----
		const TArray<TSharedPtr<FJsonValue>>* AssetArray = nullptr;
		if ((*StageObj)->TryGetArrayField(TEXT("outputAssets"), AssetArray) && AssetArray != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& AssetValue : *AssetArray)
			{
				const TSharedPtr<FJsonObject> AssetObj = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
				if (!AssetObj.IsValid())
				{
					continue;
				}

				FComfyOutputAssetSpec Asset;
				AssetObj->TryGetStringField(TEXT("semantic"), Asset.Semantic);
				if (Asset.Semantic.IsEmpty())
				{
					continue; // 没有语义键就没法索引，直接跳过而不是塞个空条目
				}
				Asset.NodeId = JsonValueToString(AssetObj->TryGetField(TEXT("nodeId")));
				AssetObj->TryGetStringField(TEXT("comfyType"), Asset.ComfyType);
				AssetObj->TryGetBoolField(TEXT("isDeliverable"), Asset.bIsDeliverable);

				const TSharedPtr<FJsonObject>* UE = nullptr;
				if (AssetObj->TryGetObjectField(TEXT("ue"), UE) && UE != nullptr)
				{
					(*UE)->TryGetStringField(TEXT("suffix"), Asset.Suffix);
					(*UE)->TryGetStringField(TEXT("role"), Asset.Role);
					(*UE)->TryGetStringField(TEXT("compressionSettings"), Asset.CompressionSettings);
					(*UE)->TryGetBoolField(TEXT("srgb"), Asset.bSRGB);
					(*UE)->TryGetBoolField(TEXT("flipGreenChannel"), Asset.bFlipGreenChannel);
					(*UE)->TryGetStringField(TEXT("lodGroup"), Asset.LODGroup);
					(*UE)->TryGetStringField(TEXT("addressMode"), Asset.AddressMode);
					(*UE)->TryGetStringField(TEXT("usage"), Asset.Usage);
					(*UE)->TryGetStringField(TEXT("warning"), Asset.Warning);
					(*UE)->TryGetStringField(TEXT("orphanReason"), Asset.OrphanReason);
					// importByDefault 决定 Q3 的默认行为；缺失时退回 isDeliverable
					if (!(*UE)->TryGetBoolField(TEXT("importByDefault"), Asset.bImportByDefault))
					{
						Asset.bImportByDefault = Asset.bIsDeliverable;
					}
				}
				else
				{
					Asset.bImportByDefault = Asset.bIsDeliverable;
				}

				Spec.OutputAssets.Add(Asset.Semantic, MoveTemp(Asset));
			}
		}

		OutStages.Add(StageKey, MoveTemp(Spec));
	}

	return true;
}

TArray<FString> FComfyStageSpec::GetImportableSemantics() const
{
	TArray<FString> Keys;
	OutputAssets.GetKeys(Keys);

	// 按 ComfyChannels::All() 的固定顺序排（交付物在前），未知键按字典序垫后。
	// 固定顺序的意义：报告与 UI 的行序每次都一样，便于逐张比对。
	const TArray<FString>& Canonical = ComfyChannels::All();
	Keys.Sort([&Canonical](const FString& A, const FString& B)
	{
		const int32 IndexA = Canonical.IndexOfByKey(A);
		const int32 IndexB = Canonical.IndexOfByKey(B);
		if (IndexA != INDEX_NONE && IndexB != INDEX_NONE)
		{
			return IndexA < IndexB;
		}
		if (IndexA != IndexB)
		{
			return IndexA != INDEX_NONE; // 认识的排前面
		}
		return A < B;
	});
	return Keys;
}

// --------------------------------------------------------------------------- //
// 装载 API 图
// --------------------------------------------------------------------------- //

bool FComfyWorkflowGraph::LoadFromFile(const FString& ApiJsonPath, const FComfyStageSpec& InSpec, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ApiJsonPath))
	{
		OutError = FString::Printf(TEXT("读不到工作流图：%s"), *ApiJsonPath);
		return false;
	}

	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		OutError = FString::Printf(TEXT("工作流图不是合法 JSON：%s"), *ApiJsonPath);
		return false;
	}

	Root = Parsed;
	Spec = InSpec;

	// 把每个槽位的地址解析成真实节点 id，顺便校验节点确实存在 ——
	// 早失败比提交到 ComfyUI 再被拒要好得多。
	TArray<FString> Missing;
	const TArray<FString> NodeIds = GetNodeIds();
	for (TPair<FString, FComfySlotBinding>& Pair : Spec.Slots)
	{
		FComfySlotBinding& Binding = Pair.Value;
		Binding.ResolvedAddress = ResolveAddress(Binding.Address);

		FString NodeId;
		FString Widget;
		if (!Binding.ResolvedAddress.Split(TEXT("."), &NodeId, &Widget))
		{
			Missing.Add(FString::Printf(TEXT("%s -> 地址无法解析（%s）"), *Binding.SemanticKey, *Binding.Address));
			continue;
		}
		if (!NodeIds.Contains(NodeId))
		{
			Missing.Add(FString::Printf(TEXT("%s -> 节点 %s 不在图中"), *Binding.SemanticKey, *NodeId));
		}
	}

	if (Missing.Num() > 0)
	{
		OutError = FString::Printf(TEXT("%s 有 %d 个槽位地址对不上：%s"),
			*ApiJsonPath, Missing.Num(), *FString::Join(Missing, TEXT("；")));
		return false;
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("装载 %s：%d 节点 / %d 槽位"),
		*FPaths::GetCleanFilename(ApiJsonPath), NodeIds.Num(), Spec.Slots.Num());
	return true;
}

// --------------------------------------------------------------------------- //
// 槽位读写
// --------------------------------------------------------------------------- //

bool FComfyWorkflowGraph::HasSlot(const FString& SemanticKey) const
{
	return Spec.Slots.Contains(SemanticKey);
}

const FComfySlotBinding* FComfyWorkflowGraph::FindSlot(const FString& SemanticKey) const
{
	return Spec.Slots.Find(SemanticKey);
}

FString FComfyWorkflowGraph::ResolveAddress(const FString& Address) const
{
	FString NodeId;
	FString Widget;
	if (!Address.Split(TEXT("."), &NodeId, &Widget))
	{
		return Address;
	}
	if (!Spec.InputNodeToken.IsEmpty() && NodeId == Spec.InputNodeToken)
	{
		NodeId = Spec.InputNodeId;
	}
	return FString::Printf(TEXT("%s.%s"), *NodeId, *Widget);
}

TSharedPtr<FJsonObject> FComfyWorkflowGraph::ResolveInputsObject(
	const FString& InAddress, FString& OutWidgetName, FString& OutError) const
{
	if (!Root.IsValid())
	{
		OutError = TEXT("工作流图尚未装载");
		return nullptr;
	}

	const FString Address = ResolveAddress(InAddress);
	FString NodeId;
	if (!Address.Split(TEXT("."), &NodeId, &OutWidgetName) || NodeId.IsEmpty() || OutWidgetName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("地址格式不对：%s（应为 <node>.<widget>）"), *InAddress);
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* NodeObj = nullptr;
	if (!Root->TryGetObjectField(NodeId, NodeObj) || NodeObj == nullptr)
	{
		OutError = FString::Printf(TEXT("图中没有节点 %s（地址 %s）；当前节点：%s"),
			*NodeId, *InAddress, *FString::Join(GetNodeIds(), TEXT(",")));
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Inputs = nullptr;
	if (!(*NodeObj)->TryGetObjectField(TEXT("inputs"), Inputs) || Inputs == nullptr)
	{
		OutError = FString::Printf(TEXT("节点 %s 没有 inputs 对象"), *NodeId);
		return nullptr;
	}

	return *Inputs;
}

bool FComfyWorkflowGraph::SetNodeInputString(const FString& Address, const FString& Value, FString& OutError)
{
	FString Widget;
	const TSharedPtr<FJsonObject> Inputs = ResolveInputsObject(Address, Widget, OutError);
	if (!Inputs.IsValid())
	{
		return false;
	}
	Inputs->SetStringField(Widget, Value);
	return true;
}

bool FComfyWorkflowGraph::SetNodeInputNumber(const FString& Address, double Value, FString& OutError)
{
	FString Widget;
	const TSharedPtr<FJsonObject> Inputs = ResolveInputsObject(Address, Widget, OutError);
	if (!Inputs.IsValid())
	{
		return false;
	}
	Inputs->SetNumberField(Widget, Value);
	return true;
}

bool FComfyWorkflowGraph::SetNodeInputBool(const FString& Address, bool Value, FString& OutError)
{
	FString Widget;
	const TSharedPtr<FJsonObject> Inputs = ResolveInputsObject(Address, Widget, OutError);
	if (!Inputs.IsValid())
	{
		return false;
	}
	Inputs->SetBoolField(Widget, Value);
	return true;
}

bool FComfyWorkflowGraph::GetNodeInputString(const FString& Address, FString& OutValue) const
{
	FString Error;
	FString Widget;
	const TSharedPtr<FJsonObject> Inputs = ResolveInputsObject(Address, Widget, Error);
	if (!Inputs.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Found = Inputs->TryGetField(Widget);
	if (!Found.IsValid())
	{
		return false;
	}
	OutValue = JsonValueToString(Found);
	return true;
}

bool FComfyWorkflowGraph::SetSlotFromString(const FString& SemanticKey, const FString& Value, FString& OutError)
{
	const FComfySlotBinding* Binding = FindSlot(SemanticKey);
	if (Binding == nullptr)
	{
		TArray<FString> Keys;
		Spec.Slots.GetKeys(Keys);
		Keys.Sort();
		OutError = FString::Printf(TEXT("manifest 里没有语义键 %s；可用键：%s"),
			*SemanticKey, *FString::Join(Keys, TEXT(",")));
		return false;
	}

	switch (ParseSlotType(Binding->Type))
	{
	case ESlotType::Int:    return SetNodeInputNumber(Binding->Address, FCString::Atoi(*Value), OutError);
	case ESlotType::Float:  return SetNodeInputNumber(Binding->Address, FCString::Atof(*Value), OutError);
	case ESlotType::Bool:   return SetNodeInputBool(Binding->Address, ParseBoolText(Value), OutError);
	case ESlotType::String:
	default:                return SetNodeInputString(Binding->Address, Value, OutError);
	}
}

bool FComfyWorkflowGraph::SetSlotString(const FString& SemanticKey, const FString& Value, FString& OutError)
{
	const FComfySlotBinding* Binding = FindSlot(SemanticKey);
	if (Binding == nullptr)
	{
		OutError = FString::Printf(TEXT("manifest 里没有语义键 %s"), *SemanticKey);
		return false;
	}
	return SetNodeInputString(Binding->Address, Value, OutError);
}

bool FComfyWorkflowGraph::SetSlotNumber(const FString& SemanticKey, double Value, FString& OutError)
{
	const FComfySlotBinding* Binding = FindSlot(SemanticKey);
	if (Binding == nullptr)
	{
		OutError = FString::Printf(TEXT("manifest 里没有语义键 %s"), *SemanticKey);
		return false;
	}
	return SetNodeInputNumber(Binding->Address, Value, OutError);
}

bool FComfyWorkflowGraph::SetSlotBool(const FString& SemanticKey, bool Value, FString& OutError)
{
	const FComfySlotBinding* Binding = FindSlot(SemanticKey);
	if (Binding == nullptr)
	{
		OutError = FString::Printf(TEXT("manifest 里没有语义键 %s"), *SemanticKey);
		return false;
	}
	return SetNodeInputBool(Binding->Address, Value, OutError);
}

bool FComfyWorkflowGraph::GetSlotValueAsString(const FString& SemanticKey, FString& OutValue) const
{
	const FComfySlotBinding* Binding = FindSlot(SemanticKey);
	if (Binding == nullptr)
	{
		return false;
	}
	return GetNodeInputString(Binding->Address, OutValue);
}

int32 FComfyWorkflowGraph::ReplaceInputPlaceholder(const FString& UploadedName, FString& OutError)
{
	const FComfySlotBinding* Binding = FindSlot(TEXT("inputImage"));
	if (Binding == nullptr)
	{
		OutError = TEXT("manifest 里没有 inputImage 槽位，无法定位阶段 B 的输入图");
		return -1;
	}

	FString Current;
	if (!GetNodeInputString(Binding->Address, Current))
	{
		OutError = FString::Printf(TEXT("读不到输入图字段（%s）"), *Binding->Address);
		return -1;
	}

	// 命中占位符说明这张图还是导出时的原样；否则图被人改过，调用方应当警觉。
	// （comfy validate 不会检查文件存在性 —— 阶段 B 带着不存在的占位文件照样报 valid=true，
	//   所以这一步必须由我们自己确认，见 M0 结论 2。）
	const int32 Hits = (Current == Spec.InputPlaceholder) ? 1 : 0;

	if (!SetNodeInputString(Binding->Address, UploadedName, OutError))
	{
		return -1;
	}
	return Hits;
}

// --------------------------------------------------------------------------- //
// 其他
// --------------------------------------------------------------------------- //

FString FComfyWorkflowGraph::Serialize() const
{
	if (!Root.IsValid())
	{
		return FString();
	}
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Out;
}

TArray<FString> FComfyWorkflowGraph::GetNodeIds() const
{
	TArray<FString> Ids;
	if (Root.IsValid())
	{
		for (const auto& Pair : Root->Values)
		{
			Ids.Add(ComfyJsonKeyToString(Pair.Key));
		}
		SortNodeIds(Ids);
	}
	return Ids;
}

bool FComfyWorkflowGraph::GetNodeClassType(const FString& NodeId, FString& OutClassType) const
{
	if (!Root.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* NodeObj = nullptr;
	if (!Root->TryGetObjectField(NodeId, NodeObj) || NodeObj == nullptr)
	{
		return false;
	}
	return (*NodeObj)->TryGetStringField(TEXT("class_type"), OutClassType);
}
