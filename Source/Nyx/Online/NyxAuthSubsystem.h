// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Nyx/Data/NyxTypes.h"
#include "NyxAuthSubsystem.generated.h"

class UNyxNetworkSubsystem;

/**
 * Manages the two-phase authentication flow:
 *
 *   Phase 1: Login via Epic Online Services (EOS)
 *     - Authenticates with Epic/Steam/PSN/Xbox via EOS Connect
 *     - Obtains an OIDC-compliant id_token
 *     - Retrieves the ProductUserId (cross-platform identity)
 *
 *   Phase 2: Authenticate with SpacetimeDB
 *     - Connects to SpacetimeDB (via NyxNetworkSubsystem)
 *     - Passes EOS token to SpacetimeDB via WithToken() or reducer call
 *     - Server module links SpacetimeDB Identity to EOS ProductUserId
 *
 * This subsystem depends on UNyxNetworkSubsystem.
 */
UCLASS()
class NYX_API UNyxAuthSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Get the current auth state. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Auth")
	ENyxAuthState GetAuthState() const { return AuthState; }

	/** Get the local player identity (valid only when FullyAuthenticated). */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Auth")
	const FNyxPlayerIdentity& GetLocalPlayerIdentity() const { return LocalIdentity; }

	/**
	 * Start the full login flow.
	 * @param LoginType - EOS login type (e.g., "accountportal", "developer", "persistentauth")
	 * @param SpacetimeDBHost - SpacetimeDB host address
	 * @param DatabaseName - SpacetimeDB database name
	 * @param bUseMockDB - If true, skip real SpacetimeDB and use mock
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Auth")
	void Login(const FString& LoginType, const FString& SpacetimeDBHost, const FString& DatabaseName, bool bUseMockDB = false);

	/** Logout and disconnect everything. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Auth")
	void Logout();

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthStateChangedBP, ENyxAuthState, NewState);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|Auth")
	FOnAuthStateChangedBP OnAuthStateChangedBP;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnLoginCompleteBP, bool, bSuccess, const FString&, ErrorMessage);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|Auth")
	FOnLoginCompleteBP OnLoginCompleteBP;

private:
	/** Phase 1: Start EOS Login. */
	void StartEOSLogin(const FString& LoginType);

	/** Phase 1 complete: EOS login succeeded. */
	void OnEOSLoginSuccess();

	/** Phase 1 failed. */
	void OnEOSLoginFailure(const FString& ErrorMessage);

	/** Phase 2: Connect to SpacetimeDB and authenticate. */
	void StartSpacetimeDBAuth();

	/** Phase 2: SpacetimeDB connected, call authenticate reducer. */
	void OnSpacetimeDBConnected(ENyxConnectionState NewState);

	/** Phase 2: Auth reducer response. */
	void OnSpacetimeDBAuthComplete(bool bSuccess);

	void SetAuthState(ENyxAuthState NewState);

	ENyxAuthState AuthState = ENyxAuthState::NotAuthenticated;
	FNyxPlayerIdentity LocalIdentity;

	/** Cached EOS data from Phase 1 */
	FString CachedEOSIdToken;
	FString CachedEOSProductUserId;
	FString CachedDisplayName;

	/** SpacetimeDB connection parameters (stored between phases) */
	FString PendingSpacetimeDBHost;
	FString PendingDatabaseName;
	bool bPendingUseMock = false;

	/** Handle for connection state delegate */
	FDelegateHandle ConnectionStateDelegateHandle;
	FDelegateHandle AuthCompleteDelegateHandle;
};
