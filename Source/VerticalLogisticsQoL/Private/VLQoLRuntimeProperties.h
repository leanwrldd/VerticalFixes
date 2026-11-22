#pragma once

#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "UObject/UnrealType.h"
#include "VerticalLogisticsQoL.h"

// CL520140 moved conveyor members without updated public headers. Resolve these
// properties from the running game instead of compiling the old member offsets.
namespace VLQoLRuntimeProperties
{
template<typename PropertyType>
const PropertyType* RequireProperty(UClass* owner, FName name)
{
	const auto* property = FindFProperty<PropertyType>(owner, name);
	if (property == nullptr)
	{
		UE_LOG(LogVerticalLogisticsQoL, Fatal,
			TEXT("Unsupported game property %s.%s: missing or changed type."),
			*owner->GetName(), *name.ToString());
	}
	return property;
}

template<typename ElementType>
const FArrayProperty* RequireObjectArray(UClass* owner, FName name)
{
	const auto* property = RequireProperty<FArrayProperty>(owner, name);
	const auto* inner = CastField<FObjectProperty>(property->Inner);
	if (inner == nullptr || inner->PropertyClass != ElementType::StaticClass()
		|| inner->GetSize() != sizeof(TObjectPtr<ElementType>)
		|| property->GetSize() != sizeof(TArray<TObjectPtr<ElementType>>))
	{
		UE_LOG(LogVerticalLogisticsQoL, Fatal,
			TEXT("Unsupported game array layout for %s.%s."), *owner->GetName(), *name.ToString());
	}
	return property;
}

inline const TArray<TObjectPtr<UFGHologramOverride>>& GetHologramOverrides(const AFGBuildableConveyorAttachment* attachment)
{
	static const auto* property = RequireObjectArray<UFGHologramOverride>(
		AFGBuildableConveyorAttachment::StaticClass(), TEXT("mHologramOverrides"));
	return *property->ContainerPtrToValuePtr<TArray<TObjectPtr<UFGHologramOverride>>>(attachment);
}

inline const TArray<TObjectPtr<AFGBuildablePassthrough>>& GetSnappedPassthroughs(const AFGBuildableConveyorLift* lift)
{
	static const auto* property = RequireObjectArray<AFGBuildablePassthrough>(
		AFGBuildableConveyorLift::StaticClass(), TEXT("mSnappedPassthroughs"));
	return *property->ContainerPtrToValuePtr<TArray<TObjectPtr<AFGBuildablePassthrough>>>(lift);
}

inline UFGFactoryConnectionComponent* GetConnection0(const AFGBuildableConveyorLift* lift)
{
	static const auto* property = RequireProperty<FObjectProperty>(
		AFGBuildableConveyorBase::StaticClass(), TEXT("mConnection0"));
	return Cast<UFGFactoryConnectionComponent>(property->GetObjectPropertyValue_InContainer(lift));
}

inline UFGFactoryConnectionComponent* GetConnection1(const AFGBuildableConveyorLift* lift)
{
	static const auto* property = RequireProperty<FObjectProperty>(
		AFGBuildableConveyorBase::StaticClass(), TEXT("mConnection1"));
	return Cast<UFGFactoryConnectionComponent>(property->GetObjectPropertyValue_InContainer(lift));
}
} // namespace VLQoLRuntimeProperties
