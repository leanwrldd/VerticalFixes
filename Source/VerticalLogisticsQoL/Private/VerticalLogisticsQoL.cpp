#include "VerticalLogisticsQoL.h"

#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildablePassthrough.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "Hologram/FGConveyorAttachmentHologram.h"
#include "Hologram/FGConveyorLiftHologram.h"
#include "Hologram/FGPipelineHologram.h"
#include "Hologram/FGPipelinePoleHologram.h"
#include "Hologram/FGWallAttachmentHologram.h"
#include "Net/UnrealNetwork.h"
#include "Patching/NativeHookManager.h"
#include "VLQoLBuildModes.h"
#include "VLQoLModConfiguration.h"
#include "VLQoLGameInstanceModule.h"

DEFINE_LOG_CATEGORY(LogVerticalLogisticsQoL)

namespace
{

bool IsVerticalConnector(float normalUp)
{
	// We aren't expecting any diagonal connectors, so really this could be closer to 1, but 0.5 is
	// being used to match the checks that the game does in similar places.
	return FMath::Abs(normalUp) > 0.5f;
}

bool CanConnectVertically(const AFGConveyorLiftHologram* lift, const UFGFactoryConnectionComponent* from, const UFGFactoryConnectionComponent* to, double maxVerticalDistance)
{
	static constexpr double HorizontalTolerance = 0.1;

	const FVector toNormal = to->GetConnectorNormal();
	if (!IsVerticalConnector(toNormal.Z))
		return false;	// Not a vertical connection, not our problem.
	const FVector fromLocation = from->GetConnectorLocation();
	const FVector toLocation = to->GetConnectorLocation();
	const FVector connectionVector = toLocation - fromLocation;
	if (!FVector2D(connectionVector).IsNearlyZero(HorizontalTolerance))
		return false;	// Not aligned horizontally with the lift.
	if (FMath::Abs(connectionVector.Z) > maxVerticalDistance)
		return false;	// Too far away vertically.
	if ((toNormal.Z >= 0.0f) == (toLocation.Z >= lift->GetActorLocation().Z))
		return false;	// Wrong side.

	return true;
}

} // namespace

void FVerticalLogisticsQoLModule::StartupModule()
{
	if constexpr (!WITH_EDITOR)
	{
		FixLostPassthroughLinks();
		FixBrokenConnectionsWhenMergingInBlueprintDesigner();
		FixHologramLocking();
		FixLiftOnAttachmentOffByHalf();
		FixAttachmentOnLiftOffByHalf();
		FixClearanceWarnings();
		FixMassDismantleVerticalAttachmentAndLifts();
		FixReverseLiftConnectionFromSnapPoint();
		AllowConnectionToExistingAttachment();
		HideLiftArrowWhenSnappedTopToAttachment();
		PrepareCustomAttachmentHologram();
		AddPipeBetterStraightBuildMode();
		NetworkVerticalAttachmentFlowDirection();
		NetworkLiftMeshRotationFlag();
	}
}

void FVerticalLogisticsQoLModule::ShutdownModule()
{
}

void FVerticalLogisticsQoLModule::FixLostPassthroughLinks()
{
	// When the game merges and splits lifts, which can happen when adding and removing conveyor
	// attachments, it will correctly transfer snapped passthroughs from the old lifts to the new lifts.
	// However it forgets to update the connections on the passthroughs, which means that they don't
	// think that they have anything snapped to them.

	const class
	{
	public:
		// Duplicate
		void operator()(AFGBuildableConveyorLift* result, AFGBuildableConveyorLift* originalLift, bool dismantleOriginalLift) const
		{
			if (dismantleOriginalLift)
			{
				Repair(result);
			}
		}

		// Merge
		void operator()(AFGBuildableConveyorLift* result, const TArray<AFGBuildableConveyorLift*>& lifts) const
		{
			Repair(result);
		}

		// Split
		void operator()(const TArray<AFGBuildableConveyorLift*>& result, AFGBuildableConveyorLift* lift, float offset) const
		{
			for (AFGBuildableConveyorLift* newLift : result)
			{
				Repair(newLift);
			}
		}

	private:
		static void Repair(AFGBuildableConveyorLift* lift)
		{
			if (lift == nullptr)
				return;

			const auto& snappedPassthroughs = lift->mSnappedPassthroughs;

			if (snappedPassthroughs.Num() != 2)
				return;	// Shouldn't ever happen?
			if (snappedPassthroughs[0] == nullptr && snappedPassthroughs[1] == nullptr)
				return;	// Not snapped to any passthroughs.

			// Don't do anything if the passthroughs do in fact link back to the lift, as that probably means
			// that we're running on a game version where the original bug has been fixed.
			for (AFGBuildablePassthrough* passthrough : snappedPassthroughs)
			{
				if (passthrough != nullptr)
				{
					if (UFGConnectionComponent* top = passthrough->mTopSnappedConnection)
					{
						if (top->GetOwner() == lift)
							return;
					}
					if (UFGConnectionComponent* bottom = passthrough->mBottomSnappedConnection)
					{
						if (bottom->GetOwner() == lift)
							return;
					}
				}
			}

			UFGFactoryConnectionComponent* connectionComponents[] =
			{
				lift->GetConnection0(),
				lift->GetConnection1(),
			};

			for (int i = 0; i != 2; ++i)
			{
				AFGBuildablePassthrough* passthrough = snappedPassthroughs[i];

				if (passthrough == nullptr)
					continue;

				UFGFactoryConnectionComponent* connection = connectionComponents[i];
				UFGFactoryConnectionComponent* oppositeConnection = connectionComponents[!i];

				// Use the vertical position to infer which side of the passthrough it must be snapped to.
				if (passthrough->GetActorLocation().Z < oppositeConnection->GetComponentLocation().Z)
				{
					passthrough->SetTopSnappedConnection(connection);
				}
				else
				{
					passthrough->SetBottomSnappedConnection(connection);
				}
			}
		}
	} hook;

	SUBSCRIBE_METHOD_AFTER(AFGBuildableConveyorLift::DuplicateLift, hook);
	SUBSCRIBE_METHOD_AFTER(AFGBuildableConveyorLift::Merge, hook);
	SUBSCRIBE_METHOD_AFTER(AFGBuildableConveyorLift::Split, hook);
}

void FVerticalLogisticsQoLModule::FixBrokenConnectionsWhenMergingInBlueprintDesigner()
{
	// When merging conveyor lifts, which can happen when deleting a vertical attachment, the game will
	// set up the connections before it tells the lift about the blueprint designer. This causes the
	// connections to fail when in a blueprint designer because it doesn't know that the lift is in
	// there too and so it blocks the connections. The correct way to do this is to make sure that
	// SetInsideBlueprintDesigner happens before the SetConnection calls, The game does this properly
	// for splitting but not merging for some reason.

	struct PendingMerge { UFGFactoryConnectionComponent* input; UFGFactoryConnectionComponent* output; };
	thread_local const PendingMerge* t_pendingMerge;

	SUBSCRIBE_UOBJECT_METHOD(AFGBuildableConveyorLift, BeginPlay,
		[](auto& scope, AFGBuildableConveyorLift* lift)
		{
			// If this lift is being constructed as part of a merge operation, then we need to fix up the
			// connections that would've been rejected earlier. Unfortunately this needs to happen before
			// BeginPlay so that the lift gets the right meshes and clearance, but there's nothing that we can
			// hook between Merge and BeginPlay so we'll have to hook BeginPlay for all lifts. It's only a
			// thread-local read and one branch for non-merge lifts so it should be insignificant enough.
			const PendingMerge* pendingMerge = t_pendingMerge;
			if (UNLIKELY(pendingMerge != nullptr))
			{
				t_pendingMerge = nullptr;
				if (UFGFactoryConnectionComponent* input = pendingMerge->input; IsValid(input))
				{
					UFGFactoryConnectionComponent* connection = lift->GetConnection0();
					if (connection->GetConnection() == nullptr)
						connection->SetConnection(input);
				}
				if (UFGFactoryConnectionComponent* output = pendingMerge->output; IsValid(output))
				{
					UFGFactoryConnectionComponent* connection = lift->GetConnection1();
					if (connection->GetConnection() == nullptr)
						connection->SetConnection(output);
				}
			}
		});

	SUBSCRIBE_METHOD(AFGBuildableConveyorLift::Merge,
		([](auto& scope, const TArray<AFGBuildableConveyorLift*>& lifts)
		{
			if (lifts.Num() != 2)
				return;

			AFGBuildableConveyorLift* lift0 = lifts[0];
			AFGBuildableConveyorLift* lift1 = lifts[1];

			if (lift0 == nullptr || lift1 == nullptr)
				return;

			// If the lifts aren't in the same blueprint designer, then it isn't our problem.
			{
				const AFGBuildableBlueprintDesigner* designer = lift0->GetBlueprintDesigner();
				if (designer == nullptr || designer != lift1->GetBlueprintDesigner())
					return;
			}

			UFGFactoryConnectionComponent* input = nullptr;
			UFGFactoryConnectionComponent* output = nullptr;

			// Figure out what the input and output for the new lift will be.
			{
				UFGFactoryConnectionComponent* lift0Input = lift0->GetConnection0()->GetConnection();
				UFGFactoryConnectionComponent* lift0Output = lift0->GetConnection1()->GetConnection();
				UFGFactoryConnectionComponent* lift1Input = lift1->GetConnection0()->GetConnection();
				UFGFactoryConnectionComponent* lift1Output = lift1->GetConnection1()->GetConnection();

				if (lift0Input != nullptr && lift0Input->GetOuterBuildable() == lift1)
				{
					// Lift 0 is on the output side.
					input = lift1Input;
					output = lift0Output;
				}
				else if (lift0Output != nullptr && lift0Output->GetOuterBuildable() == lift1)
				{
					// Lift 0 is on the input side.
					input = lift0Input;
					output = lift1Output;
				}
				else
				{
					return;
				}
			}

			const PendingMerge pendingMerge = { .input = input, .output = output, };
			t_pendingMerge = &pendingMerge;
			scope(lifts);
			t_pendingMerge = nullptr;
		}));
}

void FVerticalLogisticsQoLModule::FixHologramLocking()
{
	// The hologram throws away its snap state in PreHologramPlacement on the assumption that it'll be
	// followed by a call to TrySnapToActor to re-calculate the state, however that won't happen when
	// the hologram is locked so it just ends up forgetting what it's snapped to.

	SUBSCRIBE_UOBJECT_METHOD(AFGConveyorAttachmentHologram, PreHologramPlacement,
		[](auto& scope, AFGConveyorAttachmentHologram* hologram, const FHitResult& hitResult, bool callForChildren)
		{
			if (!hologram->IsHologramLocked())
				return;

			// The hologram is locked, make sure that the snap state is restored.

			const auto upgradedConveyorAttachment = hologram->mUpgradedConveyorAttachment;
			const auto snappedConveyor = hologram->mSnappedConveyor;
			const auto snappedConveyorOffset = hologram->mSnappedConveyorOffset;

			scope(hologram, hitResult, callForChildren);

			if (hologram->mUpgradedConveyorAttachment == nullptr)
			{
				hologram->mUpgradedConveyorAttachment = upgradedConveyorAttachment;
			}

			if (hologram->mSnappedConveyor == nullptr)
			{
				hologram->mSnappedConveyor = snappedConveyor;
				hologram->mSnappedConveyorOffset = snappedConveyorOffset;
			}
		});
}

// PORT 1.2 BISECT SWITCH.
// Set to 1 to re-enable the UpdateTopTransform hook.
//
// Sibling of VLQOL_ENABLE_ATTACHMENT_OFFSET_FIX below: both compensate for the game adding
// an extra 0.5m before calculating steps for vertical connections. 1.2 made attachment
// placement on lifts freeform, which appears to have retired that behaviour, so the
// compensation is now applied on top of an already-correct result.
//
// Symptom with this hook active: the top ~0.5m of a lift connecting to an attachment is
// invisible. mTopTransform is shifted up by 50uu AFTER the original has generated the
// lift, so the mesh stops 0.5m short of where the lift now ends.
#define VLQOL_ENABLE_LIFT_TOP_TRANSFORM_FIX 1

void FVerticalLogisticsQoLModule::FixLiftOnAttachmentOffByHalf()
{
#if VLQOL_ENABLE_LIFT_TOP_TRANSFORM_FIX
	// If the base of the lift isn't aligned to 1m, then the extra offset should be applied after
	// rounding to the step height so that you don't just round away the offset. The game does this
	// properly for passthroughs, but for vertical connections it adds the extra 0.5m before calculating
	// the steps. We can't easily patch the dodgy bit of code that deals with vertical connections in
	// this function, so we'll have to hackily hide the connection from it before calling so that it
	// skips that code and then we can manually fix it up afterwards.

	SUBSCRIBE_METHOD(AFGConveyorLiftHologram::UpdateTopTransform,
		[](auto& scope, AFGConveyorLiftHologram* hologram, const FHitResult& hitResult, const FRotator& rotation)
		{
			if (hologram->mSnappedPassthroughs[0] != nullptr)
				return;	// Snapped to a passthrough.
			UFGFactoryConnectionComponent* connection = hologram->mSnappedConnectionComponents[0];
			if (connection == nullptr)
				return;	// Not snapped to a connection.
			const float normalUp = connection->GetConnectorNormal().Z;
			if (!IsVerticalConnector(normalUp))
				return;	// The snapped connection isn't vertical.

			// The original function uses 2.5m and 3.5m for vertical connections, but we've taken off the extra
			// 0.5m and will apply it afterwards.
			const float minimumHeight = normalUp >= 0.0f ? 200.0f : 300.0f;
			const float origMinimumHeight = hologram->mMinimumHeight;

			// Call the original function as if there's no connection.
			{
				hologram->mSnappedConnectionComponents[0] = nullptr;
				hologram->mMinimumHeight = minimumHeight;

				scope(hologram, hitResult, rotation);

				hologram->mSnappedConnectionComponents[0] = connection;
				hologram->mMinimumHeight = origMinimumHeight;
			}

			// Add the 0.5m back on.
			FVector top = hologram->mTopTransform.GetLocation();
			top.Z += top.Z >= 0.0f ? 50.0f : -50.0f;
			hologram->mTopTransform.SetLocation(top);

			// The lift's meshes are generated inside the original call, from the transform as
			// it stood then. Moving the transform afterwards leaves the geometry 0.5m short,
			// which shows in game as an invisible segment where the lift meets the attachment.
			// mTopTransform is ReplicatedUsing=OnRep_TopTransform, so that handler is the
			// game's own "transform changed, rebuild from it" path - run it to resync.
			hologram->OnRep_TopTransform();
		});
#endif
}

// PORT 1.2 BISECT SWITCH.
// Set to 1 to re-enable the FindOffsetClosestToLocation hook.
//
// With this hook active, 1.2 crashes during hologram placement:
//   EXCEPTION_ACCESS_VIOLATION writing 0x1
//   AFGBuildableConveyorLift::GetLocationAndDirectionAtOffset()
//   <- this lambda, inside the scope() call to the original
//   <- AFGConveyorAttachmentHologram::CheckValidPlacement()
// The fault is in stock game code, reached before any of our logic runs, so it cannot be
// fixed by guarding our own code.
//
// Note the "Looks like the bug has been fixed?" escape hatch below: if vanilla 1.2 now
// rounds this offset correctly, the right outcome is deleting this hook, not repairing it.
#define VLQOL_ENABLE_ATTACHMENT_OFFSET_FIX 0

void FVerticalLogisticsQoLModule::FixAttachmentOnLiftOffByHalf()
{
#if VLQOL_ENABLE_ATTACHMENT_OFFSET_FIX
	// The offset used for placing attachment on lifts doesn't account for the extra length added by
	// vertical connections, which can push it off grid.

	SUBSCRIBE_UOBJECT_METHOD(AFGBuildableConveyorLift, FindOffsetClosestToLocation,
		[](auto& scope, const AFGBuildableConveyorLift* lift, const FVector& location)
		{
			const float offset = scope(lift, location);
			if (FMath::RoundToInt(offset) % 100 != 0)
				return;	// Looks like the bug has been fixed?

			float extraOffset;

			// mSnappedPassthroughs is a plain dynamic array with no size guarantee, and Shipping
			// builds do not bounds-check TArray::operator[]. FixLostPassthroughLinks already
			// guards this; this site did not.
			if (lift->mSnappedPassthroughs.Num() > 0)
			if (const AFGBuildablePassthrough* passthrough = lift->mSnappedPassthroughs[0])
			{
				// The lift starts from the center of the passthrough, which means that there's half of the
				// passthrough's thickness on either side of the lift before we get to the usable bits. When using
				// 1m foundations, half is 0.5m and that throws off the alignment.
				const int halfThickness = FMath::RoundToInt(0.5f * passthrough->mSnappedBuildingThickness);
				const int remainder = halfThickness % 100;
				if (remainder == 0)
					return;
				extraOffset = static_cast<float>(remainder);
			}
			else
			{
				// Vertical connections on a splitter/merger are inset by 0.5m.
				const UFGFactoryConnectionComponent* connection = lift->mConnection0->GetConnection();
				if (connection == nullptr)
					return;	// Not connected to anything.
				if (!IsVerticalConnector(connection->GetConnectorNormal().Z))
					return;	// Not a vertical connection.
				extraOffset = 50.0f;
			}

			scope.Override(offset + FMath::Sign(offset) * extraOffset);
		});
#endif
}

void FVerticalLogisticsQoLModule::FixClearanceWarnings()
{
	// The opposing vertical connections on a conveyor attachment have the same location, it's just the
	// rotation that differs. This means that a lift on the bottom connection has the same location as a
	// lift on the top connection, which triggers the overlap warning. We need to find what's connected
	// to the opposing connection and ignore it when doing clearance checks.

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorLiftHologram, GetIgnoredClearanceActors,
		[](const AFGConveyorLiftHologram* hologram, TSet<AActor*>& ignoredActors)
		{
			for (const UFGFactoryConnectionComponent* connection : hologram->mSnappedConnectionComponents)
			{
				if (connection == nullptr)
					continue;

				const FVector connectorNormal = connection->GetConnectorNormal();
				if (!IsVerticalConnector(connectorNormal.Z))
					continue;
				const FVector connectorLocation = connection->GetConnectorLocation();

				for (auto* otherConnection : TInlineComponentArray<UFGFactoryConnectionComponent*>(connection->GetOuterBuildable()))
				{
					if (otherConnection == connection)
						continue;	// We only care about other connections.
					UFGFactoryConnectionComponent* connectedTo = otherConnection->GetConnection();
					if (connectedTo == nullptr)
						continue;	// Not connected to anything, so there's nothing to clip with.
					if (!otherConnection->GetConnectorLocation().Equals(connectorLocation))
						continue;	// Doesn't have the same location, so it won't clip.
					if (!FVector::Coincident(otherConnection->GetConnectorNormal(), -connectorNormal))
						continue;	// Not going in the opposite direction.
					ignoredActors.Add(connectedTo->GetOuterBuildable());
				}
			}
		});
}

void FVerticalLogisticsQoLModule::FixMassDismantleVerticalAttachmentAndLifts()
{
	// When dismantling a vertical attachment, it will join the top and bottom lifts together and leave
	// that behind. This is the desired behavior when dismantling just the attachment, but if you're
	// trying to mass-dismantle everything together then (if the attachment gets dismantled first)
	// you'll end up with a lift at the end that you don't want. This has been fixed by adding a
	// dismantle dependency from the attachment to its lifts, so if they're getting dismantled together
	// the lifts will go first and there'll therefore be nothing left for the attachment to merge.

	SUBSCRIBE_METHOD_VIRTUAL_AFTER(IFGDismantleInterface::GetDismantleDependencies_Implementation,
		static_cast<const IFGDismantleInterface*>(GetDefault<AFGBuildable>()),
		[](const IFGDismantleInterface* buildable, TArray<AActor*>& out_dismantleDependencies)
		{
			auto* attachment = Cast<AFGBuildableConveyorAttachment>(static_cast<const AFGBuildable*>(buildable));
			if (attachment == nullptr)
				return;

			const FName verticalName1 = AFGConveyorAttachmentHologram::mLiftConnection_Bottom;
			const FName verticalName2 = AFGConveyorAttachmentHologram::mLiftConnection_Top;

			for (auto* connection : TInlineComponentArray<UFGFactoryConnectionComponent*>(attachment))
			{
				const FName name = connection->GetFName();
				if (name != verticalName1 && name != verticalName2)
					continue;
				UFGFactoryConnectionComponent* connectedComponent = connection->GetConnection();
				if (connectedComponent == nullptr)
					continue;
				auto* lift = Cast<AFGBuildableConveyorLift>(connectedComponent->GetOuterBuildable());
				if (lift == nullptr)
					continue;
				out_dismantleDependencies.Add(lift);
			}
		});
}

void FVerticalLogisticsQoLModule::FixReverseLiftConnectionFromSnapPoint()
{
	// Lifts are configured such that their first connection is the input, and their second connection
	// is the output. By default it will connect the input to the first snapped connection, but if
	// that's also an input then it'll use the second snapped connection because you can't connect an
	// input to another input. This works fine in a world where the only option is input or output, but
	// it doesn't account for wall holes and conveyor poles; they could effectively be an input from the
	// perspective of the lift, but their actual type is FCD_SNAP_ONLY so the lift won't do anything
	// special because it sees that it isn't of type FCD_INPUT and therefore assumes that it's an
	// output. If it doesn't swap them then it has the opposite problem where it tries to connect the
	// lift output to a snapped output and that causes the connection to fail, so to help it out we're
	// doing the swapping for it if we find a SNAP_ONLY connection that should really be treated as an
	// input.

	SUBSCRIBE_UOBJECT_METHOD(AFGConveyorLiftHologram, ConfigureComponents,
		[](auto& scope, const AFGConveyorLiftHologram* hologram, AFGBuildable* inBuildable)
		{
			UFGFactoryConnectionComponent* connection0 = hologram->mSnappedConnectionComponents[0];
			if (connection0 == nullptr || connection0->GetDirection() != EFactoryConnectionDirection::FCD_SNAP_ONLY)
				return;
			UFGFactoryConnectionComponent* connection1 = hologram->mSnappedConnectionComponents[1];
			if (connection1 == nullptr || connection1->GetDirection() != EFactoryConnectionDirection::FCD_OUTPUT)
				return;
			auto* mutableHologram = const_cast<AFGConveyorLiftHologram*>(hologram);
			mutableHologram->mSnappedConnectionComponents[0] = connection1;
			mutableHologram->mSnappedConnectionComponents[1] = connection0;
			scope(hologram, inBuildable);
			mutableHologram->mSnappedConnectionComponents[0] = connection0;
			mutableHologram->mSnappedConnectionComponents[1] = connection1;
		});
}

void FVerticalLogisticsQoLModule::AllowConnectionToExistingAttachment()
{
	// Lifts look for connections a bit in front of the top transform, so they're much more likely to
	// find the horizontal connections on an attachment even when going straight down through a
	// vertical connection. If we've found a connection on an attachment, we should check to see if
	// there's a vertical connection that's more suitable to use.

	SUBSCRIBE_METHOD(UFGFactoryConnectionComponent::FindCompatibleOverlappingConnections,
		[](auto& scope, UFGFactoryConnectionComponent* component, const FVector& location, const AActor* priorityActor, float radius)
		{
			UFGFactoryConnectionComponent* result = scope(component, location, priorityActor, radius);
			if (result == nullptr)
				return;

			// Only care about connecting lifts to conveyor attachments.
			auto* lift = Cast<AFGConveyorLiftHologram>(component->GetOwner());
			if (lift == nullptr)
				return;
			auto* attachment = Cast<AFGBuildableConveyorAttachment>(result->GetOuterBuildable());
			if (attachment == nullptr)
				return;

			for (auto* attachmentConnection : TInlineComponentArray<UFGFactoryConnectionComponent*>(attachment))
			{
				if (component->CanSnapTo(attachmentConnection)
					&& CanConnectVertically(lift, component, attachmentConnection, radius))
				{
					// Prioritize vertical connections.
					scope.Override(attachmentConnection);
					break;
				}
			}
		});

	// The base implementation filters out vertical connections because the normal for a lift connection
	// points out horizontally, which it doesn't see as aligned with vertical connections so it doesn't
	// allow them.

	SUBSCRIBE_METHOD(AFGConveyorLiftHologram::CanConnectToConnection,
		[](auto& scope, const AFGConveyorLiftHologram* hologram, UFGFactoryConnectionComponent* from, UFGFactoryConnectionComponent* to)
		{
			if (scope(hologram, from, to))
				return;	// Already handled.
			if (from == nullptr || to == nullptr)
				return;	// Nothing to connect.
			const float maxVerticalDistance = hologram->mStepHeight + to->GetConnectorClearance();
			if (CanConnectVertically(hologram, from, to, maxVerticalDistance))
				scope.Override(true);
		});
}

void FVerticalLogisticsQoLModule::HideLiftArrowWhenSnappedTopToAttachment()
{
	// When the top of the lift is snapped to a vertical attachment, the arrow still points out
	// horizontally even though the flow is vertical. It doesn't add much anyway, since the attachment
	// has arrows of its own, so we're hiding it to avoid the confusion.

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorLiftHologram, PostHologramPlacement,
		[](AFGConveyorLiftHologram* hologram, const FHitResult& hitResult, bool callForChildren)
		{
			USceneComponent* arrow = hologram->mArrowComponent;
			if (arrow == nullptr)
				return;

			bool showArrow = true;

			if (const UFGFactoryConnectionComponent* connection = hologram->mSnappedConnectionComponents[1])
			{
				const FName name = connection->GetFName();
				if (name == AFGConveyorAttachmentHologram::mLiftConnection_Bottom
					|| name == AFGConveyorAttachmentHologram::mLiftConnection_Top)
				{
					showArrow = false;
				}
			}

			arrow->SetVisibility(showArrow);
		});
}

void FVerticalLogisticsQoLModule::PrepareCustomAttachmentHologram()
{
	// Our hologram expects to be given the regular recipe, and it will switch to the vertical one based
	// on the build mode, so we need to make sure that the vertical recipe is never directly used. This
	// can only happen when sampling an existing vertical attachment, as there's no other way to select
	// them in the build menu.

	SUBSCRIBE_METHOD(UFGBuildGunStateBuild::SetActiveRecipe,
		[](auto& scope, UFGBuildGunStateBuild* state, TSubclassOf<UFGRecipe> recipe)
		{
			if (auto* gameInstanceModule = UVLQoLGameInstanceModule::Get(state))
			{
				if (TSubclassOf<UFGRecipe> overrideRecipe = gameInstanceModule->GetRegularConveyorAttachmentRecipe(recipe))
				{
					scope(state, overrideRecipe);
				}
			}
		});
}

// Quarter turns the player has given a Better Straight pipe's free end pole with the scroll wheel, per hologram.
static TMap<TWeakObjectPtr<AFGPipelineHologram>, int32> GPipeEndQuarterTurns;

// The direction a Better Straight pipe's free start pole faced when the end started being placed, per hologram.
// The pole itself is turned to follow the pipe after that, so it can't be read back.
static TMap<TWeakObjectPtr<AFGPipelineHologram>, FVector2D> GPipeStartForward;

// The wall pole a Better Straight pipe ends on, with its wall normal and the axis the pipe passes through it on,
// per hologram, from the last route. Only set when the route was used rather than falling back to auto.
struct FPipeEndWallPole
{
	TWeakObjectPtr<AFGWallAttachmentHologram> Pole;
	FVector WallNormal;
	FVector PassAxis;
};
static TMap<TWeakObjectPtr<AFGPipelineHologram>, FPipeEndWallPole> GPipeEndWallPole;

// The same for a wall pole the pipe starts from, squared to a quarter turn when it was left at 45 degrees.
static TMap<TWeakObjectPtr<AFGPipelineHologram>, FPipeEndWallPole> GPipeStartWallPole;

void FVerticalLogisticsQoLModule::AddPipeBetterStraightBuildMode()
{
	// The game's Straight mode keeps a pipe level, and Horizontal-to-Vertical keeps it level then plumb.
	// Better Straight combines them: every straight run is either level along one of the start's two
	// horizontal axes, or plumb, like Straight extended into the vertical.

	// Whether end i of the pipe is free (on a pole) rather than snapped to a real port. Ending on a wall, the
	// game snaps the pipe to its own child wall pole's connection, which is still a free end for us.
	static const auto isFreePipeEnd = [](const AFGPipelineHologram* hologram, int32 i)
	{
		const UFGPipeConnectionComponentBase* connection = hologram->mSnappedConnectionComponents[i];
		return connection == nullptr || Cast<AFGHologram>(connection->GetOwner()) != nullptr;
	};

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGPipelineHologram, GetSupportedBuildModes_Implementation,
		[](const AFGPipelineHologram* hologram, TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes)
		{
			// With the setting on, our mode takes vanilla Straight's place in the list instead of being added.
			UClass* vanillaStraight = hologram->mBuildModeStraight.Get();
			UClass* betterStraight = UVLQoLPipeBetterStraightBuildMode::StaticClass();
			auto isVanillaStraight = [vanillaStraight](const TSubclassOf<UFGBuildGunModeDescriptor>& mode) { return mode.Get() == vanillaStraight; };
			if (GVLQoLReplaceVanillaStraight && vanillaStraight != nullptr && out_buildmodes.ContainsByPredicate(isVanillaStraight))
			{
				out_buildmodes.Remove(betterStraight);
				out_buildmodes[out_buildmodes.IndexOfByPredicate(isVanillaStraight)] = betterStraight;
			}
			else
			{
				out_buildmodes.AddUnique(betterStraight);
			}
		});

	// The scroll wheel turns the pipe's poles to any angle. While the start is being placed that sets the
	// direction the pipe leaves in, which is what we want. Once the end is being placed it would turn the
	// end pole to any angle, which moves its connector, and with it the end of the route, off the start's
	// axes. Instead it steps the direction the pipe arrives at the end pole through straight on, a quarter
	// turn left and a quarter turn right, which the route below keeps on the start's axes. (A half turn
	// would make a level pipe double back on itself at the end, with no room for a bend.)

	SUBSCRIBE_METHOD_VIRTUAL(AFGPipelineHologram::Scroll, GetDefault<AFGPipelineHologram>(),
		[](auto& scope, AFGPipelineHologram* hologram, int32 delta)
		{
			if (!hologram->IsCurrentBuildMode(UVLQoLPipeBetterStraightBuildMode::StaticClass())
				|| hologram->GetUpgradedActor() != nullptr)
				return;
			const ESplineHologramBuildStep step = hologram->GetCurrentBuildStep();
			if (step == ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding || step == ESplineHologramBuildStep::SHBS_AdjustPole)
			{
				const int32 turns = GPipeEndQuarterTurns.FindOrAdd(hologram) += delta > 0 ? 1 : -1;
				UE_LOG(LogVerticalLogisticsQoL, Display, TEXT("BetterStraight: pipeline scroll %d, end turns now %d"), delta, turns);
				scope.Cancel();
			}
			else if (step == ESplineHologramBuildStep::SHBS_FindStart || step == ESplineHologramBuildStep::SHBS_AdjustStartingPole)
			{
				// A wall pole scrolls in eighth turns, which would start the pipe at 45 degrees. Turn a start on
				// a wall a quarter turn at a time instead.
				AFGWallAttachmentHologram* wallPole = hologram->mChildWallPoleHologram[0];
				if (IsValid(wallPole) && !wallPole->IsHidden())
				{
					const int32 squared = FMath::RoundToInt(wallPole->GetScrollRotateValue() / 90.0) * 90;
					wallPole->SetScrollRotateValue(((squared + (delta > 0 ? 90 : -90)) % 360 + 360) % 360);
					UE_LOG(LogVerticalLogisticsQoL, Display, TEXT("BetterStraight: start wall pole scroll %d, rotation now %d"), delta, wallPole->GetScrollRotateValue());
					scope.Cancel();
				}
			}
		});

	// Ending on a wall, the scroll may go straight to the child wall pole hologram rather than through the
	// pipeline. AFGWallAttachmentHologram doesn't override Scroll, so this hooks AFGHologram::Scroll and
	// filters for a wall pole whose parent is a Better Straight pipe placing its end.

	SUBSCRIBE_METHOD_VIRTUAL(AFGHologram::Scroll, GetDefault<AFGWallAttachmentHologram>(),
		[](auto& scope, AFGHologram* self, int32 delta)
		{
			if (!self->IsA<AFGWallAttachmentHologram>())
				return;
			AFGPipelineHologram* pipeline = Cast<AFGPipelineHologram>(self->GetParentHologram());
			if (pipeline == nullptr || !pipeline->IsCurrentBuildMode(UVLQoLPipeBetterStraightBuildMode::StaticClass())
				|| pipeline->GetUpgradedActor() != nullptr)
				return;
			const ESplineHologramBuildStep step = pipeline->GetCurrentBuildStep();
			if (step == ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding || step == ESplineHologramBuildStep::SHBS_AdjustPole)
			{
				const int32 turns = GPipeEndQuarterTurns.FindOrAdd(pipeline) += delta > 0 ? 1 : -1;
				UE_LOG(LogVerticalLogisticsQoL, Display, TEXT("BetterStraight: wall pole scroll %d, end turns now %d"), delta, turns);
				scope.Cancel();
			}
		});

	// The route can leave a free start pole the opposite way to (or square to) the way it faces, see below,
	// and can reach a free end pole from any side, so the pipe would come out of the back or side of a pole.
	// After every re-route while the end is being placed, turn both free poles to match the ends of the pipe
	// (UpdateSplineComponent has already moved the pipe's connections to the new spline). This is the same
	// call the game makes for the start pole on one of its own paths through GenerateAndUpdateSpline.

	SUBSCRIBE_METHOD_AFTER(AFGPipelineHologram::GenerateAndUpdateSpline,
		[](AFGPipelineHologram* hologram, const FHitResult& hitResult)
		{
			if (!hologram->IsCurrentBuildMode(UVLQoLPipeBetterStraightBuildMode::StaticClass()))
				return;
			if (hologram->GetUpgradedActor() != nullptr)
				return;
			const ESplineHologramBuildStep step = hologram->GetCurrentBuildStep();

			// After routing, the game turns an end wall pole to face the start (projected onto the wall), which
			// tilts it off the scroll rotation and off the route. Turn it back so the pipe passes straight
			// through it along the route.
			if (step == ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding || step == ESplineHologramBuildStep::SHBS_AdjustPole)
			{
				for (const FPipeEndWallPole* endWallPole : { GPipeEndWallPole.Find(hologram), GPipeStartWallPole.Find(hologram) })
				{
					AFGWallAttachmentHologram* wallPole = endWallPole ? endWallPole->Pole.Get() : nullptr;
					if (IsValid(wallPole) && !wallPole->IsHidden())
						wallPole->SetActorRotation(FRotationMatrix::MakeFromXY(endWallPole->WallNormal, endWallPole->PassAxis).Rotator());
				}
			}

			if (step != ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding)
				return;

			for (int32 i = 0; i < 2; ++i)
			{
				if (!isFreePipeEnd(hologram, i))
					continue;
				AFGPipelinePoleHologram* pole = hologram->mChildPoleHologram[i];
				UFGPipeConnectionComponentBase* connection = hologram->mConnectionComponents[i];
				if (IsValid(pole) && !pole->IsHidden() && IsValid(connection))
					pole->SnapToConnection(connection, hologram);
			}
		});

	// The route is a path of corners: from a horizontal start, a run along the start direction, a run
	// across to below/above the end, then a plumb run to the end; from a vertical start, a plumb run to
	// the end's height, then a level run to the end. Runs that would be shorter than a centimetre are
	// dropped, so a level pipe is vanilla Straight's L and a pipe straight ahead is one straight segment.
	// Each corner is then rounded with an arc of the bend radius vanilla Straight uses (mBendRadius, which
	// AutoRouteStraightSpline reads; mBendRadius2D is wider and looked too curvy), shrunk where a run is too
	// short for it; validation still rejects a bend tighter than the game allows.
	//
	// RouteSelectedSplineMode falls back to AutoRouteSpline for modes it doesn't know, so the hook routes
	// our mode itself. GenerateAndUpdateSpline (from PostHologramPlacement) calls UpdateSplineComponent
	// straight after, which rebuilds the meshes and connections, so replacing the spline data here is enough.
	//
	// Hooking SetHologramLocationAndRotation does NOT work: in 1.2 the pipe version only positions the
	// child pole holograms, and the spline is re-routed afterwards in PostHologramPlacement.

	SUBSCRIBE_METHOD_AFTER(AFGPipelineHologram::RouteSelectedSplineMode,
		[](AFGPipelineHologram* hologram, FVector startLocation, FVector startNormal, FVector endLocation, FVector endNormal)
		{
			if (!hologram->IsCurrentBuildMode(UVLQoLPipeBetterStraightBuildMode::StaticClass()))
				return;
			if (hologram->GetUpgradedActor() != nullptr)
				return;	// Replacing an existing pipe, which keeps its own shape.

			// Before the start is placed the hologram is only a preview stub at the cursor (both ends at the
			// same pole), and routing it would turn it into a loop, so leave vanilla's preview alone.
			const ESplineHologramBuildStep step = hologram->GetCurrentBuildStep();
			if (step != ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding && step != ESplineHologramBuildStep::SHBS_AdjustPole)
			{
				// A new pipe starts with the default end direction, and its start direction isn't set yet.
				GPipeEndQuarterTurns.Remove(hologram);
				GPipeStartForward.Remove(hologram);
				GPipeEndWallPole.Remove(hologram);
				GPipeStartWallPole.Remove(hologram);
				return;
			}
			GPipeEndWallPole.Remove(hologram);
			GPipeStartWallPole.Remove(hologram);
			if ((endLocation - startLocation).Size() < 1.0)
				return;

			TArray<FVector> corners;
			corners.Add(startLocation);

			// A pipe hole in a floor, ceiling or wall is a passthrough, which has no pipe connection, so an end
			// snapped into one looks free. It's really a port facing through the hole (startNormal/endNormal),
			// so treat it as snapped: no pole to lead into or turn.
			bool startsInPassthrough = false;
			bool endsInPassthrough = false;
			for (const AFGBuildablePassthrough* passthrough : hologram->mSnappedPassthroughs)
			{
				if (passthrough == nullptr)
					continue;
				const FVector passthroughLocation = passthrough->GetActorLocation();
				if (FVector::Dist(passthroughLocation, endLocation) < FVector::Dist(passthroughLocation, startLocation))
					endsInPassthrough = true;
				else
					startsInPassthrough = true;
			}

			const bool isFreeStart = isFreePipeEnd(hologram, 0) && !startsInPassthrough;

			// A start snapped to a sloped port (the end of a pipe that was still turning) faces neither level
			// nor plumb. Carry on along the port just far enough for a bend of the bend radius to straighten
			// out to the nearest of level or plumb, and route on from there.
			FVector routeStart = startLocation;
			if (!isFreeStart)
			{
				const FVector portDirection = startNormal.GetSafeNormal();
				const FVector2D portHorizontal(portDirection.X, portDirection.Y);
				if (FMath::Abs(portDirection.Z) > 0.01 && portHorizontal.Size() > 0.01)
				{
					const FVector straightened = FMath::Abs(portDirection.Z) > 0.7
						? FVector(0.0, 0.0, FMath::Sign(portDirection.Z))
						: FVector(portHorizontal.GetSafeNormal(), 0.0);
					const double tilt = FMath::Acos(FMath::Clamp(FVector::DotProduct(portDirection, straightened), -1.0, 1.0));
					routeStart = startLocation + portDirection * (hologram->mBendRadius * FMath::Tan(tilt / 2.0));
					corners.Add(routeStart);
				}
			}

			const bool hasHeight = FMath::Abs(endLocation.Z - routeStart.Z) >= 1.0;
			const FVector2D end2D(endLocation.X, endLocation.Y);
			const FVector2D startToEnd = end2D - FVector2D(routeStart);

			// The world grid axis pointing most towards the end.
			FVector2D axisForward = FVector2D::ZeroVector;
			if (FMath::Abs(startToEnd.X) >= FMath::Abs(startToEnd.Y) && FMath::Abs(startToEnd.X) >= 1.0)
				axisForward = FVector2D(FMath::Sign(startToEnd.X), 0.0);
			else if (FMath::Abs(startToEnd.Y) >= 1.0)
				axisForward = FVector2D(0.0, FMath::Sign(startToEnd.Y));

			// A free start on a wall is a wall pole, which can't be turned, and whose connector can really face
			// up or down. A free start elsewhere is a floor pole, whose connector always faces sideways.
			AFGWallAttachmentHologram* startWallPole = hologram->mChildWallPoleHologram[0];
			const bool isWallStart = isFreeStart && IsValid(startWallPole) && !startWallPole->IsHidden();

			const FVector gameStartNormal = startNormal;

			// A wall pole the pipe starts from may have been left at 45 degrees. Its X axis is out of the wall
			// and the pipe runs through it along its Y axis: square that to straight up or down, or level along
			// the wall (on a ceiling or floor, to the nearest world axis), and start the pipe that way. The pole
			// is turned to match after routing, like an end wall pole.
			TOptional<FPipeEndWallPole> squaredStartWallPole;
			FVector startWallNormal = FVector::ZeroVector;
			if (isWallStart)
			{
				const FTransform poleTransform = startWallPole->GetActorTransform();
				const FVector wallNormal = poleTransform.GetUnitAxis(EAxis::X);
				startWallNormal = wallNormal;
				const FVector poleAxis = poleTransform.GetUnitAxis(EAxis::Y);
				FVector squaredAxis;
				if (FMath::Abs(wallNormal.Z) < 0.7)
				{
					const FVector alongWall = FVector::CrossProduct(FVector::UpVector, wallNormal).GetSafeNormal();
					squaredAxis = FMath::Abs(poleAxis.Z) >= 0.5
						? FVector(0.0, 0.0, FMath::Sign(poleAxis.Z))
						: alongWall * FMath::Sign(FVector::DotProduct(poleAxis, alongWall));
				}
				else
				{
					squaredAxis = FMath::Abs(poleAxis.X) >= FMath::Abs(poleAxis.Y)
						? FVector(FMath::Sign(poleAxis.X), 0.0, 0.0)
						: FVector(0.0, FMath::Sign(poleAxis.Y), 0.0);
				}
				// Always start along the pole: like an end wall pole's endNormal, the startNormal the game passes
				// for a wall pole needn't follow the pole.
				if (!squaredAxis.IsZero())
				{
					// The pipe can go through from either side; leave towards the end.
					if (FVector::DotProduct(squaredAxis, endLocation - startLocation) < 0.0)
						squaredAxis = -squaredAxis;
					startNormal = squaredAxis;
					squaredStartWallPole = (FPipeEndWallPole{ startWallPole, wallNormal, squaredAxis });
				}
			}

			const FVector2D startForward = FVector2D(startNormal.X, startNormal.Y).GetSafeNormal();
			FVector2D forward = startForward.IsZero() ? axisForward : startForward;
			if (isFreeStart && !isWallStart)
			{
				// A free start is a pole, and the player sets the pipe's direction by turning it with the scroll
				// wheel before placing it. A floor pole hung from a ceiling has a vertical start normal, but its
				// connector still faces sideways along the pole.
				if (const FVector2D* storedForward = GPipeStartForward.Find(hologram))
				{
					forward = *storedForward;
				}
				else
				{
					if (startForward.IsZero() && !isWallStart)
					{
						if (AFGPipelinePoleHologram* pole = hologram->mChildPoleHologram[0])
						{
							const FVector poleForward = pole->GetActorForwardVector();
							const FVector2D poleDirection = FVector2D(poleForward.X, poleForward.Y).GetSafeNormal();
							if (!poleDirection.IsZero())
								forward = poleDirection;
						}
					}
					GPipeStartForward.Add(hologram, forward);
				}

				// Leave along that axis towards the end, so an end behind the pole doesn't bend back on itself.
				// An end square to the side would give no run along the axis at all, so turn to face it. A wall
				// pole can't turn, so a start on a wall always leaves straight out of the wall.
				const double ahead = FVector2D::DotProduct(startToEnd, forward);
				if (!isWallStart)
				{
					if (FMath::Abs(ahead) < 1.0 && startToEnd.Size() >= 1.0)
					{
						const FVector2D side(-forward.Y, forward.X);
						forward = FVector2D::DotProduct(startToEnd, side) >= 0.0 ? side : -side;
					}
					else if (ahead < 0.0)
					{
						forward = -forward;
					}
				}
			}

			const double bendRadius = hologram->mBendRadius;
			const FVector2D left(-forward.Y, forward.X);

			// A free end is a pole, whose connector faces sideways, so the pipe has to arrive level. The level
			// route goes to a point a bend radius short of the end (levelTarget2D), any plumb run is made there,
			// and the pipe bends into a final level run into the pole.
			//
			// On a wall that's a wall pole. The scroll wheel turns it in quarter turns (all four: up, down and
			// both ways along the wall), set here and picked up when the pole is next placed, so endNormal
			// follows a frame later. If its connector faces level the pipe arrives along it, and if it faces up
			// or down the pipe arrives on a plumb run (endArrivesPlumb).
			//
			// A floor pole arrives on one of the start's axes: by default the way the level route was already
			// heading, or a quarter turn either way with the scroll wheel. The game turns the pole to match.
			const bool isFreeEnd = isFreePipeEnd(hologram, 1) && !endsInPassthrough;
			// The start's wall pole can be close to the end too, so take the one nearer the end than the start.
			AFGWallAttachmentHologram* endWallPole = nullptr;
			double endWallPoleDistance = TNumericLimits<double>::Max();
			for (int32 i = 0; i < 2; ++i)
			{
				AFGWallAttachmentHologram* wallPole = hologram->mChildWallPoleHologram[i];
				if (!IsValid(wallPole) || wallPole->IsHidden())
					continue;
				const double distance = FVector::Dist(wallPole->GetActorLocation(), endLocation);
				if (distance < endWallPoleDistance && distance < FVector::Dist(wallPole->GetActorLocation(), startLocation))
				{
					endWallPole = wallPole;
					endWallPoleDistance = distance;
				}
			}

			// Diagnostics for wall ends, logged only when something changes.
			{
				static FString lastEndState;
				const UFGPipeConnectionComponentBase* snappedEnd = hologram->mSnappedConnectionComponents[1];
				const AFGWallAttachmentHologram* wallPole0 = hologram->mChildWallPoleHologram[0];
				const AFGWallAttachmentHologram* wallPole1 = hologram->mChildWallPoleHologram[1];
				const AFGPipelinePoleHologram* floorPole1 = hologram->mChildPoleHologram[1];
				auto describeWallPole = [&](const AFGWallAttachmentHologram* wallPole)
				{
					if (wallPole == nullptr)
						return FString(TEXT("None"));
					const FTransform poleTransform = wallPole->GetActorTransform();
					return FString::Printf(TEXT("%s hidden=%d dEnd=%.0f dStart=%.0f X=%s Y=%s Z=%s"),
						*wallPole->GetName(), wallPole->IsHidden() ? 1 : 0,
						FVector::Dist(wallPole->GetActorLocation(), endLocation), FVector::Dist(wallPole->GetActorLocation(), startLocation),
						*poleTransform.GetUnitAxis(EAxis::X).ToCompactString(), *poleTransform.GetUnitAxis(EAxis::Y).ToCompactString(),
						*poleTransform.GetUnitAxis(EAxis::Z).ToCompactString());
				};
				const FString endState = FString::Printf(
					TEXT("BetterStraight end: free=%d snapped=%s owner=%s floorPole1=%s hidden=%d | wallPole0=%s | wallPole1=%s | endWallPole=%s scrollRotation=%d turns=%d bendRadius=%.0f endNormal=%s"),
					isFreeEnd ? 1 : 0, *GetNameSafe(snappedEnd), *GetNameSafe(snappedEnd ? snappedEnd->GetOwner() : nullptr),
					*GetNameSafe(floorPole1), floorPole1 ? (floorPole1->IsHidden() ? 1 : 0) : -1,
					*describeWallPole(wallPole0), *describeWallPole(wallPole1),
					*GetNameSafe(endWallPole), endWallPole ? endWallPole->GetScrollRotateValue() : -1,
					GPipeEndQuarterTurns.FindRef(hologram), bendRadius, *endNormal.GetSafeNormal().ToCompactString());
				if (endState != lastEndState)
				{
					UE_LOG(LogVerticalLogisticsQoL, Display, TEXT("%s"), *endState);
					lastEndState = endState;
				}
			}

			FVector2D endArriveDirection = FVector2D::ZeroVector;
			bool endArrivesPlumb = false;
			FVector endWallNormal = FVector::ZeroVector;

			// An end snapped to a port facing up or down (a floor or ceiling pipe hole, the top of a vertical
			// pipe) is arrived at on a plumb run. Into a pipe hole its last bend is often short of a full radius,
			// since a pole's default height above the floor is less than that, so it may shrink to fit (see below).
			const bool endsOnPlumbPort = !isFreeEnd && FMath::Abs(endNormal.GetSafeNormal().Z) > 0.7;
			if (endsOnPlumbPort)
				endArrivesPlumb = true;

			// An end snapped to a level port (a machine, the end of another pipe) is arrived at straight along
			// the port, or the joint kinks. The port's normal faces out of it, so the pipe arrives against it.
			// endNormal isn't used for this, since it can follow the auto route rather than the port.
			bool endsOnLevelPort = false;
			if (!isFreeEnd && !endsOnPlumbPort)
			{
				if (const UFGPipeConnectionComponentBase* port = hologram->mSnappedConnectionComponents[1])
				{
					const FVector portNormal = port->GetConnectorNormal();
					const FVector2D portDirection = FVector2D(portNormal.X, portNormal.Y).GetSafeNormal();
					if (!portDirection.IsZero())
					{
						endArriveDirection = -portDirection;
						endsOnLevelPort = true;
					}
				}
			}

			if (isFreeEnd && endWallPole != nullptr)
			{
				// The pipe runs through the wall pole along its Y axis. Its X axis is out of the wall, and the
				// scroll rotation rolls it about that: on an upright wall, 0 and 180 run along the wall and 90 and
				// 270 run up and down it. Neither endNormal nor the pole's current rotation can be used for the
				// direction, since the game tilts both to follow the pipe's own end, which feeds back.
				//
				// The ring is the same from either side, so on an upright wall the scroll wheel steps through three
				// ways in: along the wall carrying on away from the start, plumb, and along the wall coming back
				// towards the start. On a ceiling or floor it steps through all four level directions.
				const FVector wallNormal = endWallPole->GetActorTransform().GetUnitAxis(EAxis::X);
				endWallNormal = wallNormal;
				const bool isUprightWall = FMath::Abs(wallNormal.Z) < 0.7;
				const int32 turns = GPipeEndQuarterTurns.FindRef(hologram);
				const int32 wallState = isUprightWall ? (turns % 3 + 3) % 3 : (turns % 4 + 4) % 4;
				const int32 wallPoleRotation = isUprightWall ? (wallState == 1 ? 90 : 0) : wallState * 90;
				if (endWallPole->GetScrollRotateValue() != wallPoleRotation)
					endWallPole->SetScrollRotateValue(wallPoleRotation);

				if (isUprightWall && wallState == 1)
				{
					endArrivesPlumb = true;
				}
				else
				{
					const FVector passAxis = isUprightWall
						? FVector::CrossProduct(FVector::UpVector, wallNormal)
						: endWallPole->GetActorTransform().GetUnitAxis(EAxis::Y);
					endArriveDirection = FVector2D(passAxis.X, passAxis.Y).GetSafeNormal();
					if (FVector2D::DotProduct(endArriveDirection, startToEnd) < 0.0)
						endArriveDirection = -endArriveDirection;
					if (isUprightWall ? wallState == 2 : wallState >= 2)
						endArriveDirection = -endArriveDirection;
				}
			}
			else if (isFreeEnd && !forward.IsZero())
			{
				// Arriving from the side needs room for a run across and the final run: two bend radii.
				const double ahead = FVector2D::DotProduct(startToEnd, forward);
				const double lateral = FVector2D::DotProduct(startToEnd, left);
				if (FMath::Abs(lateral) >= 2.0 * bendRadius)
					endArriveDirection = lateral > 0.0 ? left : -left;
				else
					endArriveDirection = ahead >= 1.0 ? forward : -forward;

				const int32 turn = (GPipeEndQuarterTurns.FindRef(hologram) % 3 + 3) % 3;
				if (turn == 1)
					endArriveDirection = FVector2D(-endArriveDirection.Y, endArriveDirection.X);
				else if (turn == 2)
					endArriveDirection = FVector2D(endArriveDirection.Y, -endArriveDirection.X);
			}

			// An offset of less than two bend radii can't be made with two full bends: squeezed in, the bends
			// overshoot and the pipe dips and comes back. Instead the final run into the end pole takes it on
			// as a gentle slope, lengthened to keep that slope to about 20 degrees. That covers a small height
			// difference from a horizontal start, and a small sideways offset when arriving straight on.
			const bool isVerticalStart = FMath::Abs(startNormal.Z) > 0.7 || startForward.IsZero();
			const double heightToEnd = endLocation.Z - routeStart.Z;
			const bool hasLead = !endArriveDirection.IsZero();
			const bool rampsHeight = hasLead && !endsOnLevelPort && !isVerticalStart && hasHeight && FMath::Abs(heightToEnd) < 2.0 * bendRadius;
			double rampedLateral = 0.0;
			if (hasLead && !endsOnLevelPort && endWallPole == nullptr && FMath::Abs(FVector2D::DotProduct(endArriveDirection, forward)) > 0.5)
			{
				const double lateral = FVector2D::DotProduct(startToEnd, left);
				if (FMath::Abs(lateral) < 2.0 * bendRadius)
					rampedLateral = lateral;
			}
			const double rampedOffset = FMath::Max(FMath::Abs(rampedLateral), rampsHeight ? FMath::Abs(heightToEnd) : 0.0);
			double leadLength = FMath::Max((double)bendRadius, rampedOffset * 2.75);
			bool sharesRoom = false;

			// Arriving the way the start faces from well off to the side takes two turns along the start's
			// direction, a bend radius each. When the end isn't two bend radii ahead, share what room there is
			// between the run out of the start and the final run, so both bends shrink alike.
			const double endAhead = FVector2D::DotProduct(startToEnd, forward);
			if (hasLead && rampedLateral == 0.0 && FVector2D::DotProduct(endArriveDirection, forward) > 0.5
				&& endAhead > 0.0 && endAhead < 2.0 * leadLength)
			{
				leadLength = endAhead / 2.0;
				sharesRoom = true;
			}

			const FVector2D levelTarget2D = hasLead ? end2D - endArriveDirection * leadLength - left * rampedLateral : end2D;

			// Level route at height z from the last corner to levelTarget2D: along forward, then across. Within
			// ~3.6 degrees of straight ahead from a port counts as straight ahead, the same tolerance vanilla
			// Straight passes to FSplineUtils::BuildStraightSpline3D (cos 0.998). A free start's direction is the
			// player's choice, so it gets an exact L instead.
			//
			// When the target is beside or behind the start, an L would cut straight back across the start (or
			// a snapped port). Run out a bend radius first, then across, then back to the target; straight
			// behind needs a sidestep of two bend radii to have room to turn round.
			//
			// When the route goes straight on from the target into the end pole (arrivesLevel, no plumb run in
			// between), the run across can't head the opposite way to that final run or the pipe would double
			// back on itself. Jog across halfway along instead, and reach the target along forward.
			bool doublesBack = false;
			auto addLevelRoute = [&](double z, bool arrivesLevel)
			{
				const FVector2D from(corners.Last());
				const FVector2D toEnd = levelTarget2D - from;
				if (toEnd.Size() < 1.0)
					return;
				const double ahead = FVector2D::DotProduct(toEnd, forward);
				const double lateral = FVector2D::DotProduct(toEnd, left);
				const double straightTolerance = isFreeStart ? 0.0 : ahead * 0.0633;
				if (ahead >= 1.0)
				{
					if (FMath::Abs(lateral) > FMath::Max(straightTolerance, 1.0))
					{
						const FVector2D across = lateral > 0.0 ? left : -left;
						if (arrivesLevel && FVector2D::DotProduct(across, endArriveDirection) < -0.5)
						{
							const FVector2D jog = from + forward * (ahead / 2.0);
							corners.Add(FVector(jog, z));
							corners.Add(FVector(jog + left * lateral, z));
						}
						else
						{
							corners.Add(FVector(from + forward * ahead, z));
						}
					}
				}
				else
				{
					const FVector2D runOut = from + forward * bendRadius;
					corners.Add(FVector(runOut, z));
					doublesBack = true;
					if (FMath::Abs(lateral) >= 1.0)
					{
						corners.Add(FVector(runOut + left * lateral, z));
					}
					else
					{
						corners.Add(FVector(runOut + left * (2.0 * bendRadius), z));
						corners.Add(FVector(levelTarget2D + left * (2.0 * bendRadius), z));
					}
				}
				corners.Add(FVector(levelTarget2D, z));
			};

			if (isVerticalStart)
			{
				// Vertical start: plumb run to the end's height first, then the level route.
				if (isFreeStart && !isWallStart && hasHeight && !forward.IsZero())
				{
					// A floor pole's connector faces sideways even when the start normal is vertical (a pole
					// hung from a ceiling). Leave it sideways and bend into the plumb run, or the pipe would come
					// out of the side of the pole.
					corners.Add(FVector(FVector2D(routeStart) + forward * bendRadius, routeStart.Z));
				}
				if (endArrivesPlumb && FVector2D::Distance(FVector2D(corners.Last()), end2D) < 2.0 * bendRadius)
				{
					// Plumb at both ends and (nearly) above each other: one straight pipe. Wall poles don't always
					// line up to the centimetre, and a jog that small would be two squeezed bends, which the game
					// rejects as an invalid shape.
				}
				else if (endArrivesPlumb)
				{
					// Plumb at both ends: the level route goes halfway up (or down), so that a pole straight
					// above the start is one straight pipe.
					const double levelZ = (routeStart.Z + endLocation.Z) / 2.0;
					corners.Add(FVector(FVector2D(corners.Last()), levelZ));
					addLevelRoute(levelZ, false);
				}
				else if (hasLead && hasHeight
					&& FVector2D::DotProduct(levelTarget2D - FVector2D(corners.Last()), endArriveDirection) < -1.0)
				{
					// The end is reached coming back towards the start, so the level route has to pass it before
					// turning in. Make it halfway up (or down) and drop to the end's height at the lead point, so
					// the route doesn't run back along itself.
					const double levelZ = (routeStart.Z + endLocation.Z) / 2.0;
					corners.Add(FVector(FVector2D(corners.Last()), levelZ));
					addLevelRoute(levelZ, false);
					corners.Add(FVector(levelTarget2D, endLocation.Z));
				}
				else
				{
					if (hasHeight)
						corners.Add(FVector(FVector2D(corners.Last()), endLocation.Z));
					addLevelRoute(endLocation.Z, true);
				}
			}
			else
			{
				// Horizontal start: the level route first, then a plumb run at the lead point (unless the final run
				// ramps the height instead).
				const bool hasPlumbRun = hasHeight && !rampsHeight;
				const double intoWall = isWallStart ? FVector::DotProduct(endLocation - startLocation, -startWallNormal) : 0.0;
				if (hasPlumbRun && intoWall >= 1.0)
				{
					// From a wall, an end on the wall's far side (on top of the foundations it's on, say) is
					// reached by going along the wall, plumb up (or down) its face, and only then across, so the
					// pipe doesn't run through the wall.
					//
					// When the end is reached heading back towards the wall, crossing straight to it would fold
					// back on itself. Climb two bend radii further along instead, cross beyond the end, and step
					// back to it, which leaves a full radius for each bend.
					const FVector2D intoWall2D = FVector2D(-startWallNormal.X, -startWallNormal.Y).GetSafeNormal();
					const bool arrivesFromBeyond = FVector2D::DotProduct(endArriveDirection, intoWall2D) < -0.5;
					const double sidestep = arrivesFromBeyond ? 2.0 * bendRadius : 0.0;
					const double along = FVector2D::DotProduct(levelTarget2D - FVector2D(routeStart), forward) + sidestep;
					const FVector2D turn = FVector2D(routeStart) + forward * along;
					// An end entered plumb (a wall pole turned up or down) is crossed to two bend radii short of
					// its height, so the pipe can turn up (or down) into it.
					const double crossZ = endArrivesPlumb
						? endLocation.Z - FMath::Sign(endLocation.Z - routeStart.Z) * 2.0 * bendRadius
						: endLocation.Z;
					corners.Add(FVector(turn, routeStart.Z));
					corners.Add(FVector(turn, crossZ));
					if (arrivesFromBeyond)
						corners.Add(FVector(levelTarget2D + forward * sidestep, crossZ));
					corners.Add(FVector(levelTarget2D, crossZ));
				}
				else
				{
					addLevelRoute(routeStart.Z, !hasPlumbRun);
					if (hasPlumbRun)
						corners.Add(FVector(levelTarget2D, endLocation.Z));
				}
			}
			corners.Add(endLocation);

			// Drop corners that coincide with a neighbour, or sit on a straight run between them. Left in, a
			// straight-through corner would split its run in two, and the bends either side would only get half
			// of each piece.
			for (int32 i = corners.Num() - 2; i >= 1; --i)
			{
				if ((corners[i] - corners[i + 1]).Size() < 1.0 || (corners[i] - corners[i - 1]).Size() < 1.0
					|| FVector::DotProduct((corners[i] - corners[i - 1]).GetSafeNormal(), (corners[i + 1] - corners[i]).GetSafeNormal()) > 0.9999)
					corners.RemoveAt(i);
			}

			// Spline points in world space, joined by straight runs and corner arcs.
			struct FRoutePoint
			{
				FVector Location;
				FVector ArriveTangent;
				FVector LeaveTangent;
			};
			TArray<FRoutePoint> route;
			route.Add({ startLocation, FVector::ZeroVector, FVector::ZeroVector });

			// A straight run to location; returns false (and adds nothing) when it would be too short.
			auto addRun = [&](const FVector& location)
			{
				const FVector tangent = location - route.Last().Location;
				if (tangent.Size() < 1.0)
					return false;
				route.Last().LeaveTangent = tangent;
				route.Add({ location, tangent, tangent });
				return true;
			};

			const double maxRadius = hologram->mBendRadius;
			double minRadius = maxRadius;
			double plumbPortBendRadius = maxRadius;
			bool foldsBack = false;
			for (int32 i = 1; i < corners.Num() - 1; ++i)
			{
				const FVector before = corners[i] - corners[i - 1];
				const FVector after = corners[i + 1] - corners[i];
				const FVector arriveDirection = before.GetSafeNormal();
				const FVector leaveDirection = after.GetSafeNormal();
				const double angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(arriveDirection, leaveDirection), -1.0, 1.0));
				if (angle < 1e-3)
					continue;
				// Turning straight back on itself (an end reached going the opposite way along the same line)
				// can't be made with a bend: fall back to the auto route.
				if (angle > 0.95 * UE_PI)
				{
					foldsBack = true;
					continue;
				}

				// The arc starts and ends this far from the corner. Inner runs are shared by two corners.
				const double tanHalf = FMath::Tan(angle / 2.0);
				const double availableBefore = i == 1 ? before.Size() : before.Size() / 2.0;
				const double availableAfter = i == corners.Num() - 2 ? after.Size() : after.Size() / 2.0;
				const double cornerDistance = FMath::Min3(maxRadius * tanHalf, availableBefore, availableAfter);
				const double radius = cornerDistance / tanHalf;
				if (endsOnPlumbPort && endsInPassthrough && i == corners.Num() - 2)
					plumbPortBendRadius = radius;
				else
					minRadius = FMath::Min(minRadius, radius);

				// Hermite tangent length of a cubic approximating the arc.
				const double handle = 4.0 * radius * FMath::Tan(angle / 4.0);

				if (!addRun(corners[i] - arriveDirection * cornerDistance))
					route.Last().Location = corners[i] - arriveDirection * cornerDistance;
				route.Last().LeaveTangent = arriveDirection * handle;
				const FVector arcEnd = corners[i] + leaveDirection * cornerDistance;
				route.Add({ arcEnd, leaveDirection * handle, leaveDirection * handle });
			}
			if (!addRun(endLocation))
				route.Last().Location = endLocation;

			route[0].ArriveTangent = route[0].LeaveTangent;
			if (route.Num() < 2)
				return;

			// When the ends are too close for the route's bends, it either squeezes a bend tighter than the bend
			// radius, or loops round to make room (an end just behind or beside the start). Fall back to the
			// game's auto route for those: RouteSelectedSplineMode already routed our mode with AutoRouteSpline,
			// and mSplineData hasn't been touched yet. Two exceptions: when the end pole was turned to face the
			// way the start faces and the two bends share the room (sharesRoom), they may go down to half the
			// radius; and the bend down (or up) into a pipe hole may go as tight as the game
			// allows, to fit whatever drop there is. A route that runs out and turns straight back into the end
			// (two supports side by side facing the same way) has its bends right at the ports, which only looks
			// right when the supports are well apart.
			double pathLength = 0.0;
			for (int32 i = 1; i < corners.Num(); ++i)
				pathLength += FVector::Dist(corners[i - 1], corners[i]);
			const double directLength = FVector::Dist(startLocation, endLocation);
			const double smallestRadius = sharesRoom ? FMath::Max(0.49 * maxRadius, (double)hologram->mMinBendRadius) : 0.98 * maxRadius;
			const TCHAR* fallbackReason = nullptr;
			if (foldsBack)
				fallbackReason = TEXT("folds back on itself");
			else if (minRadius < smallestRadius)
				fallbackReason = TEXT("bend too tight");
			else if (plumbPortBendRadius < hologram->mMinBendRadius)
				fallbackReason = TEXT("bend into pipe hole too tight");
			else if (doublesBack && directLength < 8.0 * maxRadius && FVector2D::DotProduct(endArriveDirection, forward) < -0.5)
				fallbackReason = TEXT("turns straight back into a close port");
			else if (endArrivesPlumb && corners.Num() >= 2
				&& FMath::Abs((corners.Last() - corners[corners.Num() - 2]).GetSafeNormal().Z) < 0.7)
				fallbackReason = TEXT("plumb end not reached on a plumb run");
			else if (directLength < 4.0 * maxRadius && pathLength > 2.0 * directLength)
				fallbackReason = TEXT("loops round a close end");

			// Diagnostics, logged only when something changes.
			{
				static FString lastRouteState;
				FString cornerList;
				for (const FVector& corner : corners)
					cornerList += corner.ToCompactString() + TEXT(" ");
				const FString routeState = FString::Printf(
					TEXT("BetterStraight route: %s | wallStart=%d gameStartNormal=%s startNormal=%s forward=%s arrive=%s plumbEnd=%d minRadius=%.0f | corners %s"),
					fallbackReason ? fallbackReason : TEXT("used"), isWallStart ? 1 : 0,
					*gameStartNormal.ToCompactString(), *startNormal.ToCompactString(), *forward.ToString(),
					*endArriveDirection.ToString(), endArrivesPlumb ? 1 : 0, minRadius, *cornerList);
				if (routeState != lastRouteState)
				{
					UE_LOG(LogVerticalLogisticsQoL, Display, TEXT("%s"), *routeState);
					lastRouteState = routeState;
				}
			}
			if (fallbackReason != nullptr)
				return;

			if (!endWallNormal.IsZero())
			{
				const FVector passAxis = endArrivesPlumb ? FVector::UpVector : FVector(endArriveDirection, 0.0);
				GPipeEndWallPole.Add(hologram, { endWallPole, endWallNormal, passAxis });
			}
			if (squaredStartWallPole.IsSet())
				GPipeStartWallPole.Add(hologram, squaredStartWallPole.GetValue());

			// mSplineData is in the hologram's local space.
			const FTransform& transform = hologram->GetActorTransform();
			TArray<FSplinePointData>& points = hologram->mSplineData;
			points.Reset(route.Num());
			for (const FRoutePoint& point : route)
			{
				points.Emplace(transform.InverseTransformPosition(point.Location),
					transform.InverseTransformVector(point.ArriveTangent), transform.InverseTransformVector(point.LeaveTangent));
			}
		});
}

void FVerticalLogisticsQoLModule::NetworkVerticalAttachmentFlowDirection()
{
	// Each attachment saves the direction of its connection components after it's constructed by the
	// hologram, which is needed for vertical attachments because the direction of their top and bottom
	// connections can change depending on how the attachment was built. This data (mSavedDirections)
	// isn't available on non-authoritative clients, so they won't know the direction of those
	// connections. We don't need all of the saved directions to be replicated, the side connections are
	// static and the top and bottom connections are in opposing directions, so everything can be
	// inferred from just a single flag.
	//
	// Adding this flag is a whole other problem, we can't just extend the attachment class because that
	// would throw off everything in the derived classes. Fortunately there's eome padding at the end of
	// mCachedInventorySize that we can use for some extra storage, all we need to do is make a new
	// FProperty and point it to that padding. This will obviously blow up if the layout changes in
	// future updates, but that's the same for any of the game structures that we access normally too so
	// I don't think that's any more of a problem. The only issue would be if another mod is equally
	// mischievous and has another use for that padding...

	static constexpr size_t isUpwardsFlowOffset =
		STRUCT_OFFSET(AFGBuildableConveyorAttachment, mCachedInventorySize)
		+ sizeof(AFGBuildableConveyorAttachment::mCachedInventorySize);

	static_assert(isUpwardsFlowOffset + sizeof(bool) <= STRUCT_OFFSET(AFGBuildableConveyorAttachment, mHologramOverrides));

	// Creates the property and registers it with the networking system.
	class LifetimeRepHook
	{
	public:
		explicit LifetimeRepHook(UClass* buildableClass)
		{
			using namespace UECodeGen_Private;

			FProperty* prevProperty = FindPreviousProperty(
				buildableClass,
				GET_MEMBER_NAME_CHECKED(AFGBuildableConveyorAttachment, mHologramOverrides));

			check(prevProperty);
			check(prevProperty->GetOffset_ForDebug() + prevProperty->GetSize() <= isUpwardsFlowOffset);

			IsUpwardsFlowProperty = new FBoolProperty(buildableClass, FBoolPropertyParams
			{
				.NameUTF8 = "mIsUpwardsFlow",
				.PropertyFlags = CPF_Net | CPF_Transient | CPF_NativeAccessSpecifierPrivate,
				.Flags = EPropertyGenFlags::Bool | EPropertyGenFlags::NativeBool,
				.ArrayDim = 1,
				.ElementSize = sizeof(bool),
				.SizeOfOuter = sizeof(AFGBuildableConveyorAttachment),
				.SetBitFunc = [](void* o) { *(bool*)((unsigned char*)o + isUpwardsFlowOffset) = true; },
			});

			// By default the property gets added to the front of the property list, but we're re-linking it to
			// be where it was if it was declared naturally as part of the class.
			check(buildableClass->ChildProperties == IsUpwardsFlowProperty);
			check(buildableClass->PropertyLink != IsUpwardsFlowProperty);
			buildableClass->ChildProperties = IsUpwardsFlowProperty->Next;
			IsUpwardsFlowProperty->Next = prevProperty->Next;
			IsUpwardsFlowProperty->PropertyLinkNext = prevProperty->PropertyLinkNext;
			prevProperty->Next = IsUpwardsFlowProperty;
			prevProperty->PropertyLinkNext = IsUpwardsFlowProperty;

			buildableClass->ClassFlags &= ~CLASS_ReplicationDataIsSetUp;
		}

		void operator()(const AFGBuildable* buildable, TArray<FLifetimeProperty>& OutLifetimeProps)
		{
			auto* attachment = Cast<AFGBuildableConveyorAttachment>(buildable);
			if (attachment == nullptr)
				return;

			RegisterReplicatedLifetimeProperty(IsUpwardsFlowProperty, OutLifetimeProps,
			{
				// Don't need networking for non-vertical attachments as their directions are all static.
				.Condition = IsVerticalAttachment(attachment) ? COND_InitialOnly : COND_Never,
			});
		}

	private:
		static FProperty* FindPreviousProperty(UClass* cls, FName name)
		{
			FProperty* prev = nullptr;
			FProperty* curr = cls->PropertyLink;

			while (curr != nullptr)
			{
				if (curr->GetFName() == name)
					return prev;
				prev = curr;
				curr = curr->PropertyLinkNext;
			}

			return nullptr;
		}

		static bool IsVerticalAttachment(const AFGBuildableConveyorAttachment* attachment)
		{
			for (const UFGHologramOverride* override : attachment->mHologramOverrides)
			{
				if (override && override->IsA<UFGHologramOverride_ConveyorAttachment_LiftToFloor>())
					return true;
			}
			return false;
		}

		FBoolProperty* IsUpwardsFlowProperty;
	};

	// Sets the new networked flag on the server, which the client uses to fix up the lift connection
	// directions.
	class BeginPlayHook
	{
	public:
		void operator()(AFGBuildableConveyorAttachment* attachment) const
		{
			bool& isUpwardsFlow = *(bool*)((unsigned char*)attachment + isUpwardsFlowOffset);
			TInlineComponentArray<UFGFactoryConnectionComponent*> connections(attachment);

			if (attachment->HasAuthority())
			{
				isUpwardsFlow = GetIsUpwardsFlow(connections);
			}
			else
			{
				SetIsUpwardsFlow(connections, isUpwardsFlow);
			}
		}

	private:
		static bool GetIsUpwardsFlow(const TInlineComponentArray<UFGFactoryConnectionComponent*>& connections)
		{
			const FName bottomName = AFGConveyorAttachmentHologram::mLiftConnection_Top;

			for (const UFGFactoryConnectionComponent* connection : connections)
			{
				if (connection->GetFName() == bottomName)
				{
					return connection->GetDirection() == EFactoryConnectionDirection::FCD_INPUT;
				}
			}

			return false;
		}

		static void SetIsUpwardsFlow(const TInlineComponentArray<UFGFactoryConnectionComponent*>& connections, bool isUpwardsFlow)
		{
			const FName bottomName = AFGConveyorAttachmentHologram::mLiftConnection_Top;
			const FName topName = AFGConveyorAttachmentHologram::mLiftConnection_Bottom;

			for (UFGFactoryConnectionComponent* connection : connections)
			{
				if (connection->GetDirection() != EFactoryConnectionDirection::FCD_ANY)
					continue;

				const FName name = connection->GetFName();

				if (name == bottomName)
				{
					connection->SetDirection(
						isUpwardsFlow
						? EFactoryConnectionDirection::FCD_INPUT
						: EFactoryConnectionDirection::FCD_OUTPUT);
				}
				else if (name == topName)
				{
					connection->SetDirection(
						isUpwardsFlow
						? EFactoryConnectionDirection::FCD_OUTPUT
						: EFactoryConnectionDirection::FCD_INPUT);
				}
			}
		}
	};

	UClass* buildableClass = AFGBuildableConveyorAttachment::StaticClass();
	auto* defaultBuildable = CastChecked<AFGBuildableConveyorAttachment>(buildableClass->GetDefaultObject());
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBuildable::GetLifetimeReplicatedProps, defaultBuildable, LifetimeRepHook(buildableClass));
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBuildableConveyorAttachment::BeginPlay, defaultBuildable, BeginPlayHook());
}

void FVerticalLogisticsQoLModule::NetworkLiftMeshRotationFlag()
{
	// AFGBuildableConveyorLift uses mIsBeltUsingInputRotation to determine the rotation of its mid
	// meshes, but that flag isn't networked so non-authoritative clients calculate the wrong rotation.

	UClass* liftClass = AFGBuildableConveyorLift::StaticClass();
	FProperty* flagProperty = liftClass->FindPropertyByName(GET_MEMBER_NAME_CHECKED(AFGBuildableConveyorLift, mIsBeltUsingInputRotation));

	if (flagProperty == nullptr || flagProperty->HasAnyPropertyFlags(CPF_Net))
		return;

	flagProperty->SetPropertyFlags(CPF_Net);
	liftClass->ClassFlags &= ~CLASS_ReplicationDataIsSetUp;

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGBuildableConveyorLift, GetLifetimeReplicatedProps,
		[flagProperty](const AFGBuildableConveyorLift* lift, TArray<FLifetimeProperty>& OutLifetimeProps)
		{
			RegisterReplicatedLifetimeProperty(flagProperty, OutLifetimeProps,
			{
				.Condition = COND_InitialOnly,
			});
		});
}

IMPLEMENT_MODULE(FVerticalLogisticsQoLModule, VerticalLogisticsQoL)
