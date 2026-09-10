// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Framework/Commands/Commands.h"
#include "SolarOrbzStyle.h"

class FSolarOrbzCommands : public TCommands<FSolarOrbzCommands>
{
public:

	FSolarOrbzCommands()
		: TCommands<FSolarOrbzCommands>(TEXT("SolarOrbz"), NSLOCTEXT("Contexts", "SolarOrbz", "SolarOrbz Plugin"), NAME_None, FSolarOrbzStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};