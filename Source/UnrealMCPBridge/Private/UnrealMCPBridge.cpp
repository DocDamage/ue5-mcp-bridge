// Copyright FatumGame. All Rights Reserved.

#include "UnrealMCPBridge.h"

#include "FMCPDispatchQueue.h"
#include "FMCPPythonBootstrap.h"
#include "FMCPServer.h"
#include "MCPTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogMCP);

void FUnrealMCPBridgeModule::StartupModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module starting (Phase 1 Day 2 — TCP listener + dispatch queue)"));

	// 1. Open the TCP listener. Failure here does not abort module load; we log + continue so the
	//    user can MCP.RestartListener after fixing the port conflict.
	FString StartErr;
	if (!FMCPServer::Get().Start(kMCPDefaultPort, StartErr))
	{
		UE_LOG(LogMCP, Warning,
			TEXT("MCP bridge listener failed to start: %s. Use 'MCP.RestartListener' after resolving the conflict."),
			*StartErr);
	}

	// 2. Register default game-thread handlers (editor.ping for Day 2).
	RegisterDefaultDispatchHandlers();

	// 3. Hook the OnEndFrame drain.
	OnEndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FUnrealMCPBridgeModule::OnEndFrame);

	// 4. Python sys.path bootstrap — fires immediately if Python is already up, else queues.
	FMCPPythonBootstrap::RegisterPythonInitCallback();

	bStarted = true;
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module ready (listener=%s port=%d)"),
		FMCPServer::Get().IsListening() ? TEXT("RUNNING") : TEXT("STOPPED"),
		FMCPServer::Get().GetListenPort());
}

void FUnrealMCPBridgeModule::ShutdownModule()
{
	UE_LOG(LogMCP, Log, TEXT("MCP bridge module shutting down"));

	if (OnEndFrameHandle.IsValid())
	{
		FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
		OnEndFrameHandle.Reset();
	}

	UnregisterDefaultDispatchHandlers();

	// Tear the listener down LAST so any in-flight response from a Drain caught before us can still
	// be sent (defensive — we already unregistered OnEndFrame so this is mostly cosmetic).
	FMCPServer::Get().Stop();

	bStarted = false;
}

bool FUnrealMCPBridgeModule::IsListening() const
{
	return FMCPServer::Get().IsListening();
}

int32 FUnrealMCPBridgeModule::GetListenPort() const
{
	return FMCPServer::Get().GetListenPort();
}

int64 FUnrealMCPBridgeModule::GetDispatchedRequestCount() const
{
	return FMCPDispatchQueue::Get().GetDispatchedCount();
}

void FUnrealMCPBridgeModule::OnEndFrame()
{
	// Pumps both: drains the dispatch queue (handlers run + responses sent), then GCs closed sockets.
	FMCPDispatchQueue::Get().Drain();
	FMCPServer::Get().GarbageCollectClosedConnections();
}

void FUnrealMCPBridgeModule::RegisterDefaultDispatchHandlers()
{
	const FString PingMethod = TEXT("editor.ping");
	FMCPDispatchQueue::Get().RegisterHandler(PingMethod,
		[](const FMCPRequest& Req) -> FMCPResponse
		{
			FMCPResponse Resp;
			Resp.RequestId = Req.RequestId;
			Resp.OriginalIdString = Req.OriginalIdString;
			Resp.bIsError = false;

			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetBoolField(TEXT("pong"), true);
			Resp.Result = MakeShared<FJsonValueObject>(Payload);
			return Resp;
		});
	RegisteredMethodNames.Add(PingMethod);
	UE_LOG(LogMCP, Log, TEXT("Registered dispatch handler: %s"), *PingMethod);
}

void FUnrealMCPBridgeModule::UnregisterDefaultDispatchHandlers()
{
	for (const FString& Method : RegisteredMethodNames)
	{
		FMCPDispatchQueue::Get().UnregisterHandler(Method);
	}
	RegisteredMethodNames.Reset();
}

IMPLEMENT_MODULE(FUnrealMCPBridgeModule, UnrealMCPBridge)
