// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NyxTypes.generated.h"

/**
 * Unique identifier for an entity in the SpacetimeDB world.
 * Maps to a SpacetimeDB row primary key.
 */
USTRUCT(BlueprintType)
struct NYX_API FNyxEntityId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	int64 Value = 0;

	FNyxEntityId() = default;
	explicit FNyxEntityId(int64 InValue) : Value(InValue) {}

	bool operator==(const FNyxEntityId& Other) const { return Value == Other.Value; }
	bool operator!=(const FNyxEntityId& Other) const { return Value != Other.Value; }

	friend uint32 GetTypeHash(const FNyxEntityId& Id) { return ::GetTypeHash(Id.Value); }

	FString ToString() const { return FString::Printf(TEXT("%lld"), Value); }
	bool IsValid() const { return Value != 0; }
};

/**
 * Represents a player's identity, linking SpacetimeDB Identity to EOS ProductUserId.
 */
USTRUCT(BlueprintType)
struct NYX_API FNyxPlayerIdentity
{
	GENERATED_BODY()

	/** SpacetimeDB identity (hex string of 256-bit value) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FString SpacetimeIdentity;

	/** EOS ProductUserId (cross-platform persistent ID) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FString EOSProductUserId;

	/** Display name from EOS */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FString DisplayName;

	/** Platform the player logged in from */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FString Platform;

	bool IsValid() const { return !SpacetimeIdentity.IsEmpty(); }
	bool HasEOS() const { return !EOSProductUserId.IsEmpty(); }
};

/**
 * Spatial position data for an entity in the game world.
 * Used for both player positions and spatial interest management.
 */
USTRUCT(BlueprintType)
struct NYX_API FNyxWorldPosition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FRotator Rotation = FRotator::ZeroRotator;

	/** Chunk coordinates for spatial partitioning */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FIntPoint ChunkCoord = FIntPoint::ZeroValue;

	/** Calculate chunk coordinates from world position */
	void UpdateChunkCoord(float ChunkSize)
	{
		ChunkCoord.X = FMath::FloorToInt(Location.X / ChunkSize);
		ChunkCoord.Y = FMath::FloorToInt(Location.Y / ChunkSize);
	}
};

/**
 * Complete player data as mirrored from SpacetimeDB player table.
 */
USTRUCT(BlueprintType)
struct NYX_API FNyxPlayerData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FNyxEntityId EntityId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FNyxPlayerIdentity Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	FNyxWorldPosition Position;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	int32 Health = 100;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	int32 MaxHealth = 100;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx")
	int32 Level = 1;

	bool IsValid() const { return EntityId.IsValid(); }
};

/**
 * Connection state for the SpacetimeDB connection.
 */
UENUM(BlueprintType)
enum class ENyxConnectionState : uint8
{
	Disconnected,
	Connecting,
	Connected,
	Reconnecting,
	Failed
};

/**
 * Authentication state for the combined EOS + SpacetimeDB auth flow.
 */
UENUM(BlueprintType)
enum class ENyxAuthState : uint8
{
	NotAuthenticated,
	AuthenticatingEOS,
	EOSAuthenticated,
	ConnectingSpacetimeDB,
	FullyAuthenticated,
	Failed
};
