#pragma once

#include "CoreMinimal.h"
#include "Configuration/ModConfiguration.h"
#include "VLQoLModConfiguration.generated.h"

/// The mod's settings, built in C++ (no Blueprint asset).
UCLASS()
class VERTICALLOGISTICSQOL_API UVLQoLModConfiguration : public UModConfiguration
{
	GENERATED_BODY()
public:
	UVLQoLModConfiguration();

	/// Key of the setting that replaces the vanilla Straight pipe build mode with Better Straight.
	static const TCHAR* ReplaceVanillaStraightKey;
};

/// Live value of the ReplaceVanillaStraight setting, kept up to date by UVLQoLGameInstanceModule.
extern bool GVLQoLReplaceVanillaStraight;
