// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxAuthSubsystem.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Networking/NyxDatabaseInterface.h"

void UNyxAuthSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Ensure NyxNetworkSubsystem is initialized first
	Collection.InitializeDependency<UNyxNetworkSubsystem>();

	UE_LOG(LogNyxAuth, Log, TEXT("NyxAuthSubsystem initialized"));
}

void UNyxAuthSubsystem::Deinitialize()
{
	Logout();
	UE_LOG(LogNyxAuth, Log, TEXT("NyxAuthSubsystem deinitialized"));
	Super::Deinitialize();
}

void UNyxAuthSubsystem::Login(const FString& LoginType, const FString& SpacetimeDBHost, const FString& DatabaseName, bool bUseMockDB)
{
	if (AuthState != ENyxAuthState::NotAuthenticated && AuthState != ENyxAuthState::Failed)
	{
		UE_LOG(LogNyxAuth, Warning, TEXT("Login called while already in auth state %d"), static_cast<int32>(AuthState));
		return;
	}

	// Store SpacetimeDB params for Phase 2
	PendingSpacetimeDBHost = SpacetimeDBHost;
	PendingDatabaseName = DatabaseName;
	bPendingUseMock = bUseMockDB;

	if (bUseMockDB)
	{
		// Skip EOS login in mock mode — go straight to SpacetimeDB connection
		UE_LOG(LogNyxAuth, Log, TEXT("Mock mode: skipping EOS login"));
		CachedEOSIdToken = TEXT("mock_token");
		CachedEOSProductUserId = TEXT("mock_puid");
		CachedDisplayName = TEXT("MockPlayer");
		SetAuthState(ENyxAuthState::EOSAuthenticated);
		StartSpacetimeDBAuth();
	}
	else
	{
		StartEOSLogin(LoginType);
	}
}

void UNyxAuthSubsystem::Logout()
{
	if (AuthState == ENyxAuthState::NotAuthenticated)
	{
		return;
	}

	// Disconnect SpacetimeDB
	UNyxNetworkSubsystem* NetworkSub = GetGameInstance()->GetSubsystem<UNyxNetworkSubsystem>();
	if (NetworkSub)
	{
		INyxDatabaseInterface* DbInterface = NetworkSub->GetDatabaseInterface();
		if (DbInterface)
		{
			DbInterface->OnConnectionStateChanged().Remove(ConnectionStateDelegateHandle);
			DbInterface->OnAuthComplete().Remove(AuthCompleteDelegateHandle);
		}
		NetworkSub->DisconnectFromServer();
	}

	// Clear cached data
	CachedEOSIdToken.Empty();
	CachedEOSProductUserId.Empty();
	CachedDisplayName.Empty();
	LocalIdentity = FNyxPlayerIdentity();

	SetAuthState(ENyxAuthState::NotAuthenticated);
}

void UNyxAuthSubsystem::StartEOSLogin(const FString& LoginType)
{
	UE_LOG(LogNyxAuth, Log, TEXT("Starting EOS login with type: %s"), *LoginType);
	SetAuthState(ENyxAuthState::AuthenticatingEOS);

	// TODO (Spike 4): Implement real EOS login via UE::Online::IOnlineServices.
	// Requires researching UE5.7 Online Services API signatures (FPlatformUserId,
	// IAuthPtr, FAuthLogin::Params, TOnlineResult, etc.) and adding correct module
	// dependencies to Nyx.Build.cs. For now, stub with placeholder values so the
	// rest of the auth pipeline can be tested end-to-end with mock connections.

	CachedEOSProductUserId = TEXT("eos_stub_puid");
	CachedDisplayName = TEXT("EOSPlayer_Stub");
	CachedEOSIdToken = TEXT("eos_stub_token");

	UE_LOG(LogNyxAuth, Warning, TEXT("EOS login STUBBED — using placeholder credentials. Implement in Spike 4."));
	OnEOSLoginSuccess();
}

void UNyxAuthSubsystem::OnEOSLoginSuccess()
{
	SetAuthState(ENyxAuthState::EOSAuthenticated);

	LocalIdentity.EOSProductUserId = CachedEOSProductUserId;
	LocalIdentity.DisplayName = CachedDisplayName;
	LocalIdentity.Platform = TEXT("Windows"); // TODO: Detect platform

	// Proceed to Phase 2
	StartSpacetimeDBAuth();
}

void UNyxAuthSubsystem::OnEOSLoginFailure(const FString& ErrorMessage)
{
	UE_LOG(LogNyxAuth, Error, TEXT("EOS Login failed: %s"), *ErrorMessage);
	SetAuthState(ENyxAuthState::Failed);
	OnLoginCompleteBP.Broadcast(false, ErrorMessage);
}

void UNyxAuthSubsystem::StartSpacetimeDBAuth()
{
	UE_LOG(LogNyxAuth, Log, TEXT("Phase 2: Connecting to SpacetimeDB at %s/%s"),
		*PendingSpacetimeDBHost, *PendingDatabaseName);
	SetAuthState(ENyxAuthState::ConnectingSpacetimeDB);

	UNyxNetworkSubsystem* NetworkSub = GetGameInstance()->GetSubsystem<UNyxNetworkSubsystem>();
	if (!NetworkSub)
	{
		OnEOSLoginFailure(TEXT("NyxNetworkSubsystem not available"));
		return;
	}

	// Connect to SpacetimeDB
	NetworkSub->ConnectToServer(PendingSpacetimeDBHost, PendingDatabaseName, bPendingUseMock);

	INyxDatabaseInterface* DbInterface = NetworkSub->GetDatabaseInterface();
	if (!DbInterface)
	{
		OnEOSLoginFailure(TEXT("Failed to create database connection"));
		return;
	}

	// Listen for connection state changes
	ConnectionStateDelegateHandle = DbInterface->OnConnectionStateChanged().AddUObject(
		this, &UNyxAuthSubsystem::OnSpacetimeDBConnected);

	// Listen for auth completion
	AuthCompleteDelegateHandle = DbInterface->OnAuthComplete().AddUObject(
		this, &UNyxAuthSubsystem::OnSpacetimeDBAuthComplete);

	// If already connected (mock connects synchronously), proceed immediately
	if (DbInterface->GetConnectionState() == ENyxConnectionState::Connected)
	{
		OnSpacetimeDBConnected(ENyxConnectionState::Connected);
	}
}

void UNyxAuthSubsystem::OnSpacetimeDBConnected(ENyxConnectionState NewState)
{
	if (NewState == ENyxConnectionState::Connected && AuthState == ENyxAuthState::ConnectingSpacetimeDB)
	{
		UE_LOG(LogNyxAuth, Log, TEXT("SpacetimeDB connected. Calling authenticate_with_eos reducer..."));

		UNyxNetworkSubsystem* NetworkSub = GetGameInstance()->GetSubsystem<UNyxNetworkSubsystem>();
		if (NetworkSub && NetworkSub->GetDatabaseInterface())
		{
			NetworkSub->GetDatabaseInterface()->CallAuthenticateWithEOS(
				CachedEOSIdToken,
				CachedEOSProductUserId,
				CachedDisplayName,
				LocalIdentity.Platform);
		}
	}
	else if (NewState == ENyxConnectionState::Failed)
	{
		OnEOSLoginFailure(TEXT("SpacetimeDB connection failed"));
	}
}

void UNyxAuthSubsystem::OnSpacetimeDBAuthComplete(bool bSuccess)
{
	if (bSuccess)
	{
		UE_LOG(LogNyxAuth, Log, TEXT("Authentication complete! EOS PUID: %s linked to SpacetimeDB identity"),
			*CachedEOSProductUserId);
		SetAuthState(ENyxAuthState::FullyAuthenticated);
		OnLoginCompleteBP.Broadcast(true, TEXT(""));
	}
	else
	{
		OnEOSLoginFailure(TEXT("SpacetimeDB EOS authentication reducer failed"));
	}
}

void UNyxAuthSubsystem::SetAuthState(ENyxAuthState NewState)
{
	if (AuthState != NewState)
	{
		UE_LOG(LogNyxAuth, Log, TEXT("Auth state: %d → %d"), static_cast<int32>(AuthState), static_cast<int32>(NewState));
		AuthState = NewState;
		OnAuthStateChangedBP.Broadcast(AuthState);
	}
}
