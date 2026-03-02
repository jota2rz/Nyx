// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ModuleBindings/Types/PlayerType.g.h"
#include "NyxMovementComponent.generated.h"

class UDbConnection;

/**
 * A single predicted move stored in the prediction buffer.
 * Kept until the server confirms it via OnUpdate with matching Seq.
 */
USTRUCT()
struct FNyxPredictedMove
{
	GENERATED_BODY()

	/** Client-side sequence number */
	uint32 Seq = 0;

	/** Position AFTER this move was applied */
	FVector Position = FVector::ZeroVector;

	/** Yaw AFTER this move was applied */
	float Yaw = 0.f;

	/** The delta we applied (movement input * speed * dt) */
	FVector Delta = FVector::ZeroVector;

	/** DeltaTime for this frame (for replaying moves) */
	float DeltaTime = 0.f;
};

/**
 * Stores position snapshots for interpolation of remote players.
 */
USTRUCT()
struct FNyxRemoteSnapshot
{
	GENERATED_BODY()

	FVector Position = FVector::ZeroVector;
	float Yaw = 0.f;
	double Timestamp = 0.0; // Platform seconds
};

/**
 * Movement component for Spike 6: Client-Side Prediction & Reconciliation.
 *
 * Two operating modes:
 *
 * 1. LOCAL PLAYER (bIsLocalPlayer = true):
 *    - Processes movement input immediately (prediction)
 *    - Sends move_player reducer at SendRate Hz
 *    - Stores predicted moves in a ring buffer
 *    - On server OnUpdate: reconcile — compare server pos to predicted,
 *      discard confirmed, replay unconfirmed if mismatch
 *
 * 2. REMOTE PLAYER (bIsLocalPlayer = false):
 *    - Receives position updates from server via OnUpdate
 *    - Buffers snapshots and interpolates between them
 *    - Handles gaps (no update for 500ms+) with snap fallback
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class NYX_API UNyxMovementComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNyxMovementComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ──── Configuration ────

	/** Movement speed in UE units/sec (cm/s). 600 = 6 m/s ≈ jogging speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Movement")
	float MoveSpeed = 600.f;

	/** How often to send position updates to the server (Hz). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Movement")
	float SendRate = 20.f;

	/** Distance threshold (cm) before server reconciliation triggers a correction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Movement")
	float ReconciliationThreshold = 5.f;

	/** Interpolation delay for remote players (seconds). Higher = smoother but more lag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Movement")
	float InterpolationDelay = 0.1f;

	// ──── Setup ────

	/** Initialize for a local player (has input, sends to server). */
	void InitAsLocalPlayer(UDbConnection* InConnection);

	/** Initialize for a remote player (receives server updates, interpolates). */
	void InitAsRemotePlayer();

	/** Called from pawn's input handler to queue movement for this frame. */
	void AddMovementInput(const FVector& WorldDirection);

	/**
	 * Called when the server sends an OnUpdate for this player's row.
	 * For local player: triggers reconciliation.
	 * For remote player: adds to interpolation buffer.
	 */
	void OnServerUpdate(const FPlayerType& OldRow, const FPlayerType& NewRow);

	/** Is this a locally controlled player? */
	bool IsLocal() const { return bIsLocalPlayer; }

	// ──── Debug ────

	/** Number of unconfirmed predicted moves in the buffer. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Debug")
	int32 GetPendingMoveCount() const { return PredictionBuffer.Num(); }

	/** Total reconciliation corrections applied this session. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Debug")
	int32 GetReconciliationCount() const { return ReconciliationCount; }

private:
	// ──── Local player ────

	void TickLocalPlayer(float DeltaTime);
	void SendMovesToServer();
	void Reconcile(const FPlayerType& ServerState);

	/** Replay all unconfirmed moves from the prediction buffer, starting from a base position. */
	FVector ReplayMoves(const FVector& BasePosition) const;

	// ──── Remote player ────

	void TickRemotePlayer(float DeltaTime);

	// ──── State ────

	bool bIsLocalPlayer = false;

	/** SpacetimeDB connection for sending reducers. Only set for local player. */
	UPROPERTY()
	TObjectPtr<UDbConnection> Connection;

	/** Accumulated movement input for this frame (normalized direction). */
	FVector PendingInput = FVector::ZeroVector;

	/** Current sequence number (incremented each time we send to server). */
	uint32 CurrentSeq = 0;

	/** Time accumulator for send rate limiting. */
	float SendTimer = 0.f;

	/** Buffer of predicted moves awaiting server confirmation. */
	TArray<FNyxPredictedMove> PredictionBuffer;

	/** Max prediction buffer size (prevents unbounded growth on bad connections). */
	static constexpr int32 MaxPredictionBufferSize = 256;

	/** Reconciliation correction count (for diagnostics). */
	int32 ReconciliationCount = 0;

	/** Whether we've actually moved since last send (avoid spamming idle position). */
	bool bHasMoved = false;

	// ──── Remote player interpolation ────

	/** Buffered position snapshots for interpolation. */
	TArray<FNyxRemoteSnapshot> InterpolationBuffer;

	/** Max interpolation buffer entries. */
	static constexpr int32 MaxInterpolationBufferSize = 16;
};
