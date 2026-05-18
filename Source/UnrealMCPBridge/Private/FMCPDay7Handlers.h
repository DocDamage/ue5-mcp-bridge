// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

/**
 * Day 7 C++ dispatch handlers — async jobs (job.*), log streaming (log.*), and the first-class
 * tools.list verb that replaces mcp_server's exec_python ferry.
 *
 * All handlers run on the game thread via FMCPDispatchQueue::Drain. job.submit dispatches the body
 * to a separate worker pool (see FMCPJobRegistry); the dispatch CALL itself is cheap (single map
 * insert + queue push).
 *
 * Threading mirror for log.* — FMCPLogStream::GetLastN / Search hold an internal critical section,
 * so call duration is bounded by ring depth (5000) × per-entry copy cost. Empirically ~1-2 ms.
 *
 * Failure surfaces (all return FMCPResponse{bIsError=true}):
 *   -32602 InvalidParams         missing / wrong-type args
 *   -32004 ObjectNotFound        job id unknown to registry (job.status, job.result, job.cancel)
 *   -32603 InternalError         registry unavailable / pool failure
 */
class FMCPDay7Handlers
{
public:
	// ----- job.* family ---------------------------------------------------------------------
	/**
	 * job.submit
	 *   args:   { "method": string, "args": object?, "description"?: string, "game_thread"?: bool }
	 *   return: { "job_id": "<guid>" }
	 *
	 * Body wraps a CallPythonTool(method, args) invocation in a job. game_thread defaults true
	 * because most useful Python tools touch the editor. Set false for pure-compute tools.
	 *
	 * NOTE: method is the Python tool name — C++ handlers cannot be wrapped today (they already
	 * run synchronously on Drain in <5 ms). Wrapping them would just add overhead.
	 */
	static FMCPResponse JobSubmit(const FMCPRequest& Request);

	/**
	 * job.status
	 *   args:   { "job_id": string }
	 *   return: { "id": str, "state": "Pending|Running|Succeeded|Failed|Cancelled",
	 *             "progress": float, "submitted_at": double, "started_at": double, "finished_at": double,
	 *             "description": str, "cancel_requested": bool, "game_thread": bool, "message"?: str }
	 */
	static FMCPResponse JobStatus(const FMCPRequest& Request);

	/**
	 * job.result
	 *   args:   { "job_id": string, "wait_timeout_s"?: number (default 0) }
	 *   return on Succeeded:  { "ok": true, "result": <body-return-value> }
	 *   return on Failed:     { "ok": false, "error": str }
	 *   return on Cancelled:  { "ok": false, "cancelled": true }
	 *   return on still-running with wait_timeout_s elapsed: { "ok": false, "pending": true }
	 *
	 * Day 7: wait_timeout_s is HONOURED via FPlatformProcess::Sleep loop on the GAME THREAD —
	 * which stalls Drain. Use ONLY for short waits (default 0). Production clients SHOULD poll
	 * job.status with their own external timer instead.
	 */
	static FMCPResponse JobResult(const FMCPRequest& Request);

	/**
	 * job.cancel
	 *   args:   { "job_id": string }
	 *   return: { "accepted": bool }  // true if id valid and not yet terminal
	 */
	static FMCPResponse JobCancel(const FMCPRequest& Request);

	/**
	 * job.list_active
	 *   args:   {}
	 *   return: { "jobs": [ {id,description,state,progress,...}, ... ] }
	 */
	static FMCPResponse JobListActive(const FMCPRequest& Request);

	// ----- log.* family ---------------------------------------------------------------------
	/**
	 * log.tail
	 *   args:   { "lines"?: int (default 200), "category"?: string }
	 *   return: { "entries": [ {timestamp,category,verbosity,message}, ... ], "total_observed": int64 }
	 *
	 * Entries returned in chronological order (oldest first within the requested window).
	 */
	static FMCPResponse LogTail(const FMCPRequest& Request);

	/**
	 * log.subscribe
	 *   args:   { "category"?: string }
	 *   return: { "subscribed": true, "note": "phase-1 ack stub; push streaming arrives in phase 2" }
	 *
	 * Phase 1 ack stub. Real push subscription needs a protocol upgrade (WebSocket or framed bidirectional
	 * dispatch) — deferred to Phase 2.
	 */
	static FMCPResponse LogSubscribe(const FMCPRequest& Request);

	/**
	 * log.search
	 *   args:   { "pattern": string (regex), "max_results"?: int (default 100) }
	 *   return: { "entries": [ ... ], "total_scanned": int }
	 */
	static FMCPResponse LogSearch(const FMCPRequest& Request);

	// ----- tools.list -----------------------------------------------------------------------
	/**
	 * tools.list
	 *   args:   {}
	 *   return: { "python_tools": {name: {schema_in, schema_out, thread_safe, failure_modes}, ...},
	 *             "cpp_handlers": [ "editor.ping", "marshall.*", "job.*", "log.*", "tools.list", ... ] }
	 *
	 * Combined Python-registry query + C++ handler enumeration. Replaces mcp_server's
	 * exec_python ferry that previously did `from MCPTools.registry import get_all_tools; ...`.
	 *
	 * If Python isn't initialised, python_tools is an empty object and cpp_handlers is still
	 * populated (so the client can degrade gracefully).
	 */
	static FMCPResponse ToolsList(const FMCPRequest& Request);
};
