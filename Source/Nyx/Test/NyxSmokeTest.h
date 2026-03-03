// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "ModuleBindings/Types/CharacterStatsType.g.h"
#include "NyxSmokeTest.generated.h"

class UDbConnection;
struct FSpacetimeDBIdentity;
struct FEventContext;
struct FSubscriptionEventContext;
struct FErrorContext;

/**
 * End-to-end smoke test for the Option 4 architecture pipeline.
 *
 * Exercises the full server→SpacetimeDB flow from within the editor:
 *   1. Connect to SpacetimeDB (ws://127.0.0.1:3000)
 *   2. Subscribe to character_stats, combat_event, zone_server tables
 *   3. Register a zone server
 *   4. Load a character (creates if not exists)
 *   5. Verify character stats arrive via OnInsert/cache
 *   6. Resolve a hit → verify HP changes via OnUpdate
 *   7. Save character state
 *   8. Deregister zone server
 *   9. Disconnect
 *
 * Results are logged to the Output Log and stored for RESEARCH.md reporting.
 *
 * Usage: Nyx.SmokeTest [host] [database]
 */
UCLASS()
class NYX_API UNyxSmokeTest : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Run the full end-to-end smoke test.
	 * @param Host - SpacetimeDB host (default: 127.0.0.1:3000)
	 * @param DatabaseName - SpacetimeDB database name (default: nyx)
	 */
	void Run(const FString& Host = TEXT("127.0.0.1:3000"),
		const FString& DatabaseName = TEXT("nyx"));

	/** Manually tick the connection (for commandlet mode where engine tick doesn't run). */
	void ManualTick();

	/** Get the final test report string. */
	const FString& GetReport() const { return Report; }

	/** Is the test currently running? */
	bool IsRunning() const { return bRunning; }

private:
	// ──── Test Flow (state machine) ────

	enum class ETestStep : uint8
	{
		Connecting,
		Subscribing,
		RegisteringZone,
		LoadingCharacter,
		WaitingForStats,
		ResolvingHit,
		WaitingForHPChange,
		SavingCharacter,
		DeregisteringZone,
		Disconnecting,
		Complete,
		Failed
	};

	ETestStep CurrentStep = ETestStep::Connecting;

	void AdvanceTo(ETestStep Step);
	void Pass(const FString& StepName, const FString& Detail);
	void Fail(const FString& StepName, const FString& Detail);
	void Finish();

	// ──── SpacetimeDB Callbacks ────

	UFUNCTION()
	void HandleConnect(UDbConnection* Connection, FSpacetimeDBIdentity Identity, const FString& Token);

	UFUNCTION()
	void HandleDisconnect(UDbConnection* Connection, const FString& Error);

	UFUNCTION()
	void HandleConnectError(const FString& ErrorMessage);

	UFUNCTION()
	void HandleSubscriptionApplied(FSubscriptionEventContext Context);

	UFUNCTION()
	void HandleSubscriptionError(FErrorContext Context);

	UFUNCTION()
	void HandleCharacterStatsInsert(const FEventContext& Context, const FCharacterStatsType& NewRow);

	UFUNCTION()
	void HandleCharacterStatsUpdate(const FEventContext& Context,
		const FCharacterStatsType& OldRow, const FCharacterStatsType& NewRow);

	// ──── State ────

	UPROPERTY()
	TObjectPtr<UDbConnection> Connection;

	FSpacetimeDBIdentity ServerIdentity;
	FString TestServerId;
	FString TestZoneId;
	FString TestCharacterName;

	int32 InitialHP = 0;
	int32 PostHitHP = 0;
	bool bCharacterLoaded = false;
	bool bHitResolved = false;

	bool bRunning = false;
	double StartTime = 0.0;
	FString Report;

	// Timeout handle
	FTimerHandle TimeoutHandle;
	void OnTimeout();

	struct FStepResult
	{
		FString Name;
		bool bPassed;
		FString Detail;
		double ElapsedMs;
	};

	TArray<FStepResult> StepResults;
	double StepStartTime = 0.0;
};
