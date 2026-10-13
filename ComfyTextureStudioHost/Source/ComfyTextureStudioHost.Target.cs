using UnrealBuildTool;
using System.Collections.Generic;

public class ComfyTextureStudioHostTarget : TargetRules
{
	public ComfyTextureStudioHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("ComfyTextureStudioHost");
	}
}
