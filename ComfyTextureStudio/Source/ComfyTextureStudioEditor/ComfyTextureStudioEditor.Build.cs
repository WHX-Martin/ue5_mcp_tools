// ComfyTextureStudio · Editor 模块构建规则（M2）
//
// 这个模块才允许碰编辑器专用 API：资产导入（UnrealEd）、材质图（MaterialEditor）、
// Slate。Runtime 模块保持干净，通过 ComfyImportTypes.h 里的接口与本模块解耦。
//
// 注意：虽然名叫 Editor，但它在 **UnrealEditor-Cmd 里也会被加载** —— commandlet
// 正是靠它跑导入与建材质做程序化验收（见 ComfyTextureStudioCommandlet 的 -Import）。

using UnrealBuildTool;

public class ComfyTextureStudioEditor : ModuleRules
{
	public ComfyTextureStudioEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ComfyTextureStudio", // Runtime：任务状态机、HTTP、导入接口层
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",         // UAssetImportTask / IAssetTools / UMaterialFactoryNew
			"MaterialEditor",   // UMaterialEditingLibrary（PRD §6.2）
			"AssetTools",       // FAssetToolsModule
			"AssetRegistry",    // 命名冲突检测（Q6：不静默覆盖）
			"ContentBrowser",   // 「在 Content Browser 中显示」
			"Slate",
			"SlateCore",
			"ImageWrapper",     // PNG 解码（缩略图）
			"RenderCore",
			"InputCore",
			"ToolMenus",        // Window 菜单 + 关卡编辑器工具栏入口（PRD §7.1）
			"LevelEditor",
			"EditorStyle",
			"ApplicationCore",  // FPlatformApplicationMisc::ClipboardCopy（复制导入报告）
			"Projects",
			"Json",
			"JsonUtilities",
		});
	}
}
