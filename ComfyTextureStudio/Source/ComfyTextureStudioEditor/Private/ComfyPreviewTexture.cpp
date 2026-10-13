#include "ComfyPreviewTexture.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"

FComfyPreviewTexture::~FComfyPreviewTexture()
{
	// 先放 brush：它内部持有贴图指针，顺序反了会在 GC 后短暂留下悬空引用
	Brush.Reset();

	if (Texture)
	{
		Texture->RemoveFromRoot();
		Texture = nullptr;
	}
}

TSharedPtr<FComfyPreviewTexture> FComfyPreviewTexture::LoadFromPng(const FString& PngPath)
{
	if (PngPath.IsEmpty())
	{
		return nullptr;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *PngPath))
	{
		return nullptr;
	}

	// FImageUtils 按文件头识别格式；PNG 由 ImageWrapper 模块提供解码器
	UTexture2D* Loaded = FImageUtils::ImportBufferAsTexture2D(Bytes);
	if (!Loaded)
	{
		return nullptr;
	}

	// transient 贴图只有我们在引用：不 AddToRoot 会被 GC 掉（表现是预览变白）
	Loaded->AddToRoot();

	TSharedPtr<FComfyPreviewTexture> Out = MakeShared<FComfyPreviewTexture>();
	Out->Texture = Loaded;
	Out->PngPath = PngPath;
	Out->ImageSize = FVector2D(
		static_cast<double>(Loaded->GetSizeX()),
		static_cast<double>(Loaded->GetSizeY()));
	Out->Brush = MakeShared<FSlateDynamicImageBrush>(Loaded, Out->ImageSize, FName(*PngPath));
	return Out;
}
