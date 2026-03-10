// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxPlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

void ANyxPlayerCameraManager::UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime)
{
	// Let the engine compute the normal camera state first.
	Super::UpdateViewTarget(OutVT, DeltaTime);

	APlayerController* PC = PCOwner;
	if (!PC)
	{
		return;
	}

	if (PC->GetPawn() != nullptr)
	{
		// Normal gameplay — cache the camera transform so we have a stable
		// fallback if the pawn disappears during a DSTM migration.
		CachedCameraLocation = OutVT.POV.Location;
		CachedCameraRotation = OutVT.POV.Rotation;
		bHasValidCache = true;
	}
	else if (bHasValidCache)
	{
		// Migration gap — no pawn exists yet on the new server.
		// Hold the last known camera position to avoid a flicker/jump.
		OutVT.POV.Location = CachedCameraLocation;
		OutVT.POV.Rotation = CachedCameraRotation;
	}
}
