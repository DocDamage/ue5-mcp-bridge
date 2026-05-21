// Copyright FatumGame. All Rights Reserved.

#include "Utils/MCPMaterialUtils.h"

#include "Utils/MCPAssetPathUtils.h"
#include "MCPTypes.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace FMCPMaterialUtils
{

FString MakeMaterialClassMismatchMessage(
	const FString& Path,
	const FString& ActualClassName,
	bool bWritePath)
{
	// Frozen advisory text — smoke tests assert "UMaterialInstanceConstant" substring AND the
	// "Phase 7" hint so future material.edit_node work has a stable callout to remove. The same
	// helper covers both read-side (path is not UMaterialInterface) and write-side (path resolved
	// but is base UMaterial / dynamic / etc.) — the bWritePath toggle selects the explanatory tail.
	if (bWritePath)
	{
		return FString::Printf(
			TEXT("path '%s' is class '%s'; expected UMaterialInstanceConstant (for writes) or ")
			TEXT("UMaterialInterface (for reads); mutating base UMaterial requires graph edits ")
			TEXT("(out of Phase 4 scope; future Phase 7 may add material.edit_node)"),
			*Path, *ActualClassName);
	}
	return FString::Printf(
		TEXT("path '%s' is class '%s'; expected UMaterialInterface (UMaterial, ")
		TEXT("UMaterialInstanceConstant, UMaterialInstanceDynamic)"),
		*Path, *ActualClassName);
}

UMaterialInterface* LoadMaterialInterfaceByPath(
	const FString& Path,
	int32& OutErrorCode,
	FString& OutError)
{
	if (Path.IsEmpty())
	{
		OutErrorCode = kMCPErrorInvalidPath;
		OutError = TEXT("material_path is empty");
		return nullptr;
	}

	// Normalise + validate mount prefix (rejects backslashes, ``..``, drive letters, unknown mounts).
	const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
	if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
	{
		OutErrorCode = kMCPErrorInvalidPath;
		OutError = FString::Printf(
			TEXT("material_path '%s' is malformed or references an unknown mount point"),
			*Path);
		return nullptr;
	}

	// Try package-name form first (LoadObject handles the leaf-name suffix attachment internally
	// for most classes). Try object-path form (``...:Name.Name``) as the fallback.
	UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
	if (!Loaded)
	{
		const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
		if (!ObjectPath.IsEmpty() && ObjectPath != Normalised)
		{
			Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
		}
	}
	if (!Loaded)
	{
		OutErrorCode = kMCPErrorObjectNotFound;
		OutError = FString::Printf(
			TEXT("material_path '%s' could not be loaded (no asset found)"),
			*Path);
		return nullptr;
	}

	UMaterialInterface* Material = Cast<UMaterialInterface>(Loaded);
	if (!Material)
	{
		OutErrorCode = kMCPErrorMaterialClassMismatch;
		OutError = MakeMaterialClassMismatchMessage(
			Path, Loaded->GetClass()->GetPathName(), /*bWritePath*/ false);
		return nullptr;
	}
	return Material;
}

UMaterialInstanceConstant* LoadMICByPath(
	const FString& Path,
	int32& OutErrorCode,
	FString& OutError)
{
	UMaterialInterface* Material = LoadMaterialInterfaceByPath(Path, OutErrorCode, OutError);
	if (!Material)
	{
		return nullptr;
	}
	UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Material);
	if (!MIC)
	{
		OutErrorCode = kMCPErrorMaterialClassMismatch;
		OutError = MakeMaterialClassMismatchMessage(
			Path, Material->GetClass()->GetPathName(), /*bWritePath*/ true);
		return nullptr;
	}
	return MIC;
}

bool IsMaterialInstanceConstant(const UMaterialInterface* Asset)
{
	return Asset != nullptr && Asset->IsA<UMaterialInstanceConstant>();
}

bool IsBaseMaterial(const UMaterialInterface* Asset)
{
	if (!Asset)
	{
		return false;
	}
	// UMaterialInstanceConstant + UMaterialInstanceDynamic both descend from UMaterialInstance
	// which itself descends from UMaterialInterface alongside UMaterial. IsA<UMaterial> + NOT
	// IsA<UMaterialInstance> uniquely identifies base UMaterial assets.
	return Asset->IsA<UMaterial>() && !Asset->IsA<UMaterialInstance>();
}

UMaterial* WalkToBaseMaterial(UMaterialInterface* Asset)
{
	if (!Asset)
	{
		return nullptr;
	}
	// Fast path — already a base UMaterial.
	if (UMaterial* AsBase = Cast<UMaterial>(Asset))
	{
		return AsBase;
	}
	// Walk parent chain. UMaterialInterface::GetMaterial() returns the underlying UMaterial for any
	// instance subclass (handles parent-chain traversal internally). Returns nullptr only on
	// orphaned instances (no resolvable parent).
	return Asset->GetMaterial();
}

} // namespace FMCPMaterialUtils
