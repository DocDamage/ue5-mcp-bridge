// Copyright FatumGame. All Rights Reserved.

#include "FMCPServer.h"

#include "FMCPConnection.h"
#include "MCPTypes.h"

#include "Common/TcpListener.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/ScopeLock.h"

namespace
{
	/** Worker thread stack size in bytes. Recv buffer (64 KiB) + parser scratch fits easily. */
	constexpr uint32 kConnectionThreadStackBytes = 128 * 1024;
}

FMCPServer& FMCPServer::Get()
{
	static FMCPServer Instance;
	return Instance;
}

FMCPServer::FMCPServer() = default;

FMCPServer::~FMCPServer()
{
	Stop();
}

bool FMCPServer::Start(int32 Port, FString& OutError)
{
	if (bListening.load(std::memory_order_acquire))
	{
		OutError = TEXT("listener already running");
		UE_LOG(LogMCP, Warning, TEXT("FMCPServer::Start called but listener is already active on port %d"),
			ListenPort.load(std::memory_order_acquire));
		return true; // idempotent
	}

	const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), static_cast<uint16>(Port));

	// FTcpListener spawns its own thread internally; it owns the bound listening socket.
	Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(250));
	if (!Listener->GetSocket())
	{
		OutError = FString::Printf(TEXT("failed to bind to 127.0.0.1:%d (port in use?)"), Port);
		UE_LOG(LogMCP, Error, TEXT("FMCPServer::Start failed — %s"), *OutError);
		Listener.Reset();
		return false;
	}

	Listener->OnConnectionAccepted().BindRaw(this, &FMCPServer::OnConnectionAccepted);

	ListenPort.store(Port, std::memory_order_release);
	bListening.store(true, std::memory_order_release);
	UE_LOG(LogMCP, Log, TEXT("MCP bridge listening on 127.0.0.1:%d"), Port);
	return true;
}

void FMCPServer::Stop()
{
	if (!bListening.load(std::memory_order_acquire) && !Listener.IsValid())
	{
		return;
	}

	bListening.store(false, std::memory_order_release);

	// Tear down the listener first (stops accepting new connections; FTcpListener's destructor joins).
	if (Listener.IsValid())
	{
		Listener.Reset();
	}

	// Now drop all per-connection threads. Each TSharedPtr's destructor kills+joins its thread and
	// closes its socket — order-safe because Run() reads bStopRequested and bails.
	TArray<TSharedPtr<FMCPConnection>> ToDestroy;
	{
		FScopeLock Lock(&ConnectionsLock);
		ToDestroy = MoveTemp(Connections);
		Connections.Reset();
	}
	// Signal everyone first (parallel shutdown), then let RAII destructors join.
	for (const TSharedPtr<FMCPConnection>& Conn : ToDestroy)
	{
		if (Conn.IsValid())
		{
			Conn->Stop();
		}
	}
	ToDestroy.Reset(); // explicit teardown for clarity

	ListenPort.store(0, std::memory_order_release);
	UE_LOG(LogMCP, Log, TEXT("MCP bridge listener stopped"));
}

bool FMCPServer::OnConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& InRemoteEndpoint)
{
	// Accept-thread context. Do the minimum: wrap in FMCPConnection, spawn its thread, register.
	// Never block here — FTcpListener uses the return value to decide whether to keep the socket.
	if (!bListening.load(std::memory_order_acquire))
	{
		// Edge: socket accepted between Stop() flagging and listener teardown. Reject so caller closes.
		return false;
	}

	int32 NewId = 0;
	TSharedPtr<FMCPConnection> Conn;
	{
		FScopeLock Lock(&ConnectionsLock);
		NewId = NextConnectionId++;
		Conn = MakeShared<FMCPConnection>(NewId, InSocket, InRemoteEndpoint);
		Connections.Add(Conn);
	}

	// Spawn the per-connection worker. We hand the thread pointer back so the connection can join+delete in dtor.
	const FString ThreadName = FString::Printf(TEXT("MCPConn-%d"), NewId);
	FRunnableThread* WorkerThread = FRunnableThread::Create(
		Conn.Get(), *ThreadName, kConnectionThreadStackBytes, TPri_Normal);
	if (!WorkerThread)
	{
		UE_LOG(LogMCP, Error, TEXT("Failed to spawn worker thread for MCP connection %d"), NewId);
		FScopeLock Lock(&ConnectionsLock);
		Connections.RemoveSingleSwap(Conn);
		return false; // tells FTcpListener to close+destroy the socket
	}
	Conn->SetThread(WorkerThread);

	TotalAccepted.fetch_add(1, std::memory_order_relaxed);

	// Returning true tells FTcpListener "I took ownership of the socket; don't close it."
	return true;
}

int32 FMCPServer::GetConnectionCount() const
{
	FScopeLock Lock(&ConnectionsLock);
	return Connections.Num();
}

void FMCPServer::SendResponse(int32 ConnectionId, const FMCPResponse& Response)
{
	TSharedPtr<FMCPConnection> Target;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (const TSharedPtr<FMCPConnection>& Conn : Connections)
		{
			if (Conn.IsValid() && Conn->GetConnectionId() == ConnectionId)
			{
				Target = Conn;
				break;
			}
		}
	}

	if (!Target.IsValid())
	{
		UE_LOG(LogMCP, Warning, TEXT("FMCPServer::SendResponse: no connection with id=%d (already closed?)"),
			ConnectionId);
		return;
	}

	if (!Target->SendResponse(Response))
	{
		UE_LOG(LogMCP, Warning, TEXT("FMCPServer::SendResponse: write failed for connection %d"),
			ConnectionId);
	}
}

void FMCPServer::GarbageCollectClosedConnections()
{
	FScopeLock Lock(&ConnectionsLock);
	for (int32 i = Connections.Num() - 1; i >= 0; --i)
	{
		if (Connections[i].IsValid() && Connections[i]->IsClosed())
		{
			// Releasing the shared ptr triggers ~FMCPConnection which joins the worker thread + frees the socket.
			Connections.RemoveAtSwap(i, EAllowShrinking::No);
		}
	}
}
