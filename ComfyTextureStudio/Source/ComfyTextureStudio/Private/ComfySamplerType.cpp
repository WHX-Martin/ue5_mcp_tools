#include "ComfySamplerType.h"

#include "Engine/Texture.h"
#include "Materials/MaterialExpressionUtils.h"
#include "UObject/Class.h"

namespace ComfySamplerType
{

EMaterialSamplerType ForTexture(const UTexture* Texture)
{
	if (!Texture)
	{
		return SAMPLERTYPE_Color;
	}

	// 单一真相在引擎那边：它是唯一能跟上 TC_* / sRGB / 虚拟纹理组合规则的实现。
	return MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
}

FString DisplayName(EMaterialSamplerType Type)
{
	// 用枚举反射取显示名，而不是自己写 switch：
	// 显示名必须与引擎报错里印出来的那个词一致，否则用户拿我们的报告去比对报错会对不上。
	if (const UEnum* Enum = StaticEnum<EMaterialSamplerType>())
	{
		const FString Name = Enum->GetDisplayNameTextByValue(static_cast<int64>(Type)).ToString();
		if (!Name.IsEmpty())
		{
			return Name;
		}
	}
	return FString::Printf(TEXT("SamplerType(%d)"), static_cast<int32>(Type));
}

bool Verify(const UTexture* Texture, EMaterialSamplerType Actual, FString& OutError)
{
	if (!Texture)
	{
		OutError = TEXT("采样节点没有指定贴图");
		return false;
	}

	// 直接调引擎的校验器，连错误文案都用它的。
	// 这样"我们自检说通过"和"编辑器编译报错"永远是同一把尺子，
	// 不会出现自检绿、打开材质却红的情况（这正是本文件诞生的原因）。
	return MaterialExpressionUtils::VerifySamplerType(
		Texture->GetPathName(),
		ForTexture(Texture),
		Texture->SRGB != 0,
		Actual,
		OutError);
}

} // namespace ComfySamplerType
