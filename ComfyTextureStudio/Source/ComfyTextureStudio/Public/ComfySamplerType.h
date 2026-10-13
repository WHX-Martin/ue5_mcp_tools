// ComfyTextureStudio · 贴图采样类型（SamplerType）的唯一权威来源
//
// 为什么单独立一个文件：
//
//   材质里的 TextureSampleParameter2D 节点必须把自己的 SamplerType 与所采样贴图的
//   **压缩格式**对齐，否则引擎在编译材质时直接报错：
//
//     Sampler type is Grayscale, should be Linear Grayscale for /Game/.../T_xxx_H
//     Sampler type is Linear Color, should be Masks  for /Game/.../T_xxx_ORM
//
//   报错来自 MaterialExpressionUtils::VerifySamplerType（引擎在
//   MaterialExpressions.cpp 里由 UMaterialExpressionTextureSample::Compile 调用，
//   级别是 Compiler->Errorf）。
//
//   ⚠ 这类错误**必须前置校验**，不能指望材质编译兜底：
//   UMaterialEditingLibrary::RecompileMaterial 的实现在
//     `Material->GetMaterialResource(GMaxRHIShaderPlatform)` 返回 null 时
//     **静默 return {}**（见引擎 MaterialEditingLibrary.cpp:1054）。
//   无头 commandlet 下正是这种情况 —— 于是「编译 0 错误」是假绿灯，编译根本没跑。
//
// 设计原则：
//   期望值一律来自引擎自己的 MaterialExpressionUtils，**不手写映射表**。
//   手写表等于第二份真相，引擎改了规则我们就静默跑偏。附带好处是这里的错误文本
//   与用户在编辑器里看到的那条**完全同源**，不会出现"我们说过、引擎说不过"的分裂。

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"   // EMaterialSamplerType

class UTexture;

namespace ComfySamplerType
{
	/**
	 * 该贴图**应该**用哪个 SamplerType。
	 *
	 * 转发给 MaterialExpressionUtils::GetSamplerTypeForTexture（引擎权威推导）：
	 *   TC_Normalmap            → Normal
	 *   TC_Masks                → Masks
	 *   TC_Grayscale + sRGB=off → LinearGrayscale
	 *   TC_Grayscale + sRGB=on  → Grayscale
	 *   TC_Default/其余 + sRGB  → Color / LinearColor
	 *
	 * 贴图为空时返回 SAMPLERTYPE_Color（与引擎行为一致，调用方应另行判空）。
	 */
	COMFYTEXTURESTUDIO_API EMaterialSamplerType ForTexture(const UTexture* Texture);

	/**
	 * 核对 Actual 是否与贴图匹配。不符时 OutError 填引擎原文。
	 * 无贴图视为不通过（这是我们比引擎更严的地方：没有贴图的采样节点本身就是缺陷）。
	 */
	COMFYTEXTURESTUDIO_API bool Verify(const UTexture* Texture, EMaterialSamplerType Actual, FString& OutError);

	/** 枚举显示名（"Color" / "Masks" / "Linear Grayscale" …），用于报告与日志。 */
	COMFYTEXTURESTUDIO_API FString DisplayName(EMaterialSamplerType Type);
}
