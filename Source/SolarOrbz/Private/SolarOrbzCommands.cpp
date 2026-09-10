// Copyright Epic Games, Inc. All Rights Reserved.

#include "SolarOrbzCommands.h"

#define LOCTEXT_NAMESPACE "FSolarOrbzModule"

void FSolarOrbzCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "SolarOrbz", "Bring up SolarOrbz window", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
