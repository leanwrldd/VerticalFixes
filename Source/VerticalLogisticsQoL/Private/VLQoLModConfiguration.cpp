#include "VLQoLModConfiguration.h"

#include "Configuration/Properties/WidgetExtension/CP_Bool.h"
#include "Configuration/Properties/WidgetExtension/CP_Section.h"

const TCHAR* UVLQoLModConfiguration::ReplaceVanillaStraightKey = TEXT("ReplaceVanillaStraight");

bool GVLQoLReplaceVanillaStraight = true;

UVLQoLModConfiguration::UVLQoLModConfiguration()
{
	ConfigId.ModReference = TEXT("VerticalFixes");
	DisplayName = INVTEXT("Vertical Fixes");

	// The settings menu only draws SML's Blueprint property classes; the native ones register and save but
	// show no fields. Fall back to the native class if the Blueprint one can't be loaded.
	static UClass* sectionClass = StaticLoadClass(UCP_Section::StaticClass(), nullptr,
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertySection.BP_ConfigPropertySection_C"));
	static UClass* boolClass = StaticLoadClass(UCP_Bool::StaticClass(), nullptr,
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyBool.BP_ConfigPropertyBool_C"));

	// UCP_Section and UCP_Bool are abstract, so the fallback is the plain native class.
	UConfigPropertySection* root = NewObject<UConfigPropertySection>(this,
		sectionClass ? sectionClass : UConfigPropertySection::StaticClass(), TEXT("RootSection"));
	if (UCP_Section* widgetSection = Cast<UCP_Section>(root))
		widgetSection->WidgetType = ECP_SectionWidgetType::CPS_Vertical;
	RootSection = root;

	UConfigPropertyBool* replaceStraight = NewObject<UConfigPropertyBool>(root,
		boolClass ? boolClass : UConfigPropertyBool::StaticClass(), ReplaceVanillaStraightKey);
	replaceStraight->DisplayName = INVTEXT("Better Straight replaces vanilla Straight");
	replaceStraight->Tooltip = INVTEXT("Pipes and hypertubes: removes the vanilla Straight build mode and puts Better Straight in its place. Applies to the next pipe you pick up.");
	replaceStraight->DefaultValue = true;
	replaceStraight->Value = true;
	replaceStraight->bRequiresWorldReload = false;
	root->SectionProperties.Add(ReplaceVanillaStraightKey, replaceStraight);
}
