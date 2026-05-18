// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

/**
 * Game-thread dispatch queue for inbound MCP requests.
 *
 * **Producer:** TCP worker threads (FMCPConnection::Run) call Push().
 * **Consumer:** game thread (FCoreDelegates::OnEndFrame) calls Drain() — pops every pending request,
 * looks up a handler by `Method`, runs it synchronously, and ships the response back to the
 * originating connection via FMCPServer::SendResponse.
 *
 * Phase 1 Day 2 scope:
 * - Single handler registered: `editor.ping` (returns `{"pong":true}`).
 * - No Python eval yet (Day 3).
 * - No async / job system (Day 5+).
 *
 * Handlers run on the GAME THREAD. They can safely touch UObjects, GWorld, asset registry, etc.
 * Long-running work MUST be promoted to a job — Day 2 handlers are expected to be < 1 ms.
 */
class UNREALMCPBRIDGE_API FMCPDispatchQueue
{
public:
	/** Method dispatch signature: takes the parsed Args object (may be null), returns a populated response. */
	using FHandler = TFunction<FMCPResponse(const FMCPRequest&)>;

	/** Singleton accessor. The instance lives for the lifetime of the bridge module. */
	static FMCPDispatchQueue& Get();

	/**
	 * Producer-side: enqueue a request from any thread (typically a TCP worker).
	 * The request is moved into the queue. Caller MUST have set RequestId / SourceConnectionId
	 * before calling — Drain() relies on both for response routing.
	 */
	void Push(FMCPRequest&& Request);

	/**
	 * Game-thread sink: pop ALL pending requests, dispatch each by Method, and send responses.
	 * Called from FCoreDelegates::OnEndFrame. If a handler throws/asserts the caller is responsible
	 * for translating to an error response — currently we just rely on UE's assertion handler.
	 */
	void Drain();

	/**
	 * Register a synchronous game-thread handler for the given dotted method name.
	 * Replaces any existing handler with the same name (last-writer-wins; logs a warning on overwrite).
	 * Thread-safe — guarded by HandlersLock so it can be called during module bring-up while the
	 * listener is already accepting.
	 */
	void RegisterHandler(const FString& Method, FHandler&& Handler);

	/** Remove a handler. No-op if not registered. */
	void UnregisterHandler(const FString& Method);

	/** Diagnostic counter — total requests successfully dispatched since process start. */
	int64 GetDispatchedCount() const { return DispatchedCount.load(std::memory_order_relaxed); }

	/** Diagnostic counter — total requests enqueued (including unknown methods). */
	int64 GetEnqueuedCount() const { return EnqueuedCount.load(std::memory_order_relaxed); }

private:
	FMCPDispatchQueue() = default;
	~FMCPDispatchQueue() = default;
	FMCPDispatchQueue(const FMCPDispatchQueue&) = delete;
	FMCPDispatchQueue& operator=(const FMCPDispatchQueue&) = delete;

	/** Build a `method not found` error response. Caller sets RequestId. */
	static FMCPResponse MakeMethodNotFoundError(const FMCPRequest& Request);

	TQueue<FMCPRequest, EQueueMode::Mpsc> InboundQueue;

	/** Method → handler table. Read on game thread (Drain), written from any thread (RegisterHandler). */
	TMap<FString, FHandler> Handlers;
	mutable FCriticalSection HandlersLock;

	std::atomic<int64> EnqueuedCount{0};
	std::atomic<int64> DispatchedCount{0};
};
