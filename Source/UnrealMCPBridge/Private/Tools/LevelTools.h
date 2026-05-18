// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Phase 3 — Category A (Level operations). Day 1 ships 2 read-only tools + 1 Lane B sanity probe;
 * Days 2-3 expand the surface to all 12 user-visible Level tools (load/save/create/unload/etc.).
 *
 * **All tools are Lane A** (``bThreadSafe=false``) except the Lane B sanity probe. Reasons:
 *   - Editor world traversal requires GAME THREAD (GEditor / UWorld / ULevel APIs not thread-safe).
 *   - Mutators wrap in FScopedTransaction (game-thread only).
 *   - Save/load operations call FEditorFileUtils / UEditorAssetSubsystem which assert IsInGameThread().
 *
 * **Mutator PIE-guard.** Every Days-2-3 write-side handler will check
 * ``FMCPWorldContext::IsPIEActive`` first and refuse with ``kMCPErrorPIEActive`` (-32027) + frozen
 * message. Read-only handlers (list_loaded, current_map, ...) work transparently during PIE — they
 * see ``GEditor->PlayWorld`` when present, ``GetEditorWorld`` otherwise.
 *
 * **Lane B sanity probe.** ``_phase3_lane_b_sanity`` is a Lane B handler used to verify the
 * listener-thread router still works after the Phase 2 Lane-A demotion of every AR tool. Returns
 * ``{echo, thread_id}`` where ``thread_id`` is the FPlatformTLS::GetCurrentThreadId() — a
 * non-game-thread id confirms the request bypassed the Drain queue.
 */
namespace FLevelTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Lane B sanity (per critic N1) ────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_Phase3LaneBSanity(const FMCPRequest& Request);

	// ─── Day 1 ────────────────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_ListLoaded(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_CurrentMap(const FMCPRequest& Request);
}
