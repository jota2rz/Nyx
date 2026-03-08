// Copyright Nyx MMO Project. All Rights Reserved.

using UnrealBuildTool;

public class NyxDSTMTransport : ModuleRules
{
	public NyxDSTMTransport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// MultiServer beacon infrastructure (AMultiServerBeaconClient, UMultiServerNode)
			"MultiServerReplication",

			// AOnlineBeaconClient base class
			"OnlineSubsystemUtils"
		});
	}
}
