using UnrealBuildTool;
using System.Collections.Generic;

public class ComfyTextureStudioHostEditorTarget : TargetRules
{
	public ComfyTextureStudioHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("ComfyTextureStudioHost");
	}
}
