// Copyright Nyx MMO Project. All Rights Reserved.

#include "Nyx.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"

DEFINE_LOG_CATEGORY(LogNyx);
DEFINE_LOG_CATEGORY(LogNyxNet);
DEFINE_LOG_CATEGORY(LogNyxAuth);
DEFINE_LOG_CATEGORY(LogNyxWorld);

// ─── FNyxModule ────────────────────────────────────────────────────
// Custom module to register proxy net-driver configuration early enough.
// When the proxy runs as a server (-server), GameInstance::Init() handles the
// reconfiguration.  This module-level hook is a safety net that also clears
// RepGraph CDOs as early as possible via OnPostEngineInit.

class FNyxModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();

		if (FString(FCommandLine::Get()).Contains(TEXT("-ProxyGameServers=")))
		{
			FCoreDelegates::OnPostEngineInit.AddStatic(&FNyxModule::ConfigureProxyNetDrivers);
		}
	}

	static void ConfigureProxyNetDrivers()
	{
		UE_LOG(LogNyx, Log, TEXT("FNyxModule::ConfigureProxyNetDrivers — configuring PROXY net drivers"));

		if (!GEngine)
		{
			UE_LOG(LogNyx, Error, TEXT("  GEngine is NULL — cannot configure proxy"));
			return;
		}

		// Remove existing GameNetDriver entry (default IpNetDriver)
		GEngine->NetDriverDefinitions.RemoveAll([](const FNetDriverDefinition& Def) {
			return Def.DefName == FName(TEXT("GameNetDriver"));
		});

		// Add ProxyNetDriver as the GameNetDriver
		FNetDriverDefinition ProxyDef;
		ProxyDef.DefName = FName(TEXT("GameNetDriver"));
		ProxyDef.DriverClassName = FName(TEXT("/Script/MultiServerReplication.ProxyNetDriver"));
		ProxyDef.DriverClassNameFallback = FName(TEXT("/Script/MultiServerReplication.ProxyNetDriver"));
		GEngine->NetDriverDefinitions.Add(ProxyDef);

		// Add ProxyBackendNetDriver (created dynamically by the proxy)
		FNetDriverDefinition BackendDef;
		BackendDef.DefName = FName(TEXT("ProxyBackendNetDriver"));
		BackendDef.DriverClassName = FName(TEXT("/Script/MultiServerReplication.ProxyBackendNetDriver"));
		BackendDef.DriverClassNameFallback = FName(TEXT("/Script/MultiServerReplication.ProxyBackendNetDriver"));
		GEngine->NetDriverDefinitions.Add(BackendDef);

		UE_LOG(LogNyx, Log, TEXT("  NetDriverDefinitions reconfigured (%d entries):"), GEngine->NetDriverDefinitions.Num());
		for (const FNetDriverDefinition& Def : GEngine->NetDriverDefinitions)
		{
			UE_LOG(LogNyx, Log, TEXT("    DefName=%s  ClassName=%s"), *Def.DefName.ToString(), *Def.DriverClassName.ToString());
		}

		// Clear NyxReplicationGraph from the proxy's NetDriver CDOs.
		// The proxy forwards packets — it must NOT use a custom RepGraph that blocks
		// RelevantToOwner actors whose backend connections don't match frontend nodes.
		for (const TCHAR* DriverPath : {
			TEXT("/Script/MultiServerReplication.ProxyNetDriver"),
			TEXT("/Script/OnlineSubsystemUtils.IpNetDriver") })
		{
			UClass* DriverClass = FindObject<UClass>(nullptr, DriverPath);
			if (DriverClass)
			{
				UNetDriver* CDO = Cast<UNetDriver>(DriverClass->GetDefaultObject());
				if (CDO)
				{
					CDO->ReplicationDriverClassName.Empty();
					UE_LOG(LogNyx, Log, TEXT("  Cleared ReplicationDriverClassName on CDO of %s"), DriverPath);
				}
			}
		}
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FNyxModule, Nyx, "Nyx");
