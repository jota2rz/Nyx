// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NyxCharacterMovementComponent.generated.h"

/**
 * Custom CMC for multi-server proxy architecture.
 *
 * The multi-server proxy introduces clock desync between client and game servers.
 * The server auto-advances its client timestamp tracking via ForcePositionUpdate
 * while waiting for client moves. When moves arrive through the proxy, their
 * timestamps may be "behind" the server's tracking, causing the default CMC to
 * reject all moves (TimeStamp expired). This causes:
 *   - Position rubber-banding (server corrects client back to spawn)
 *   - Movement appearing to not work at all
 *
 * Fix: Track the real client timeline separately. When ForcePositionUpdate has
 *      inflated the server's timestamp tracking, restore the last accepted client
 *      timestamp before processing each move. This gives correct DeltaTime
 *      (NewTimestamp - PreviousTimestamp) instead of near-zero.
 */
UCLASS()
class NYX_API UNyxCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	/**
	 * Override timestamp validation to handle proxy clock desync.
	 * Tracks real client timestamps so DeltaTime computation stays correct
	 * even when ForcePositionUpdate inflates the server's tracking.
	 */
	virtual bool VerifyClientTimeStamp(float TimeStamp, FNetworkPredictionData_Server_Character& ServerData) override;

	/**
	 * Override SimulatedTick to synthesize acceleration for simulated proxies.
	 *
	 * Problem: The standard ABP_Unarmed AnimBP checks GetCurrentAcceleration() > 0
	 * to decide if the character should play walk/run animations. For simulated proxies
	 * (other players visible on a client), the CMC has no input, so Acceleration is
	 * always zero — the character appears to slide in T-pose/idle.
	 *
	 * Fix: After the base SimulatedTick runs, set Acceleration to match the velocity
	 * direction when the character is moving. This gives the AnimBP the signal it needs
	 * to transition out of idle into locomotion.
	 */
	virtual void SimulatedTick(float DeltaSeconds) override;

	/**
	 * Reset CMC state after DSTM migration arrival.
	 *
	 * When a pawn returns to a server it previously lived on, the engine
	 * reuses the zombie actor (removed from world but not destroyed during
	 * TransferObjectOwnershipToRemoteServer). Its CMC still holds stale
	 * ServerPredictionData from the previous lifecycle. The stale
	 * ServerTimeStamp causes ForcePositionUpdate to compute a huge DeltaTime
	 * (world_time - old_timestamp), triggering GetSimulationTimeStep's
	 * max-iterations warning and displacing the character vertically.
	 *
	 * Also starts a brief grace window where the server trusts the client
	 * position up to a speed-based tolerance, preventing the jitter caused
	 * by the client predicting movement during the DSTM transit gap.
	 */
	void ResetForDSTMArrival();

protected:
	/**
	 * Override position error check to tolerate client drift after DSTM arrival.
	 *
	 * During the transit gap the client keeps predicting movement while the
	 * server pawn sits at the serialized position. The first moves to arrive
	 * will overshoot the default 1.73 UU threshold, causing a correction and
	 * visible jitter. Instead, we allow error up to:
	 *   ArrivalSpeed * max(TransitAllowance - TimeSinceArrival, 0) + SafetyMargin
	 * This starts high to absorb transit drift and decays back to just the
	 * safety margin as the server catches up. The server pawn is snapped to
	 * the client position while within tolerance.
	 */
	virtual bool ServerExceedsAllowablePositionError(
		float ClientTimeStamp, float DeltaTime, const FVector& Accel,
		const FVector& ClientWorldLocation, const FVector& RelativeClientLocation,
		UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName,
		uint8 ClientMovementMode) override;

private:
	/** Last client timestamp we successfully accepted (either via Super or our proxy fix). */
	float ProxyLastAcceptedTimeStamp = 0.f;

	/** World time when the most recent DSTM arrival was processed. 0 = no active grace period. */
	float DSTMArrivalWorldTime = 0.f;

	/** Speed of the character at the moment of DSTM arrival (used for tolerance calculation). */
	float DSTMArrivalSpeed = 0.f;

	/** How long (seconds) the grace window lasts after DSTM arrival. */
	static constexpr float DSTMGraceDuration = 5.0f;

	/** Max expected DSTM transit time (seconds). Tolerance is highest at t=0
	 *  and decays linearly to just the margin at t=TransitAllowance. */
	static constexpr float DSTMTransitAllowance = 0.5f;

	/** Fixed safety margin (UU) always added to the speed-based tolerance. */
	static constexpr float DSTMSafetyMargin = 50.f;
};
