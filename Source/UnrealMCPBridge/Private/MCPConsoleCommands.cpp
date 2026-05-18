// Copyright FatumGame. All Rights Reserved.

#include "FMCPDispatchQueue.h"
#include "FMCPServer.h"
#include "MCPTypes.h"
#include "UnrealMCPBridge.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace
{
	void HandleMCPStatus(FOutputDevice& Out)
	{
		FMCPServer& Server = FMCPServer::Get();
		FMCPDispatchQueue& Queue = FMCPDispatchQueue::Get();

		Out.Logf(TEXT("[MCP] listener=%s port=%d connections=%d total_accepted=%d enqueued=%lld dispatched=%lld"),
			Server.IsListening() ? TEXT("RUNNING") : TEXT("STOPPED"),
			Server.GetListenPort(),
			Server.GetConnectionCount(),
			Server.GetTotalAcceptedConnections(),
			static_cast<long long>(Queue.GetEnqueuedCount()),
			static_cast<long long>(Queue.GetDispatchedCount()));
	}

	void HandleMCPRestartListener(FOutputDevice& Out)
	{
		FMCPServer& Server = FMCPServer::Get();
		const int32 PreviousPort = Server.GetListenPort();
		const int32 RestartPort = PreviousPort > 0 ? PreviousPort : kMCPDefaultPort;

		Out.Logf(TEXT("[MCP] Restarting listener on port %d ..."), RestartPort);
		Server.Stop();

		FString Err;
		if (Server.Start(RestartPort, Err))
		{
			Out.Logf(TEXT("[MCP] Listener restarted on port %d"), RestartPort);
		}
		else
		{
			Out.Logf(ELogVerbosity::Error, TEXT("[MCP] Listener restart failed: %s"), *Err);
		}
	}

	// Use the output-device variants so output reliably appears in the console panel.
	static FAutoConsoleCommandWithOutputDevice GMCPStatusCmd(
		TEXT("MCP.Status"),
		TEXT("Reports MCP bridge listener state: port, connection count, dispatched request count."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&HandleMCPStatus));

	static FAutoConsoleCommandWithOutputDevice GMCPRestartListenerCmd(
		TEXT("MCP.RestartListener"),
		TEXT("Stops then restarts the MCP TCP listener on its currently-bound port (or default)."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&HandleMCPRestartListener));
}
