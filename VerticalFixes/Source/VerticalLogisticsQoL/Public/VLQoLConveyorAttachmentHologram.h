#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGConveyorAttachmentHologram.h"
#include "VLQoLConveyorAttachmentHologram.generated.h"

class AFGBuildableConveyorAttachment;
enum class EFactoryConnectionDirection : uint8;
class UFGFactorySettings;
class UStaticMeshComponent;

UENUM()
enum class EVLQoLConveyorAttachmentMode : uint8
{
	Regular,
	VerticalAuto,
	VerticalUp,
	VerticalDown,
};

/// A conveyor attachment hologram that has build modes that let you build the vertical versions
/// without snapping to a lift.
///
/// The base implementation uses the hologram override system when it wants to switch to the
/// vertical version, which will nuke the entire hologram and spin up a new one with a different
/// recipe. This is problematic for us because build modes are a property of the hologram, so
/// there'd be a circular dependency if we try to use build modes to change the hologram. Instead
/// we're keeping the same hologram but are switching the meshes and other internal state when
/// changing what kind of attachment we're building, this makes it look like a new hologram but
/// really it's the same. The main caveat is that we can't change the recipe on the fly, as that's
/// stored in the build gun, so we keep the recipe the same right up until construction where we
/// construct using the actual recipe that we want to use.
UCLASS()
class VERTICALLOGISTICSQOL_API AVLQoLConveyorAttachmentHologram : public AFGConveyorAttachmentHologram
{
	GENERATED_BODY()

public:
	AVLQoLConveyorAttachmentHologram();

	// AFGHologram
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const override;
	virtual void OnBuildModeChanged(TSubclassOf<UFGHologramBuildModeDescriptor> buildMode) override;
	virtual void CheckValidFloor() override;
	virtual void PostHologramPlacement(const FHitResult& hitResult, bool callForChildren) override;
	virtual TOptional<TSubclassOf<UFGRecipe>> ProcessHologramOverride(const FHitResult& hitResult) const override;
	virtual TArray<FItemAmount> GetBaseCost() const override;
	virtual void PostConstructMessageDeserialization() override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void ConfigureComponents(AFGBuildable* inBuildable) const override;

protected:
	void SetConveyorAttachmentMode(EVLQoLConveyorAttachmentMode newMode);

	UFUNCTION()
	void OnRep_ConveyorAttachmentMode();

	/// A version of the build mode that is replicated to other clients.
	UPROPERTY(ReplicatedUsing = OnRep_ConveyorAttachmentMode, CustomSerialization)
	EVLQoLConveyorAttachmentMode mConveyorAttachmentMode;

	/// The "real" recipe switches between the regular/vertical versions, whereas the recipe that the
	/// hologram reports stays the same so that it doesn't go out of sync with the build gun.
	UPROPERTY()
	TSubclassOf<UFGRecipe> mRealRecipe;

	/// Cached buildable from mRealRecipe.
	UPROPERTY()
	TSubclassOf<AFGBuildableConveyorAttachment> mRealBuildClass;

	/// Cached vertical version of the recipe, shouldn't change after spawn.
	UPROPERTY()
	TSubclassOf<UFGRecipe> mVerticalRecipe;

private:
	static constexpr int32 CONNECTION_COUNT = 4;

	EVLQoLConveyorAttachmentMode CalculateAutoMode() const;
	bool UpdateRecipe();
	void UpdateClearance();
	void UpdateHologramComponents(const UFGFactorySettings* settings);
	void UpdateVerticalConnections(const UFGFactorySettings* settings);
	void SetConnectionDirection(int32 connectionIndex, EFactoryConnectionDirection direction, const UFGFactorySettings* settings);
	static void UpdateArrowMesh(UStaticMeshComponent* arrowMesh, EFactoryConnectionDirection direction, const UFGFactorySettings* settings);

	UStaticMeshComponent* mArrowMeshes[CONNECTION_COUNT] = {};
	int32 mBottomConnectionIndex = -1;
	int32 mTopConnectionIndex = -1;
};
