// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameMode.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxGameInstance.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Networking/NyxDatabaseInterface.h"
#include "Nyx/World/NyxEntityManager.h"

ANyxGameMode::ANyxGameMode()
{
	// No default pawn — we'll spawn the local player via entity manager
	DefaultPawnClass = nullptr;
}

void ANyxGameMode::StartPlay()
{
	Super::StartPlay();
	UE_LOG(LogNyx, Log, TEXT("NyxGameMode::StartPlay"));

	if (bAutoLoginMock)
	{
		UE_LOG(LogNyx, Log, TEXT("Auto-login with mock backend enabled"));

		UNyxGameInstance* GI = Cast<UNyxGameInstance>(GetGameInstance());
		if (GI)
		{
			GI->StartGame(/*bUseMock=*/true);
		}
		else
		{
			// GameInstance may not be NyxGameInstance if not configured yet
			// This is expected on first run before setting the GameInstance class
			UE_LOG(LogNyx, Warning,
				TEXT("GameInstance is not UNyxGameInstance. Set GameInstanceClass in Project Settings or DefaultEngine.ini."));
		}
	}
}

void ANyxGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UNyxEntityManager* EntityMgr = GetWorld()->GetSubsystem<UNyxEntityManager>();
	if (EntityMgr)
	{
		EntityMgr->StopListening();
	}

	Super::EndPlay(EndPlayReason);
}

void ANyxGameMode::EnterWorld()
{
	UE_LOG(LogNyx, Log, TEXT("EnterWorld: Starting entity listening and world subscription"));

	// Start entity manager
	UNyxEntityManager* EntityMgr = GetWorld()->GetSubsystem<UNyxEntityManager>();
	if (EntityMgr)
	{
		EntityMgr->StartListening();
	}

	// Subscribe to world data around spawn point
	UGameInstance* GI = GetGameInstance();
	if (GI)
	{
		UNyxNetworkSubsystem* NetworkSub = GI->GetSubsystem<UNyxNetworkSubsystem>();
		if (NetworkSub)
		{
			// Subscribe to spawn area (0, 0, 0 for now)
			NetworkSub->UpdateSpatialSubscription(FVector::ZeroVector);

			// Create the local player in the world
			INyxDatabaseInterface* Db = NetworkSub->GetDatabaseInterface();
			if (Db)
			{
				UNyxAuthSubsystem* AuthSub = GI->GetSubsystem<UNyxAuthSubsystem>();
				FString PlayerName = AuthSub ? AuthSub->GetLocalPlayerIdentity().DisplayName : TEXT("Player");
				Db->CallCreatePlayer(PlayerName);
			}
		}
	}
}

void ANyxGameMode::OnAuthStateChanged(ENyxAuthState NewState)
{
	if (NewState == ENyxAuthState::FullyAuthenticated)
	{
		EnterWorld();
	}
}
