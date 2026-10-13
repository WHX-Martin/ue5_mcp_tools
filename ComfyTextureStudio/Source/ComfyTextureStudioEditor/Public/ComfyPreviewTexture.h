// ComfyTextureStudio · 预览贴图的自持有句柄
//
// 为什么不让查看器直接复用面板的 brush 缓存：
//   查看器是一个**独立窗口**，用户完全可能关掉面板之后继续看图。
//   面板析构时会 ReleasePreviewTextures() 把贴图 RemoveFromRoot()，
//   那时若查看器还引用着旧 brush，画出来的就是一张可能已被 GC 的贴图 ——
//   和 M1 那个 HTTP 野指针是同一类生命周期事故（见 memory 的 `UE 侧踩过的坑` 第 1 条）。
//
//   ⇒ 原则：**谁画的谁持有。** 查看器自己加载、自己 AddToRoot、自己释放。

#pragma once

#include "CoreMinimal.h"

class UTexture2D;
struct FSlateBrush;
struct FSlateDynamicImageBrush;

/**
 * 一张预览贴图 + 它的 Slate brush，两者同生共死。
 *
 * 用 `TSharedPtr` 传递：只要还有 widget 在画它，贴图就不会被 GC。
 */
class FComfyPreviewTexture
{
public:
	/** 只做默认初始化；**有意义的构造入口是 LoadFromPng**。
	 *  设为 public 是因为 MakeShared 需要访问它（私有构造会 C2248）。 */
	FComfyPreviewTexture() = default;

	~FComfyPreviewTexture();

	/** 从 PNG 文件加载。文件读不到或解码失败时返回 nullptr（不抛异常）。 */
	static TSharedPtr<FComfyPreviewTexture> LoadFromPng(const FString& PngPath);

	/** 画图用的 brush。构造失败时为 nullptr。 */
	const FSlateBrush* GetBrush() const { return Brush.Get(); }

	UTexture2D* GetTexture() const { return Texture; }

	/** 贴图像素尺寸（如 1024×1024），1:1 缩放的基准。 */
	FVector2D GetImageSize() const { return ImageSize; }

	const FString& GetPngPath() const { return PngPath; }

private:
	/** 已 AddToRoot 的 transient 贴图。析构时 RemoveFromRoot。 */
	UTexture2D* Texture = nullptr;

	TSharedPtr<FSlateDynamicImageBrush> Brush;

	FVector2D ImageSize = FVector2D::ZeroVector;
	FString PngPath;
};
