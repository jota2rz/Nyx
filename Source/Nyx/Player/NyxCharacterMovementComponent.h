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

private:
	/** Last client timestamp we successfully accepted (either via Super or our proxy fix). */
	float ProxyLastAcceptedTimeStamp = 0.f;
};
