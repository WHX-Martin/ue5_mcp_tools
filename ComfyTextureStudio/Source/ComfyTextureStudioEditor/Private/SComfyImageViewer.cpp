#include "SComfyImageViewer.h"

#include "ComfyPreviewTexture.h"
#include "HAL/PlatformProcess.h"
#include "Input/CursorReply.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ComfyImageViewer"

namespace
{
	/** 工具条按钮统一的小字号。 */
	const FSlateFontInfo& ToolbarFont()
	{
		static const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		return Font;
	}

	FSlateFontInfo HintFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 8);
	}
}

// --------------------------------------------------------------------------- //
// SComfyImageCanvas
// --------------------------------------------------------------------------- //

void SComfyImageCanvas::Construct(const FArguments& InArgs)
{
	Preview = InArgs._Preview;

	// 超出控件范围的部分必须裁掉，否则放大后会画到工具条和相邻面板上
	SetClipping(EWidgetClipping::ClipToBounds);
}

FVector2f SComfyImageCanvas::GetContentSize() const
{
	FVector2f Size = FVector2f::ZeroVector;
	if (Preview.IsValid())
	{
		Size = UE::Slate::CastToVector2f(Preview->GetImageSize());
	}
	if (bTiled)
	{
		Size *= 2.0f;
	}
	return Size;
}

float SComfyImageCanvas::EffectiveZoom(const FVector2f& ViewSize) const
{
	if (!bFitToWindow)
	{
		return Zoom;
	}

	const FVector2f Content = GetContentSize();
	if (Content.X <= 0.0f || Content.Y <= 0.0f || ViewSize.X <= 0.0f || ViewSize.Y <= 0.0f)
	{
		return 1.0f;
	}

	// 留 2% 内边距：贴边的话连图像边界都看不出来
	const float Fit = FMath::Min(ViewSize.X / Content.X, ViewSize.Y / Content.Y) * 0.98f;
	return FMath::Clamp(Fit, MinZoom, MaxZoom);
}

FVector2f SComfyImageCanvas::ComputeOrigin(const FVector2f& ViewSize, float EffZoom) const
{
	const FVector2f Content = GetContentSize() * EffZoom;
	return (ViewSize - Content) * 0.5f + Pan;
}

int32 SComfyImageCanvas::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// 工具条按钮会在 paint 之外调 SetZoom，需要一份最近的控件尺寸
	CachedViewSize = UE::Slate::CastToVector2f(AllottedGeometry.GetLocalSize());

	const FSlateBrush* Brush = Preview.IsValid() ? Preview->GetBrush() : nullptr;
	if (Brush == nullptr || CachedViewSize.IsNearlyZero())
	{
		return LayerId;
	}

	const float EffZoom = EffectiveZoom(CachedViewSize);
	const FVector2f Origin = ComputeOrigin(CachedViewSize, EffZoom);
	const FVector2f TileSize = UE::Slate::CastToVector2f(Preview->GetImageSize()) * EffZoom;

	// 2×2 时画四份：四方连续的贴图拼起来不该有可见接缝，这是最省事的自检视角。
	// 只画一张时两者等价，所以不必分两条分支。
	const int32 Count = bTiled ? 4 : 1;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2f TileOffset = bTiled
			? FVector2f(static_cast<float>(Index % 2) * TileSize.X,
				static_cast<float>(Index / 2) * TileSize.Y)
			: FVector2f::ZeroVector;

		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(TileSize, FSlateLayoutTransform(Origin + TileOffset)),
			Brush,
			ESlateDrawEffect::None,
			FLinearColor::White);
	}

	return LayerId + 1;
}

void SComfyImageCanvas::FitToWindow()
{
	bFitToWindow = true;
	Pan = FVector2f::ZeroVector;
}

void SComfyImageCanvas::ZoomAround(const FVector2f& Anchor, float NewZoom)
{
	const float Clamped = FMath::Clamp(NewZoom, MinZoom, MaxZoom);

	// 控件还没画过（尺寸未知）时没法做锚定，退化成直接设值
	if (CachedViewSize.X <= 0.0f || CachedViewSize.Y <= 0.0f)
	{
		Zoom = Clamped;
		bFitToWindow = false;
		return;
	}

	const float OldZoom = EffectiveZoom(CachedViewSize);
	const FVector2f OldOrigin = ComputeOrigin(CachedViewSize, OldZoom);

	// 锚点落在内容的哪个像素上；缩放后要让同一个像素仍停在锚点下
	const FVector2f AnchorPoint = (Anchor - OldOrigin) / OldZoom;

	const FVector2f NewContent = GetContentSize() * Clamped;
	Pan = (Anchor - AnchorPoint * Clamped) - (CachedViewSize - NewContent) * 0.5f;

	Zoom = Clamped;
	bFitToWindow = false;
}

void SComfyImageCanvas::SetZoom(float InZoom)
{
	// 按钮没有鼠标位置，就以视图中心为锚点
	ZoomAround(CachedViewSize * 0.5f, InZoom);
}

void SComfyImageCanvas::SetTiled(bool bInTiled)
{
	bTiled = bInTiled;
	if (bFitToWindow)
	{
		// 内容尺寸变了，适应模式会在下次 paint 里自动重算；这里只是清掉平移
		Pan = FVector2f::ZeroVector;
	}
}

float SComfyImageCanvas::GetZoomPercent() const
{
	return EffectiveZoom(CachedViewSize) * 100.0f;
}

FReply SComfyImageCanvas::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const float Delta = MouseEvent.GetWheelDelta();
	if (FMath::IsNearlyZero(Delta))
	{
		return FReply::Unhandled();
	}

	const FVector2f Anchor = UE::Slate::CastToVector2f(
		MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));

	// 1.15^Delta：一格滚轮约 15%，手感与常见看图工具接近
	ZoomAround(Anchor, EffectiveZoom(CachedViewSize) * FMath::Pow(1.15f, Delta));
	return FReply::Handled();
}

FReply SComfyImageCanvas::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const bool bLeftOrMiddle = MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton
		|| MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton;
	if (!bLeftOrMiddle)
	{
		return FReply::Unhandled();
	}

	bPanning = true;
	PanAnchorMouse = UE::Slate::CastToVector2f(
		MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
	PanAnchorPan = Pan;

	return FReply::Handled().CaptureMouse(SharedThis(this));
}

FReply SComfyImageCanvas::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bPanning || !HasMouseCapture())
	{
		return FReply::Unhandled();
	}

	const FVector2f Now = UE::Slate::CastToVector2f(
		MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));

	// 始终以「按下时」为基准重算。逐帧累加 delta 在掉帧时会把位移算丢。
	Pan = PanAnchorPan + (Now - PanAnchorMouse);
	return FReply::Handled();
}

FReply SComfyImageCanvas::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	(void)MyGeometry;
	(void)MouseEvent;

	if (!bPanning)
	{
		return FReply::Unhandled();
	}

	bPanning = false;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply SComfyImageCanvas::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	(void)MyGeometry;

	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	// 双击复位：和大多数看图工具一致
	FitToWindow();
	return FReply::Handled();
}

FCursorReply SComfyImageCanvas::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) const
{
	(void)MyGeometry;
	(void)MouseEvent;

	return FCursorReply::Cursor(bPanning ? EMouseCursor::GrabHandClosed : EMouseCursor::GrabHand);
}

// --------------------------------------------------------------------------- //
// SComfyImageViewer
// --------------------------------------------------------------------------- //

void SComfyImageViewer::Construct(const FArguments& InArgs)
{
	Preview = InArgs._Preview;
	Title = InArgs._Title;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 8.0f, 8.0f, 6.0f)
		[
			BuildToolbar()
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
			.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.025f, 1.0f))
			.Padding(0.0f)
			[
				SAssignNew(Canvas, SComfyImageCanvas)
				.Preview(Preview)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f, 8.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(ComfyImageViewerText::Hint()))
			.Font(HintFont())
			.ColorAndOpacity(FLinearColor(0.55f, 0.55f, 0.58f, 1.0f))
		]
	];

	if (Canvas.IsValid())
	{
		Canvas->SetTiled(InArgs._StartTiled);
	}
}

bool SComfyImageViewer::HasPreview() const
{
	return Preview.IsValid() && Preview->GetBrush() != nullptr;
}

bool SComfyImageViewer::CanOpenExternally() const
{
	// 导出/外部打开靠的是磁盘上的原文件，而中转 PNG 在导入后会被清理。
	// 文件没了就禁用按钮 —— 比点下去毫无反应好。
	return Preview.IsValid()
		&& !Preview->GetPngPath().IsEmpty()
		&& FPaths::FileExists(Preview->GetPngPath());
}

TSharedRef<SWidget> SComfyImageViewer::BuildToolbar()
{
	// 按钮统一尺寸，免得一长一短看着乱。
	// Enabled 走构造期的属性（Slate 的 .IsEnabled 只能在这里给，TSharedRef<SButton> 上没有）：
	// 不用会失效的「用系统看图器打开」按钮比点了没反应好。
	const auto MakeButton = [this](const FText& Label, const FText& Tip,
		TFunction<FReply()> Action, TAttribute<bool> Enabled = TAttribute<bool>(true))
	{
		return SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "Button")
			.ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(Tip)
			.IsEnabled(Enabled)
			.OnClicked_Lambda([Action = MoveTemp(Action)]()
			{
				return Action();
			})
			[
				SNew(STextBlock).Text(Label).Font(ToolbarFont())
			];
	};

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Title).Font(ToolbarFont())
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
				.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SComfyImageViewer::GetZoomText)
				.Font(ToolbarFont())
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.72f, 1.0f))
				.MinDesiredWidth(64.0f)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				MakeButton(LOCTEXT("Fit", "适应窗口"),
					LOCTEXT("FitTip", "把整张图缩放到窗口大小（双击画布同效）"),
					[this]() { if (Canvas.IsValid()) { Canvas->FitToWindow(); } return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				MakeButton(LOCTEXT("Zoom100", "100%"),
					LOCTEXT("Zoom100Tip", "1:1 像素 —— 看噪点与接缝细节用这个"),
					[this]() { if (Canvas.IsValid()) { Canvas->SetZoom(1.0f); } return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				MakeButton(LOCTEXT("Zoom200", "200%"),
					LOCTEXT("Zoom200Tip", "放大到两倍像素"),
					[this]() { if (Canvas.IsValid()) { Canvas->SetZoom(2.0f); } return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				MakeButton(FText::FromString(TEXT("2×2 平铺")),
					LOCTEXT("TiledTip", "把图并排四份 —— 四方连续的贴图拼起来不该有可见接缝"),
					[this]()
					{
						if (Canvas.IsValid())
						{
							Canvas->SetTiled(!Canvas->IsTiled());
						}
						return FReply::Handled();
					})
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				MakeButton(LOCTEXT("External", "用系统看图器打开"),
					LOCTEXT("ExternalTip",
						"交给系统默认看图程序（看超大贴图时更顺手）\n"
						"注：导入后中转 PNG 会被清理，那时此按钮不可用"),
					[this]()
					{
						if (CanOpenExternally())
						{
							FPlatformProcess::LaunchFileInDefaultExternalApplication(
								*Preview->GetPngPath());
						}
						return FReply::Handled();
					},
					TAttribute<bool>::CreateSP(this, &SComfyImageViewer::CanOpenExternally))
			]
		];
}

FText SComfyImageViewer::GetZoomText() const
{
	if (!Canvas.IsValid())
	{
		return FText::GetEmpty();
	}
	if (Canvas->IsFitting())
	{
		return LOCTEXT("Fitting", "适应窗口");
	}
	return FText::FromString(FString::Printf(TEXT("%d%%"),
		FMath::RoundToInt(Canvas->GetZoomPercent())));
}

#undef LOCTEXT_NAMESPACE
