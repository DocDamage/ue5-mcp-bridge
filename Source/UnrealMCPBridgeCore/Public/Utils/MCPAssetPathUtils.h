// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FAssetData;

/**
 * Phase 2 — asset-path canonicalisation. Every Phase 2 tool that accepts a path argument runs it
 * through ``Normalize`` first, then converts via ``ToPackageName`` / ``ToObjectPath`` depending on
 * which downstream API needs which shape.
 *
 * **Accepted forms** (D1 decision):
 *   - ``/Game/Foo/Bar``                 (package-name, leaf-only)
 *   - ``/Game/Foo/Bar.Bar``             (object path, class-stripped)
 *   - ``/Engine/...``                   (engine content)
 *   - ``/Plugins/<plugin_name>/...``    (loose alias for ``/<plugin_name>/...`` mount)
 *   - ``/<mount_name>/...``             (any registered content mount discovered via
 *                                        ``FPackageName::QueryRootContentPaths``)
 *   - ``/Script/<module_name>``         (class path used by ``asset.search_by_class``)
 *   - ``/Script/<module_name>.<Class>`` (full class top-level path)
 *
 * **Rejected** (returns false from ``IsValidGameOrPlugin`` / empty from ``Normalize``):
 *   - Empty string
 *   - Contains ``\`` (backslash) — only forward slashes allowed
 *   - Contains ``..`` (relative path escape)
 *   - Starts with a drive letter (``C:/``) or any other non-``/`` first char
 *   - Mount-point not in the registry (e.g. ``/RandomMount/Foo``)
 *
 * **CRITICAL:** Lane B-safe — no UObject access, no LoadObject, no FindObject. Only string
 * manipulation + a stateless ``FPackageName`` query (which is thread-safe per UE 5.0+ docs).
 */
namespace FMCPAssetPathUtils
{
	/**
	 * Strip surrounding whitespace, normalise slash direction (`\` rejected — see header), drop any
	 * trailing ``.LeafName`` suffix so the canonical form is package-name. Returns empty string
	 * if the path is malformed.
	 *
	 * Examples:
	 *   ``/Game/Foo/Bar.Bar``        → ``/Game/Foo/Bar``
	 *   ``/Game/Foo/Bar``            → ``/Game/Foo/Bar``
	 *   ``  /Engine/Maps/X  ``       → ``/Engine/Maps/X``
	 *   ``/Game\Foo\Bar``            → ``""`` (backslash rejected)
	 *   ``/Game/../Other``           → ``""`` (.. rejected)
	 */
	UNREALMCPBRIDGECORE_API FString Normalize(const FString& InPath);

	/**
	 * Validate that ``InPath`` references a known mount point. Cheap O(N) scan over the
	 * mount-points table (N ~ 5-30 in practice). Caller MUST have already normalised.
	 */
	UNREALMCPBRIDGECORE_API bool IsValidGameOrPlugin(const FString& NormalizedPath);

	/**
	 * Wave S+7 (2026-05-24): Validate that ``InPath`` is in a WRITEABLE content mount —
	 * i.e. ``/Game/`` or a writable plugin content directory. Excludes engine-owned mounts
	 * (``/Engine/``, ``/Script/``, ``/Memory/``) and any read-only plugin content paths.
	 *
	 * Use this in create/rename/duplicate flows to prevent user assets polluting engine
	 * namespaces. The looser ``IsValidGameOrPlugin`` is still appropriate for READ-side
	 * operations (asset.exists, asset.get_property, asset.list, etc.) where reading from
	 * /Engine is legitimate.
	 *
	 * Returns false for: empty input, paths starting with /Engine/, /Script/, /Memory/,
	 * or any mount NOT reported by ``QueryRootContentPaths(bIncludeReadOnlyRoots=false)``.
	 */
	UNREALMCPBRIDGECORE_API bool IsWriteableMountPoint(const FString& NormalizedPath);

	/**
	 * Package-name form: ``/Game/Foo/Bar`` (no class suffix, no leading whitespace). Equivalent to
	 * ``Normalize`` today — the function is named explicitly so handler code reads as
	 * intent-revealing.
	 */
	UNREALMCPBRIDGECORE_API FString ToPackageName(const FString& NormalizedPath);

	/**
	 * Object-path form: ``/Game/Foo/Bar.Bar`` (leaf name appended after ``.``). Used by APIs that
	 * want a fully-qualified object path, e.g. ``IAR.GetAssetByObjectPath(FSoftObjectPath(...))``.
	 *
	 * If the input already has a ``.`` segment, it's preserved verbatim — handles sub-objects
	 * like ``/Game/Foo/Bar.Bar:SubObject`` that already encode the leaf differently.
	 */
	UNREALMCPBRIDGECORE_API FString ToObjectPath(const FString& NormalizedPath);

	/**
	 * Look up a single FAssetData via the asset registry. Returns whether the lookup succeeded;
	 * ``OutData`` is populated on true and left default on false. Internally goes through
	 * ``IAssetRegistry::GetAssetByObjectPath(FSoftObjectPath)``.
	 *
	 * Lane B-safe — read-only AR query.
	 */
	UNREALMCPBRIDGECORE_API bool ResolveAssetData(const FString& AnyPath, FAssetData& OutData);
}
