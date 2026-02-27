// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameInstance.h"
#include "Nyx/Nyx.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"

void UNyxGameInstance::Init()
{
	Super::Init();
	UE_LOG(LogNyx, Log, TEXT("NyxGameInstance initialized"));
	UE_LOG(LogNyx, Log, TEXT("  SpacetimeDB Host: %s"), *SpacetimeDBHost);
	UE_LOG(LogNyx, Log, TEXT("  Database: %s"), *SpacetimeDBDatabaseName);

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

	// Bind to login completion
	AuthSub->OnLoginCompleteBP.AddDynamic(this, &UNyxGameInstance::OnLoginComplete);

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

	UE_LOG(LogNyx, Log, TEXT("Registered console commands: Nyx.Connect, Nyx.ConnectMock, Nyx.Disconnect, Nyx.StartGame"));
}

void UNyxGameInstance::UnregisterConsoleCommands()
{
	for (IConsoleObject* Cmd : ConsoleCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Cmd);
	}
	ConsoleCommands.Empty();
}
