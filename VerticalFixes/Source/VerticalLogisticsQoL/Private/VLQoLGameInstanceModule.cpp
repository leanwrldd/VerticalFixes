#include "VLQoLGameInstanceModule.h"

#include "Algo/Copy.h"
#include "Algo/Transform.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Components/MeshComponent.h"
#include "Engine/AssetManager.h"
#include "FGFactoryConnectionComponent.h"
#include "Hologram/FGConveyorAttachmentHologram.h"
#include "Module/GameInstanceModuleManager.h"
#include "VerticalLogisticsQoL.h"

UVLQoLGameInstanceModule* UVLQoLGameInstanceModule::Get(UObject* worldContext)
{
	if (worldContext != nullptr)
	{
		return Get(worldContext->GetWorld());
	}
	return nullptr;
}

UVLQoLGameInstanceModule* UVLQoLGameInstanceModule::Get(UWorld* world)
{
	if (world != nullptr)
	{
		if (UGameInstance* gameInstance = world->GetGameInstance())
		{
			if (auto* manager = gameInstance->GetSubsystem<UGameInstanceModuleManager>())
			{
				auto* module = manager->FindModule(UE_MODULE_NAME);
				return Cast<UVLQoLGameInstanceModule>(module);
			}
		}
	}
	return nullptr;
}

namespace
{

/// Kicks off an async load request for all subclasses of the given class.
static TSharedPtr<FStreamableHandle> LoadDerivedClasses(const UClass* rootClass)
{
	TArray<FSoftObjectPath> softPaths;
	{
		TSet<FTopLevelAssetPath> classNames;
		IAssetRegistry::Get()->GetDerivedClassNames({rootClass->GetClassPathName()}, {}, classNames);
		softPaths.Reserve(classNames.Num());
		Algo::Transform(classNames, softPaths,
			[](auto&& name) { return FSoftObjectPath(Forward<decltype(name)>(name)); });
	}
	return UAssetManager::GetStreamableManager().RequestAsyncLoad(MoveTemp(softPaths));
}

/// Counts the number components in the hierarchy that are derived from the given class.
struct CountComponents
{
	int32 rootCount = 0;
	int32 childCount = 0;
	TSet<const UClass*, DefaultKeyFuncs<const UClass*>, TInlineSetAllocator<8>> seenTypes;

	CountComponents(const UBlueprintGeneratedClass* actorClass, const UClass* componentClass)
	{
		const bool isSceneComponent = componentClass->IsChildOf<USceneComponent>();

		// Count native components.
		{
			auto* actor = actorClass->GetDefaultObject<const AActor>();
			actor->ForEachComponent(false,
				[=, this, rootComponent = actor->GetRootComponent()](UActorComponent* component)
				{
					if (!component->IsA(componentClass))
						return;

					const bool isAttachedToRoot =
						!isSceneComponent
						|| static_cast<USceneComponent*>(component)->GetAttachParent() == rootComponent;

					++(isAttachedToRoot ? rootCount : childCount);
					seenTypes.Add(component->GetClass());
				});
		}

		// Count blueprint components.
		UBlueprintGeneratedClass::ForEachGeneratedClassInHierarchy(actorClass,
			[=, this](const UBlueprintGeneratedClass* actorClass)
			{
				if (const USimpleConstructionScript* simpleConstructionScript = actorClass->SimpleConstructionScript)
				{
					TSet<const USCS_Node*, DefaultKeyFuncs<const USCS_Node*>, TInlineSetAllocator<64>> rootNodes;
					rootNodes.Reserve(simpleConstructionScript->GetRootNodes().Num());
					Algo::Copy(simpleConstructionScript->GetRootNodes(), rootNodes);

					for (const USCS_Node* node : simpleConstructionScript->GetAllNodes())
					{
						if (!node->ComponentClass->IsChildOf(componentClass))
							continue;

						++(rootNodes.Contains(node) ? rootCount : childCount);
						seenTypes.Add(node->ComponentClass);
					}
				}
				return true;
			});
	}
};

} // namespace

void UVLQoLGameInstanceModule::DispatchLifecycleEvent(ELifecyclePhase phase)
{
	Super::DispatchLifecycleEvent(phase);

	if (!WITH_EDITOR && phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		// Dynamically discover (and patch) all of the conveyor attachment types.
		UClass* rootClass = AFGBuildableConveyorAttachment::StaticClass();
		if (TSharedPtr<FStreamableHandle> loadHandle = LoadDerivedClasses(rootClass))
		{
			if (loadHandle->HasLoadCompleted())
			{
				FinishSetup(loadHandle.Get());
			}
			else
			{
				loadHandle->BindCompleteDelegate(FStreamableDelegate::CreateUObject(
					this,
					&UVLQoLGameInstanceModule::FinishSetup,
					const_cast<const FStreamableHandle*>(loadHandle.Get())));
			}
		}
	}
}

void UVLQoLGameInstanceModule::FinishSetup(const FStreamableHandle* loadRequest)
{
	check(CDOEdits.IsEmpty());

	struct OverrideInfo { UClass* recipeClass; UClass* buildableClass; };
	TArray<TPair<UClass* /* buildableClass */, OverrideInfo>, TInlineAllocator<64>> floorToLiftOverrides;
	TMap<UClass* /* buildableClass */, OverrideInfo, TInlineSetAllocator<64>> liftToFloorOverrides;

	// Parse the hologram overrides on each of the buildables, ignoring any that wouldn't work with our
	// custom hologram implementation.
	loadRequest->ForEachLoadedAsset([&](UObject* asset)
	{
		auto* buildableClass = Cast<UBlueprintGeneratedClass>(asset);
		if (buildableClass == nullptr)
			return;
		auto* buildable = Cast<AFGBuildableConveyorAttachment>(buildableClass->GetDefaultObject());
		if (buildable == nullptr)
			return;

		if (UClass* hologramClass = buildable->mHologramClass)
		{
			// Leave the buildable alone if it has a non-default hologram class, as that's probably a custom
			// hologram from another mod that we shouldn't touch.
			if (hologramClass != DefaultConveyorAttachmentHologram
				&& hologramClass != AFGConveyorAttachmentHologram::StaticClass())
			{
				UE_LOG(LogVerticalLogisticsQoL, Log,
					TEXT("Skipping %s because it uses non-default hologram class %s."),
					*buildableClass->GetName(), *hologramClass->GetName());
				return;
			}
		}
		else
		{
			// Buildables without a hologram are probably abstract and therefore not interesting.
			UE_LOG(LogVerticalLogisticsQoL, Verbose,
				TEXT("Skipping %s because it doesn't have a hologram class."),
				*buildableClass->GetName());
			return;
		}

		// If the buildable doesn't have exactly four factory connections then it probably isn't a normal
		// merger or splitter and therefore isn't something that we can handle.
		{
			constexpr int32 expectedConnectionCount = 4;
			const CountComponents connectionCount(buildableClass, UFGConnectionComponent::StaticClass());

			switch (connectionCount.seenTypes.Num())
			{
			case 0:
				UE_LOG(LogVerticalLogisticsQoL, Log,
					TEXT("Skipping %s because it doesn't have any connection components."),
					*buildableClass->GetName());
				return;
			case 1:
				if (!(*connectionCount.seenTypes.CreateConstIterator())->IsChildOf<UFGFactoryConnectionComponent>())
				{
				default:
					UE_LOG(LogVerticalLogisticsQoL, Log,
						TEXT("Skipping %s because it has non-factory connection components."),
						*buildableClass->GetName());
					return;
				}
				if (connectionCount.childCount != 0)
				{
					UE_LOG(LogVerticalLogisticsQoL, Log,
						TEXT("Skipping %s because it has non-root factory connection components."),
						*buildableClass->GetName());
					return;
				}
				if (connectionCount.rootCount != expectedConnectionCount)
				{
					UE_LOG(LogVerticalLogisticsQoL, Log,
						TEXT("Skipping %s because it has an unsupported factory connection count; expected %i, found %i."),
						*buildableClass->GetName(), expectedConnectionCount, connectionCount.rootCount);
					return;
				}
			}
		}

		// Our hologram class can only handle meshes if they're attached to the root. This isn't relevant
		// for any of the base game conveyor attachments because they use abstract instances for their
		// meshes, but some mods don't.
		{
			const CountComponents meshCount(buildableClass, UMeshComponent::StaticClass());

			if (meshCount.childCount != 0)
			{
				UE_LOG(LogVerticalLogisticsQoL, Log,
					TEXT("Skipping %s because it has non-root UMeshComponent instances."),
					*buildableClass->GetName());
				return;
			}
		}

		const UFGHologramOverride* hologramOverride = nullptr;

		// We only know how to deal with buildables that have a single override, but the game treats null
		// overrides as if they don't exist so we should try to support that too.
		for (const UFGHologramOverride* currentHologramOverride : buildable->mHologramOverrides)
		{
			if (currentHologramOverride != nullptr)
			{
				if (hologramOverride == nullptr)
				{
					hologramOverride = currentHologramOverride;
				}
				else
				{
					UE_LOG(LogVerticalLogisticsQoL, Log,
						TEXT("Skipping %s because it has more than one hologram override."),
						*buildableClass->GetName());
					return;
				}
			}
		}

		if (hologramOverride == nullptr)
		{
			// No overrides means no vertical version, which doesn't interest us.
			UE_LOG(LogVerticalLogisticsQoL, Verbose,
				TEXT("Skipping %s because it doesn't have any hologram overrides."),
				*buildableClass->GetName());
			return;
		}

		UClass* hologramOverrideClass = hologramOverride->GetClass();
		UClass* overrideRecipeClass = hologramOverride->GetHologramOverrideWithoutChecks();
		bool isRegularAttachment = false;

		// Validate the override type, the game only has these two at the time of writing.
		if (hologramOverrideClass == UFGHologramOverride_ConveyorAttachment_FloorToLift::StaticClass())
		{
			isRegularAttachment = true;
		}
		else if (hologramOverrideClass != UFGHologramOverride_ConveyorAttachment_LiftToFloor::StaticClass())
		{
			UE_LOG(LogVerticalLogisticsQoL, Log,
				TEXT("Skipping %s because it has an unknown hologram override class %s."),
				*buildableClass->GetName(), *hologramOverrideClass->GetName());
			return;
		}

		UClass* overrideBuildableClass = AFGBuildable::GetBuildableClassFromRecipe(overrideRecipeClass);

		// By default you can technically have a hologram override that points to any kind of buildable, but
		// we're only expecting the case where you go to/from a vertical attachment.
		if (overrideBuildableClass == nullptr || !overrideBuildableClass->IsChildOf<AFGBuildableConveyorAttachment>())
		{
			UE_LOG(LogVerticalLogisticsQoL, Log,
				TEXT("Skipping %s because its hologram override points to %s, which isn't an AFGBuildableConveyorAttachment."),
				*buildableClass->GetName(), overrideBuildableClass ? *overrideBuildableClass->GetName() : TEXT("null"));
			return;
		}

		const OverrideInfo overrideInfo
		{
			.recipeClass = overrideRecipeClass,
			.buildableClass = overrideBuildableClass,
		};

		if (isRegularAttachment)
		{
			floorToLiftOverrides.Emplace(buildableClass, overrideInfo);
		}
		else
		{
			liftToFloorOverrides.Add(buildableClass, overrideInfo);
		}
	});

	// Reserve memory.
	{
		const int32 count = floorToLiftOverrides.Num();
		CDOEdits.Reserve(count);
		RegularToVerticalRecipeMap.Reserve(count);
		VerticalToRegularRecipeMap.Reserve(count);
	}

	// Match up the regular/vertical versions and apply the hologram patch.
	for (auto [buildableClass, verticalInfo] : floorToLiftOverrides)
	{
		const OverrideInfo* regularInfo = liftToFloorOverrides.Find(verticalInfo.buildableClass);

		// Validate the consistency of the regular/vertical pair.
		if (regularInfo == nullptr)
		{
			UE_LOG(LogVerticalLogisticsQoL, Log,
				TEXT("Skipping %s because its vertical version doesn't point back to itself."),
				*buildableClass->GetName())
			continue;
		}
		else if (regularInfo->buildableClass != buildableClass)
		{
			UE_LOG(LogVerticalLogisticsQoL, Log,
				TEXT("Skipping %s because its vertical version points back to unrelated class %s."),
				*buildableClass->GetName(), *regularInfo->buildableClass->GetName())
			continue;
		}

		UE_LOG(LogVerticalLogisticsQoL, Log,
			TEXT("Overriding the hologram for %s, detected vertical version is %s."),
			*buildableClass->GetName(), *verticalInfo.buildableClass->GetName());

		// Write our hologram class into the buildable's CDO.
		{
			auto* buildable = buildableClass->GetDefaultObject<AFGBuildableConveyorAttachment>();
			buildable->mHologramClass = HookConveyorAttachmentHologram;
			CDOEdits.Add(buildable);
		}

		// Cache the mapping between the regular/vertical classes.
		RegularToVerticalRecipeMap.Add(regularInfo->recipeClass, verticalInfo.recipeClass);
		VerticalToRegularRecipeMap.Add(verticalInfo.recipeClass, regularInfo->recipeClass);
	}
}
