// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FMCPConnection;
class FSocket;
class FTcpListener;
struct FIPv4Endpoint;
struct FMCPResponse;

/**
 * TCP listener owner + per-connection registry for the MCP bridge.
 *
 * - Holds a single FTcpListener bound to 127.0.0.1:<port>.
 * - On accept (called from the listener's own accept thread) we wrap the FSocket in a
 *   new FMCPConnection runnable and spawn its worker thread. Connection added to a
 *   FCriticalSection-guarded list so the dispatcher can later find it for response routing.
 * - Periodic GC (driven by FMCPDispatchQueue::Drain on the game thread) prunes finished
 *   connections — Day 2 piggybacks: every Drain call also sweeps closed connections.
 *
 * Singleton-like: there is exactly one FMCPServer for the module's lifetime, but it's a
 * regular instance (the module owns it via TUniquePtr).
 */
class UNREALMCPBRIDGECORE_API FMCPServer
{
public:
	/** Module-owned singleton accessor. Returns a never-null reference once StartupModule has run. */
	static FMCPServer& Get();

	/** Construct uninitialised — call Start to actually open the socket. */
	FMCPServer();
	~FMCPServer();

	/**
	 * Open the listener on 127.0.0.1:Port. Returns false on bind failure; sets OutError.
	 * Idempotent — calling on an already-running listener is a no-op + warning.
	 */
	bool Start(int32 Port, FString& OutError);

	/** Tear down listener + all open connections. Safe to call multiple times. */
	void Stop();

	/** True iff Start() succeeded and Stop() has not been called. */
	bool IsListening() const { return bListening.load(std::memory_order_acquire); }

	/** Currently-bound port. Returns 0 when not listening. */
	int32 GetListenPort() const { return ListenPort.load(std::memory_order_acquire); }

	/** Live connection count (under lock). Cheap — exposed for MCP.Status. */
	int32 GetConnectionCount() const;

	/**
	 * Game-thread response routing: look up the connection by id and write the response.
	 * No-op (with warning log) if the connection has already closed — out-of-order responses are
	 * expected during shutdown / client disconnects.
	 */
	void SendResponse(int32 ConnectionId, const FMCPResponse& Response);

	/**
	 * Game-thread maintenance: drop closed connections from the list. Called from
	 * FMCPDispatchQueue::Drain (which is the OnEndFrame handler).
	 */
	void GarbageCollectClosedConnections();

	/** Total connections accepted since Start (for diagnostics). */
	int32 GetTotalAcceptedConnections() const { return TotalAccepted.load(std::memory_order_relaxed); }

private:
	/** FTcpListener delegate target. Runs on the accept thread — DOES NOT BLOCK. */
	bool OnConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& InRemoteEndpoint);

	/** Connection bookkeeping (id allocator + list). All mutating access under ConnectionsLock. */
	int32 NextConnectionId = 1;
	TArray<TSharedPtr<FMCPConnection>> Connections;
	mutable FCriticalSection ConnectionsLock;

	TUniquePtr<FTcpListener> Listener;

	std::atomic<bool> bListening{false};
	std::atomic<int32> ListenPort{0};
	std::atomic<int32> TotalAccepted{0};

	FMCPServer(const FMCPServer&) = delete;
	FMCPServer& operator=(const FMCPServer&) = delete;
};
