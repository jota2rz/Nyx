// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxZoneBoundary.h"
#include "Nyx/Nyx.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

#if WITH_EDITOR
#include "Materials/MaterialExpressionVectorParameter.h"
#endif

ANyxZoneBoundary::ANyxZoneBoundary()
{
	PrimaryActorTick.bCanEverTick = false;

	// Spike 21: Disable replication — the MultiServer proxy cannot forward
	// non-player world actors (ActorChannelFailure kills the client connection).
	// The pillar visuals still render on the server for debugging.
	bReplicates = false;
	bAlwaysRelevant = false;

	RootScene = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(RootScene);
}

// ---------------------------------------------------------------------------
// Create an unlit material with a "Color" VectorParameter → EmissiveColor.
// BasicShapeMaterial has NO exposed vector parameters, so SetVectorParameterValue
// silently does nothing. This helper builds a proper parent material once,
// caches it, and returns it for pillar MID creation.
// Guarded by WITH_EDITOR because material expressions are editor-only.
// On the dedicated server (non-editor), rendering is disabled, so we fall back
// to the default material which is never actually drawn.
// ---------------------------------------------------------------------------
static UMaterial* GetOrCreatePillarMaterial()
{
#if WITH_EDITOR
	static UMaterial* CachedMaterial = nullptr;
	if (CachedMaterial)
	{
		return CachedMaterial;
	}

	CachedMaterial = NewObject<UMaterial>(
		GetTransientPackage(), TEXT("M_PillarColor"), RF_Public | RF_Standalone | RF_Transient);
	CachedMaterial->MaterialDomain = MD_Surface;
	CachedMaterial->SetShadingModel(MSM_Unlit);
	CachedMaterial->BlendMode = BLEND_Opaque;
	CachedMaterial->TwoSided = true;

	// Create vector parameter "Color" — this gives MIDs a real parameter to override
	UMaterialExpressionVectorParameter* ColorParam =
		NewObject<UMaterialExpressionVectorParameter>(CachedMaterial);
	ColorParam->ParameterName = TEXT("Color");
	ColorParam->DefaultValue = FLinearColor::White;

	CachedMaterial->GetExpressionCollection().AddExpression(ColorParam);

	// Wire "Color" output → EmissiveColor input
	UMaterialEditorOnlyData* EditorData = CachedMaterial->GetEditorOnlyData();
	if (EditorData)
	{
		EditorData->EmissiveColor.Connect(0, ColorParam);
	}

	// Force-compile the material so it's usable immediately
	CachedMaterial->PreEditChange(nullptr);
	CachedMaterial->PostEditChange();

	// Prevent garbage collection
	CachedMaterial->AddToRoot();

	UE_LOG(LogNyx, Log, TEXT("Created runtime PillarColor material (unlit + Color param)"));
	return CachedMaterial;
#else
	// Dedicated server — no rendering, return default material
	return UMaterial::GetDefaultMaterial(MD_Surface);
#endif
}

void ANyxZoneBoundary::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANyxZoneBoundary, BoundaryX);
	DOREPLIFETIME(ANyxZoneBoundary, PillarColor);
	DOREPLIFETIME(ANyxZoneBoundary, ZoneLabel);
}

void ANyxZoneBoundary::OnRep_PillarColor()
{
	// Re-apply color to all cached pillar meshes when replicated value arrives
	for (UStaticMeshComponent* Pillar : PillarMeshes)
	{
		if (!Pillar) continue;
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Pillar->GetMaterial(0));
		if (MID)
		{
			MID->SetVectorParameterValue(TEXT("Color"), PillarColor);
		}
	}
	// Update label color too
	if (LabelComponent)
	{
		LabelComponent->SetTextRenderColor(FColor(
			FMath::Clamp<uint8>(PillarColor.R * 255, 0, 255),
			FMath::Clamp<uint8>(PillarColor.G * 255, 0, 255),
			FMath::Clamp<uint8>(PillarColor.B * 255, 0, 255)));
	}
	UE_LOG(LogNyx, Log, TEXT("OnRep_PillarColor: Updated to (%.1f, %.1f, %.1f)"),
		PillarColor.R, PillarColor.G, PillarColor.B);
}

void ANyxZoneBoundary::OnRep_ZoneLabel()
{
	if (LabelComponent && !ZoneLabel.IsEmpty())
	{
		LabelComponent->SetText(FText::FromString(ZoneLabel));
	}
}

void ANyxZoneBoundary::BeginPlay()
{
	Super::BeginPlay();
	SpawnPillars();
	UE_LOG(LogNyx, Log, TEXT("ZoneBoundary spawned at X=%.0f with %d pillars"), BoundaryX, NumPillars);
}

void ANyxZoneBoundary::SpawnPillars()
{
	// Use engine cylinder mesh
	UStaticMesh* CylinderMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!CylinderMesh)
	{
		UE_LOG(LogNyx, Error, TEXT("ZoneBoundary: Failed to load Cylinder mesh"));
		return;
	}

	// Get our custom unlit material with a proper "Color" VectorParameter
	UMaterial* PillarMat = GetOrCreatePillarMaterial();

	const float HalfSpan = (NumPillars - 1) * PillarSpacing * 0.5f;

	for (int32 i = 0; i < NumPillars; ++i)
	{
		const float Y = -HalfSpan + i * PillarSpacing;

		UStaticMeshComponent* Pillar = NewObject<UStaticMeshComponent>(this);
		Pillar->SetupAttachment(RootScene);
		Pillar->SetStaticMesh(CylinderMesh);
		Pillar->RegisterComponent();

		// Scale: default cylinder is 100 UU diameter × 100 UU tall
		const float ScaleXY = PillarRadius / 50.f; // 50 = default cylinder radius
		const float ScaleZ = PillarHeight / 100.f;  // 100 = default cylinder height
		Pillar->SetRelativeScale3D(FVector(ScaleXY, ScaleXY, ScaleZ));

		// Position: center at boundary X, offset Y, half-height up
		Pillar->SetRelativeLocation(FVector(BoundaryX, Y, PillarHeight * 0.5f));

		// Create colored MID from our custom material (NOT from BasicShapeMaterial!)
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(PillarMat, this);
		if (MID)
		{
			MID->SetVectorParameterValue(TEXT("Color"), PillarColor);
			Pillar->SetMaterial(0, MID);
		}

		// No collision — just visual markers
		Pillar->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// Cache for OnRep re-coloring
		PillarMeshes.Add(Pillar);
	}

	// Add a floating zone label so players immediately know which server they're on
	{
		UTextRenderComponent* Label = NewObject<UTextRenderComponent>(this);
		Label->SetupAttachment(RootScene);
		Label->RegisterComponent();
		Label->SetText(FText::FromString(ZoneLabel.IsEmpty() ? TEXT("ZONE") : *ZoneLabel));
		Label->SetRelativeLocation(FVector(BoundaryX, 0.f, PillarHeight + 200.f));
		Label->SetHorizontalAlignment(EHTA_Center);
		Label->SetVerticalAlignment(EVRTA_TextCenter);
		Label->SetWorldSize(150.f);
		Label->SetTextRenderColor(FColor(
			FMath::Clamp<uint8>(PillarColor.R * 255, 0, 255),
			FMath::Clamp<uint8>(PillarColor.G * 255, 0, 255),
			FMath::Clamp<uint8>(PillarColor.B * 255, 0, 255)));
		LabelComponent = Label;
	}
}
