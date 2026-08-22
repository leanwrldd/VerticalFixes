#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVerticalLogisticsQoL, Log, All)

class FVerticalLogisticsQoLModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void FixLostPassthroughLinks();
	void FixBrokenConnectionsWhenMergingInBlueprintDesigner();
	void FixHologramLocking();
	void FixLiftOnAttachmentOffByHalf();
	void FixAttachmentOnLiftOffByHalf();
	void FixClearanceWarnings();
	void FixMassDismantleVerticalAttachmentAndLifts();
	void FixReverseLiftConnectionFromSnapPoint();
	void AllowConnectionToExistingAttachment();
	void HideLiftArrowWhenSnappedTopToAttachment();
	void PrepareCustomAttachmentHologram();
	void NetworkVerticalAttachmentFlowDirection();
	void NetworkLiftMeshRotationFlag();
};
