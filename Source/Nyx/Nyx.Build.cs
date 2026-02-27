// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Nyx : ModuleRules
{
	public Nyx(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Online Services (EOS) — uncomment when implementing Spike 4
			// "OnlineServicesInterface",

			// WebSocket support (for SpacetimeDB fallback / future use)
			"WebSockets",

			// JSON parsing (for EOS token inspection)
			"Json",
			"JsonUtilities"
		});

		// SpacetimeDB Unreal Plugin (Spike 1: confirmed working)
		PrivateDependencyModuleNames.Add("SpacetimeDbSdk");
	}
}
