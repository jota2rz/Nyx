// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxAuthSubsystem.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Networking/NyxDatabaseInterface.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"
#include "Online/OnlineServices.h"
#include "Online/Auth.h"
#include "Online/OnlineAsyncOpHandle.h"
#include "Online/OnlineResult.h"
#include "Online/OnlineError.h"

using namespace UE::Online;

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
		// Unbind BP connection state delegate
		NetworkSub->OnConnectionStateChangedBP.RemoveDynamic(
			this, &UNyxAuthSubsystem::OnSpacetimeDBConnectionStateChanged);

		// Unbind reducer callback if still bound
		UDbConnection* Conn = NetworkSub->GetSpacetimeDBConnection();
		if (Conn && Conn->Reducers)
		{
			Conn->Reducers->OnAuthenticateWithEos.RemoveDynamic(this, &UNyxAuthSubsystem::HandleAuthReducerResult);
		}

		// Unbind mock interface delegates
		INyxDatabaseInterface* DbInterface = NetworkSub->GetDatabaseInterface();
		if (DbInterface)
		{
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

FName UNyxAuthSubsystem::ResolveCredentialsType(const FString& LoginType)
{
	FString Lower = LoginType.ToLower();
	if (Lower == TEXT("accountportal")) return LoginCredentialsType::AccountPortal;
	if (Lower == TEXT("developer"))     return LoginCredentialsType::Developer;
	if (Lower == TEXT("persistentauth")) return LoginCredentialsType::PersistentAuth;
	if (Lower == TEXT("exchangecode"))  return LoginCredentialsType::ExchangeCode;
	if (Lower == TEXT("externalauth"))  return LoginCredentialsType::ExternalAuth;
	if (Lower == TEXT("password"))      return LoginCredentialsType::Password;
	if (Lower == TEXT("auto"))          return LoginCredentialsType::Auto;

	UE_LOG(LogNyxAuth, Warning, TEXT("Unknown login type '%s', defaulting to AccountPortal"), *LoginType);
	return LoginCredentialsType::AccountPortal;
}

void UNyxAuthSubsystem::StartEOSLogin(const FString& LoginType)
{
	UE_LOG(LogNyxAuth, Log, TEXT("Starting EOS login with type: %s"), *LoginType);
	SetAuthState(ENyxAuthState::AuthenticatingEOS);

	// Get the EOS Online Services
	TSharedPtr<IOnlineServices> Services = GetServices(EOnlineServices::Epic);
	if (!Services)
	{
		OnEOSLoginFailure(TEXT("Failed to get EOS Online Services. Check DefaultEngine.ini [OnlineServices] config."));
		return;
	}

	IAuthPtr Auth = Services->GetAuthInterface();
	if (!Auth)
	{
		OnEOSLoginFailure(TEXT("Failed to get EOS Auth interface"));
		return;
	}

	// Build login params
	FAuthLogin::Params LoginParams;
	LoginParams.PlatformUserId = FPlatformMisc::GetPlatformUserForUserIndex(0);
	LoginParams.CredentialsType = ResolveCredentialsType(LoginType);

	// For Developer login type, set the DevAuth tool address and credential name
	if (LoginParams.CredentialsType == LoginCredentialsType::Developer)
	{
		LoginParams.CredentialsId = TEXT("localhost:6300");
		LoginParams.CredentialsToken = TVariant<FString, FExternalAuthToken>(TInPlaceType<FString>(), TEXT("Nyx"));
	}

	UE_LOG(LogNyxAuth, Log, TEXT("Calling EOS Auth->Login with CredentialsType=%s"), *LoginParams.CredentialsType.ToString());

	// Perform async login
	Auth->Login(MoveTemp(LoginParams))
		.OnComplete(this, [this](const TOnlineResult<FAuthLogin>& Result)
		{
			if (Result.IsOk())
			{
				const FAuthLogin::Result& LoginResult = Result.GetOkValue();
				TSharedRef<FAccountInfo> AccountInfo = LoginResult.AccountInfo;

				// Extract display name from attributes
				if (const FSchemaVariant* NameAttr = AccountInfo->Attributes.Find(AccountAttributeData::DisplayName))
				{
					CachedDisplayName = NameAttr->GetString();
				}
				else
				{
					CachedDisplayName = TEXT("UnknownPlayer");
				}

				CachedAccountId = AccountInfo->AccountId;
				CachedEOSProductUserId = UE::Online::ToLogString(AccountInfo->AccountId);

				UE_LOG(LogNyxAuth, Log, TEXT("EOS Login succeeded! DisplayName=%s, AccountId=%s"),
					*CachedDisplayName, *CachedEOSProductUserId);

				OnEOSLoginSuccess(AccountInfo->AccountId);
			}
			else
			{
				FOnlineError Error = Result.GetErrorValue();
				OnEOSLoginFailure(FString::Printf(TEXT("EOS Login failed: %s"), *Error.GetLogString()));
			}
		});
}

void UNyxAuthSubsystem::OnEOSLoginSuccess(FAccountId AccountId)
{
	UE_LOG(LogNyxAuth, Log, TEXT("EOS Login complete, querying external auth token..."));

	// Query the external auth token (id_token / JWT) for server-side verification
	TSharedPtr<IOnlineServices> Services = GetServices(EOnlineServices::Epic);
	if (!Services)
	{
		// Token query optional — proceed without it
		UE_LOG(LogNyxAuth, Warning, TEXT("Could not get services for token query, proceeding without id_token"));
		OnExternalAuthTokenReceived(TEXT(""));
		return;
	}

	IAuthPtr Auth = Services->GetAuthInterface();
	if (!Auth)
	{
		UE_LOG(LogNyxAuth, Warning, TEXT("Could not get auth interface for token query, proceeding without id_token"));
		OnExternalAuthTokenReceived(TEXT(""));
		return;
	}

	FAuthQueryExternalAuthToken::Params TokenParams;
	TokenParams.LocalAccountId = AccountId;

	Auth->QueryExternalAuthToken(MoveTemp(TokenParams))
		.OnComplete(this, [this](const TOnlineResult<FAuthQueryExternalAuthToken>& Result)
		{
			if (Result.IsOk())
			{
				const FString& IdToken = Result.GetOkValue().ExternalAuthToken.Data;
				UE_LOG(LogNyxAuth, Log, TEXT("Got external auth token (length=%d)"), IdToken.Len());
				OnExternalAuthTokenReceived(IdToken);
			}
			else
			{
				// Token query failed — not fatal, proceed without token
				UE_LOG(LogNyxAuth, Warning, TEXT("External auth token query failed: %s. Proceeding without id_token."),
					*Result.GetErrorValue().GetLogString());
				OnExternalAuthTokenReceived(TEXT(""));
			}
		});
}

void UNyxAuthSubsystem::OnExternalAuthTokenReceived(const FString& IdToken)
{
	CachedEOSIdToken = IdToken;

	SetAuthState(ENyxAuthState::EOSAuthenticated);

	LocalIdentity.EOSProductUserId = CachedEOSProductUserId;
	LocalIdentity.DisplayName = CachedDisplayName;
	LocalIdentity.Platform = TEXT("Windows"); // TODO: Detect platform at runtime

	UE_LOG(LogNyxAuth, Log, TEXT("Phase 1 complete. PUID=%s, DisplayName=%s, HasIdToken=%s"),
		*CachedEOSProductUserId, *CachedDisplayName, CachedEOSIdToken.IsEmpty() ? TEXT("NO") : TEXT("YES"));

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

	// Listen for connection state changes via the BP delegate on NetworkSubsystem
	// This works for both mock and real SpacetimeDB connections
	NetworkSub->OnConnectionStateChangedBP.AddDynamic(
		this, &UNyxAuthSubsystem::OnSpacetimeDBConnectionStateChanged);

	// Connect to SpacetimeDB
	NetworkSub->ConnectToServer(PendingSpacetimeDBHost, PendingDatabaseName, bPendingUseMock);

	// For mock: also listen for auth completion via the interface
	if (bPendingUseMock)
	{
		INyxDatabaseInterface* DbInterface = NetworkSub->GetDatabaseInterface();
		if (DbInterface)
		{
			AuthCompleteDelegateHandle = DbInterface->OnAuthComplete().AddUObject(
				this, &UNyxAuthSubsystem::OnSpacetimeDBAuthComplete);
		}
	}

	// If already connected (mock connects synchronously), proceed immediately
	if (NetworkSub->GetConnectionState() == ENyxConnectionState::Connected)
	{
		OnSpacetimeDBConnected(ENyxConnectionState::Connected);
	}
}

void UNyxAuthSubsystem::OnSpacetimeDBConnectionStateChanged(ENyxConnectionState NewState)
{
	OnSpacetimeDBConnected(NewState);
}

void UNyxAuthSubsystem::OnSpacetimeDBConnected(ENyxConnectionState NewState)
{
	if (NewState == ENyxConnectionState::Connected && AuthState == ENyxAuthState::ConnectingSpacetimeDB)
	{
		UE_LOG(LogNyxAuth, Log, TEXT("SpacetimeDB connected. Calling authenticate_with_eos reducer..."));

		UNyxNetworkSubsystem* NetworkSub = GetGameInstance()->GetSubsystem<UNyxNetworkSubsystem>();
		if (!NetworkSub)
		{
			OnEOSLoginFailure(TEXT("NyxNetworkSubsystem not available for reducer call"));
			return;
		}

		if (NetworkSub->IsMockConnection())
		{
			// Mock path: use the interface
			if (NetworkSub->GetDatabaseInterface())
			{
				NetworkSub->GetDatabaseInterface()->CallAuthenticateWithEOS(
					CachedEOSIdToken,
					CachedEOSProductUserId,
					CachedDisplayName,
					LocalIdentity.Platform);
			}
		}
		else
		{
			// Real SpacetimeDB path: call generated reducer directly
			UDbConnection* Conn = NetworkSub->GetSpacetimeDBConnection();
			if (Conn && Conn->Reducers)
			{
				// Bind a one-shot callback to know when the reducer completes
				Conn->Reducers->OnAuthenticateWithEos.AddDynamic(this, &UNyxAuthSubsystem::HandleAuthReducerResult);

				Conn->Reducers->AuthenticateWithEos(
					CachedEOSProductUserId,
					CachedDisplayName,
					LocalIdentity.Platform);
			}
			else
			{
				OnEOSLoginFailure(TEXT("SpacetimeDB connection or Reducers is null"));
			}
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

void UNyxAuthSubsystem::HandleAuthReducerResult(const FReducerEventContext& Context, const FString& EosProductUserId, const FString& DisplayName, const FString& Platform)
{
	// Unbind so we don't get called again for other players' auth calls
	UNyxNetworkSubsystem* NetworkSub = GetGameInstance()->GetSubsystem<UNyxNetworkSubsystem>();
	if (NetworkSub)
	{
		UDbConnection* Conn = NetworkSub->GetSpacetimeDBConnection();
		if (Conn && Conn->Reducers)
		{
			Conn->Reducers->OnAuthenticateWithEos.RemoveDynamic(this, &UNyxAuthSubsystem::HandleAuthReducerResult);
		}
	}

	if (Context.Event.Status.IsCommitted())
	{
		UE_LOG(LogNyxAuth, Log, TEXT("authenticate_with_eos reducer committed! PUID=%s"), *EosProductUserId);
		OnSpacetimeDBAuthComplete(true);
	}
	else if (Context.Event.Status.IsFailed())
	{
		FString ErrorMsg = Context.Event.Status.GetAsFailed();
		UE_LOG(LogNyxAuth, Error, TEXT("authenticate_with_eos reducer failed: %s"), *ErrorMsg);
		OnSpacetimeDBAuthComplete(false);
	}
	else
	{
		UE_LOG(LogNyxAuth, Error, TEXT("authenticate_with_eos reducer returned unexpected status"));
		OnSpacetimeDBAuthComplete(false);
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
