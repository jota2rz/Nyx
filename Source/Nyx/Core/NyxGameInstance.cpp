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
}

void UNyxGameInstance::Shutdown()
{
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
