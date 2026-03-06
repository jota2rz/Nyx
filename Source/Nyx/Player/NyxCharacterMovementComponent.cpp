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

				UE_LOG(LogNyx, Verbose,
					TEXT("NyxCMC: Proxy desync recovery — restored timestamp ")
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


