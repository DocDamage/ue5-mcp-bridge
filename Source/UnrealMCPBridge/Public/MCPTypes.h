// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

/** Default TCP port for the MCP bridge listener (loopback only). */
inline constexpr int32 kMCPDefaultPort = 30020;

/**
 * Per-line frame cap (bytes). Raised from v1's 8 MiB to 64 MiB per blueprint v2 §C6 — large
 * screenshot/thumbnail payloads exceed the older limit even after base64 wrapping. Any single
 * inbound line larger than this aborts the connection (DoS guard).
 */
inline constexpr int32 kMCPFrameMaxBytes = 64 * 1024 * 1024;

/**
 * Distinguishes the high-level kind of a wire request.
 *
 * Phase 1 baseline — values map 1:1 to the protocol verbs described in the
 * v2 blueprint (D:/tmp/mcp_unreal_blueprint_v2_patch.md §3). Additional kinds
 * (live coding, transactions, etc.) will be appended in later phases; existing
 * values MUST stay numerically stable so wire-format remains backward compatible.
 */
enum class EMCPRequestKind : uint8
{
	Ping,
	CallFunction,
	ExecPython,
	GetTools,
	JobSubmit,
	JobStatus,
	JobResult,
	JobCancel,
	LogTail,
	LogSubscribe,
	Shutdown
};

/**
 * In-process request POD. Built by the TCP listener after JSON parsing and handed
 * off to the dispatch queue. Phase 1 Day 1: type declared only; no producers yet.
 *
 * Args ownership: the shared pointer is the canonical home for parsed JSON during
 * the request's lifetime. Do not retain raw pointers into Args past completion.
 */
struct FMCPRequest
{
	/** Unique request id assigned by the server (correlates with FMCPResponse::RequestId). */
	FGuid RequestId;

	/** High-level kind dispatched on by the worker. */
	EMCPRequestKind Kind = EMCPRequestKind::Ping;

	/** For CallFunction / GetTools / JobSubmit etc., the method/tool name. */
	FString Method;

	/** Parsed args object. May be null for kinds that carry no payload (Ping, GetTools). */
	TSharedPtr<FJsonObject> Args;

	/** Opaque listener-side connection id; used for routing async responses. */
	int32 SourceConnectionId = INDEX_NONE;

	/** Wall-clock seconds when the listener accepted this request (for timeouts/metrics). */
	double ReceivedAtSeconds = 0.0;

	/**
	 * Verbatim "id" string from the wire frame. Preserved so we can echo back exactly what the
	 * client sent — even if it wasn't GUID-shaped (e.g. "test-1"). RequestId above is synthesised
	 * from this when it's not parseable as a GUID; OriginalIdString is the source of truth for
	 * round-trip correlation.
	 */
	FString OriginalIdString;
};

/**
 * In-process response POD. Built by the dispatcher; serialised back to JSON by the listener.
 *
 * Error semantics:
 * - bIsError=true MUST be paired with non-empty ErrorMessage and a non-zero ErrorCode.
 * - ErrorCode follows JSON-RPC 2.0 conventions (-32000..-32099 = server-defined errors).
 */
struct FMCPResponse
{
	/** Echo of FMCPRequest::RequestId for correlation. */
	FGuid RequestId;

	/**
	 * Verbatim "id" string copied from FMCPRequest::OriginalIdString. Serialised as the response "id"
	 * field on the wire so opaque (non-GUID) client ids round-trip exactly. Empty falls back to
	 * RequestId.ToString(EGuidFormats::DigitsWithHyphens).
	 */
	FString OriginalIdString;

	/** True iff this is a structured error rather than a successful result. */
	bool bIsError = false;

	/** Set when bIsError=true. Zero on success. */
	int32 ErrorCode = 0;

	/** Human-readable diagnostic; empty on success. */
	FString ErrorMessage;

	/** Tool-defined result payload (any JSON value). Null on error. */
	TSharedPtr<FJsonValue> Result;
};
