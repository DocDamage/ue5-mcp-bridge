// Copyright FatumGame. All Rights Reserved.

#include "FMCPDispatchQueue.h"

#include "FMCPServer.h"
#include "UnrealMCPBridge.h"

FMCPDispatchQueue& FMCPDispatchQueue::Get()
{
	static FMCPDispatchQueue Instance;
	return Instance;
}

void FMCPDispatchQueue::Push(FMCPRequest&& Request)
{
	EnqueuedCount.fetch_add(1, std::memory_order_relaxed);
	InboundQueue.Enqueue(MoveTemp(Request));
}

void FMCPDispatchQueue::RegisterHandler(const FString& Method, FHandler&& Handler)
{
	check(!Method.IsEmpty());

	FScopeLock Lock(&HandlersLock);
	if (Handlers.Contains(Method))
	{
		UE_LOG(LogMCP, Warning, TEXT("Dispatch handler '%s' replaced (was registered, now overwritten)"), *Method);
	}
	Handlers.Add(Method, MoveTemp(Handler));
}

void FMCPDispatchQueue::UnregisterHandler(const FString& Method)
{
	FScopeLock Lock(&HandlersLock);
	Handlers.Remove(Method);
}

void FMCPDispatchQueue::Drain()
{
	check(IsInGameThread());

	FMCPRequest Request;
	while (InboundQueue.Dequeue(Request))
	{
		FHandler HandlerCopy;
		bool bHandlerFound = false;
		{
			FScopeLock Lock(&HandlersLock);
			if (const FHandler* Found = Handlers.Find(Request.Method))
			{
				HandlerCopy = *Found;
				bHandlerFound = true;
			}
		}

		FMCPResponse Response;
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
		if (bHandlerFound)
		{
			// Handler invoked under no lock — long-running work would block other Drain iterations
			// but cannot block the inbound TCP threads (they only Push).
			Response = HandlerCopy(Request);
			// Defensive — caller MUST preserve id; we re-stamp both fields.
			Response.RequestId = Request.RequestId;
			Response.OriginalIdString = Request.OriginalIdString;
			DispatchedCount.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			Response = MakeMethodNotFoundError(Request);
			UE_LOG(LogMCP, Warning, TEXT("Dispatch: no handler for method '%s' (conn=%d, id=%s)"),
				*Request.Method, Request.SourceConnectionId, *Request.OriginalIdString);
		}

		// Route response back to the originating connection. -1 means "no source" (synthetic / test).
		if (Request.SourceConnectionId != INDEX_NONE)
		{
			FMCPServer::Get().SendResponse(Request.SourceConnectionId, Response);
		}
	}
}

FMCPResponse FMCPDispatchQueue::MakeMethodNotFoundError(const FMCPRequest& Request)
{
	FMCPResponse Response;
	Response.RequestId = Request.RequestId;
	Response.OriginalIdString = Request.OriginalIdString;
	Response.bIsError = true;
	Response.ErrorCode = -32601; // JSON-RPC: Method not found
	Response.ErrorMessage = FString::Printf(TEXT("method not found: %s"), *Request.Method);
	return Response;
}
