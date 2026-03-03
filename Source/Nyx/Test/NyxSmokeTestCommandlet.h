// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "NyxSmokeTestCommandlet.generated.h"

/**
 * Commandlet wrapper for the Option 4 end-to-end smoke test.
 * Runs the test headless from the command line:
 *
 *   UnrealEditor-Cmd.exe Nyx.uproject -run=NyxSmokeTest
 *   UnrealEditor-Cmd.exe Nyx.uproject -run=NyxSmokeTest -host=127.0.0.1:3000 -db=nyx
 */
UCLASS()
class NYX_API UNyxSmokeTestCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UNyxSmokeTestCommandlet();

	virtual int32 Main(const FString& Params) override;
};
