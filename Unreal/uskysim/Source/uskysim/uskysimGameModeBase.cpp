// Copyright Epic Games, Inc. All Rights Reserved.


#include "uskysimGameModeBase.h"

#include "SkySimSpectatorPawn.h"

AuskysimGameModeBase::AuskysimGameModeBase()
{
	DefaultPawnClass = ASkySimSpectatorPawn::StaticClass();
	SpectatorClass = ASkySimSpectatorPawn::StaticClass();
}
