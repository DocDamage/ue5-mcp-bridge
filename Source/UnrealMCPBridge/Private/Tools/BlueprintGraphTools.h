// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave B Tier 4 — Blueprint graph-level node construction. 2 user-visible tools, Lane A.
 *
 * Tool roster:
 *   bp.add_node      → instantiate a K2Node subclass on a UEdGraph and AddNode/AllocateDefaultPins
 *   bp.connect_pins  → wire two pins via UEdGraphSchema_K2::TryCreateConnection
 *
 * **All Lane A.** UEdGraph mutation + UK2Node spawn + schema connect all assume game-thread
 * execution under the editor's transaction system. Blueprint asset state mutation also goes
 * through the standard ``Modify`` / ``MarkPackageDirty`` path so editor Undo (Ctrl-Z) reverts.
 *
 * **PIE-guarded.** Both tools refuse during PIE with -32027 PIEActive — mutating a Blueprint
 * asset during PIE corrupts the shared asset (PIE editor uses a cloned world but the BP asset
 * pointer is shared).
 *
 * **Graph resolution.** ``graph_name`` (default ``"EventGraph"``) searches:
 *   1. ``Blueprint->UbergraphPages`` (canonical event graph name is "EventGraph")
 *   2. ``Blueprint->FunctionGraphs`` (user functions + construction script)
 *   3. ``Blueprint->MacroGraphs`` (macro definitions)
 *
 * **Node config.** Common K2Node subclasses receive extra type-specific config:
 *   - ``K2Node_VariableGet`` / ``K2Node_VariableSet`` → ``variable_name`` (self-member)
 *   - ``K2Node_CallFunction``                         → ``function_name`` + ``function_class``
 *                                                       (self if class omitted)
 *   - ``K2Node_CustomEvent``                          → ``event_name`` (CustomFunctionName)
 *   - All others: bare default-pin allocation via ``AllocateDefaultPins`` only. Caller wires
 *     specific properties via subsequent ``marshall.write_property`` (out of Phase-7 scope:
 *     full graph-data binding).
 *
 * **Pin connection.** ``bp.connect_pins`` resolves nodes by Guid (returned by ``bp.add_node``
 * and ``bp.list_nodes_in_function``) and pins by FName. Uses
 * ``UEdGraphSchema_K2::TryCreateConnection`` which:
 *   - Validates pin direction (one input + one output required)
 *   - Validates type compatibility (with widening / promote-to-ref)
 *   - Breaks existing single-link pin connections (reported in ``broke_existing_count``)
 *   - Returns false if both pins are inputs / outputs, or types are incompatible
 *
 * **Error codes (reuses Phase 4 codes plus 2 new):**
 *   -32602 InvalidParams       missing required args
 *   -32004 ObjectNotFound      blueprint or graph not found
 *   -32010 InvalidPath         malformed blueprint_path
 *   -32011 WrongClass          node_class doesn't resolve to UK2Node subclass
 *   -32020 ClassNotFound       node_class not loadable
 *   -32027 PIEActive           PIE running
 *   -32031 BlueprintTypeMismatch  asset isn't a UBlueprint
 *   -32050 GraphNotFound       (NEW) graph_name not in UbergraphPages/FunctionGraphs/MacroGraphs
 *   -32051 NodeNotFound        (NEW) from_node/to_node Guid not in target graph
 *   -32052 PinNotFound         (NEW) from_pin/to_pin name not on target node
 *   -32053 PinConnectionRefused (NEW) schema rejected the connection (incompatible types,
 *                              both same direction, etc.) — message carries schema's reason
 *
 * Note: -32050 / -32051 / -32052 / -32053 land in MCPTypes.h alongside this file.
 */
namespace FBlueprintGraphTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Tier 4: graph node construction ────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddNode(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ConnectPins(const FMCPRequest& Request);
}
