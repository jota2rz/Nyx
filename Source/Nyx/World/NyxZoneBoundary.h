// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NyxZoneBoundary.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/**
 * Visual zone boundary marker — a line of glowing pillars at the zone split.
 *
 * Spike 19: Cross-Server Player Transfer
 *
 * The boundary is at X=0 by default. Server-1 owns X < 0, Server-2 owns X >= 0.
 * Pillars are spawned along the Y axis at regular intervals so players can
 * clearly see where the boundary is.
 *
 * This actor is spawned by NyxGameMode on the dedicated server and replicated
 * to clients so they can see the boundary markers.
 */
UCLASS()
class NYX_API ANyxZoneBoundary : public AActor
{
	GENERATED_BODY()

public:
	ANyxZoneBoundary();

	virtual void BeginPlay() override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** The X coordinate of the boundary line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category = "Zone")
	float BoundaryX = 0.f;

	/** Number of pillars to spawn along the Y axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
	int32 NumPillars = 21;

	/** Spacing between pillars (in cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
	float PillarSpacing = 500.f;

	/** Height of each pillar (in cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
	float PillarHeight = 600.f;

	/** Radius of each pillar (in cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
	float PillarRadius = 25.f;

	/** Pillar glow color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing = OnRep_PillarColor, Category = "Zone")
	FLinearColor PillarColor = FLinearColor(0.f, 0.5f, 1.f, 1.f); // Cyan-blue

	/** Floating zone label text (e.g. "WEST ZONE"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing = OnRep_ZoneLabel, Category = "Zone")
	FString ZoneLabel;

protected:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> RootScene;

	UFUNCTION()
	void OnRep_PillarColor();

	UFUNCTION()
	void OnRep_ZoneLabel();

private:
	void SpawnPillars();

	/** Cached pillar components for re-coloring on RepNotify. */
	TArray<TObjectPtr<UStaticMeshComponent>> PillarMeshes;

	/** Cached label component for re-labeling on RepNotify. */
	TObjectPtr<class UTextRenderComponent> LabelComponent;
};
