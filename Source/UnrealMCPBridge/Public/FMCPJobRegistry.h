// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

class FQueuedThreadPool;
class IQueuedWork;

/**
 * Job lifecycle states. Wire-serialised as the string returned by LexJobState().
 *
 *   Pending   — submitted, not yet picked up by a worker.
 *   Running   — worker has started executing the body.
 *   Succeeded — body returned a non-null FJsonValue and didn't throw.
 *   Failed    — body returned null OR set FMCPJob::ErrorMessage.
 *   Cancelled — body honoured bCancelRequested and bailed early, OR was Pending when cancel arrived
 *               and never started.
 */
enum class EMCPJobState : uint8
{
	Pending,
	Running,
	Succeeded,
	Failed,
	Cancelled
};

UNREALMCPBRIDGE_API const TCHAR* LexJobState(EMCPJobState State);

/**
 * Single async job tracked by FMCPJobRegistry.
 *
 * **Lifetime contract:** the registry owns the FMCPJob via TSharedPtr. The body lambda captures a
 * TSharedRef<FMCPJob> by value so the job survives at least until the body returns, independent of
 * any concurrent reader (job.status / job.result / TTL GC).
 *
 * **Thread layout (per blueprint v2 §3.5 + critic M4):**
 *   - Producer (game thread): SubmitJob() flips State Pending→Running before AddQueuedWork (worker
 *     thread doesn't have to claim state itself).
 *   - Consumer (worker pool thread): runs Body, sets Result/ErrorMessage, transitions to terminal.
 *   - Readers (any thread): GetState / GetResult / RequestCancel are atomic-safe.
 *
 * **Game-thread-required bodies** set bGameThreadRequired=true at submit time. The worker thread
 * acts as a coordinator: it dispatches the body via AsyncTask(ENamedThreads::GameThread, ...) and
 * synchronously waits on a TPromise so the worker slot blocks until the GT body completes. This is
 * deliberate — long GT bodies SHOULD prefer a fan-out micro-job model rather than monopolising a
 * worker, but Day 7 ships the simple coordinator form; revisit in Phase 2 if it starves the pool.
 *
 * Day 7 scope: no progress UI, no streaming intermediate results, no cancellation acknowledgement
 * round-trip. Cancellation is cooperative — body MUST poll bCancelRequested.load() and bail.
 */
struct FMCPJob
{
	FGuid Id;

	/** Free-form description supplied by submitter (echoed in job.list_active). */
	FString Description;

	/** Authoritative state. Writers under FMCPJobRegistry::JobsLock; readers via GetState() atomic snapshot. */
	std::atomic<EMCPJobState> State{EMCPJobState::Pending};

	/** [0,1] progress hint set by body. UI-only; no enforcement / clamping by registry. */
	std::atomic<float> Progress{0.0f};

	/** Cancellation flag. Body must poll. Registry transitions State→Cancelled when body returns null after cancel. */
	std::atomic<bool> bCancelRequested{false};

	/** True when body MUST run on game thread (touches UObject / GWorld / asset registry). */
	bool bGameThreadRequired = false;

	/** Set by body on failure. Empty on success. */
	FString ErrorMessage;

	/** Body return value. Null until terminal state, then either Result OR ErrorMessage is set (XOR). */
	TSharedPtr<FJsonValue> Result;

	/** Wall-clock seconds (FPlatformTime::Seconds()) when SubmitJob added the job. */
	double SubmittedAt = 0.0;

	/** Wall-clock seconds when the worker thread (or GT dispatch) entered the body. 0 while Pending. */
	double StartedAt = 0.0;

	/** Wall-clock seconds when the body returned. 0 until terminal. */
	double FinishedAt = 0.0;

	FMCPJob() = default;
	FMCPJob(const FMCPJob&) = delete;
	FMCPJob& operator=(const FMCPJob&) = delete;
};

/**
 * Async job tracker + thread-pool driver for MCP bridge.
 *
 * **Singleton.** Instance lives for module lifetime. Pool is created in EnsureInitialized() (first
 * SubmitJob) so module bring-up doesn't pay the cost when nobody asks for async work.
 *
 * Worker pool sizing: `max(1, num_cores - 2)` to leave headroom for game thread + Jolt sim thread.
 * Separate from UE's GThreadPool / GTaskGraph so a long cook/build job doesn't starve UE internals.
 *
 * TTL cleanup: FTSTicker fires every 60s, removes jobs in terminal state whose FinishedAt is older
 * than 600s. Pending/Running jobs are NEVER reaped — only terminal.
 *
 * **No reentrancy lock around the body invocation.** Body may call any registry method (e.g. submit
 * a sub-job, query its own state). Only the JobsLock around the map operations is held briefly.
 */
class UNREALMCPBRIDGE_API FMCPJobRegistry
{
public:
	/** Job body signature. Receives mutable FMCPJob& so it can write Progress + ErrorMessage. */
	using FBody = TFunction<TSharedPtr<FJsonValue>(FMCPJob&)>;

	static FMCPJobRegistry& Get();

	/**
	 * Submit a new job. Allocates an FMCPJob, assigns a fresh FGuid, enqueues onto the worker pool.
	 * Returns the job id; query via GetState/GetResult/RequestCancel.
	 *
	 * @param Description     Free-form label for diagnostics.
	 * @param Body            Callable executed on the worker (or game thread if bGameThreadRequired).
	 *                        Returning null + setting ErrorMessage => Failed. Returning null with
	 *                        empty ErrorMessage + bCancelRequested=true => Cancelled. Returning
	 *                        non-null => Succeeded (ErrorMessage ignored).
	 * @param bGameThreadRequired  Body must touch UObject/GWorld — dispatched via AsyncTask(GT).
	 */
	FGuid SubmitJob(const FString& Description, FBody Body, bool bGameThreadRequired = false);

	/** Returns Pending if id unknown — caller can't distinguish "expired" from "never existed". */
	EMCPJobState GetState(const FGuid& Id) const;

	/** Returns null if job is missing OR not yet in a terminal Succeeded state. */
	TSharedPtr<FJsonValue> GetResult(const FGuid& Id) const;

	/**
	 * Snapshot of common fields useful for job.status. Returns false if id unknown.
	 * Captures atomics under their own load order — values are point-in-time consistent only
	 * with respect to themselves, not across fields.
	 */
	struct FStatusSnapshot
	{
		FGuid Id;
		FString Description;
		EMCPJobState State;
		float Progress;
		bool bCancelRequested;
		bool bGameThreadRequired;
		FString ErrorMessage;
		double SubmittedAt;
		double StartedAt;
		double FinishedAt;
	};
	bool GetStatus(const FGuid& Id, FStatusSnapshot& Out) const;

	/**
	 * Set the cancellation flag. Returns true if the job exists and is not already terminal.
	 * Body is responsible for honouring the flag — Pending jobs that never start become Cancelled
	 * when popped from the queue (state transition happens just before AsyncTask dispatch).
	 */
	bool RequestCancel(const FGuid& Id);

	/** Snapshot of every Pending/Running job. Terminal jobs excluded — they go to a separate accessor. */
	TArray<FStatusSnapshot> GetActive() const;

	/** Diagnostic — count of currently tracked jobs (active + terminal not yet TTL'd). */
	int32 GetTrackedJobCount() const;

	/**
	 * Module shutdown hook. Cancels all jobs, destroys the pool, removes the TTL ticker. Safe to
	 * call multiple times. Job bodies that ignore bCancelRequested may delay shutdown by their full
	 * runtime — there is no forced abort.
	 */
	void Shutdown();

private:
	FMCPJobRegistry() = default;
	~FMCPJobRegistry();
	FMCPJobRegistry(const FMCPJobRegistry&) = delete;
	FMCPJobRegistry& operator=(const FMCPJobRegistry&) = delete;

	/** Lazy init on first SubmitJob — creates the thread pool + TTL ticker. */
	void EnsureInitialized();

	/** TTL sweep — called by FTSTicker every 60s. */
	bool TickTtlCleanup(float DeltaTime);

	/** Worker entry — pulled out so FMCPJobWork can call without friending. */
	void ExecuteJobOnWorker(TSharedRef<FMCPJob> Job, FBody Body);

	/** Allow the worker wrapper to call into us. */
	friend class FMCPJobWork;

	FQueuedThreadPool* Pool = nullptr;
	std::atomic<bool> bInitialised{false};
	std::atomic<bool> bShutdown{false};

	/** Map<id → shared job>. All access under JobsLock. */
	TMap<FGuid, TSharedRef<FMCPJob>> Jobs;
	mutable FCriticalSection JobsLock;

	FTSTicker::FDelegateHandle TtlTickerHandle;

	/** TTL cutoff in seconds for terminal-state jobs. Hardcoded per blueprint §3.5 (cvar deferred). */
	static constexpr double kTtlSeconds = 600.0;

	/** TTL sweep period in seconds. */
	static constexpr float kTtlSweepIntervalSeconds = 60.0f;
};
