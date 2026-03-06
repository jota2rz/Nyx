// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxHUD.h"
#include "Nyx/Player/NyxCharacter.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"

void ANyxHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas) return;

	APlayerController* PC = GetOwningPlayerController();
	if (!PC) return;

	// ── Gather info ──

	// Proxy: extract from the connection URL (the address we connected to)
	FString ProxyInfo = TEXT("N/A");
	if (UNetConnection* Conn = PC->GetNetConnection())
	{
		ProxyInfo = Conn->URL.Host + TEXT(":") + FString::FromInt(Conn->URL.Port);
	}

	// Server + Zone: replicated from the game server via NyxCharacter
	FString ServerInfo = TEXT("N/A");
	FString ZoneInfo = TEXT("N/A");
	FString PosInfo = TEXT("N/A");

	ANyxCharacter* NyxChar = Cast<ANyxCharacter>(PC->GetPawn());
	if (NyxChar)
	{
		if (!NyxChar->ServerName.IsEmpty())
		{
			ServerInfo = NyxChar->ServerName;
		}
		if (!NyxChar->ZoneName.IsEmpty())
		{
			ZoneInfo = NyxChar->ZoneName;
		}

		FVector Loc = NyxChar->GetActorLocation();
		PosInfo = FString::Printf(TEXT("%.0f, %.0f, %.0f"), Loc.X, Loc.Y, Loc.Z);
	}

	// ── Draw ──

	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font) return;

	const float Scale = 1.5f;
	const float X = 20.f;
	float Y = 20.f;
	const float LineHeight = 22.f;
	const FLinearColor LabelColor(0.7f, 0.7f, 0.7f, 1.f);
	const FLinearColor ValueColor(1.f, 1.f, 0.3f, 1.f);

	// Background box
	const float BoxW = 500.f;
	const float BoxH = LineHeight * 6 + 10.f;
	FCanvasTileItem Bg(FVector2D(X - 5, Y - 5), FVector2D(BoxW, BoxH), FLinearColor(0.f, 0.f, 0.f, 0.6f));
	Bg.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Bg);

	auto DrawLine = [&](const FString& Label, const FString& Value)
	{
		FCanvasTextItem LabelItem(FVector2D(X, Y), FText::FromString(Label), Font, LabelColor);
		LabelItem.Scale = FVector2D(Scale, Scale);
		Canvas->DrawItem(LabelItem);

		FCanvasTextItem ValueItem(FVector2D(X + 90.f, Y), FText::FromString(Value), Font, ValueColor);
		ValueItem.Scale = FVector2D(Scale, Scale);
		Canvas->DrawItem(ValueItem);

		Y += LineHeight;
	};

	DrawLine(TEXT("Proxy:"), ProxyInfo);
	DrawLine(TEXT("Server:"), ServerInfo);
	DrawLine(TEXT("Zone:"), ZoneInfo);
	DrawLine(TEXT("Pos:"), PosInfo);

	// Boundary distance hint (boundary is at X=0 by default)
	if (NyxChar)
	{
		float PlayerX = NyxChar->GetActorLocation().X;
		FString BoundaryInfo = FString::Printf(TEXT("X=0  (you: %.0f, %s)"),
			PlayerX,
			PlayerX < 0.f ? TEXT("walk +X to cross ->") : TEXT("<- walk -X to cross"));
		DrawLine(TEXT("Border:"), BoundaryInfo);
	}
}
