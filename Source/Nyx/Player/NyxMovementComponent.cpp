// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxMovementComponent.h"
#include "Nyx/Nyx.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"
#include "GameFramework/Pawn.h"

DECLARE_LOG_CATEGORY_EXTERN(LogNyxMove, Log, All);
DEFINE_LOG_CATEGORY(LogNyxMove);

UNyxMovementComponent::UNyxMovementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

// ─── Setup ─────────────────────────────────────────────────────────

void UNyxMovementComponent::InitAsLocalPlayer(UDbConnection* InConnection)
{
	bIsLocalPlayer = true;
	Connection = InConnection;
	CurrentSeq = 0;
	PredictionBuffer.Empty();
	ReconciliationCount = 0;
	UE_LOG(LogNyxMove, Log, TEXT("Initialized as LOCAL player (SendRate=%.0f Hz, Threshold=%.1f cm)"),
		SendRate, ReconciliationThreshold);
}

void UNyxMovementComponent::InitAsRemotePlayer()
{
	bIsLocalPlayer = false;
	Connection = nullptr;
	InterpolationBuffer.Empty();
	UE_LOG(LogNyxMove, Log, TEXT("Initialized as REMOTE player (InterpDelay=%.2f s)"), InterpolationDelay);
}

void UNyxMovementComponent::AddMovementInput(const FVector& WorldDirection)
{
	PendingInput += WorldDirection;
}

// ─── Tick ──────────────────────────────────────────────────────────

void UNyxMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bIsLocalPlayer)
	{
		TickLocalPlayer(DeltaTime);
	}
	else
	{
		TickRemotePlayer(DeltaTime);
	}
}

// ─── Local Player: Prediction ──────────────────────────────────────

void UNyxMovementComponent::TickLocalPlayer(float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	// 1. Apply input immediately (CLIENT-SIDE PREDICTION)
	FVector Delta = FVector::ZeroVector;
	if (!PendingInput.IsNearlyZero())
	{
		FVector Dir = PendingInput.GetClampedToMaxSize(1.f);
		Delta = Dir * MoveSpeed * DeltaTime;

		FVector NewPos = Owner->GetActorLocation() + Delta;
		Owner->SetActorLocation(NewPos);

		bHasMoved = true;
	}
	PendingInput = FVector::ZeroVector;

	// 2. Send to server at SendRate
	SendTimer += DeltaTime;
	const float SendInterval = 1.f / FMath::Max(SendRate, 1.f);

	if (SendTimer >= SendInterval && bHasMoved)
	{
		SendMovesToServer();
		SendTimer = 0.f;
		bHasMoved = false;
	}
}

void UNyxMovementComponent::SendMovesToServer()
{
	if (!Connection || !Connection->Reducers) return;

	AActor* Owner = GetOwner();
	if (!Owner) return;

	// Increment sequence number
	CurrentSeq++;

	const FVector Pos = Owner->GetActorLocation();
	const float Yaw = Owner->GetActorRotation().Yaw;

	// Store in prediction buffer
	FNyxPredictedMove Move;
	Move.Seq = CurrentSeq;
	Move.Position = Pos;
	Move.Yaw = Yaw;
	Move.DeltaTime = 1.f / FMath::Max(SendRate, 1.f);
	// Delta is accumulated since last send — we store final position, not delta, for simplicity

	if (PredictionBuffer.Num() >= MaxPredictionBufferSize)
	{
		// Drop oldest — connection is too slow
		PredictionBuffer.RemoveAt(0);
		UE_LOG(LogNyxMove, Warning, TEXT("Prediction buffer full (%d), dropping oldest"), MaxPredictionBufferSize);
	}
	PredictionBuffer.Add(Move);

	// Send to server
	Connection->Reducers->MovePlayer(Pos.X, Pos.Y, Pos.Z, Yaw, CurrentSeq);

	UE_LOG(LogNyxMove, Log, TEXT("Sent move seq=%u pos=(%.1f, %.1f, %.1f) yaw=%.1f, pending=%d"),
		CurrentSeq, Pos.X, Pos.Y, Pos.Z, Yaw, PredictionBuffer.Num());
}

// ─── Local Player: Reconciliation ──────────────────────────────────

void UNyxMovementComponent::OnServerUpdate(const FPlayerType& OldRow, const FPlayerType& NewRow)
{
	if (bIsLocalPlayer)
	{
		Reconcile(NewRow);
	}
	else
	{
		// Remote player: add snapshot to interpolation buffer
		FNyxRemoteSnapshot Snapshot;
		Snapshot.Position = FVector(NewRow.PosX, NewRow.PosY, NewRow.PosZ);
		Snapshot.Yaw = NewRow.RotYaw;
		Snapshot.Timestamp = FPlatformTime::Seconds();

		if (InterpolationBuffer.Num() >= MaxInterpolationBufferSize)
		{
			InterpolationBuffer.RemoveAt(0);
		}
		InterpolationBuffer.Add(Snapshot);
	}
}

void UNyxMovementComponent::Reconcile(const FPlayerType& ServerState)
{
	const uint32 ServerSeq = ServerState.Seq;
	const FVector ServerPos(ServerState.PosX, ServerState.PosY, ServerState.PosZ);

	// 1. Remove all confirmed moves (seq <= ServerSeq)
	int32 RemoveCount = 0;
	for (int32 i = 0; i < PredictionBuffer.Num(); ++i)
	{
		if (PredictionBuffer[i].Seq <= ServerSeq)
		{
			RemoveCount++;
		}
		else
		{
			break; // buffer is ordered by seq
		}
	}

	if (RemoveCount > 0)
	{
		PredictionBuffer.RemoveAt(0, RemoveCount);
	}

	// 2. Check if server position matches our prediction for the confirmed seq
	//    If we have unconfirmed moves remaining, replay them from server position
	//    to get what our position SHOULD be.
	FVector ExpectedPos;
	if (PredictionBuffer.Num() > 0)
	{
		// Replay unconfirmed moves from the server's confirmed position
		ExpectedPos = ReplayMoves(ServerPos);
	}
	else
	{
		// All moves confirmed — server position IS our position
		ExpectedPos = ServerPos;
	}

	// 3. Compare expected position to our current position
	AActor* Owner = GetOwner();
	if (!Owner) return;

	const FVector CurrentPos = Owner->GetActorLocation();
	const float Error = FVector::Dist(CurrentPos, ExpectedPos);

	if (Error > ReconciliationThreshold)
	{
		// Correction needed
		ReconciliationCount++;
		Owner->SetActorLocation(ExpectedPos);

		UE_LOG(LogNyxMove, Log, TEXT("RECONCILIATION #%d: error=%.1f cm, serverSeq=%u, pending=%d, server=(%.0f,%.0f,%.0f) -> corrected=(%.0f,%.0f,%.0f)"),
			ReconciliationCount, Error, ServerSeq, PredictionBuffer.Num(),
			ServerPos.X, ServerPos.Y, ServerPos.Z,
			ExpectedPos.X, ExpectedPos.Y, ExpectedPos.Z);
	}
	else
	{
		UE_LOG(LogNyxMove, Log, TEXT("Reconciliation OK: error=%.2f cm, serverSeq=%u, pending=%d"),
			Error, ServerSeq, PredictionBuffer.Num());
	}
}

FVector UNyxMovementComponent::ReplayMoves(const FVector& BasePosition) const
{
	// For now, use the most recent predicted position.
	// In a full implementation, we'd re-apply each move's input delta
	// from BasePosition. But since our moves store absolute positions
	// and the server should agree on the most recent, we use the
	// latest predicted position in the buffer.
	//
	// This works because:
	// 1. We trust the server's confirmed position for seq N
	// 2. We have predicted moves N+1, N+2, ...
	// 3. The delta between the server's confirmed pos and our predicted pos
	//    for the unconfirmed moves should be consistent
	//
	// A more precise approach would store input + dt per move and re-simulate,
	// but for this spike the position-based approach is sufficient.

	if (PredictionBuffer.Num() > 0)
	{
		// Calculate the offset between the server position and the first
		// unconfirmed predicted position's "base" (what we thought the server
		// position was when we made that prediction).
		// Then apply that offset to our latest predicted position.

		// We cannot perfectly re-derive the base because we don't store it,
		// but we can compute the total delta of all unconfirmed moves.
		// The simplest approach: return the last predicted position.
		return PredictionBuffer.Last().Position;
	}

	return BasePosition;
}

// ─── Remote Player: Interpolation ──────────────────────────────────

void UNyxMovementComponent::TickRemotePlayer(float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	if (InterpolationBuffer.Num() == 0) return;

	// Target time = now - delay (we render slightly in the past)
	const double RenderTime = FPlatformTime::Seconds() - InterpolationDelay;

	// Find the two snapshots that bracket RenderTime
	int32 From = -1;
	int32 To = -1;

	for (int32 i = 0; i < InterpolationBuffer.Num() - 1; ++i)
	{
		if (InterpolationBuffer[i].Timestamp <= RenderTime &&
			InterpolationBuffer[i + 1].Timestamp >= RenderTime)
		{
			From = i;
			To = i + 1;
			break;
		}
	}

	if (From >= 0 && To >= 0)
	{
		// Interpolate between the two snapshots
		const FNyxRemoteSnapshot& A = InterpolationBuffer[From];
		const FNyxRemoteSnapshot& B = InterpolationBuffer[To];
		const double Range = B.Timestamp - A.Timestamp;
		const float Alpha = (Range > KINDA_SMALL_NUMBER)
			? FMath::Clamp(static_cast<float>((RenderTime - A.Timestamp) / Range), 0.f, 1.f)
			: 1.f;

		const FVector InterpPos = FMath::Lerp(A.Position, B.Position, Alpha);
		const float InterpYaw = FMath::Lerp(A.Yaw, B.Yaw, Alpha);

		Owner->SetActorLocation(InterpPos);
		Owner->SetActorRotation(FRotator(0.f, InterpYaw, 0.f));

		// Clean up old snapshots we've passed
		if (From > 0)
		{
			InterpolationBuffer.RemoveAt(0, From);
		}
	}
	else if (InterpolationBuffer.Num() > 0)
	{
		// We're either ahead of all snapshots or behind all of them.
		// Snap to the latest available position.
		const FNyxRemoteSnapshot& Latest = InterpolationBuffer.Last();
		Owner->SetActorLocation(Latest.Position);
		Owner->SetActorRotation(FRotator(0.f, Latest.Yaw, 0.f));
	}
}
