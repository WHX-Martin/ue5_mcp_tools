// ComfyTextureStudio · 材质图构建（PRD §6.2）
//
// 这个类的**全部难点只有一个**：把 BumpOffset 的 UV 输出接到 albedo / normal / ORM
// 三张交付贴图的 Coordinates 上。只接 BaseColor 是本设计里最典型的错误 —— 表现是
// 「贴图看着有深度但法线不跟着动」，而且肉眼很难判定。
// 所以建完之后我们**回读图结构**做自检（SamplesDrivenByBumpOffset），不靠"以为接上了"。
//
// 另一处刻意偏离 PRD §6.2 的地方：PRD 建议用 UMaterialEditingLibrary::ConnectMaterialProperty
// 按**输出名**（"R"/"G"/"B"）连线。实测引擎源码发现该函数内部会经
// UMaterialGraphNode::GetShortenPinName 处理名字，属于依赖内部行为。这里改成**按输出索引**
// 连（TextureSample 的输出顺序在构造函数里硬编码：RGB=0, R=1, G=2, B=3, A=4），
// 并在连完后回读 Input->Expression / OutputIndex 断言，确定性更高。

#pragma once

#include "ComfyImportTypes.h"
#include "CoreMinimal.h"

class UMaterial;
class UMaterialExpression;
class UMaterialExpressionTextureSampleParameter2D;
class UTexture2D;

class FComfyMaterialBuilder
{
public:
	struct FInputs
	{
		/** 资产落点，如 /Game/ComfyTextures/Sand_Coarse_01。 */
		FString DestinationPath;

		FString MaterialName;          // M_Sand_Coarse_01
		FString MaterialInstanceName;  // MI_Sand_Coarse_01

		/** 已导入的贴图（nullptr 表示该通道缺，会跳过对应接线并记警告）。 */
		UTexture2D* Albedo = nullptr;
		UTexture2D* Normal = nullptr;
		UTexture2D* Height = nullptr;
		UTexture2D* ORM = nullptr;

		/** Q4：是否把高度图接进 BumpOffset。 */
		bool bUseHeightParallax = true;

		/** 是否创建材质实例。 */
		bool bCreateInstance = true;

		/** BumpOffset 的 HeightRatio 初值。 */
		float BumpDepth = 0.02f;
	};

	/** UI 上给这几个标量参数的默认值（也是写进 M_ 的 DefaultValue）。 */
	static constexpr float DefaultTiling = 1.0f;
	static constexpr float DefaultNormalStrength = 1.0f;

	/**
	 * 建材质 + 材质实例。**必须在游戏线程调用**。返回 false 时 OutError 有可读原因，
	 * OutEntry 里仍会带上已采集到的诊断信息。
	 */
	static bool Build(const FInputs& In, FComfyMaterialBuildEntry& OutEntry, FString& OutError);

	/** 参数名（材质实例上可调）。 */
	static const TCHAR* ParamTiling()          { return TEXT("Tiling"); }
	static const TCHAR* ParamNormalStrength()  { return TEXT("NormalStrength"); }
	static const TCHAR* ParamBumpDepth()       { return TEXT("BumpDepth"); }

	/**
	 * 按名字找一个表达式的输入端口（忽略大小写，容忍 `Input` 后缀差异）。
	 * 公开出来是给冷加载核对用的 —— 验证必须用与构建**完全相同**的查找逻辑，
	 * 否则两边对不上时会分不清是"接错了"还是"找错了"。
	 */
	static FExpressionInput* FindInputByName(UMaterialExpression* Expr, const TCHAR* Name);

private:
	/** 建完后回读图结构，填 OutEntry 的 4 个结构自检字段。 */
	static void VerifyStructure(
		UMaterial* Material,
		UMaterialExpression* BumpOffset,
		UMaterialExpression* HeightSample,
		UMaterialExpression* Multiplier,
		const TArray<UMaterialExpression*>& DeliverableSamples,
		FComfyMaterialBuildEntry& OutEntry);

	/**
	 * 核对每个采样节点的 SamplerType 是否与所采样贴图的压缩格式一致。
	 *
	 * 为什么必须在**建完之后立刻**做、而不能等材质编译报错：无头 commandlet 下
	 * `RecompileMaterial` 拿不到 MaterialResource 就静默返回空数组，"0 错误"是假绿灯
	 * （详见 ComfySamplerType.h）。这条错误只有前置校验抓得住。
	 */
	static void VerifySamplerTypes(
		const TArray<UMaterialExpressionTextureSampleParameter2D*>& Samples,
		FComfyMaterialBuildEntry& OutEntry);
};
