// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxCharacterMovementComponent.h"
#include "Nyx/Nyx.h"
#include "GameFramework/Character.h"

bool UNyxCharacterMovementComponent::VerifyClientTimeStamp(
	float TimeStamp,
	FNetworkPredictionData_Server_Character& ServerData)
{
	// Default validation handles the normal (non-proxy) case
	if (Super::VerifyClientTimeStamp(TimeStamp, ServerData))
	{
		// Track the accepted timestamp for proxy desync recovery
		ProxyLastAcceptedTimeStamp = TimeStamp;
		return true;
	}

	// PROXY CLOCK DESYNC FIX:
	// Multi-server proxy introduces timestamp desync between client and game servers.
	// The server auto-advances CurrentClientTimeStamp via ForcePositionUpdate while
	// waiting for client moves. Through the proxy, client timestamps may arrive
	// "behind" the server's tracking, causing all moves to be rejected as expired.
	//
	// Root cause chain:
	//   1. Server waits for client moves → ForcePositionUpdate inflates CurrentClientTimeStamp
	//   2. Client moves arrive through proxy with timestamp < server's inflated tracking
	//   3. Default VerifyClientTimeStamp rejects as expired
	//   4. Server character stays at spawn → corrections snap client back → rubber-banding
	//
	// Fix: Restore CurrentClientTimeStamp to the last REAL client timestamp before
	// accepting the move. This lets GetServerMoveDeltaTime compute the correct
	// DeltaTime (NewTimestamp - PreviousTimestamp), giving the server the actual
	// frame delta the client predicted with.
	if (TimeStamp > 0.f && FMath::IsFinite(TimeStamp)
		&& TimeStamp > ProxyLastAcceptedTimeStamp)
	{
		const float ServerInflated = ServerData.CurrentClientTimeStamp;
		const float Delta = ServerInflated - TimeStamp;

		// Accept timestamps up to 60 seconds behind the server's inflated tracking.
		// This covers proxy startup delay + accumulated ForcePositionUpdate drift.
		if (Delta > 0.f && Delta < 60.0f)
		{
			if (ProxyLastAcceptedTimeStamp > 0.f)
			{
				// Normal recovery: restore real client timeline so DeltaTime is correct
				// DeltaTime will be (TimeStamp - ProxyLastAcceptedTimeStamp) = actual client frame delta
				ServerData.CurrentClientTimeStamp = ProxyLastAcceptedTimeStamp;

				UE_LOG(LogNyx, Warning,
					TEXT("NyxCMC: Proxy desync recovery - restored timestamp ")
					TEXT("from %.3f to %.3f (client dt=%.4fs) for %s"),
					ServerInflated, ProxyLastAcceptedTimeStamp,
					TimeStamp - ProxyLastAcceptedTimeStamp,
					*GetNameSafe(CharacterOwner));
			}
			else
			{
				// First move ever: no previous timestamp to restore.
				// Use tiny DeltaTime for initial sync (one-time correction is expected).
				ServerData.CurrentClientTimeStamp = TimeStamp - MIN_TICK_TIME;

				UE_LOG(LogNyx, Log,
					TEXT("NyxCMC: Proxy desync — first move sync ")
					TEXT("(server was at %.3f, client at %.3f) for %s"),
					ServerInflated, TimeStamp,
					*GetNameSafe(CharacterOwner));
			}

			ProxyLastAcceptedTimeStamp = TimeStamp;
			return true;
		}
	}

	return false;
}

void UNyxCharacterMovementComponent::ResetForDSTMArrival()
{
	// Reset proxy timestamp tracking so the first move after DSTM arrival
	// is treated as a fresh connection.
	ProxyLastAcceptedTimeStamp = 0.f;

	// If server prediction data exists (zombie pawn from a previous lifecycle
	// on this server), reset timestamps to prevent ForcePositionUpdate from
	// computing a huge DeltaTime (world_time - stale_timestamp).
	if (HasPredictionData_Server())
	{
		FNetworkPredictionData_Server_Character* ServerData = GetPredictionData_Server_Character();
		ServerData->ServerTimeStamp = 0.f;
		ServerData->CurrentClientTimeStamp = 0.f;
		ServerData->ServerAccumulatedClientTimeStamp = 0.0;
		ServerData->ResetForcedUpdateState();

		UE_LOG(LogNyx, Log,
			TEXT("NyxCMC: Reset stale ServerPredictionData after DSTM arrival for %s"),
			*GetNameSafe(CharacterOwner));
	}

	// Start the position-tolerance grace window.
	// During the DSTM transit gap the client kept predicting movement, so the
	// first moves will have drifted from the serialized position. We record
	// the current speed to compute a reasonable tolerance envelope.
	if (const UWorld* World = GetWorld())
	{
		DSTMArrivalWorldTime = World->GetTimeSeconds();
		DSTMArrivalSpeed = Velocity.Size();

		UE_LOG(LogNyx, Log,
			TEXT("NyxCMC: DSTM grace window started (speed=%.1f, margin=%.1f, duration=%.1fs) for %s"),
			DSTMArrivalSpeed, DSTMSafetyMargin, DSTMGraceDuration,
			*GetNameSafe(CharacterOwner));
	}
}

bool UNyxCharacterMovementComponent::ServerExceedsAllowablePositionError(
	float ClientTimeStamp, float DeltaTime, const FVector& Accel,
	const FVector& ClientWorldLocation, const FVector& RelativeClientLocation,
	UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName,
	uint8 ClientMovementMode)
{
	// During the DSTM grace window, use a decaying speed-based tolerance
	// instead of the default ~1.73 UU threshold. The client keeps predicting
	// during the DSTM transit gap, so the first moves arrive ahead of the
	// serialized server position. The tolerance starts high to absorb this
	// transit drift and decays back to just the safety margin:
	//   tolerance = speed * max(transitAllowance - elapsed, 0) + margin
	if (DSTMArrivalWorldTime > 0.f)
	{
		const UWorld* World = GetWorld();
		const float TimeSinceArrival = World ? (World->GetTimeSeconds() - DSTMArrivalWorldTime) : DSTMGraceDuration;

		if (TimeSinceArrival < DSTMGraceDuration)
		{
			const FVector ServerLoc = UpdatedComponent->GetComponentLocation();
			const FVector LocDiff = ServerLoc - ClientWorldLocation;
			const float ErrorSq = LocDiff.SizeSquared();
			const float TransitRemaining = FMath::Max(DSTMTransitAllowance - TimeSinceArrival, 0.f);
			const float Tolerance = DSTMArrivalSpeed * TransitRemaining + DSTMSafetyMargin;

			if (ErrorSq <= FMath::Square(Tolerance))
			{
				// Trust the client — snap server pawn to client position.
				UpdatedComponent->MoveComponent(
					ClientWorldLocation - ServerLoc,
					UpdatedComponent->GetComponentQuat(),
					true, nullptr, EMoveComponentFlags::MOVECOMP_NoFlags,
					ETeleportType::TeleportPhysics);
				bJustTeleported = true;
				return false; // no correction needed
			}

			// Beyond tolerance — fall through to default correction.
			UE_LOG(LogNyx, Warning,
				TEXT("NyxCMC: DSTM grace exceeded tolerance (error=%.1f > tol=%.1f) for %s"),
				FMath::Sqrt(ErrorSq), Tolerance, *GetNameSafe(CharacterOwner));
			DSTMArrivalWorldTime = 0.f; // end grace early
		}
		else
		{
			// Grace period expired.
			DSTMArrivalWorldTime = 0.f;
		}
	}

	return Super::ServerExceedsAllowablePositionError(
		ClientTimeStamp, DeltaTime, Accel, ClientWorldLocation,
		RelativeClientLocation, ClientMovementBase, ClientBaseBoneName,
		ClientMovementMode);
}

void UNyxCharacterMovementComponent::SimulatedTick(float DeltaSeconds)
{
	Super::SimulatedTick(DeltaSeconds);

	// Synthesize acceleration for simulated proxies so AnimBP locomotion works.
	// ABP_Unarmed checks GetCurrentAcceleration().Size() > 0 to decide ShouldMove.
	// Without this, simulated proxies slide in idle animation.
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		const float SpeedSq = Velocity.SizeSquared2D();
		if (SpeedSq > 9.0f) // GroundSpeed > 3.0
		{
			Acceleration = Velocity.GetSafeNormal2D() * GetMaxAcceleration();
		}
		else
		{
			Acceleration = FVector::ZeroVector;
		}
	}
}


