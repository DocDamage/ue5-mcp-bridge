// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FMCPDispatchQueue;

/**
 * Wave B Tier 4 + Wave F Surface 1 — Blueprint graph-level CRUD. 7 user-visible tools, Lane A.
 *
 * Tool roster:
 *   bp.add_node             → instantiate a K2Node subclass on a UEdGraph and AddNode/AllocateDefaultPins
 *   bp.connect_pins         → wire two pins via UEdGraphSchema_K2::TryCreateConnection
 *   bp.set_node_property    → (Wave F1) write a UPROPERTY on a K2Node by name + type-marshalled JSON
 *   bp.set_pin_default      → (Wave F1) set DefaultValue / DefaultObject on an UNCONNECTED pin
 *   bp.delete_node          → (Wave F1) remove a node from its graph; auto-breaks all pin links
 *   bp.disconnect_pin       → (Wave F1) BreakAllPinLinks on a single pin
 *   bp.move_node            → (Wave F1) reposition a node on the graph (NodePosX/NodePosY)
 *
 * **All Lane A.** UEdGraph mutation + UK2Node spawn + schema connect + pin / property edits all
 * assume game-thread execution under the editor's transaction system. Blueprint asset state
 * mutation also goes through the standard ``Modify`` / ``MarkPackageDirty`` path so editor Undo
 * (Ctrl-Z) reverts.
 *
 * **PIE-guarded.** Every tool refuses during PIE with -32027 PIEActive — mutating a Blueprint
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
 *     specific properties via subsequent ``bp.set_node_property`` (Wave F1) or
 *     ``marshall.write_property`` for non-K2 owners.
 *
 * **Pin connection.** ``bp.connect_pins`` resolves nodes by Guid (returned by ``bp.add_node``
 * and ``bp.list_nodes_in_function``) and pins by FName. Uses
 * ``UEdGraphSchema_K2::TryCreateConnection`` which:
 *   - Validates pin direction (one input + one output required)
 *   - Validates type compatibility (with widening / promote-to-ref)
 *   - Breaks existing single-link pin connections (reported in ``broke_existing_count``)
 *   - Returns false if both pins are inputs / outputs, or types are incompatible
 *
 * **Node-property writes (Wave F1).** ``bp.set_node_property`` walks the K2Node's UClass with
 * ``FindPropertyByName`` and reuses the Phase-2 ``FMCPReflection::WritePropertyValueAt`` pipeline
 * (so JSON-typed values for vectors / enums / object refs round-trip through the same marshaller
 * as ``marshall.write_property``). Wrapped in ``FMCPWritePropertyScope`` so the 4-step contract
 * (PreEditChange → Modify → write → PostEditChangeProperty) fires correctly. ``Node->ReconstructNode``
 * follows the write so pin layout reflects any property-driven pin changes (e.g. ``bIsPureFunc``
 * toggling exec pin presence on ``K2Node_CallFunction``).
 *
 * **Pin-default writes (Wave F1).** ``bp.set_pin_default`` refuses pins with ``LinkedTo.Num() > 0``
 * (-32602) — a connected pin uses the linked value, not the default. Routes to either
 * ``UEdGraphSchema_K2::TrySetDefaultObject`` (hard object refs when value is a path string AND
 * pin category is PC_Object/PC_Class) or ``UEdGraphSchema_K2::TrySetDefaultValue`` (the canonical
 * string parser path for primitives, enums, structs, soft refs). Both schema paths call
 * ``Node->PinDefaultValueChanged`` + ``MarkBlueprintAsModified`` internally.
 *
 * **Node deletion (Wave F1).** ``bp.delete_node`` refuses K2Node_FunctionEntry / K2Node_FunctionResult
 * / K2Node_Event (excluding K2Node_CustomEvent — user-created events are deletable). Also checks
 * ``Node->CanUserDeleteNode()`` as a defense-in-depth guard against any future engine-blessed
 * undeletable subclass (Composites, Tunnels). On approval calls ``UEdGraph::RemoveNode(Node)``
 * which auto-breaks all linked pins.
 *
 * **Error codes (reuses Phase 4 codes plus Wave B Tier 4's 4 codes; no new codes for Wave F1):**
 *   -32602 InvalidParams       missing required args, deleting entry/result/event, default on connected pin
 *   -32004 ObjectNotFound      blueprint or graph not found
 *   -32005 PropertyNotFound    (Wave F1) set_node_property: bad property name
 *   -32006 PropertyTypeMismatch (Wave F1) set_node_property: value type wrong / set_pin_default: schema rejected
 *   -32010 InvalidPath         malformed blueprint_path
 *   -32011 WrongClass          node_class doesn't resolve to UK2Node subclass
 *   -32020 ClassNotFound       node_class not loadable
 *   -32027 PIEActive           PIE running
 *   -32031 BlueprintTypeMismatch  asset isn't a UBlueprint
 *   -32050 GraphNotFound       graph_name not in UbergraphPages/FunctionGraphs/MacroGraphs
 *   -32051 NodeNotFound        node_guid not in target graph
 *   -32052 PinNotFound         pin_name not on target node
 *   -32053 PinConnectionRefused schema rejected the connection
 */
namespace FBlueprintGraphTools
{
	UNREALMCPBRIDGE_API void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames);

	// ─── Tier 4: graph node construction ────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_AddNode(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_ConnectPins(const FMCPRequest& Request);

	// ─── Wave F1: graph CRUD ─────────────────────────────────────────────────────────────────────
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetNodeProperty(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_SetPinDefault(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DeleteNode(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_DisconnectPin(const FMCPRequest& Request);
	UNREALMCPBRIDGE_API FMCPResponse Tool_MoveNode(const FMCPRequest& Request);
}
