#include "SComfyTextureStudioPanel.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "ComfyAssetNaming.h"
#include "ComfyPaths.h"
#include "ComfyPreviewTexture.h"
#include "ComfyTextureImportService.h"
#include "ComfyTextureStudioEditor.h"
#include "ComfyTextureStudioSettings.h"
#include "ComfyWorkflowGraph.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SComfyImageViewer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ComfyTextureStudioPanel"

namespace
{
	// ---------------------------------------------------------------- 样式 //

	// FSlateColor 没有三浮点构造，要走 FLinearColor（免得颜色语义在两处漂移）
	const FSlateColor ColorMuted(FLinearColor(0.55f, 0.55f, 0.55f));
	const FSlateColor ColorOk(FLinearColor(0.20f, 0.72f, 0.32f));
	const FSlateColor ColorWarn(FLinearColor(0.92f, 0.64f, 0.12f));
	const FSlateColor ColorErr(FLinearColor(0.88f, 0.26f, 0.22f));

	FSlateFontInfo SmallFont() { return FCoreStyle::GetDefaultFontStyle("Regular", 9); }
	FSlateFontInfo BoldFont()  { return FCoreStyle::GetDefaultFontStyle("Bold", 9); }

	/** 预览资源缓存上限。见 TrimPreviewCache()：只淘汰文件还在磁盘上的条目。 */
	constexpr int32 MaxPreviewCache = 24;

	/** 参数行：固定宽度的标签 + 内容。 */
	FSlateFontInfo SectionFont() { return FCoreStyle::GetDefaultFontStyle("Bold", 11); }

	// -------------------------------------------------- 提示词骨架/拆分的逻辑 //
	// 已经搬到 Runtime 模块的 ComfyPromptScaffold.h/.cpp（ComfyPrompt::Split /
	// Join / FComfyPromptEditor）。搬家的理由：原来的实现在这里（Slate widget 的
	// 匿名命名空间内），既不能单测也不能在 commandlet 里回归 —— 于是"编辑描述会
	// 悄悄丢骨架"这个缺陷存在了很久没人发现。现在它由 `-TestPromptScaffold` 守着。

	/**
	 * 在枚举下拉里挑一个可用值：优先保留当前选中的，其次回退到 Preferred，
	 * 都没有才取第一项。
	 *
	 * **不要用 `Options[0]` 代替它。** ComfyUI 返回的枚举顺序与"我们希望默认哪个"
	 * 毫无关系 —— `sampler_name` 第一项是 euler、`scheduler` 第一项是 normal，
	 * 拿 [0] 当默认值就等于每次探测都悄悄改配置。
	 */
	TSharedPtr<FString> PickValidOption(
		const TArray<TSharedPtr<FString>>& Options,
		const TSharedPtr<FString>& Current,
		const FString& Preferred)
	{
		auto Find = [&Options](const FString& Value) -> TSharedPtr<FString>
		{
			for (const TSharedPtr<FString>& Item : Options)
			{
				if (Item.IsValid() && *Item == Value)
				{
					return Item;
				}
			}
			return nullptr;
		};

		if (Current.IsValid())
		{
			if (TSharedPtr<FString> Kept = Find(*Current))
			{
				return Kept;
			}
		}
		if (TSharedPtr<FString> Fallback = Find(Preferred))
		{
			return Fallback;
		}
		return Options.Num() > 0 ? Options[0] : Current;
	}

	/** 面板里的只读小标签。 */
	TSharedRef<SWidget> MakeNote(const FText& Text, const FSlateColor& Color = ColorMuted)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(SmallFont())
			.ColorAndOpacity(Color)
			.AutoWrapText(true);
	}
}

// --------------------------------------------------------------------------- //
// 构造与析构
// --------------------------------------------------------------------------- //

void SComfyTextureStudioPanel::Construct(const FArguments& InArgs)
{
	ProbeClient = MakeUnique<FComfyHttpClient>();

	// 通道规格（属性表来源）：读不到就明确显示出来，而不是默默用一套内置默认值
	FString SpecError;
	if (const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>())
	{
		if (!FComfyTextureImportService::LoadStageBSpecs(*Settings, ChannelSpecs, SpecError))
		{
			SpecLoadError = SpecError;
		}
	}
	else
	{
		SpecLoadError = TEXT("读不到插件设置");
	}

	// 把阶段 A 图里的原始提示词读进来，拆成前后缀 + 描述
	if (const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>())
	{
		TMap<FString, FComfyStageSpec> Stages;
		FString Error;
		const FString ManifestPath = FComfyTextureImportService::ResolvePluginPath(Settings->WorkflowManifestFile);
		if (FComfyWorkflowGraph::LoadManifest(ManifestPath, Stages, Error))
		{
			if (const FComfyStageSpec* StageA = Stages.Find(TEXT("stageA")))
			{
				// 工作流文件的位置必须用 ResolveWorkflowFile 解析，**不能把字段直接
				// 拼到插件根**：manifest 里这个字段只写了文件名（`stage_a_base_texture.json`）
				// 时，拼出来的路径是不存在的，而"读不到"在这里的后果不是简单报错 ——
				// 是**提示词模板为空**。骨架空着，用户一编辑描述框，完整模板就被一条
				// 裸描述顶掉，出图没有光照/几何线索，阶段B 的法线几乎全平。
				// 2026-10-14 用户报的"极端问题"就是这个（潜伏自 M2）。
				FComfyWorkflowGraph Graph;
				const FString GraphPath = ComfyPaths::ResolveWorkflowFile(
					StageA->ApiWorkflowFile, ManifestPath);
				if (GraphPath.IsEmpty())
				{
					PromptLoadError = FString::Printf(
						TEXT("找不到阶段A 的工作流文件「%s」"),
						*StageA->ApiWorkflowFile);
					UE_LOG(LogComfyTextureStudioEditor, Error,
						TEXT("%s（manifest 的 stages.stageA.apiWorkflowFile 字段，"
							"已尝试 插件根 / manifest 同目录 / Config/Workflows 三个基准）。"
							"提示词模板没载入，出图会缺光照与几何线索。"),
						*PromptLoadError);
				}
				else if (Graph.LoadFromFile(GraphPath, *StageA, Error))
				{
					// 骨架在这里**只初始化一次**，之后由 PromptEditor 持有，
					// 描述框怎么改都不会动它（见 ComfyPromptScaffold.h 的说明）。
					FString Prompt;
					if (Graph.GetSlotValueAsString(TEXT("positive"), Prompt) && !Prompt.IsEmpty())
					{
						PromptEditor.ResetFromPrompt(Prompt);
						UE_LOG(LogComfyTextureStudioEditor, Log,
							TEXT("提示词模板已载入（%d 字）· 骨架 %s"),
							Prompt.Len(),
							PromptEditor.HasScaffold() ? TEXT("完整") : TEXT("缺失"));
					}
					else
					{
						PromptLoadError = TEXT("工作流里读不到 positive 槽位");
						UE_LOG(LogComfyTextureStudioEditor, Error,
							TEXT("%s（%s），正向提示词会为空。"), *PromptLoadError, *GraphPath);
					}

					FString Neg;
					if (Graph.GetSlotValueAsString(TEXT("negative"), Neg))
					{
						NegativePrompt = Neg;
					}
				}
				else
				{
					PromptLoadError = FString::Printf(TEXT("装载阶段A 工作流失败：%s"), *Error);
					UE_LOG(LogComfyTextureStudioEditor, Error, TEXT("%s"), *PromptLoadError);
				}

				// 采样器/调度的默认值也来自 manifest。之前这里硬编码，而 ProbeServer
				// 拿到真实枚举后又无条件重置成列表第 0 项 —— ComfyUI 的 sampler_name
				// 第一项正好是 euler，于是「打开面板」就会把默认的 dpmpp_2m 换掉。
				if (const FComfySlotBinding* SamplerSlot = StageA->Slots.Find(TEXT("sampler")))
				{
					if (SamplerSlot->Default.IsValid())
					{
						SamplerDefault = SamplerSlot->Default->AsString();
					}
				}
				if (const FComfySlotBinding* SchedulerSlot = StageA->Slots.Find(TEXT("scheduler")))
				{
					if (SchedulerSlot->Default.IsValid())
					{
						SchedulerDefault = SchedulerSlot->Default->AsString();
					}
				}
			}
		}
	}

	// 从配置里取默认值
	if (const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>())
	{
		bFlipGreenChannel = Settings->DefaultFlipGreenChannel;
		bUseHeightParallax = Settings->DefaultImportHeight;
		bImportOrphanMaps = Settings->DefaultImportOrphanMaps;
		bCreateMaterialInstance = Settings->DefaultCreateMaterialInstance;
		BumpDepth = Settings->DefaultBumpDepth;
		bUseTemplateNaming = (Settings->NamingMode == EComfyNamingMode::Template);
	}

	// 默认 LoRA 从 manifest 读（换 LoRA 通道 Q7 的默认值）
	if (ChannelSpecs.Num() == 0 && !SpecLoadError.IsEmpty())
	{
		UE_LOG(LogComfyTextureStudioEditor, Warning, TEXT("通道规格装载失败：%s"), *SpecLoadError);
	}

	FString LoraDefault;
	{
		const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
		TMap<FString, FComfyStageSpec> Stages;
		FString Error;
		if (Settings && FComfyWorkflowGraph::LoadManifest(
			FComfyTextureImportService::ResolvePluginPath(Settings->WorkflowManifestFile), Stages, Error))
		{
			if (const FComfyStageSpec* StageA = Stages.Find(TEXT("stageA")))
			{
				if (const FComfySlotBinding* LoraSlot = StageA->Slots.Find(TEXT("lora")))
				{
					if (LoraSlot->Default.IsValid())
					{
						LoraDefault = LoraSlot->Default->AsString();
					}
				}
			}
		}
	}
	LoraName = LoraDefault;
	LoraOptions.Add(MakeShared<FString>(LoraDefault.IsEmpty() ? TEXT("(默认)") : LoraDefault));

	// 采样器/调度的初始候选（随后由 /object_info 覆盖为真实枚举）。
	// 初值取 manifest 的默认值，别写死 —— 写死的话一旦 manifest 改了就对不上。
	SamplerOptions.Add(MakeShared<FString>(SamplerDefault));
	SchedulerOptions.Add(MakeShared<FString>(SchedulerDefault));
	SelectedSampler = SamplerOptions[0];
	SelectedScheduler = SchedulerOptions[0];

	ChildSlot
	[
		SNew(SVerticalBox)

		// 标题栏
		+ SVerticalBox::Slot().AutoHeight()
		[
			BuildHeaderRow()
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SSeparator)
		]

		// 主体：左参数 / 右预览
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SSplitter)
			+ SSplitter::Slot().Value(0.52f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot().Padding(4)
				[
					BuildStageASection()
				]
			]
			+ SSplitter::Slot().Value(0.48f)
			[
				BuildPreviewColumn()
			]
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SSeparator)
		]

		// ② 生成 PBR 全套
		+ SVerticalBox::Slot().AutoHeight()
		[
			BuildStageBSection()
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SSeparator)
		]

		// ③ 导入 UE
		+ SVerticalBox::Slot().FillHeight(0.6f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(4)
			[
				BuildImportSection()
			]
		]
	];

	// 打开面板就探一次服务：连接状态与枚举都要用到
	ProbeServer();

	// 驱动任务状态机（编辑器里由 Slate 的 tick 回调推）。
	// 存下句柄，析构时显式注销 —— 虽然 Slate 也会在 widget 销毁时清理，
	// 但显式注销能保证 ticker 不会多活一帧去碰已析构的成员。
	ActiveTimerHandle = RegisterActiveTimer(
		0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SComfyTextureStudioPanel::OnActiveTimer));
}

SComfyTextureStudioPanel::~SComfyTextureStudioPanel()
{
	if (ActiveTimerHandle.IsValid())
	{
		UnRegisterActiveTimer(ActiveTimerHandle.ToSharedRef());
		ActiveTimerHandle.Reset();
	}
	ReleasePreviewTextures();
}

// --------------------------------------------------------------------------- //
// 小工具
// --------------------------------------------------------------------------- //

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildLabeledRow(
	const FText& Label, TSharedRef<SWidget> Content, const FText& Tooltip)
{
	TSharedRef<SWidget> Row =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 2, 6, 2)
		[
			SNew(SBox).WidthOverride(72.0f)
			[
				SNew(STextBlock).Text(Label).Font(SmallFont())
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			Content
		];

	if (!Tooltip.IsEmpty())
	{
		return SNew(SBox).ToolTipText(Tooltip)[Row];
	}
	return Row;
}

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildHeaderRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 6, 8, 6)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelTitle", "Comfy Texture Studio"))
			.Font(SectionFont())
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SComfyTextureStudioPanel::GetConnectionText)
			.ColorAndOpacity(this, &SComfyTextureStudioPanel::GetConnectionColor)
			.Font(SmallFont())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("Reconnect", "重新探测"))
			.ToolTipText(LOCTEXT("ReconnectTip", "重新检查 ComfyUI 连接并刷新模型/LoRA/采样器枚举"))
			.OnClicked_Lambda([this]()
			{
				ProbeServer();
				return FReply::Handled();
			})
		];
}

// --------------------------------------------------------------------------- //
// ① 生成基础贴图
// --------------------------------------------------------------------------- //

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildStageASection()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("StageAHeader", "① 生成基础贴图（SDXL + LoRA）"))
		.Font(BoldFont())
	];

	if (!SpecLoadError.IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			MakeNote(FText::Format(
				LOCTEXT("SpecError", "⚠ 读不到通道规格：{0}（导入会被禁用）"),
				FText::FromString(SpecLoadError)), ColorErr)
		];
	}

	// 材质描述
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		BuildLabeledRow(LOCTEXT("DescLabel", "材质描述"),
			SNew(SBox).MinDesiredHeight(46.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(PromptEditor.GetDescription()); })
				.OnTextChanged_Lambda([this](const FText& NewText)
				{
					// 只换描述。**不要在这里重新推导骨架** —— 描述被清空（全选重打必然
					// 经过这个中间态）时，启发式会把骨架当成描述一起吞掉且永久丢失。
					PromptEditor.SetDescription(NewText.ToString());
				})
				.AutoWrapText(true)
			],
			LOCTEXT("DescTip", "材质内容描述。前后固定 tag（LoRA 触发词、光照）来自训练集，不要改。"))
	];

	Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		MakeNote(LOCTEXT("DescHint",
			"ⓘ 前后固定 tag 来自 LoRA 训练集，改描述不会动它们。完整提示词见下方折叠区。"))
	];

	// 提示词相关的问题必须可见 —— 这些失败以前只写在日志里，面板上什么都看不见，
	// 用户看到的是"出图不对"，完全无从联想（2026-10-14 就是这么被坑的）。
	{
		// 上游原因（文件/路径/槽位）优先；没有才显示骨架本身的问题
		const FString Problem = !PromptLoadError.IsEmpty()
			? PromptLoadError
			: PromptEditor.GetWarning();

		if (!Problem.IsEmpty())
		{
			Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[
				MakeNote(FText::Format(
					LOCTEXT("PromptProblem", "⚠ {0}"),
					FText::FromString(Problem)),
					PromptLoadError.IsEmpty() ? ColorWarn : ColorErr)
			];
		}
	}

	// 完整正向提示词（可直接改）
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("PositiveTitle", "正向提示词（完整，可编辑）"))
		.BodyContent()
		[
			SNew(SBox).MinDesiredHeight(60.0f).Padding(0, 2)
			[
				SNew(SMultiLineEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(PromptEditor.GetFullPrompt()); })
				.OnTextChanged_Lambda([this](const FText& NewText)
				{
					// 逃生舱：允许在这里连骨架一起改。但 SetFullPrompt 只在**重新拆得出
					// 骨架**时才覆盖骨架 —— 拆不出就保留原样并按新描述重拼，
					// 免得一次误删把 LoRA 触发词永久抹掉。
					PromptEditor.SetFullPrompt(NewText.ToString());
				})
				.AutoWrapText(true)
			]
		]
	];

	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("NegativeTitle", "负向提示词（折叠）"))
		.BodyContent()
		[
			SNew(SBox).MinDesiredHeight(60.0f).Padding(0, 2)
			[
				SNew(SMultiLineEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(NegativePrompt); })
				.OnTextChanged_Lambda([this](const FText& NewText) { NegativePrompt = NewText.ToString(); })
				.AutoWrapText(true)
			]
		]
	];

	// 种子
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		BuildLabeledRow(LOCTEXT("SeedLabel", "种子"),
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SNumericEntryBox<int32>)
				.Value_Lambda([this]() { return TOptional<int32>(Seed); })
				.OnValueChanged_Lambda([this](int32 V) { Seed = FMath::Max(0, V); })
				.MinValue(0)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("RandomSeed", "🎲"))
				.ToolTipText(LOCTEXT("RandomSeedTip", "换一个随机种子。ComfyUI 有节点级缓存，同种子重跑会直接命中缓存（看着很快但没重算）。"))
				.OnClicked_Lambda([this]()
				{
					Seed = FMath::RandRange(1, 2147483646);
					return FReply::Handled();
				})
			],
			LOCTEXT("SeedTip", "同种子同参数会命中 ComfyUI 的节点级结果缓存"))
	];

	// 步数 / CFG
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			BuildLabeledRow(LOCTEXT("StepsLabel", "步数"),
				SNew(SNumericEntryBox<int32>)
				.Value_Lambda([this]() { return TOptional<int32>(Steps); })
				.OnValueChanged_Lambda([this](int32 V) { Steps = FMath::Clamp(V, 1, 200); })
				.MinValue(1).MaxValue(200))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 0, 0)
		[
			BuildLabeledRow(LOCTEXT("CfgLabel", "CFG"),
				SNew(SNumericEntryBox<float>)
				.Value_Lambda([this]() { return TOptional<float>(CFG); })
				.OnValueChanged_Lambda([this](float V) { CFG = FMath::Clamp(V, 0.0f, 30.0f); })
				.MinValue(0.0f).MaxValue(30.0f))
		]
	];

	// 采样器 / 调度
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			BuildLabeledRow(LOCTEXT("SamplerLabel", "采样器"),
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&SamplerOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
				{
					return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					SelectedSampler = Item;
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return FText::FromString(SelectedSampler.IsValid() ? *SelectedSampler : TEXT("(默认)"));
					})
				])
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 0, 0)
		[
			BuildLabeledRow(LOCTEXT("SchedulerLabel", "调度"),
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&SchedulerOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
				{
					return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					SelectedScheduler = Item;
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return FText::FromString(SelectedScheduler.IsValid() ? *SelectedScheduler : TEXT("(默认)"));
					})
				])
		]
	];

	// 尺寸
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 2, 6, 2)
		[
			SNew(SBox).WidthOverride(72.0f)[SNew(STextBlock).Text(LOCTEXT("SizeLabel", "尺寸")).Font(SmallFont())]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SNumericEntryBox<int32>)
			.Value_Lambda([this]() { return TOptional<int32>(Width); })
			.OnValueChanged_Lambda([this](int32 V)
			{
				Width = FMath::Clamp(V, 256, 2048);
				if (bLockSquare) { Height = Width; }
			})
			.MinValue(256).MaxValue(2048).MinSliderValue(256).MaxSliderValue(2048)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("Times", "×")).Font(SmallFont())
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SNumericEntryBox<int32>)
			.Value_Lambda([this]() { return TOptional<int32>(Height); })
			.OnValueChanged_Lambda([this](int32 V) { Height = FMath::Clamp(V, 256, 2048); })
			.IsEnabled_Lambda([this]() { return !bLockSquare; })
			.MinValue(256).MaxValue(2048).MinSliderValue(256).MaxSliderValue(2048)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bLockSquare ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S)
			{
				bLockSquare = (S == ECheckBoxState::Checked);
				if (bLockSquare) { Height = Width; }
			})
			[
				SNew(STextBlock).Text(LOCTEXT("LockSquare", "锁定 1:1")).Font(SmallFont())
			]
		]
	];

	// LoRA（Q7：预留换 LoRA 的通道）
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		BuildLabeledRow(LOCTEXT("LoraLabel", "LoRA"),
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&LoraOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
			{
				return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")));
			})
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
			{
				if (Item.IsValid()) { LoraName = *Item; }
			})
			[
				SNew(STextBlock).Text_Lambda([this]()
				{
					return FText::FromString(LoraName.IsEmpty() ? TEXT("(默认)") : LoraName);
				})
			],
			LOCTEXT("LoraTip", "换非沙地 LoRA 时，记得连正向提示词里的固定 tag 一起换（那些 tag 是 LoRA 训练集的一部分）。"))
	];

	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			BuildLabeledRow(LOCTEXT("LoraModelLabel", "模型强度"),
				SNew(SNumericEntryBox<float>)
				.Value_Lambda([this]() { return TOptional<float>(LoraStrengthModel); })
				.OnValueChanged_Lambda([this](float V) { LoraStrengthModel = FMath::Clamp(V, 0.0f, 2.0f); })
				.MinValue(0.0f).MaxValue(2.0f))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 0, 0)
		[
			BuildLabeledRow(LOCTEXT("LoraClipLabel", "CLIP 强度"),
				SNew(SNumericEntryBox<float>)
				.Value_Lambda([this]() { return TOptional<float>(LoraStrengthClip); })
				.OnValueChanged_Lambda([this](float V) { LoraStrengthClip = FMath::Clamp(V, 0.0f, 2.0f); })
				.MinValue(0.0f).MaxValue(2.0f))
		]
	];

	// 生成按钮 + 取消
	Box->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("Generate", "⚡ 生成基础贴图"))
			.IsEnabled(this, &SComfyTextureStudioPanel::CanGenerate)
			.OnClicked_Lambda([this]()
			{
				StartStageA(/*bNewSeed=*/false);
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("Cancel", "取消"))
			.IsEnabled_Lambda([this]() { return IsJobRunning(); })
			.OnClicked_Lambda([this]()
			{
				RequestCancel();
				return FReply::Handled();
			})
		]
	];

	// 进度
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SProgressBar)
		.Percent(this, &SComfyTextureStudioPanel::GetProgressFraction)
	];

	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(STextBlock)
		.Text(this, &SComfyTextureStudioPanel::GetStatusText)
		.ColorAndOpacity(this, &SComfyTextureStudioPanel::GetStatusColor)
		.Font(SmallFont())
		.AutoWrapText(true)
	];

	return Box;
}

// --------------------------------------------------------------------------- //
// 右栏预览
// --------------------------------------------------------------------------- //

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildTiledPreview(float TileSize)
{
	// 2×2 平铺自检：把同一张图并排 4 份。四方连续的贴图拼起来不该有可见接缝；
	// 一旦接缝出现，这里是最容易看出来的地方（比单张盯久了有效）。
	//
	// lambda 只捕获 this（**不捕获路径**）：路径每帧从成员里取，
	// 否则构造期那个空串会被固化进 4 个槽位，平铺区永远空白。
	TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(0));
	for (int32 i = 0; i < 4; ++i)
	{
		Grid->AddSlot(i % 2, i / 2)
		[
			SNew(SBox).WidthOverride(TileSize).HeightOverride(TileSize)
			[
				SNew(SImage)
				.Image_Lambda([this]() { return GetPreviewBrush(PreviewHistory.GetFocused()); })
			]
		];
	}

	// 点开时直接以平铺模式打开：150px 下接缝太难判，大窗里 2×2 才看得清
	return SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.ContentPadding(0.0f)
		.ToolTipText(LOCTEXT("OpenTiledTip",
			"点击放大查看（直接以 2×2 平铺打开，便于判断四方连续是否有接缝）"))
		.OnClicked_Lambda([this]()
		{
			OpenImageViewer(PreviewHistory.GetFocused(), TEXT("基础贴图 · 平铺"), /*bStartTiled=*/true);
			return FReply::Handled();
		})
		[
			Grid
		];
}

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildHistoryCandidate(int32 Index)
{
	// 只捕获**下标**，不捕获路径：历史候选每帧都在变（新的插到头部，
	// 所有条目的下标都跟着挪），捕获路径就等于把构造期那一瞬间的内容固化成常量。
	return SNew(SVerticalBox)
		.Visibility_Lambda([this, Index]()
		{
			return PreviewHistory.IsValidIndex(Index) ? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.ContentPadding(0.0f)
			.ToolTipText_Lambda([this, Index]()
			{
				if (!PreviewHistory.IsValidIndex(Index))
				{
					return FText::GetEmpty();
				}
				return FText::Format(
					LOCTEXT("CandidateTip",
						"#{0} · 单击在右栏查看\n{1}\n（右栏大图可再点开放大 / 2×2 平铺）"),
					FText::AsNumber(Index + 1),
					FText::FromString(FPaths::GetCleanFilename(PreviewHistory.At(Index))));
			})
			.OnClicked_Lambda([this, Index]()
			{
				// 单击 = 把右栏切到这张。**钉住**：之后新到的结果不许抢焦点，
				// 否则用户正在对比第 2 张时画面会被第 4 张跳走。
				if (PreviewHistory.FocusCandidate(Index))
				{
					UE_LOG(LogComfyTextureStudioEditor, Log,
						TEXT("预览焦点切到历史候选 #%d：%s（已钉住，新结果不会抢焦点）"),
						Index + 1, *FPaths::GetCleanFilename(PreviewHistory.GetFocused()));
				}
				return FReply::Handled();
			})
			[
				SNew(SBox).WidthOverride(48.0f).HeightOverride(48.0f)
				[
					SNew(SImage)
					.Image_Lambda([this, Index]()
					{
						return PreviewHistory.IsValidIndex(Index)
							? GetPreviewBrush(PreviewHistory.At(Index))
							: nullptr;
					})
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text_Lambda([this, Index]()
			{
				if (!PreviewHistory.IsValidIndex(Index))
				{
					return FText::GetEmpty();
				}
				// 用文字标记当前项而不是加边框：不用去猜引擎里哪个 brush 名可靠，
				// 也不会给贴图叠一层色偏（判色时那是有害的）。
				return PreviewHistory.IsCandidateFocused(Index)
					? FText::Format(LOCTEXT("CandidateFocused", "#{0} ✓当前"), FText::AsNumber(Index + 1))
					: FText::Format(LOCTEXT("CandidateIndex", "#{0}"), FText::AsNumber(Index + 1));
			})
			.Font(SmallFont())
			.ColorAndOpacity_Lambda([this, Index]()
			{
				return PreviewHistory.IsCandidateFocused(Index) ? ColorOk : ColorMuted;
			})
		];
}

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildPreviewColumn()
{
	// 历史候选行按「下标」建固定数量的槽位（见 BuildHistoryCandidate 的注释）。
	TSharedRef<SHorizontalBox> HistoryRow = SNew(SHorizontalBox);
	for (int32 Index = 0; Index < ComfyPreview::FStageAHistory::MaxEntries; ++Index)
	{
		HistoryRow->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
		[
			BuildHistoryCandidate(Index)
		];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 6, 6, 2)
		[
			SNew(STextBlock).Text(LOCTEXT("PreviewHeader", "预览")).Font(BoldFont())
		]

		// 原图 + 2×2
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SButton)
					.ButtonStyle(FCoreStyle::Get(), "NoBorder")
					.ContentPadding(0.0f)
					.ToolTipText(LOCTEXT("OpenOriginalTip",
						"点击放大查看：滚轮缩放 · 拖拽平移 · 双击复位"))
					.OnClicked_Lambda([this]()
					{
						OpenImageViewer(PreviewHistory.GetFocused(), TEXT("基础贴图"));
						return FReply::Handled();
					})
					[
						SNew(SBox).WidthOverride(150.0f).HeightOverride(150.0f)
						[
							SNew(SImage)
							.Image_Lambda([this]() { return GetPreviewBrush(PreviewHistory.GetFocused()); })
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetFocusedPreviewText(); })
					.Font(SmallFont())
					.ColorAndOpacity(ColorMuted)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Top)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					BuildTiledPreview(75.0f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					MakeNote(LOCTEXT("TiledLabel", "2×2 平铺自检（看接缝）"))
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(6, 2)
		[
			SNew(STextBlock)
			.Text(this, &SComfyTextureStudioPanel::GetStageAInfoText)
			.Font(SmallFont())
			.ColorAndOpacity(ColorMuted)
		]

		// 历史候选：PRD §7.2 的 #1~#4，实现最多留 FStageAHistory::MaxEntries 项（最新在前）
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 4, 6, 2)
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				if (PreviewHistory.Num() == 0)
				{
					return LOCTEXT("HistoryLabelEmpty", "历史候选");
				}
				return FText::Format(
					LOCTEXT("HistoryLabelN", "历史候选（{0}）· 单击切换预览焦点"),
					FText::AsNumber(PreviewHistory.Num()));
			})
			.Font(SmallFont())
			.ColorAndOpacity(ColorMuted)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				SNew(SBox).MinDesiredHeight(52.0f)
				.Visibility_Lambda([this]()
				{
					return PreviewHistory.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SScrollBox).Orientation(Orient_Horizontal)
					+ SScrollBox::Slot().Padding(0, 0, 4, 0)
					[
						HistoryRow
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("HistoryEmpty",
					"每次生成（含「换种子重出」）都会留在这里，方便横向对比。"))
				.Font(SmallFont())
				.ColorAndOpacity(ColorMuted)
				.AutoWrapText(true)
				.Visibility_Lambda([this]()
				{
					return PreviewHistory.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
			]
		]

		// 审阅动作（只在 StageAReady 可点）
		+ SVerticalBox::Slot().AutoHeight().Padding(6, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("Reseed", "换种子重出"))
				.ToolTipText(LOCTEXT("ReseedTip", "换一个随机种子重新跑阶段 A（不会重跑成同一张）"))
				.IsEnabled(this, &SComfyTextureStudioPanel::CanReviewStageA)
				.OnClicked_Lambda([this]()
				{
					StartStageA(/*bNewSeed=*/true);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("Discard", "丢弃"))
				.IsEnabled(this, &SComfyTextureStudioPanel::CanDiscardStageA)
				.OnClicked_Lambda([this]()
				{
					DiscardStageAResult();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.4f).Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("AcceptStageA", "✓ 用这张继续"))
				.IsEnabled(this, &SComfyTextureStudioPanel::CanContinueToStageB)
				.OnClicked_Lambda([this]()
				{
					RequestContinueToStageB();
					return FReply::Handled();
				})
			]
		];
}

// --------------------------------------------------------------------------- //
// ② 生成 PBR 全套
// --------------------------------------------------------------------------- //

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildChannelThumb(const FString& Semantic)
{
	const FComfyOutputAssetSpec* Spec = ChannelSpecs.Find(Semantic);
	const bool bDeliverable = Spec ? Spec->bImportByDefault : ComfyChannels::IsDeliverable(Semantic);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.ContentPadding(0.0f)
			.ToolTipText(FText::Format(
				LOCTEXT("ChannelTip", "点击放大查看（可切 2×2 平铺看接缝）\n{0}"),
				FText::FromString(Spec ? Spec->Role : FString())))
			.OnClicked_Lambda([this, Semantic]()
			{
				OpenImageViewer(
					GetChannelPngPath(Semantic),
					ComfyChannels::DisplayName(Semantic));
				return FReply::Handled();
			})
			[
				SNew(SBox).WidthOverride(68.0f).HeightOverride(68.0f)
				[
					SNew(SImage)
					.Image_Lambda([this, Semantic]()
					{
						return GetPreviewBrush(GetChannelPngPath(Semantic));
					})
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			MakeNote(FText::FromString(ComfyChannels::DisplayName(Semantic)), bDeliverable ? ColorOk : ColorMuted)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeNote(
				bDeliverable ? LOCTEXT("Imported", "✓ 参与材质") : LOCTEXT("Orphan", "灰·不参与材质"),
				bDeliverable ? ColorOk : ColorMuted)
		];
}

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildStageBSection()
{
	TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(6, 2));
	const TArray<FString>& Semantics = ComfyChannels::All();
	for (int32 i = 0; i < Semantics.Num(); ++i)
	{
		Grid->AddSlot(i, 0)[BuildChannelThumb(Semantics[i])];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("StageBHeader", "② 生成 PBR 全套")).Font(BoldFont())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
			[
				MakeNote(LOCTEXT("StageBSub", "DSINE 几何反演 + 去光照 · 无 MoGe 对照支路（D2）"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("GeneratePBR", "生成 PBR 全套"))
				.ToolTipText(LOCTEXT("GeneratePBRTip", "用阶段 A 确认的贴图做几何反演，产出整套 PBR（约 5 秒）"))
				.IsEnabled(this, &SComfyTextureStudioPanel::CanContinueToStageB)
				.OnClicked_Lambda([this]()
				{
					RequestContinueToStageB();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return LastResult.BaseTexturePath.IsEmpty()
						? LOCTEXT("NoInputYet", "输入：尚未有阶段 A 产物")
						: FText::Format(LOCTEXT("StageBInput", "输入：{0}"),
							FText::FromString(FPaths::GetCleanFilename(LastResult.BaseTexturePath)));
				})
				.Font(SmallFont())
				.ColorAndOpacity(ColorMuted)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			Grid
		];
}

// --------------------------------------------------------------------------- //
// ③ 导入 UE
// --------------------------------------------------------------------------- //

TSharedRef<SWidget> SComfyTextureStudioPanel::BuildImportSection()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot().AutoHeight().Padding(8, 4, 8, 0)
	[
		SNew(STextBlock).Text(LOCTEXT("StageCHeader", "③ 导入 UE 并创建材质")).Font(BoldFont())
	];

	// 命名模式（Q6 的双通道）
	Box->AddSlot().AutoHeight().Padding(8, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
		[
			SNew(SCheckBox)
			.Style(FCoreStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this]() { return bUseTemplateNaming ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState) { bUseTemplateNaming = true; })
			[
				SNew(STextBlock).Text(LOCTEXT("ModeTemplate", "模板")).Font(SmallFont())
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
		[
			SNew(SCheckBox)
			.Style(FCoreStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this]() { return bUseTemplateNaming ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState) { bUseTemplateNaming = false; })
			[
				SNew(STextBlock).Text(LOCTEXT("ModeManual", "手填")).Font(SmallFont())
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NamingTemplateHint", "M_{material}_{grain}_{index}"))
			.Font(SmallFont())
			.ColorAndOpacity(ColorMuted)
			.Visibility_Lambda([this]() { return bUseTemplateNaming ? EVisibility::Visible : EVisibility::Collapsed; })
		]
	];

	// 模板模式的两个输入
	Box->AddSlot().AutoHeight().Padding(8, 2)
	[
		SNew(SHorizontalBox)
		.Visibility_Lambda([this]() { return bUseTemplateNaming ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			BuildLabeledRow(LOCTEXT("MaterialLabel", "材质"),
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(TemplateMaterial); })
				.OnTextChanged_Lambda([this](const FText& T) { TemplateMaterial = T.ToString(); }))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 0, 0)
		[
			BuildLabeledRow(LOCTEXT("GrainLabel", "粒度"),
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(TemplateGrain); })
				.OnTextChanged_Lambda([this](const FText& T) { TemplateGrain = T.ToString(); }))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("GuessFromDesc", "从描述推断"))
			.ToolTipText(LOCTEXT("GuessFromDescTip", "按常见材质名词表从材质描述里猜出材质与粒度，可手工改"))
			.OnClicked_Lambda([this]()
			{
				FString M, G;
				FComfyAssetNaming::SuggestFromDescription(PromptEditor.GetDescription(), M, G);
				TemplateMaterial = M;
				TemplateGrain = G;
				return FReply::Handled();
			})
		]
	];

	// 手填模式的输入
	Box->AddSlot().AutoHeight().Padding(8, 2)
	[
		SNew(SHorizontalBox)
		.Visibility_Lambda([this]() { return bUseTemplateNaming ? EVisibility::Collapsed : EVisibility::Visible; })
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			BuildLabeledRow(LOCTEXT("ManualNameLabel", "资产名"),
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(ManualName); })
				.OnTextChanged_Lambda([this](const FText& T) { ManualName = T.ToString(); }))
		]
	];

	// 资产路径实时预览
	Box->AddSlot().AutoHeight().Padding(8, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("AssetPathLabel", "资产路径")).Font(SmallFont())
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SComfyTextureStudioPanel::GetResolvedNamePreview)
			.Font(SmallFont())
		]
	];

	Box->AddSlot().AutoHeight().Padding(8, 0, 8, 4)
	[
		MakeNote(LOCTEXT("ConflictHint", "ⓘ 命名冲突会报错，不会静默覆盖（Q6）"))
	];

	// 导入选项
	Box->AddSlot().AutoHeight().Padding(8, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bCreateMaterialInstance ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bCreateMaterialInstance = (S == ECheckBoxState::Checked); })
			[ SNew(STextBlock).Text(LOCTEXT("OptMI", "生成材质实例(MI)")).Font(SmallFont()) ]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bFlipGreenChannel ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bFlipGreenChannel = (S == ECheckBoxState::Checked); })
			.ToolTipText(LOCTEXT("OptFlipTip",
				"ComfyUI 输出的是 OpenGL 约定法线，UE 需要 DirectX ⇒ 必须翻绿通道。\n"
				"不翻的表现是凹凸方向反（凹坑看起来像凸起），肉眼不易察觉。"))
			[ SNew(STextBlock).Text(LOCTEXT("OptFlip", "翻转法线绿通道")).Font(SmallFont()) ]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bUseHeightParallax ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bUseHeightParallax = (S == ECheckBoxState::Checked); })
			.ToolTipText(LOCTEXT("OptHeightTip",
				"把高度图接进 BumpOffset 做视差。其 UV 输出会接到 albedo/normal/ORM 三张采样上 ——\n"
				"只接 BaseColor 会导致视差与法线错位。"))
			[ SNew(STextBlock).Text(LOCTEXT("OptHeight", "高度接入 BumpOffset")).Font(SmallFont()) ]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bImportOrphanMaps ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bImportOrphanMaps = (S == ECheckBoxState::Checked); })
			.ToolTipText(LOCTEXT("OptOrphanTip",
				"ORM 合并图已含 AO 与粗糙度，这两张单图默认不导入也不接材质（Q3）。\n"
				"勾选后仍只是导入资产，不接入材质。"))
			[ SNew(STextBlock).Text(LOCTEXT("OptOrphan", "额外导入 AO/Roughness")).Font(SmallFont()) ]
		]
	];

	// 导入按钮
	Box->AddSlot().AutoHeight().Padding(8, 4)
	[
		SNew(SButton)
		.HAlign(HAlign_Center)
		.Text(LOCTEXT("RunImport", "📦 导入并创建材质"))
		.IsEnabled(this, &SComfyTextureStudioPanel::CanImport)
		.OnClicked_Lambda([this]()
		{
			RequestImport();
			return FReply::Handled();
		})
	];

	Box->AddSlot().AutoHeight().Padding(8, 2)
	[
		SNew(STextBlock)
		.Text(this, &SComfyTextureStudioPanel::GetImportHintText)
		.Font(SmallFont())
		.AutoWrapText(true)
	];

	// 报告区（§7.4）
	Box->AddSlot().AutoHeight().Padding(8, 4)
	[
		SNew(SVerticalBox)
		.Visibility_Lambda([this]()
		{
			return ImportState.bRan ? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).MinDesiredHeight(120.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(ImportState.ReportText); })
				.IsReadOnly(true)
				.AutoWrapText(false)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("CopyReport", "复制报告"))
				.OnClicked_Lambda([this]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*ImportState.ReportText);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ShowInCB", "在 Content Browser 中显示"))
				.IsEnabled_Lambda([this]() { return !ImportState.Report.DestinationPath.IsEmpty(); })
				.OnClicked_Lambda([this]()
				{
					// 用引擎自带的同步命令，避免直接依赖 ContentBrowser 模块 API
					const FString Command = FString::Printf(TEXT("SyncToAssets %s"), *ImportState.Report.DestinationPath);
					GEditor->Exec(nullptr, *Command);
					return FReply::Handled();
				})
			]
		]
	];

	return Box;
}

// --------------------------------------------------------------------------- //
// 任务控制
// --------------------------------------------------------------------------- //

void SComfyTextureStudioPanel::StartStageA(bool bNewSeed)
{
	if (IsJobRunning())
	{
		return;
	}

	if (bNewSeed)
	{
		// 关键：ComfyUI 有节点级结果缓存，同图同种子重提交会直接命中缓存。
		// 「换种子重出」必须真的换种子，否则用户会以为重算了其实没有（M0 实测 12.3s→1.5s）。
		Seed = FMath::RandRange(1, 2147483646);
	}

	// 把完整提示词与采样参数记进日志。2026-10-13 那次提示词悄悄退化成一条裸描述，
	// 事后只能从 stage_a_api.json 反查 —— 这次让它当场可查。
	UE_LOG(LogComfyTextureStudioEditor, Log,
		TEXT("阶段A 提交 · 提示词 %d 字 · 采样器 %s/%s · 种子 %d"),
		PromptEditor.GetFullPrompt().Len(),
		SelectedSampler.IsValid() ? **SelectedSampler : TEXT("(默认)"),
		SelectedScheduler.IsValid() ? **SelectedScheduler : TEXT("(默认)"),
		Seed);
	UE_LOG(LogComfyTextureStudioEditor, Log, TEXT("  正向：%s"), *PromptEditor.GetFullPrompt());
	if (!PromptEditor.HasScaffold())
	{
		UE_LOG(LogComfyTextureStudioEditor, Warning,
			TEXT("  提示词骨架缺失（%s）—— 出图可能缺少光照/几何线索，阶段B 的法线会偏平。"),
			*PromptEditor.GetWarning());
	}

	FComfyTextureJobParams Params;
	Params.PositivePrompt = PromptEditor.GetFullPrompt();
	Params.NegativePrompt = NegativePrompt;
	Params.Seed = Seed;
	Params.Steps = Steps;
	Params.Width = Width;
	Params.Height = Height;
	Params.LoraName = LoraName;
	Params.bPauseAfterStageA = true;   // 向导：阶段 A 跑完停下来等审阅

	Params.SlotOverrides.Add(TEXT("cfg"), FString::SanitizeFloat(CFG));
	Params.SlotOverrides.Add(TEXT("loraModelStrength"), FString::SanitizeFloat(LoraStrengthModel));
	Params.SlotOverrides.Add(TEXT("loraClipStrength"), FString::SanitizeFloat(LoraStrengthClip));
	if (SelectedSampler.IsValid())
	{
		Params.SlotOverrides.Add(TEXT("sampler"), *SelectedSampler);
	}
	if (SelectedScheduler.IsValid())
	{
		Params.SlotOverrides.Add(TEXT("scheduler"), *SelectedScheduler);
	}

	// 清掉上一轮的状态
	ChannelPngPaths.Reset();
	ImportState = FComfyPanelImportState();
	LastResult = FComfyJobResult();
	StatusText.Reset();

	// 新一轮开始 ⇒ 解除"钉住"，新产物到了就让预览跟过去（用户此刻想看的是新的那张）。
	// 焦点故意**不清**：生成要好几秒，留着上一张比让右栏空着好，
	// 新的那张一到就会被换掉。
	PreviewHistory.Unpin();

	Job = MakeUnique<FComfyTextureJob>();
	JobStartTime = FPlatformTime::Seconds();
	Job->OnStateChanged = [this](EComfyJobState NewState, const FString& Text)
	{
		OnJobStateChanged(NewState, Text);
	};
	Job->OnFinished = [this](const FComfyJobResult& InResult)
	{
		OnJobFinished(InResult);
	};
	Job->Start(Params);
}

void SComfyTextureStudioPanel::RequestContinueToStageB()
{
	if (!Job.IsValid())
	{
		return;
	}

	// 「用这张继续」要用**右栏此刻正在显示的那张**。
	//
	// Job 默认喂它自己那份阶段A 产物（Result.BaseTexturePath），但用户可以先点历史候选里的
	// 第 2 张再继续 —— 那就成了「UI 显示 A、实际拿 B 去跑几何反演」，产出的 PBR 与看到的
	// 对不上，而且完全静默（面板上没有任何东西会因此变色）。这类"同一份数据两个入口行为
	// 不一致"正是要主动堵掉的。
	//
	// 判定规则在 FStageAHistory::ResolveStageBInput 里（含"焦点是阶段B 产物时不许覆盖"
	// 这条，否则再点一次就会拿 albedo 二次反演），并有 -TestPreviewFocus 守着。
	const FString JobBaseTexture = Job->GetResult().BaseTexturePath;
	const FString FeedPath = PreviewHistory.ResolveStageBInput(JobBaseTexture);

	if (FeedPath.IsEmpty())
	{
		UE_LOG(LogComfyTextureStudioEditor, Log,
			TEXT("阶段B 输入用本次阶段A 的产物：%s"),
			JobBaseTexture.IsEmpty() ? TEXT("(未就绪)") : *FPaths::GetCleanFilename(JobBaseTexture));
	}
	else
	{
		UE_LOG(LogComfyTextureStudioEditor, Log,
			TEXT("阶段B 输入改用预览聚焦的那张历史候选：%s"),
			*FPaths::GetCleanFilename(FeedPath));
	}

	Job->ContinueToStageB(FeedPath);
}

void SComfyTextureStudioPanel::RequestCancel()
{
	if (Job.IsValid())
	{
		Job->Cancel();
	}
}

void SComfyTextureStudioPanel::DiscardStageAResult()
{
	if (IsJobRunning())
	{
		return;
	}
	Job.Reset();
	ChannelPngPaths.Reset();
	ImportState = FComfyPanelImportState();
	LastResult = FComfyJobResult();
	PreviewHistory.ClearFocus();
	LastState = EComfyJobState::Idle;
	// 历史候选**不清**：那张图还在磁盘上，丢掉只是让用户找不回来。
	// 想恢复就点一下历史里的缩略图。
	StatusText = TEXT("已丢弃阶段 A 的结果。可以改参数重新生成。（那张图仍留在「历史候选」里，点一下就能找回）");
}

void SComfyTextureStudioPanel::RequestImport()
{
	if (!CanImport())
	{
		return;
	}

	FComfyImportRequest Request;
	FString Error;
	if (!BuildImportRequest(Request, Error))
	{
		ImportState = FComfyPanelImportState();
		ImportState.bRan = true;
		ImportState.bSuccess = false;
		ImportState.ErrorText = Error;
		ImportState.ReportText = FString::Printf(TEXT("导入未执行：\n%s"), *Error);
		return;
	}

	IComfyAssetImportProvider* Provider = FComfyImportRegistry::Get().GetProvider();
	if (!Provider)
	{
		ImportState = FComfyPanelImportState();
		ImportState.bRan = true;
		ImportState.ReportText = TEXT("导入服务未注册（Editor 模块没加载？）。");
		return;
	}

	LastState = EComfyJobState::Importing;
	StatusText = TEXT("正在导入贴图并创建材质…");

	const FComfyImportResult Result = Provider->ImportAndBuildMaterial(Request);

	ImportState.bRan = true;
	ImportState.bSuccess = Result.bSuccess;
	ImportState.ErrorText = Result.ErrorText;
	ImportState.Report = Result.Report;
	ImportState.ReportText = Result.Report.ToText();
	if (!Result.bSuccess && !Result.ErrorText.IsEmpty())
	{
		ImportState.ReportText += FString::Printf(TEXT("\n\n失败原因：%s"), *Result.ErrorText);
	}

	LastState = Result.bSuccess ? EComfyJobState::Done : EComfyJobState::Failed;
	StatusText = Result.bSuccess
		? FString::Printf(TEXT("导入完成：%s"), *Request.BaseName)
		: FString::Printf(TEXT("导入失败：%s"), *Result.ErrorText);

	// 落一份报告到任务目录，便于事后核对
	if (!LastResult.OutputDir.IsEmpty())
	{
		const FString ReportPath = FPaths::Combine(LastResult.OutputDir, TEXT("report_import.json"));
		FFileHelper::SaveStringToFile(Result.Report.ToJson(), *ReportPath);
	}

	if (Result.bSuccess)
	{
		FNotificationInfo Info(FText::Format(
			LOCTEXT("ImportOk", "ComfyTextureStudio：{0} 已导入，材质 {1} 已创建"),
			FText::FromString(Request.BaseName), FText::FromString(Request.MaterialName)));
		Info.ExpireDuration = 5.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

// --------------------------------------------------------------------------- //
// 服务探测（连接状态 + 枚举）
// --------------------------------------------------------------------------- //

void SComfyTextureStudioPanel::ProbeServer()
{
	if (!ProbeClient.IsValid())
	{
		ProbeClient = MakeUnique<FComfyHttpClient>();
	}
	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	if (Settings)
	{
		ProbeClient->Configure(Settings->ServerUrl, Settings->RequestTimeoutSeconds, Settings->VerboseHttpLog);
	}

	// 双保险：令牌机制已经保证客户端销毁后回调不会触发，但面板比客户端活得更久的
	// 情况（比如将来把 ProbeClient 改成共享持有）仍可能发生 —— Slate 的标准做法
	// 是让回调自己确认宿主 widget 还在。
	const TWeakPtr<SWidget> WeakSelf = AsShared();

	ProbeClient->GetObjectInfo([this, WeakSelf](bool bOk, const TSharedPtr<FJsonObject>& ObjectInfo, const FString& Error)
	{
		if (!WeakSelf.IsValid())
		{
			return; // 面板已关闭，结果丢弃
		}
		if (!bOk || !ObjectInfo.IsValid())
		{
			bServerReachable = false;
			ServerProbeText = Error.IsEmpty() ? TEXT("连接失败") : Error;
			return;
		}

		bServerReachable = true;
		ServerProbeText.Reset();

		// 用真实枚举覆盖候选列表（Q7：换 LoRA 的通道靠这个）
		CacheEnumFromObjectInfo(ObjectInfo, TEXT("LoraLoader"), TEXT("lora_name"), LoraOptions);
		if (LoraOptions.Num() > 0 && (LoraName.IsEmpty() || !LoraOptions.ContainsByPredicate(
			[this](const TSharedPtr<FString>& S) { return S.IsValid() && *S == LoraName; })))
		{
			LoraName = *LoraOptions[0];
		}

		CacheEnumFromObjectInfo(ObjectInfo, TEXT("KSampler"), TEXT("sampler_name"), SamplerOptions);
		CacheEnumFromObjectInfo(ObjectInfo, TEXT("KSampler"), TEXT("scheduler"), SchedulerOptions);

		// 这里的回退必须「只在当前值确实不在新列表里」时才发生，与上面 LoraName 一致。
		// 绝不能无条件取 [0]：ComfyUI 的 sampler_name 列表第一项是 euler、scheduler
		// 第一项是 normal/simple，于是「打开面板」这个动作就会把 manifest 的
		// dpmpp_2m/karras 悄悄换掉 —— 2026-10-13 用户那次跑出 euler/simple 就是这么来的。
		SelectedSampler = PickValidOption(SamplerOptions, SelectedSampler, SamplerDefault);
		SelectedScheduler = PickValidOption(SchedulerOptions, SelectedScheduler, SchedulerDefault);
	});
}

void SComfyTextureStudioPanel::CacheEnumFromObjectInfo(
	const TSharedPtr<FJsonObject>& ObjectInfo,
	const FString& NodeClass,
	const FString& InputName,
	TArray<TSharedPtr<FString>>& OutValues)
{
	if (!ObjectInfo.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* NodeObj = nullptr;
	if (!ObjectInfo->TryGetObjectField(NodeClass, NodeObj) || NodeObj == nullptr)
	{
		return;
	}
	const TSharedPtr<FJsonObject>* Input = nullptr;
	if (!(*NodeObj)->TryGetObjectField(TEXT("input"), Input) || Input == nullptr)
	{
		return;
	}
	const TSharedPtr<FJsonObject>* Required = nullptr;
	if (!(*Input)->TryGetObjectField(TEXT("required"), Required) || Required == nullptr)
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Spec = nullptr;
	if (!(*Required)->TryGetArrayField(InputName, Spec) || Spec == nullptr || Spec->Num() == 0)
	{
		return;
	}

	// COMBO 的形态是 [ [候选值...], {选项} ]
	const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
	if (!(*Spec)[0]->TryGetArray(Candidates) || Candidates == nullptr || Candidates->Num() == 0)
	{
		return;
	}

	TArray<TSharedPtr<FString>> Values;
	Values.Reserve(Candidates->Num());
	for (const TSharedPtr<FJsonValue>& V : *Candidates)
	{
		FString Text;
		if (V.IsValid() && V->TryGetString(Text) && !Text.IsEmpty())
		{
			Values.Add(MakeShared<FString>(MoveTemp(Text)));
		}
	}

	if (Values.Num() > 0)
	{
		OutValues = MoveTemp(Values);
	}
}

// --------------------------------------------------------------------------- //
// 任务回调
// --------------------------------------------------------------------------- //

void SComfyTextureStudioPanel::OnJobStateChanged(EComfyJobState NewState, const FString& Text)
{
	LastState = NewState;
	StatusText = Text;
}

void SComfyTextureStudioPanel::OnJobFinished(const FComfyJobResult& InResult)
{
	LastResult = InResult;

	if (InResult.bSuccess)
	{
		// 阶段 B 的 6 个产物路径按语义归位；预览默认切到 albedo
		for (const TPair<FString, FString>& Pair : InResult.ChannelPaths)
		{
			ChannelPngPaths.Add(Pair.Key, Pair.Value);
		}

		// 兜底：非向导流程（不暂停）时阶段A 产物没机会走 OnActiveTimer 那条分支
		PreviewHistory.Remember(InResult.BaseTexturePath);

		if (const FString* Albedo = InResult.ChannelPaths.Find(TEXT("albedo")))
		{
			PreviewHistory.Focus(*Albedo, /*bPinned=*/false);
		}
		else if (!InResult.BaseTexturePath.IsEmpty())
		{
			PreviewHistory.Focus(InResult.BaseTexturePath, /*bPinned=*/false);
		}

		LastState = EComfyJobState::Ready;
	}
	else if (InResult.Error.IsSet())
	{
		LastState = (InResult.Error.Kind == EComfyErrorKind::Cancelled)
			? EComfyJobState::Cancelled
			: EComfyJobState::Failed;

		const FNotificationInfo Info(FText::Format(
			LOCTEXT("JobFailed", "ComfyTextureStudio：{0}"),
			FText::FromString(InResult.Error.UserText())));
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

EActiveTimerReturnType SComfyTextureStudioPanel::OnActiveTimer(double InCurrentTime, float InDeltaTime)
{
	// 编辑器里 HTTP 由引擎驱动，所以 Tick 只需要推进状态机。
	if (Job.IsValid())
	{
		Job->Tick(InDeltaTime);

		// 阶段 A 刚下载完的那张图进「历史候选」，顺便把预览切过去
		if (Job->IsAwaitingStageAReview() && !LastResult.BaseTexturePath.IsEmpty())
		{
			PreviewHistory.Remember(LastResult.BaseTexturePath);

			// ★ 新结果要**无条件**把预览带过去（除非用户手动钉住了某个候选 —— 那时
			// 不该把画面抢走）。判定在 FollowNewResult 里。
			//
			// 这里以前写的是 `if (FocusedPreviewPath.IsEmpty())` —— 那只在"第一次出产物"
			// 时成立，之后每次重新生成，右栏都停在第一张。用户 2026-10-14 报的
			// 「新贴图不会在预览栏显示，仍是旧的图片」就是这个：文件是新的、缓存里也有，
			// 但焦点路径根本没被改过。回归测试：-TestPreviewFocus。
			PreviewHistory.FollowNewResult(LastResult.BaseTexturePath);
		}
		// 阶段 A 的产物路径在 Job 里是逐步写的，这里同步一份到面板
		if (!Job->GetResult().BaseTexturePath.IsEmpty())
		{
			LastResult.BaseTexturePath = Job->GetResult().BaseTexturePath;
		}
	}

	return EActiveTimerReturnType::Continue;
}

// --------------------------------------------------------------------------- //
// 状态查询与门控（严格照 PRD §7.3 的表）
// --------------------------------------------------------------------------- //

bool SComfyTextureStudioPanel::IsJobRunning() const
{
	if (!Job.IsValid())
	{
		return false;
	}
	switch (LastState)
	{
	case EComfyJobState::Preflight:
	case EComfyJobState::Submitting:
	case EComfyJobState::Running:
	case EComfyJobState::Downloading:
	case EComfyJobState::Importing:
		return true;
	default:
		return false;
	}
}

bool SComfyTextureStudioPanel::CanGenerate() const
{
	if (IsJobRunning() || !bServerReachable)
	{
		return false;
	}
	// 阶段 B 还没跑（Ready 之后可以重新开始：重新生成一张新的基础贴图）
	return true;
}

bool SComfyTextureStudioPanel::CanReviewStageA() const
{
	return LastState == EComfyJobState::StageAReady && !IsJobRunning();
}

bool SComfyTextureStudioPanel::CanContinueToStageB() const
{
	// 「用这张继续」与「② 生成 PBR 全套」都走这里
	return CanReviewStageA();
}

bool SComfyTextureStudioPanel::CanDiscardStageA() const
{
	return LastState == EComfyJobState::StageAReady;
}

bool SComfyTextureStudioPanel::CanImport() const
{
	if (IsJobRunning() || !bServerReachable)
	{
		return false;
	}
	// 必须跑到 Ready（阶段 B 的 6 个产物齐备）才能导入
	if (LastState != EComfyJobState::Ready && LastState != EComfyJobState::Done)
	{
		return false;
	}
	if (ChannelPngPaths.Num() == 0)
	{
		return false;
	}
	return SpecLoadError.IsEmpty();
}

FText SComfyTextureStudioPanel::GetConnectionText() const
{
	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	const FString Url = Settings ? Settings->ServerUrl : TEXT("http://127.0.0.1:8000");

	if (bServerReachable)
	{
		return FText::Format(LOCTEXT("Connected", "● 已连接 {0}"), FText::FromString(Url));
	}
	return FText::Format(LOCTEXT("Disconnected", "● 无法连接 {0}"), FText::FromString(Url));
}

FSlateColor SComfyTextureStudioPanel::GetConnectionColor() const
{
	return bServerReachable ? ColorOk : ColorErr;
}

FText SComfyTextureStudioPanel::GetStatusText() const
{
	if (IsJobRunning())
	{
		const double Elapsed = Job.IsValid()
			? (FPlatformTime::Seconds() - JobStartTime)
			: 0.0;
		return FText::Format(LOCTEXT("RunningStatus", "{0} · 已耗时 {1}s"),
			FText::FromString(StatusText), FText::AsNumber(FMath::RoundToInt(Elapsed)));
	}
	if (!StatusText.IsEmpty())
	{
		return FText::FromString(StatusText);
	}
	if (!bServerReachable)
	{
		return LOCTEXT("StatusOffline", "无法连接 ComfyUI。请确认已启动，并检查端口是否为 8000（不是 8188）。");
	}
	return LOCTEXT("StatusIdle", "就绪");
}

FSlateColor SComfyTextureStudioPanel::GetStatusColor() const
{
	// 只有真正的异常状态才着色：正常态用主题默认色，避免在一个满是按钮的面板里
	// 到处跳颜色（特别是深色/浅色主题切换时，硬编码色很容易适配失败）。
	switch (LastState)
	{
	case EComfyJobState::Failed:
		return ColorErr;
	case EComfyJobState::Cancelled:
	case EComfyJobState::StageAReady:
		return ColorWarn;
	case EComfyJobState::Done:
		return ColorOk;
	default:
		return FSlateColor::UseForeground();
	}
}

TOptional<float> SComfyTextureStudioPanel::GetProgressFraction() const
{
	// 没有节点级进度（那需要 WebSocket，留到 M3），所以用不确定态进度条。
	return IsJobRunning() ? TOptional<float>() : TOptional<float>(LastState == EComfyJobState::Done ? 1.0f : 0.0f);
}

FText SComfyTextureStudioPanel::GetStageAInfoText() const
{
	if (LastResult.BaseTexturePath.IsEmpty())
	{
		return LOCTEXT("NoPreview", "本次尚无产物 · 右栏显示的是上一次的结果");
	}
	return FText::Format(LOCTEXT("PreviewInfo", "{0}×{1} · {2} steps · seed {3} · {4}"),
		FText::AsNumber(Width), FText::AsNumber(Height),
		FText::AsNumber(Steps), FText::AsNumber(Seed),
		FText::FromString(FPaths::GetCleanFilename(LastResult.BaseTexturePath)));
}

FText SComfyTextureStudioPanel::GetImportHintText() const
{
	if (!SpecLoadError.IsEmpty())
	{
		return FText::Format(LOCTEXT("ImportBlocked", "⚠ 通道规格装载失败，导入不可用：{0}"),
			FText::FromString(SpecLoadError));
	}
	if (LastState != EComfyJobState::Ready && LastState != EComfyJobState::Done
		&& !IsJobRunning())
	{
		return LOCTEXT("ImportNeedStageB", "需要先跑完 ② 生成 PBR 全套，才能导入。");
	}
	if (IsJobRunning())
	{
		return LOCTEXT("ImportBusy", "任务进行中…");
	}
	return FText::GetEmpty();
}

FText SComfyTextureStudioPanel::GetResolvedNamePreview() const
{
	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	if (!Settings)
	{
		return FText::GetEmpty();
	}

	TArray<FString> Suffixes;
	if (bUseHeightParallax && ChannelSpecs.Contains(TEXT("height")))
	{
		Suffixes.Add(ChannelSpecs[TEXT("height")].Suffix);
	}
	for (const FString& Semantic : ComfyChannels::Deliverable())
	{
		if (Semantic == TEXT("height"))
		{
			continue;
		}
		if (const FComfyOutputAssetSpec* Spec = ChannelSpecs.Find(Semantic))
		{
			Suffixes.Add(Spec->Suffix);
		}
	}
	if (bImportOrphanMaps)
	{
		static const TArray<FString> Orphans = { TEXT("ao"), TEXT("roughness") };
		for (const FString& Semantic : Orphans)
		{
			if (const FComfyOutputAssetSpec* Spec = ChannelSpecs.Find(Semantic))
			{
				Suffixes.Add(Spec->Suffix);
			}
		}
	}

	const FComfyResolvedAssetNaming Resolved = FComfyAssetNaming::Resolve(
		*Settings, ManualName, TemplateMaterial, TemplateGrain, bUseTemplateNaming, Suffixes);

	if (!Resolved.bSuccess)
	{
		return FText::FromString(FString::Printf(TEXT("⚠ %s"), *Resolved.Error));
	}
	return FText::FromString(FString::Printf(TEXT("%s    （材质 %s）"),
		*Resolved.DestinationPath, *Resolved.MaterialName));
}

// --------------------------------------------------------------------------- //
// 缩略图
// --------------------------------------------------------------------------- //

TSharedPtr<FComfyPreviewTexture> SComfyTextureStudioPanel::GetPreviewTexture(const FString& PngPath)
{
	if (PngPath.IsEmpty())
	{
		return nullptr;
	}

	if (TSharedPtr<FComfyPreviewTexture>* Found = PreviewCache.Find(PngPath))
	{
		return *Found;   // 可能为 nullptr：之前加载失败过，不再每帧重读磁盘
	}

	TSharedPtr<FComfyPreviewTexture> Loaded = FComfyPreviewTexture::LoadFromPng(PngPath);
	PreviewCache.Add(PngPath, Loaded);
	PreviewLoadOrder.Add(PngPath);
	TrimPreviewCache();
	return Loaded;
}

void SComfyTextureStudioPanel::TrimPreviewCache()
{
	// 只淘汰「文件仍在磁盘上」的条目。
	//
	// **不能无条件按最旧淘汰**：导入完成后 stage_b 的中转 PNG 会被删掉
	// （Settings::PreserveDownloadedPng 默认 false），那些条目一旦被淘汰就再也加载不回来 ——
	// 缩略图会变成空白，正是 M2.3 修过的那类故障换个入口重现。
	// 所以「不可重建」的条目宁可留着占内存。
	//
	// Attempts 限一圈，避免"全都不能淘汰"时在这里绕不出来。
	int32 Attempts = PreviewLoadOrder.Num();
	while (PreviewCache.Num() > MaxPreviewCache && Attempts-- > 0 && PreviewLoadOrder.Num() > 0)
	{
		const FString Oldest = PreviewLoadOrder[0];
		PreviewLoadOrder.RemoveAt(0);

		if (Oldest == PreviewHistory.GetFocused() || !FPaths::FileExists(Oldest))
		{
			PreviewLoadOrder.Add(Oldest);   // 不能淘汰 → 放回队尾，下次再说
			continue;
		}

		PreviewCache.Remove(Oldest);
	}
}

// --------------------------------------------------------------------------- //
// 预览焦点
// --------------------------------------------------------------------------- //

FText SComfyTextureStudioPanel::GetFocusedPreviewText() const
{
	const FString& Focused = PreviewHistory.GetFocused();
	if (Focused.IsEmpty())
	{
		return LOCTEXT("NoFocusedPreview", "当前预览：（空）");
	}

	const FString Name = FPaths::GetCleanFilename(Focused);
	const int32 HistoryIndex = PreviewHistory.IndexOf(Focused);
	if (HistoryIndex != INDEX_NONE)
	{
		return FText::Format(LOCTEXT("FocusedPreviewCandidate", "当前预览：{0}  ·  阶段A 候选 #{1}"),
			FText::FromString(Name), FText::AsNumber(HistoryIndex + 1));
	}
	return FText::Format(LOCTEXT("FocusedPreviewChannel", "当前预览：{0}  ·  阶段B 产物"),
		FText::FromString(Name));
}

const FSlateBrush* SComfyTextureStudioPanel::GetPreviewBrush(const FString& PngPath)
{
	const TSharedPtr<FComfyPreviewTexture> Preview = GetPreviewTexture(PngPath);
	return Preview.IsValid() ? Preview->GetBrush() : nullptr;
}

void SComfyTextureStudioPanel::ReleasePreviewTextures()
{
	// FComfyPreviewTexture 自己负责 RemoveFromRoot。**注意这里的 Reset 只是让面板
	// 不再持有**：如果有查看器窗口还开着，它持有同一份资源的引用，贴图不会被释放
	// （这正是把缓存从「裸 UTexture2D + 裸 brush」换成它要换来的性质）。
	PreviewCache.Reset();
	PreviewLoadOrder.Reset();
}

void SComfyTextureStudioPanel::OpenImageViewer(const FString& PngPath, const FString& Title, bool bStartTiled)
{
	if (PngPath.IsEmpty())
	{
		return;
	}

	// 无头环境（commandlet 里跑 -CtsTestPanel）没有 Slate —— 静默跳过，别崩
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogComfyTextureStudioEditor, Warning,
			TEXT("查看器跳过：Slate 未初始化（无头模式）。"));
		return;
	}

	// 走面板的缓存（与缩略图共享同一个 FComfyPreviewTexture，靠引用计数保命）。
	//
	// **这里不能重新读盘。** 导入完成后中转 PNG 会被删掉
	// （Settings::PreserveDownloadedPng 默认 false），而面板上的缩略图早在图还在时
	// 就加载好了 —— 于是现象变成「缩略图分明看得见，点开却没反应」。
	// 2026-10-13 用户报的「关掉预览窗口后再点打不开」就是这个。
	const TSharedPtr<FComfyPreviewTexture> Preview = GetPreviewTexture(PngPath);
	if (!Preview.IsValid())
	{
		UE_LOG(LogComfyTextureStudioEditor, Warning,
			TEXT("查看器打开失败：预览资源不可用（文件已不在磁盘，内存里也没有副本）：%s"), *PngPath);

		// 明确告诉用户为什么，别静默失败 —— 否则和"点了没反应"没区别
		FNotificationInfo Info(FText::Format(
			LOCTEXT("ViewerFailed",
				"打不开预览：{0}\n该中转 PNG 已不在磁盘上（导入后会被清理），且本次会话没有它的内存副本。"),
			FText::FromString(FPaths::GetCleanFilename(PngPath))));
		Info.ExpireDuration = 6.0f;
		Info.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> Item =
			FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return;
	}

	const FVector2D ImageSize = Preview->GetImageSize();
	const FString HeaderText = FString::Printf(TEXT("%s   %d × %d"),
		*Title,
		FMath::RoundToInt(ImageSize.X),
		FMath::RoundToInt(ImageSize.Y));

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(FString::Printf(TEXT("%s · %s"),
			*Title, *FPaths::GetCleanFilename(PngPath))))
		.ClientSize(FVector2D(920.0, 780.0))
		.SupportsMinimize(true)
		.SupportsMaximize(true)
		.SizingRule(ESizingRule::UserSized)
		[
			SNew(SComfyImageViewer)
			.Preview(Preview)
			.Title(FText::FromString(HeaderText))
			.StartTiled(bStartTiled)
		];

	// 层叠摆放：连开几个窗口对比贴图时，不会完全叠在一起
	static int32 CascadeIndex = 0;
	const FVector2D Origin(160.0, 130.0);
	const FVector2D Cascade(28.0, 24.0);
	Window->MoveWindowTo(Origin + Cascade * static_cast<double>(CascadeIndex % 10));
	++CascadeIndex;

	FSlateApplication::Get().AddWindow(Window);
	Window->BringToFront();
}

// --------------------------------------------------------------------------- //
// 通道路径与导入请求
// --------------------------------------------------------------------------- //

FString SComfyTextureStudioPanel::GetChannelPngPath(const FString& Semantic) const
{
	const FString* Found = ChannelPngPaths.Find(Semantic);
	return Found ? *Found : FString();
}

bool SComfyTextureStudioPanel::BuildImportRequest(FComfyImportRequest& OutRequest, FString& OutError)
{
	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	if (!Settings)
	{
		OutError = TEXT("读不到插件设置。");
		return false;
	}
	if (ChannelSpecs.Num() == 0)
	{
		OutError = SpecLoadError.IsEmpty() ? TEXT("通道规格为空。") : SpecLoadError;
		return false;
	}

	// 要导入哪些通道（按当前开关）——命名解析需要它们的后缀来做冲突检测
	TArray<FString> Suffixes = FComfyTextureImportService::GetSuffixesToImport(
		ChannelSpecs, bUseHeightParallax, bImportOrphanMaps);

	const FComfyResolvedAssetNaming Resolved = FComfyAssetNaming::Resolve(
		*Settings, ManualName, TemplateMaterial, TemplateGrain, bUseTemplateNaming, Suffixes);

	if (!Resolved.bSuccess)
	{
		OutError = Resolved.Error;
		return false;
	}

	OutRequest.BaseName = Resolved.BaseName;
	OutRequest.DestinationPath = Resolved.DestinationPath;
	OutRequest.MaterialName = Resolved.MaterialName;
	OutRequest.MaterialInstanceName = Resolved.MaterialInstanceName;
	OutRequest.TextureNames = Resolved.TextureNames;
	OutRequest.bFlipGreenChannel = bFlipGreenChannel;
	OutRequest.bImportHeight = bUseHeightParallax;
	OutRequest.bImportOrphanMaps = bImportOrphanMaps;
	OutRequest.bCreateMaterialInstance = bCreateMaterialInstance;
	OutRequest.BumpDepth = BumpDepth;
	OutRequest.bDeleteSourcePng = !Settings->PreserveDownloadedPng;

	// 6 个通道的 PNG（服务自己按开关过滤，这里给全）
	for (const TPair<FString, FString>& Pair : ChannelPngPaths)
	{
		OutRequest.ChannelPngPaths.Add(Pair.Key, Pair.Value);
	}

	if (OutRequest.ChannelPngPaths.Num() == 0)
	{
		OutError = TEXT("没有可用的产物路径 —— 请先跑完 ② 生成 PBR 全套。");
		return false;
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
