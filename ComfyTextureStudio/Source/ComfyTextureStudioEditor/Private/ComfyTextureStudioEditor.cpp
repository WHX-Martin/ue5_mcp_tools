// ComfyTextureStudio · Editor 模块入口
//
// 三件事：
//   1. 实例化导入服务并注册到 FComfyImportRegistry（commandlet 靠它跑无 UI 的导入）
//   2. 注册 Nomad Tab（面板可以自由停靠 / 浮出）
//   3. 挂 Window 菜单与关卡编辑器工具栏入口（PRD §7.1）

#include "ComfyTextureStudioEditor.h"

#include "ComfyImportTypes.h"
#include "ComfyTextureImportService.h"
#include "ComfyTextureStudioSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SComfyTextureStudioPanel.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FComfyTextureStudioEditorModule"

DEFINE_LOG_CATEGORY(LogComfyTextureStudioEditor);

namespace
{
	/** Tab 的注册名（与菜单/工具栏入口共用）。 */
	static const FName ComfyTextureStudioTabName(TEXT("ComfyTextureStudio"));
}

void FComfyTextureStudioEditorModule::StartupModule()
{
	// ---- 1. 注册导入服务 ----
	// Runtime 模块只声明接口（ComfyImportTypes.h），实现在这里注入。
	// commandlet 通过 FComfyImportRegistry::Get().GetProvider() 拿到它跑无 UI 导入。
	ImportService = MakeUnique<FComfyTextureImportService>();
	FComfyImportRegistry::Get().SetProvider(ImportService.Get());

	// ---- 2. Nomad Tab ----
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ComfyTextureStudioTabName,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SComfyTextureStudioPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "Comfy Texture Studio"))
		.SetTooltipText(LOCTEXT("TabTooltip", "从提示词生成材质贴图与整套 PBR，并导入 UE 建材质"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	// ---- 3. 菜单与工具栏 ----
	RegisterMenus();

	// ---- 4. 可选：面板构造自检（见 SmokeTestPanel 的注释）----
	if (FParse::Param(FCommandLine::Get(), TEXT("CtsTestPanel")))
	{
		SmokeTestPanel();
	}

	UE_LOG(LogComfyTextureStudioEditor, Log, TEXT("ComfyTextureStudioEditor 已启动，导入服务已注册。"));
}

void FComfyTextureStudioEditorModule::SmokeTestPanel()
{
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogComfyTextureStudioEditor, Warning,
			TEXT("面板自检跳过：Slate 未初始化（无头模式下可能如此）。"));
		return;
	}

	UE_LOG(LogComfyTextureStudioEditor, Log, TEXT("════ M2 · 面板构造自检 ════"));

	// 完整走一遍 Construct（布局、manifest 装载、缩略图缓存初始化、ticker 注册）
	TSharedPtr<SWidget> Panel = SNew(SComfyTextureStudioPanel);
	UE_LOG(LogComfyTextureStudioEditor, Log, TEXT("  ✓ SComfyTextureStudioPanel 构造成功"));

	// 立刻销毁：析构路径同样要能安全走完（ticker 注销、transient 贴图出根集合）
	Panel.Reset();
	UE_LOG(LogComfyTextureStudioEditor, Log, TEXT("  ✓ 析构成功（ticker 已注销、贴图已出根）"));
	UE_LOG(LogComfyTextureStudioEditor, Log, TEXT("  面板自检通过"));
}

void FComfyTextureStudioEditorModule::ShutdownModule()
{
	// 先摘掉提供者，再销毁实现 —— 否则注册表会短暂持有野指针
	FComfyImportRegistry::Get().SetProvider(nullptr);
	ImportService.Reset();

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ComfyTextureStudioTabName);
	}

	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::UnregisterOwner(this);
	}
}

void FComfyTextureStudioEditorModule::RegisterMenus()
{
	// 用 owner 作用域包起来，ShutdownModule 里 UnRegisterOwner 就能一次摘干净
	FToolMenuOwnerScoped OwnerScoped(this);

	const auto OpenPanel = []()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(ComfyTextureStudioTabName);
	};

	// ---- Window 菜单 ----
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("WindowLayout"));
		Section.AddMenuEntry(
			TEXT("OpenComfyTextureStudio"),
			LOCTEXT("MenuEntry", "Comfy Texture Studio"),
			LOCTEXT("MenuEntryTip", "打开提示词 → PBR 材质 → 导入 UE 的面板"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda(OpenPanel)));
	}

	// ---- 关卡编辑器工具栏 ----
	{
		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
		FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("ComfyTextureStudio"));
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			TEXT("ComfyTextureStudioButton"),
			FUIAction(FExecuteAction::CreateLambda(OpenPanel)),
			LOCTEXT("ToolbarButton", "Comfy Texture Studio"),
			LOCTEXT("ToolbarButtonTip", "从提示词生成 PBR 材质贴图并导入 UE"),
			FSlateIcon()));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FComfyTextureStudioEditorModule, ComfyTextureStudioEditor)
