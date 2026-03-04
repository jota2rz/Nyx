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
			// Online Services (EOS) — Spike 4
			"OnlineServicesInterface",
			"CoreOnline",

			// WebSocket support (for SpacetimeDB fallback / future use)
			"WebSockets",

			// JSON parsing (for EOS token inspection)
			"Json",
			"JsonUtilities"
		});

		// SpacetimeDB Unreal Plugin (Spike 1: confirmed working)
		PrivateDependencyModuleNames.Add("SpacetimeDbSdk");

		// MMO-scale spatial relevancy (Spike 14: ReplicationGraph)
		PrivateDependencyModuleNames.Add("ReplicationGraph");

		// MultiServer mesh for entity-sharded zones (Spike 14: Pattern A)
		PrivateDependencyModuleNames.Add("MultiServerReplication");
		PrivateDependencyModuleNames.Add("OnlineSubsystemUtils"); // AOnlineBeaconClient base class

		// GameplayDebugger (conditional — editor/debug builds only)
		SetupGameplayDebuggerSupport(Target);
	}
}
