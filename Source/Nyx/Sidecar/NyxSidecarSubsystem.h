// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "ModuleBindings/Types/PhysicsBodyType.g.h"
#include "NyxSidecarSubsystem.generated.h"

class UDbConnection;
struct FEventContext;
struct FSubscriptionEventContext;
struct FErrorContext;
struct FSpacetimeDBIdentity;

/**
 * Simulates a UE5 physics sidecar within the same process.
 *
 * Spike 8: Validates the sidecar architecture pattern:
 *  - Creates a SECOND SpacetimeDB connection (separate identity)
 *  - Subscribes to the physics_body table
 *  - When new bodies arrive (OnInsert), begins tracking them
 *  - Runs simple Euler-integration physics (gravity, velocity)
 *  - Writes updated positions back to SpacetimeDB via PhysicsUpdate reducer
 *  - Game clients see the updates via their own subscription
 *
 * In production, this would be a separate headless UE5 process
 * with full Chaos physics. The SpacetimeDB communication pattern
 * is identical — only the physics engine differs.
 */
UCLASS()
class NYX_API UNyxSidecarSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ─── FTickableGameObject ───────────────────────────────────────

	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return bSidecarActive; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual TStatId GetStatId() const override;

	// ─── Sidecar Control ──────────────────────────────────────────

	/** Start the sidecar: connect to SpacetimeDB and begin physics simulation. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Sidecar")
	void StartSidecar(const FString& Host, const FString& DatabaseName);

	/** Stop the sidecar: disconnect and stop simulation. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Sidecar")
	void StopSidecar();

	/** Is the sidecar actively simulating? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Sidecar")
	bool IsActive() const { return bSidecarActive; }

private:
	// ─── SpacetimeDB callbacks ────────────────────────────────────

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

	// ─── Physics body table callbacks ─────────────────────────────

	UFUNCTION()
	void HandlePhysicsBodyInsert(const FEventContext& Context, const FPhysicsBodyType& NewRow);

	UFUNCTION()
	void HandlePhysicsBodyUpdate(const FEventContext& Context, const FPhysicsBodyType& OldRow, const FPhysicsBodyType& NewRow);

	UFUNCTION()
	void HandlePhysicsBodyDelete(const FEventContext& Context, const FPhysicsBodyType& DeletedRow);

	// ─── Physics simulation ───────────────────────────────────────

	/** One tracked physics body on the sidecar. */
	struct FTrackedBody
	{
		uint64 EntityId = 0;
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		bool bActive = true;
		/** Time since last update was sent to SpacetimeDB. */
		float TimeSinceLastSend = 0.f;
	};

	/** Step physics for all tracked bodies. */
	void StepPhysics(float DeltaTime);

	/** Send updated positions to SpacetimeDB. */
	void SendPhysicsUpdates();

	// ─── State ────────────────────────────────────────────────────

	UPROPERTY()
	TObjectPtr<UDbConnection> SidecarConnection;

	/** All bodies currently being simulated. */
	TMap<uint64, FTrackedBody> TrackedBodies;

	bool bSidecarActive = false;

	/** Physics tick rate — how often we send updates to SpacetimeDB. */
	static constexpr float SendRateHz = 30.f;
	static constexpr float SendInterval = 1.f / SendRateHz;

	/** Gravity in cm/s² (UE5 uses cm). -980 = Earth gravity. */
	static constexpr float GravityZ = -980.f;

	/** Floor Z — bodies below this are deactivated. */
	static constexpr float FloorZ = 0.f;

	/** Total bodies simulated (for logging). */
	int32 TotalBodiesSimulated = 0;

	/** Total updates sent to SpacetimeDB (for logging). */
	int32 TotalUpdatesSent = 0;
};
