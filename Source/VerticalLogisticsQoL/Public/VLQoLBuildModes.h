#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "VLQoLBuildModes.generated.h"

/// Builds a vertical attachment with the vertical input at the bottom.
UCLASS()
class VERTICALLOGISTICSQOL_API UVLQoLVerticalUpBuildMode : public UFGHologramBuildModeDescriptor
{
	GENERATED_BODY()
public:
	UVLQoLVerticalUpBuildMode()
	{
		mDisplayName = INVTEXT("Vertical (Up)");
	}
};

/// Builds a vertical attachment with the vertical input at the top.
UCLASS()
class VERTICALLOGISTICSQOL_API UVLQoLVerticalDownBuildMode : public UFGHologramBuildModeDescriptor
{
	GENERATED_BODY()
public:
	UVLQoLVerticalDownBuildMode()
	{
		mDisplayName = INVTEXT("Vertical (Down)");
	}
};
