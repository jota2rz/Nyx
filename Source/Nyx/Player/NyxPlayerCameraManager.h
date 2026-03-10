// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/PlayerCameraManager.h"
#include "NyxPlayerCameraManager.generated.h"

/**
 * Custom camera manager that holds the last valid camera transform during
 * DSTM migration gaps.
 *
 * When a player crosses a zone boundary, the pawn is destroyed on the sending
 * server and re-spawned on the receiving server. During the brief window
 * between destruction and respawn (~500ms), the PC has no pawn and the default
 * camera manager falls back to the PC's world location — which may be at
 * origin or an arbitrary spot, causing a visible camera "flicker".
 *
 * This subclass caches the last known camera transform when a valid pawn
 * exists, and replays that cached transform during the gap so the client
 * sees a perfectly stable camera until the new pawn takes over.
 */
UCLASS()
class NYX_API ANyxPlayerCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

protected:
	/** Last camera location from a frame where a valid pawn existed. */
	FVector CachedCameraLocation = FVector::ZeroVector;

	/** Last camera rotation from a frame where a valid pawn existed. */
	FRotator CachedCameraRotation = FRotator::ZeroRotator;

	/** True once we've cached at least one valid frame. */
	bool bHasValidCache = false;

	//~ APlayerCameraManager interface
	virtual void UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime) override;
};
