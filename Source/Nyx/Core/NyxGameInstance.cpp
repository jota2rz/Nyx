// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameInstance.h"
#include "Nyx/Nyx.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Core/NyxGameMode.h"
#include "Nyx/World/NyxEntityManager.h"
#include "Nyx/Player/NyxPlayerPawn.h"
#include "Nyx/Player/NyxMovementComponent.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"

void UNyxGameInstance::Init()
{
	Super::Init();
	UE_LOG(LogNyx, Log, TEXT("NyxGameInstance initialized"));
	UE_LOG(LogNyx, Log, TEXT("  SpacetimeDB Host: %s"), *SpacetimeDBHost);
	UE_LOG(LogNyx, Log, TEXT("  Database: %s"), *SpacetimeDBDatabaseName);

	// Bind login callback once
	UNyxAuthSubsystem* AuthSub = GetSubsystem<UNyxAuthSubsystem>();
	if (AuthSub)
	{
		AuthSub->OnLoginCompleteBP.AddDynamic(this, &UNyxGameInstance::OnLoginComplete);
	}

	RegisterConsoleCommands();
}

void UNyxGameInstance::Shutdown()
{
	UnregisterConsoleCommands();
	UE_LOG(LogNyx, Log, TEXT("NyxGameInstance shutting down"));
	Super::Shutdown();
}

void UNyxGameInstance::StartGame(bool bUseMock)
{
	UE_LOG(LogNyx, Log, TEXT("Starting game (mock=%s)"), bUseMock ? TEXT("true") : TEXT("false"));

	UNyxAuthSubsystem* AuthSub = GetSubsystem<UNyxAuthSubsystem>();
	if (!AuthSub)
	{
		UE_LOG(LogNyx, Error, TEXT("NyxAuthSubsystem not available!"));
		return;
	}

	// If already authenticated, logout first so we can re-login
	if (AuthSub->GetAuthState() != ENyxAuthState::NotAuthenticated)
	{
		UE_LOG(LogNyx, Log, TEXT("Logging out before re-starting game..."));
		AuthSub->Logout();
	}

	// Start the auth flow
	AuthSub->Login(EOSLoginType, SpacetimeDBHost, SpacetimeDBDatabaseName, bUseMock);
}

void UNyxGameInstance::OnLoginComplete(bool bSuccess, const FString& ErrorMessage)
{
	if (bSuccess)
	{
		UE_LOG(LogNyx, Log, TEXT("Login successful! Player is fully authenticated."));

		// At this point:
		// - EOS auth is done (or mocked)
		// - SpacetimeDB is connected
		// - The player's identity is linked
		//
		// Next steps (driven by game flow / level blueprint / game mode):
		// 1. NyxEntityManager->StartListening() to begin receiving entity events
		// 2. NyxNetworkSubsystem->UpdateSpatialSubscription() to subscribe to nearby data
		// 3. NyxNetworkSubsystem->GetDatabaseInterface()->CallCreatePlayer() to join the world
	}
	else
	{
		UE_LOG(LogNyx, Error, TEXT("Login failed: %s"), *ErrorMessage);
		// TODO: Show error UI, offer retry
	}
}

void UNyxGameInstance::RegisterConsoleCommands()
{
	// Nyx.Connect [Host] [Database] — connect directly to SpacetimeDB (bypasses auth)
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Connect"),
		TEXT("Connect directly to SpacetimeDB. Usage: Nyx.Connect [Host] [Database]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const FString Host = Args.Num() > 0 ? Args[0] : SpacetimeDBHost;
			const FString DB = Args.Num() > 1 ? Args[1] : SpacetimeDBDatabaseName;

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Connect %s %s"), *Host, *DB);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub)
			{
				NetworkSub->ConnectToServer(Host, DB, /*bUseMock=*/false);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("NyxNetworkSubsystem not available!"));
			}
		}),
		ECVF_Default));

	// Nyx.ConnectMock — connect with mock backend
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.ConnectMock"),
		TEXT("Connect with mock backend (no SpacetimeDB needed)"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.ConnectMock"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub)
			{
				NetworkSub->ConnectToServer(SpacetimeDBHost, SpacetimeDBDatabaseName, /*bUseMock=*/true);
			}
		}),
		ECVF_Default));

	// Nyx.Disconnect — disconnect from server
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Disconnect"),
		TEXT("Disconnect from SpacetimeDB"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Disconnect"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub)
			{
				NetworkSub->DisconnectFromServer();
			}
		}),
		ECVF_Default));

	// Nyx.StartGame [mock] — full auth flow
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.StartGame"),
		TEXT("Start full game flow (EOS + SpacetimeDB). Pass 'mock' for mock mode."),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			bool bMock = Args.Num() > 0 && Args[0].ToLower() == TEXT("mock");
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.StartGame (mock=%s)"), bMock ? TEXT("true") : TEXT("false"));
			StartGame(bMock);
		}),
		ECVF_Default));

	// Nyx.Seed <count> — seed world entities for spatial stress testing
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Seed"),
		TEXT("Seed world entities for testing. Usage: Nyx.Seed <count>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			uint32 Count = 100;
			if (Args.Num() > 0)
			{
				Count = FCString::Atoi(*Args[0]);
			}
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Seed %d"), Count);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->SeedEntities(Count);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.ClearEntities — remove all world entities
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.ClearEntities"),
		TEXT("Remove all world entities from the database"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.ClearEntities"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->ClearEntities();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.Move <x> <y> <z> — teleport local player and update spatial subscription
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Move"),
		TEXT("Move player to position and update spatial subscription. Usage: Nyx.Move <x> <y> <z>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogNyx, Warning, TEXT("Usage: Nyx.Move <x> <y> <z>"));
				return;
			}

			const double X = FCString::Atod(*Args[0]);
			const double Y = FCString::Atod(*Args[1]);
			const double Z = FCString::Atod(*Args[2]);

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Move %.0f %.0f %.0f"), X, Y, Z);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				// Update server position (seq=0 for debug teleport)
				NetworkSub->GetSpacetimeDBConnection()->Reducers->MovePlayer(X, Y, Z, 0.0f, 0);
				// Update spatial subscription for new position
				NetworkSub->UpdateSpatialSubscription(FVector(X, Y, Z));
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.Walk <dx> <dy> <dz> — relative movement for testing prediction
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Walk"),
		TEXT("Apply relative movement to local player pawn. Usage: Nyx.Walk <dx> <dy> <dz>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogNyx, Warning, TEXT("Usage: Nyx.Walk <dx> <dy> [dz]"));
				return;
			}

			const float DX = FCString::Atof(*Args[0]);
			const float DY = FCString::Atof(*Args[1]);
			const float DZ = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.f;

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Walk %.0f %.0f %.0f"), DX, DY, DZ);

			// Find the local player pawn's movement component and add input
			UWorld* World = GetWorld();
			if (!World) return;

			UNyxEntityManager* EntityMgr = World->GetSubsystem<UNyxEntityManager>();
			if (EntityMgr && EntityMgr->GetLocalPlayerPawn())
			{
				if (UNyxMovementComponent* MoveComp = EntityMgr->GetLocalPlayerPawn()->GetNyxMovement())
				{
					FVector Dir(DX, DY, DZ);
					Dir.Normalize();
					MoveComp->AddMovementInput(Dir);
					UE_LOG(LogNyx, Log, TEXT("Walk input applied: (%.1f, %.1f, %.1f)"), Dir.X, Dir.Y, Dir.Z);
				}
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("No local player pawn available! Call Nyx.EnterWorld first."));
			}
		}),
		ECVF_Default));

	// Nyx.EnterWorld — create player and start entity listening (bypasses auth flow)
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.EnterWorld"),
		TEXT("Create player and start entity listening (use after Nyx.Connect)"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.EnterWorld"));

			UWorld* World = GetWorld();
			if (!World) return;

			ANyxGameMode* GM = Cast<ANyxGameMode>(World->GetAuthGameMode());
			if (GM)
			{
				GM->EnterWorld();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("GameMode is not ANyxGameMode!"));
			}
		}),
		ECVF_Default));

	UE_LOG(LogNyx, Log, TEXT("Registered console commands: Nyx.Connect, Nyx.ConnectMock, Nyx.Disconnect, Nyx.StartGame, Nyx.Seed, Nyx.ClearEntities, Nyx.Move, Nyx.EnterWorld"));
}

void UNyxGameInstance::UnregisterConsoleCommands()
{
	for (IConsoleObject* Cmd : ConsoleCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Cmd);
	}
	ConsoleCommands.Empty();
}
