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

		// Enable DSTM remote object handles for cross-server actor migration.
		// This is not supported in editor targets — only set it for packaged server builds.
		GlobalDefinitions.Add("UE_WITH_REMOTE_OBJECT_HANDLE=1");
	}
}
