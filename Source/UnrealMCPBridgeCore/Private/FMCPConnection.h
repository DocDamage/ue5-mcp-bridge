// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "MCPTypes.h"

class FSocket;
class FRunnableThread;
struct FIPv4Endpoint;

/**
 * Per-client TCP connection runnable.
 *
 * Lifecycle:
 *   1. FMCPServer::OnConnectionAccepted creates a connection wrapping the freshly-accepted FSocket.
 *   2. FRunnableThread is spawned with this as its FRunnable.
 *   3. Run() loops: Wait for bytes → Recv → append to InboundBuffer → for every '\n' parse the
 *      preceding line as JSON, build FMCPRequest, push to FMCPDispatchQueue.
 *   4. On error / EOF / Stop() → close the socket, mark bClosed, return from Run().
 *   5. FMCPServer drains closed connections from its list each accept tick.
 *
 * SendResponse() is called from the GAME THREAD by FMCPServer (which forwards from
 * FMCPDispatchQueue::Drain). It serialises the response to JSON, appends '\n', and writes to the
 * socket under SendLock. Writes are blocking but each response is small enough (single tool result)
 * that we don't bother with an outbound queue for Day 2.
 *
 * Frame cap: a single inbound line exceeding kMCPFrameMaxBytes aborts the connection (per critic
 * C6) — the buffer is dropped, the socket closed with an error response if possible.
 */
class FMCPConnection : public FRunnable
{
public:
	FMCPConnection(int32 InConnectionId, FSocket* InSocket, const FIPv4Endpoint& InRemoteEndpoint);
	virtual ~FMCPConnection();

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	/** Numeric id assigned by FMCPServer at accept time. Echoed into FMCPRequest::SourceConnectionId. */
	int32 GetConnectionId() const { return ConnectionId; }

	/** Game-thread API: serialise + write a response line. Returns false on socket error. */
	bool SendResponse(const FMCPResponse& Response);

	/** True if Run() has exited (either gracefully or via Stop). FMCPServer polls this for GC. */
	bool IsClosed() const { return bClosed.load(std::memory_order_acquire); }

	/** Spawned thread reference; owned by this connection. Joined in destructor. */
	void SetThread(FRunnableThread* InThread) { Thread = InThread; }

private:
	/** Attempt to consume `\n`-terminated frames out of InboundBuffer. Returns false on protocol error. */
	bool ConsumeBufferedFrames();

	/** Parse a single complete frame (no trailing newline) into FMCPRequest + push to dispatch queue. */
	void HandleFrame(const FString& FrameJson);

	/** Map wire string ("call_function" / "ping" / etc.) to EMCPRequestKind. Returns false if unknown. */
	static bool ParseRequestKind(const FString& KindStr, EMCPRequestKind& OutKind);

	/** Serialise a response to a single JSON line (no trailing newline). */
	static FString SerializeResponse(const FMCPResponse& Response);

	/** Push a synthetic error response back to the client. Used when frame fails to parse. */
	void SendImmediateError(const FGuid& RequestId, int32 Code, const FString& Message);

	int32 ConnectionId = INDEX_NONE;
	FSocket* Socket = nullptr; // owned — destroyed in destructor via ISocketSubsystem::DestroySocket
	FString RemoteAddressText;

	FRunnableThread* Thread = nullptr; // owned; joined+deleted in destructor

	/** Read accumulator. Stays as TArray<uint8> until we hit '\n', then we slice into FString. */
	TArray<uint8> InboundBuffer;

	/** Serialises concurrent writes from game-thread SendResponse calls. */
	FCriticalSection SendLock;

	std::atomic<bool> bStopRequested{false};
	std::atomic<bool> bClosed{false};
};
