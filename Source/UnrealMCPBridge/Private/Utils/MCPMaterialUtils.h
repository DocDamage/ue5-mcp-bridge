// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UMaterialInstanceConstant;
class UMaterial;

/**
 * Phase 4 — Material asset access helpers (mirror of FMCPBlueprintUtils for the material.* surface).
 *
 * Centralises the path → ``UMaterialInterface*`` lookup that every material.* tool performs first.
 * Each resolver normalises path shape (``/Game/Foo/MI_X`` ↔ ``/Game/Foo/MI_X.MI_X``), distinguishes
 * "not found" from "found but wrong class family", and surfaces the appropriate Phase 4 error code:
 *
 *   - ``kMCPErrorInvalidPath``            (-32010) — empty / malformed / unknown mount
 *   - ``kMCPErrorObjectNotFound``         (-32004) — LoadObject returned null after retry
 *   - ``kMCPErrorMaterialClassMismatch``  (-32034) — path resolved to a non-UMaterialInterface asset
 *
 * **Threading.** All helpers MUST run on the game thread — ``LoadObject<UMaterialInterface>`` may
 * trigger shader cache touch + Outer chain walk under GC lock. Phase 4 ships every tool Lane A.
 *
 * **No PIE guard here.** Reads are PIE-safe; the writes that need the guard call
 * ``FMCPWorldContext::IsPIEActive`` themselves before resolving the material.
 *
 * **MIC-only writes (D9).** Every ``material.set_*`` tool requires ``UMaterialInstanceConstant``
 * (not base ``UMaterial``). ``IsMaterialInstanceConstant`` + ``IsBaseMaterial`` give the two
 * predicates needed for the -32034 MaterialClassMismatch routing.
 */
namespace FMCPMaterialUtils
{
	/**
	 * Resolve ``Path`` to a ``UMaterialInterface*`` — accepts BOTH UMaterial (base) and
	 * UMaterialInstance (MIC + dynamic). Use this for READ tools (list_parameters, get_parameter,
	 * get_compile_errors) that allow any material asset.
	 *
	 * Returns the loaded material on success. On failure populates ``OutErrorCode`` + ``OutError``:
	 *   - Empty/invalid path  → -32010 InvalidPath
	 *   - LoadObject failure  → -32004 ObjectNotFound
	 *   - Loaded but wrong class family → -32034 MaterialClassMismatch (with the actual class name)
	 *
	 * Caller maps ``OutErrorCode`` to a response via its tool-private ``MAT_MakeError`` helper.
	 */
	UNREALMCPBRIDGE_API UMaterialInterface* LoadMaterialInterfaceByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError);

	/**
	 * Resolve ``Path`` to a ``UMaterialInstanceConstant*`` — strictly enforced. Use this for WRITE
	 * tools (set_scalar_param, set_vector_param, set_texture_param, set_static_switch).
	 *
	 * Returns the loaded MIC on success. On failure same error families as
	 * ``LoadMaterialInterfaceByPath`` PLUS -32034 when the asset is a base ``UMaterial`` (or any
	 * other non-MIC UMaterialInterface subclass like ``UMaterialInstanceDynamic``). Error message
	 * embeds the actual class name + the standard "Phase 4 scope" advisory pointing the caller at
	 * the future ``material.edit_node`` tool.
	 */
	UNREALMCPBRIDGE_API UMaterialInstanceConstant* LoadMICByPath(
		const FString& Path,
		int32& OutErrorCode,
		FString& OutError);

	/**
	 * True iff ``Asset`` is a ``UMaterialInstanceConstant``. Returns false for null, base
	 * ``UMaterial``, ``UMaterialInstanceDynamic``, or any other class.
	 */
	UNREALMCPBRIDGE_API bool IsMaterialInstanceConstant(const UMaterialInterface* Asset);

	/**
	 * True iff ``Asset`` is a base ``UMaterial`` (NOT any instance subclass). Returns false for
	 * null and for all UMaterialInstance variants.
	 */
	UNREALMCPBRIDGE_API bool IsBaseMaterial(const UMaterialInterface* Asset);

	/**
	 * Walk a material instance chain up to its base ``UMaterial``. Returns the original ``Asset``
	 * if it IS a base UMaterial; nullptr if walking the parent chain never reaches a UMaterial
	 * (degenerate / orphaned instance).
	 *
	 * Used by ``material.get_compile_errors`` to find the actual compile-output source from any
	 * material asset class.
	 */
	UNREALMCPBRIDGE_API UMaterial* WalkToBaseMaterial(UMaterialInterface* Asset);

	/**
	 * Build the canonical "wrong material class" diagnostic message used by -32034 paths. Centralised
	 * so the exact wording matches across all 4 MIC-only setters + the read-side wrong-class case.
	 */
	UNREALMCPBRIDGE_API FString MakeMaterialClassMismatchMessage(
		const FString& Path,
		const FString& ActualClassName,
		bool bWritePath);
}
