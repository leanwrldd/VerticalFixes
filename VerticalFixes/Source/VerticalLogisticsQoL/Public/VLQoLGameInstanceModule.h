#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "VLQoLGameInstanceModule.generated.h"

class AFGConveyorAttachmentHologram;
struct FStreamableHandle;
class UFGRecipe;

UCLASS()
class VERTICALLOGISTICSQOL_API UVLQoLGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	static UVLQoLGameInstanceModule* Get(UObject* worldContext);
	static UVLQoLGameInstanceModule* Get(UWorld* world);

	/// Gets the recipe for the vertical version of the given recipe.
	/// Returns null if the recipe doesn't represent a regular conveyor attachment.
	TSubclassOf<UFGRecipe> GetVerticalConveyorAttachmentRecipe(TSubclassOf<UFGRecipe> recipe) const
	{
		return RegularToVerticalRecipeMap.FindRef(recipe);
	}

	/// Gets the recipe for the regular (non-vertical) version of the given recipe.
	/// Returns null if the recipe doesn't represent a vertical conveyor attachment.
	TSubclassOf<UFGRecipe> GetRegularConveyorAttachmentRecipe(TSubclassOf<UFGRecipe> recipe) const
	{
		return VerticalToRegularRecipeMap.FindRef(recipe);
	}

	// UGameInstanceModule
	virtual void DispatchLifecycleEvent(ELifecyclePhase phase) override;

protected:
	/// Conveyor attachment hologram that the base game uses.
	UPROPERTY(EditDefaultsOnly)
	TSubclassOf<AFGConveyorAttachmentHologram> DefaultConveyorAttachmentHologram;

	/// Class that will replace the default conveyor attachment hologram.
	UPROPERTY(EditDefaultsOnly)
	TSubclassOf<AFGConveyorAttachmentHologram> HookConveyorAttachmentHologram;

private:
	void FinishSetup(const FStreamableHandle* loadRequest);

	UPROPERTY()
	TArray<TObjectPtr<UObject>> CDOEdits;

	UPROPERTY()
	TMap<TSubclassOf<UFGRecipe>, TSubclassOf<UFGRecipe>> RegularToVerticalRecipeMap;

	UPROPERTY()
	TMap<TSubclassOf<UFGRecipe>, TSubclassOf<UFGRecipe>> VerticalToRegularRecipeMap;
};
