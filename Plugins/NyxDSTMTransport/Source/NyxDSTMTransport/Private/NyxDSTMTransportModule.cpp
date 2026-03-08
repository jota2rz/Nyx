// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxDSTMTransportModule.h"
#include "NyxDSTMSubsystem.h"
#include "Engine/Engine.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if UE_WITH_REMOTE_OBJECT_HANDLE
#include "UObject/RemoteObjectTransfer.h"
#include "UObject/RemoteObjectTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogNyxDSTM, Log, All);

// ─── Module Interface ─────────────────────────────────────────────

void FNyxDSTMTransportModule::StartupModule()
{
	UE_LOG(LogNyxDSTM, Log, TEXT("NyxDSTMTransport: StartupModule"));

	InitializeServerIdentity();
	BindTransportDelegates();
}

void FNyxDSTMTransportModule::ShutdownModule()
{
	UE_LOG(LogNyxDSTM, Log, TEXT("NyxDSTMTransport: ShutdownModule"));
}

FNyxDSTMTransportModule& FNyxDSTMTransportModule::Get()
{
	return FModuleManager::GetModuleChecked<FNyxDSTMTransportModule>(TEXT("NyxDSTMTransport"));
}

// ─── Server Identity ──────────────────────────────────────────────

void FNyxDSTMTransportModule::InitializeServerIdentity()
{
#if UE_WITH_REMOTE_OBJECT_HANDLE
	// Parse the server ID from command line: -DedicatedServerId=server-1
	FString ServerIdStr;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-DedicatedServerId="), ServerIdStr, /*bShouldStopOnSeparator=*/false))
	{
		UE_LOG(LogNyxDSTM, Log, TEXT("NyxDSTMTransport: No -DedicatedServerId= on command line. "
			"Server identity not initialized (OK for clients/editor)."));
		return;
	}

	// Hash the string ID to a uint32 for FRemoteServerId
	const uint32 ServerId = GetTypeHash(ServerIdStr);

	// InitGlobalServerId can only be called once — it asserts on re-initialization.
	// This must happen before any UObjects are allocated with remote object handles.
	FRemoteServerId::InitGlobalServerId(FRemoteServerId(ServerId));
	bServerIdentityInitialized = true;

	UE_LOG(LogNyxDSTM, Log,
		TEXT("NyxDSTMTransport: Server identity initialized — DedicatedServerId='%s' → FRemoteServerId=%u"),
		*ServerIdStr, ServerId);
#else
	UE_LOG(LogNyxDSTM, Warning,
		TEXT("NyxDSTMTransport: UE_WITH_REMOTE_OBJECT_HANDLE is disabled. "
			"DSTM transport requires an engine build with this define set to 1."));
#endif
}

// ─── Transport Delegate Binding ───────────────────────────────────

void FNyxDSTMTransportModule::BindTransportDelegates()
{
#if UE_WITH_REMOTE_OBJECT_HANDLE
	if (!bServerIdentityInitialized)
	{
		UE_LOG(LogNyxDSTM, Log,
			TEXT("NyxDSTMTransport: Skipping delegate binding — server identity not initialized."));
		return;
	}

	// Bind the send delegate — called by the engine when an object migration
	// is ready to send. Our binding replaces the default disk I/O transport.
	// RemoteObject.cpp:317 checks !IsBound() before applying disk defaults,
	// so binding here (before InitRemoteObjects) ensures our transport wins.
	UE::RemoteObject::Transfer::RemoteObjectTransferDelegate.BindStatic(
		&FNyxDSTMTransportModule::OnRemoteObjectTransfer);

	// Bind the request delegate — called when a server needs to pull an object
	// from a remote server (pull-migration).
	UE::RemoteObject::Transfer::RequestRemoteObjectDelegate.BindStatic(
		&FNyxDSTMTransportModule::OnRequestRemoteObject);

	UE_LOG(LogNyxDSTM, Log,
		TEXT("NyxDSTMTransport: Transport delegates bound (beacon-based, replacing disk I/O)."));
#endif
}

// ─── Static Transport Callbacks ───────────────────────────────────

#if UE_WITH_REMOTE_OBJECT_HANDLE

/**
 * Find the NyxDSTMSubsystem in any active game instance.
 * Works on dedicated servers where GameViewport may not exist.
 */
static UNyxDSTMSubsystem* FindDSTMSubsystem()
{
	if (!GEngine)
	{
		return nullptr;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UGameInstance* GI = Context.OwningGameInstance;
		if (GI)
		{
			UNyxDSTMSubsystem* Sub = GI->GetSubsystem<UNyxDSTMSubsystem>();
			if (Sub)
			{
				return Sub;
			}
		}
	}

	return nullptr;
}

void FNyxDSTMTransportModule::OnRemoteObjectTransfer(
	const UE::RemoteObject::Transfer::FMigrateSendParams& Params)
{
	UNyxDSTMSubsystem* Sub = FindDSTMSubsystem();
	if (Sub)
	{
		Sub->HandleOutgoingMigration(Params);
	}
	else
	{
		UE_LOG(LogNyxDSTM, Error,
			TEXT("NyxDSTMTransport: RemoteObjectTransferDelegate fired but no NyxDSTMSubsystem found! "
				"Migration data will be lost."));
	}
}

void FNyxDSTMTransportModule::OnRequestRemoteObject(
	FRemoteWorkPriority Priority,
	FRemoteObjectId ObjectId,
	FRemoteServerId LastKnownServerId,
	FRemoteServerId DestServerId)
{
	UNyxDSTMSubsystem* Sub = FindDSTMSubsystem();
	if (Sub)
	{
		Sub->HandleObjectRequest(Priority, ObjectId, LastKnownServerId, DestServerId);
	}
	else
	{
		UE_LOG(LogNyxDSTM, Error,
			TEXT("NyxDSTMTransport: RequestRemoteObjectDelegate fired but no NyxDSTMSubsystem found!"));
	}
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE

// ─── Module Registration ──────────────────────────────────────────

IMPLEMENT_MODULE(FNyxDSTMTransportModule, NyxDSTMTransport)
