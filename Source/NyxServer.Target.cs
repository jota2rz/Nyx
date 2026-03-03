// Copyright Nyx MMO Project. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class NyxServerTarget : TargetRules
{
	public NyxServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("Nyx");
	}
}
