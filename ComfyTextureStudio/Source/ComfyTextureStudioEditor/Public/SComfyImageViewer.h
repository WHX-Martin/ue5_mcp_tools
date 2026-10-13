// ComfyTextureStudio · 图片查看器（点击缩略图后弹出的放大窗口）
//
// 为什么需要它：面板里的预览最大只有 150×150，而贴图是 1024² —— 缩到 1/7，
// 接缝、噪点、法线细节全看不出来，"换种子重出"时根本没法判断哪张更好。
//
// 提供的交互（与常见看图工具一致）：
//   滚轮      以鼠标位置为锚点缩放（0.05× ~ 16×）
//   左键拖拽  平移
//   双击      复位（回到适应窗口）
//   Ctrl+滚轮 同滚轮（不必按 Ctrl，直接滚即可）
//   工具条    适应窗口 / 50% / 100% / 200% / 2×2 平铺 / 用系统看图器打开
//
// 两个类：
//   SComfyImageCanvas  —— 纯画布：自己 OnPaint 按 zoom/pan 画 brush，处理鼠标
//   SComfyImageViewer  —— 工具条 + 画布，是给窗口用的完整内容
//
// 分成两层是必须的：SCompoundWidget 先画自己、再画子 widget，如果把画布和工具条
// 放在同一个 widget 里，自己 OnPaint 画出来的图会盖住工具条。

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FComfyPreviewTexture;
struct FGeometry;
struct FPointerEvent;
class FCursorReply;   // 是 class 不是 struct —— 前向声明写错会触发 C4099

/**
 * 画布的快捷键提示（窗口底部那行小字，两处共用同一份文案）。
 */
namespace ComfyImageViewerText
{
	inline const TCHAR* Hint() { return TEXT("滚轮缩放 · 按住左键拖拽平移 · 双击复位"); }
}

/** 纯画布：按 zoom/pan 绘制预览贴图，处理缩放与平移的手势。 */
class SComfyImageCanvas : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SComfyImageCanvas) {}
		SLATE_ARGUMENT(TSharedPtr<FComfyPreviewTexture>, Preview)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// ---- 给工具条用的控制 ----
	/** 回到"适应窗口"：缩放由控件尺寸算出，并清掉平移。 */
	void FitToWindow();
	/** 以视图中心为锚点设一个绝对缩放（1.0 = 1:1 像素）。 */
	void SetZoom(float InZoom);
	void SetTiled(bool bInTiled);
	bool IsTiled() const { return bTiled; }
	bool IsFitting() const { return bFitToWindow; }

	/** 缩放百分比（100 = 1:1）。适应模式下按当前控件尺寸算。 */
	float GetZoomPercent() const;

	float GetMinZoom() const { return MinZoom; }
	float GetMaxZoom() const { return MaxZoom; }

	// ---- 手势 ----
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) const override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	static constexpr float MinZoom = 0.05f;
	static constexpr float MaxZoom = 16.0f;

	/** 内容尺寸（未缩放）。开了 2×2 就是两倍宽两倍高。 */
	FVector2f GetContentSize() const;

	/** 实际生效的缩放：适应模式下由控件尺寸现算。 */
	float EffectiveZoom(const FVector2f& ViewSize) const;

	/** 内容左上角在控件坐标里的位置。 */
	FVector2f ComputeOrigin(const FVector2f& ViewSize, float EffZoom) const;

	/** 滚轮缩放的公共实现；Anchor 是控件局部坐标里的锚点。 */
	void ZoomAround(const FVector2f& Anchor, float NewZoom);

	TSharedPtr<FComfyPreviewTexture> Preview;

	float Zoom = 1.0f;
	FVector2f Pan = FVector2f::ZeroVector;
	bool bFitToWindow = true;
	bool bTiled = false;

	// 拖拽平移：记住按下那一刻的鼠标位置与当时的 Pan，
	// 移动时按「相对按下点」重算 —— 累加每帧 delta 会随帧率漂移。
	bool bPanning = false;
	FVector2f PanAnchorMouse = FVector2f::ZeroVector;
	FVector2f PanAnchorPan = FVector2f::ZeroVector;

	/** 最近一次 OnPaint 时的控件尺寸 —— 工具条按钮在 paint 之外调 SetZoom 时要用它。 */
	mutable FVector2f CachedViewSize = FVector2f::ZeroVector;
};

/**
 * 完整查看器内容：顶部工具条 + 画布 + 底部手势提示。
 * 直接作为 SWindow 的内容使用。
 */
class SComfyImageViewer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SComfyImageViewer) {}
		SLATE_ARGUMENT(TSharedPtr<FComfyPreviewTexture>, Preview)
		SLATE_ARGUMENT(FText, Title)
		/** 打开时是否直接进 2×2 平铺模式（点面板的平铺图时用）。 */
		SLATE_ARGUMENT(bool, StartTiled)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * 供外部（面板的自检、自动化）验证构造真的成功了。
	 * 实现在 .cpp：这里 FComfyPreviewTexture 只有前向声明，inline 里调它的成员会 C2027。
	 */
	bool HasPreview() const;

private:
	TSharedRef<SWidget> BuildToolbar();
	FText GetZoomText() const;

	TSharedPtr<FComfyPreviewTexture> Preview;
	TSharedPtr<SComfyImageCanvas> Canvas;
	FText Title;
};
