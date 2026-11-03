#pragma once

#include "CoreMinimal.h"
#include "FGConstructDisqualifier.h"
#include "VLQoLConstructDisqualifiers.generated.h"

/// Snapped to a lift that's flowing in the wrong direction.
UCLASS()
class VERTICALLOGISTICSQOL_API UVLQoLCDWrongLiftDirection : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	UVLQoLCDWrongLiftDirection()
	{
		mDisqfualifyingText = INVTEXT("Wrong lift direction!");
	}
};
