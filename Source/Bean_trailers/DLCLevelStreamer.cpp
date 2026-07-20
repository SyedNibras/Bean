// DLCLevelStreamer.cpp

#include "DLCLevelStreamer.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Engine/World.h"

void UDLCLevelStreamer::Initialize(UDLCManager* InDLCManager, UObject* InWorldContext, FName InBaseLevelName)
{
	DLCManager = InDLCManager;
	WorldContext = InWorldContext;
	BaseLevelName = InBaseLevelName;

	if (DLCManager)
	{
		// Fires once a chunk is both downloaded AND mounted (see UDLCManager::PrepareLevel)
		DLCManager->OnLevelReady.AddDynamic(this, &UDLCLevelStreamer::HandleLevelReady);

		// Resume-on-reload: if we're currently sitting on the base/startup
		// map AND a previous session left a chunk saved (e.g. the Pixel
		// Streaming browser tab was reloaded while the player was mid-level,
		// which restarts the whole game process), jump straight back into
		// that chunk instead of leaving the player stuck at the main menu.
		//
		// Guarded to only fire when the CURRENT map matches BaseLevelName so
		// this doesn't also try to "resume" every time Initialize() runs on
		// a chunk map itself.
		if (UObject* Ctx = WorldContext.Get())
		{
			const FString CurrentLevelName = UGameplayStatics::GetCurrentLevelName(Ctx, /*bRemovePrefixString=*/true);
			if (CurrentLevelName == BaseLevelName.ToString())
			{
				const int32 SavedChunkId = DLCManager->LoadLastChunkId();
				if (SavedChunkId != -1)
				{
					UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Resuming previous session - chunk %d was loaded before restart"), SavedChunkId);
					TravelToChunkLevel(SavedChunkId);
				}
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: Initialize called with null DLCManager!"));
	}
}

void UDLCLevelStreamer::RegisterHostingerChunks()
{
	if (!DLCManager)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: RegisterHostingerChunks - DLCManager is null"));
		return;
	}

	// ---- Chunk 1 : Level 1 ----
	FDLCChunkInfo Chunk1;
	Chunk1.ChunkId = 1;
	Chunk1.Name = TEXT("Level 1");
	Chunk1.Version = TEXT("1.0.0"); // bump this on your server manifest when you re-upload the PAK
	Chunk1.WindowsURL = TEXT("https://yourdomain.com/dlc/chunk1_windows.pak");
	Chunk1.AndroidURL = TEXT("https://yourdomain.com/dlc/chunk1_android.pak");
	Chunk1.LevelPath = TEXT("/Game/Levels/Level1/Level1"); // path INSIDE the pak, must match cook output
	DLCManager->AddChunkInfo(Chunk1);

	// ---- Chunk 2 : Level 2 ----
	FDLCChunkInfo Chunk2;
	Chunk2.ChunkId = 2;
	Chunk2.Name = TEXT("Level 2");
	Chunk2.Version = TEXT("1.0.0");
	Chunk2.WindowsURL = TEXT("https://yourdomain.com/dlc/chunk2_windows.pak");
	Chunk2.AndroidURL = TEXT("https://yourdomain.com/dlc/chunk2_android.pak");
	Chunk2.LevelPath = TEXT("/Game/Levels/Level2/Level2");
	DLCManager->AddChunkInfo(Chunk2);

	UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Registered Hostinger chunks 1 and 2"));

	// NOTE: instead of hardcoding URLs here, you can call
	// DLCManager->LoadManifestFromURL(TEXT("https://yourdomain.com/dlc/manifest.json"))
	// and host a manifest.json with the same fields - that way you can add/update
	// chunks without shipping a new build. See DLCManager::LoadManifestFromURL.
}

void UDLCLevelStreamer::TravelToChunkLevel(int32 ChunkId)
{
	if (!DLCManager)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: TravelToChunkLevel - DLCManager is null"));
		return;
	}

	if (ChunkId == DLCManager->GetCurrentLoadedChunkId())
	{
		UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Chunk %d already loaded"), ChunkId);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Requesting chunk %d (download-if-needed + mount)"), ChunkId);

	PendingChunkId = ChunkId;

	// PrepareLevel() internally: checks version -> downloads if missing/outdated ->
	// mounts once downloaded -> broadcasts OnLevelReady when both steps are done.
	DLCManager->PrepareLevel(ChunkId);
}

void UDLCLevelStreamer::HandleLevelReady(int32 ChunkId, bool bSuccess)
{
	// Ignore events for chunks we didn't ask for (e.g. a stray broadcast)
	if (ChunkId != PendingChunkId)
	{
		return;
	}

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: Chunk %d failed to download/mount"), ChunkId);
		PendingChunkId = -1;
		return;
	}

	// Free the previous chunk's memory before loading the new one.
	// bDeleteFromDisk=false: keep the PAK cached on disk so that, as long as
	// this Pixel Streaming session stays active, coming back to this chunk
	// later (directly or via the base map) just re-mounts it instantly
	// instead of re-downloading it. Only the memory/VRAM is freed here -
	// disk cleanup happens separately, see ClearCachedChunks().
	int32 PreviouslyLoadedChunkId = DLCManager->GetCurrentLoadedChunkId();
	if (PreviouslyLoadedChunkId != -1 && PreviouslyLoadedChunkId != ChunkId)
	{
		UnloadChunk(PreviouslyLoadedChunkId, /*bDeleteFromDisk=*/false);
	}

	DLCManager->SetCurrentLoadedChunkId(ChunkId);
	PendingChunkId = -1;

	// Find the level path for this chunk and open it
	for (const FDLCChunkInfo& Info : DLCManager->GetAvailableChunks())
	{
		if (Info.ChunkId == ChunkId)
		{
			if (UObject* Ctx = WorldContext.Get())
			{
				UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Opening level %s (chunk %d)"), *Info.LevelPath, ChunkId);
				UGameplayStatics::OpenLevel(Ctx, FName(*Info.LevelPath));
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: WorldContext is invalid, cannot OpenLevel"));
			}
			return;
		}
	}

	UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: No LevelPath found for chunk %d"), ChunkId);
}

void UDLCLevelStreamer::ReturnToBaseLevel()
{
	if (!DLCManager)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: ReturnToBaseLevel - DLCManager is null"));
		return;
	}

	int32 ChunkToUnload = DLCManager->GetCurrentLoadedChunkId();
	if (ChunkToUnload != -1)
	{
		// bDeleteFromDisk=false: only free memory. The PAK stays cached on
		// disk so that if the player picks the same (or a different, already
		// visited) level again later in this session, it loads instantly
		// with no re-download. Disk space is only reclaimed later via
		// ClearCachedChunks(), typically hooked to the actual end of the
		// Pixel Streaming session.
		UnloadChunk(ChunkToUnload, /*bDeleteFromDisk=*/false);
		DLCManager->SetCurrentLoadedChunkId(-1);
	}

	if (UObject* Ctx = WorldContext.Get())
	{
		UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Returning to base level %s"), *BaseLevelName.ToString());
		UGameplayStatics::OpenLevel(Ctx, BaseLevelName);
	}
}

void UDLCLevelStreamer::ClearCachedChunks(bool bKeepCurrentChunk)
{
	if (!DLCManager)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: ClearCachedChunks - DLCManager is null"));
		return;
	}

	const int32 ChunkIdToKeep = bKeepCurrentChunk ? DLCManager->GetCurrentLoadedChunkId() : -1;
	DLCManager->CleanupDownloadedChunks(ChunkIdToKeep);
}

void UDLCLevelStreamer::NotifyAllConnectionsClosed()
{
	UObject* Ctx = WorldContext.Get();
	UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCLevelStreamer: NotifyAllConnectionsClosed - no valid World, cannot start grace-period timer"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: All connections closed - starting %.1fs grace period before treating this as a real session end"), SessionEndGracePeriodSeconds);

	// If a previous timer was somehow still running, replace it rather than
	// stacking two.
	World->GetTimerManager().ClearTimer(SessionEndTimerHandle);
	World->GetTimerManager().SetTimer(
		SessionEndTimerHandle,
		this,
		&UDLCLevelStreamer::HandleSessionEndGracePeriodExpired,
		SessionEndGracePeriodSeconds,
		/*bLoop=*/false);
}

void UDLCLevelStreamer::NotifyConnectionRestored()
{
	UObject* Ctx = WorldContext.Get();
	UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	if (World->GetTimerManager().IsTimerActive(SessionEndTimerHandle))
	{
		UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Connection restored before grace period expired - cancelling cache cleanup (this was a reload, not a real session end)"));
		World->GetTimerManager().ClearTimer(SessionEndTimerHandle);
	}
}

void UDLCLevelStreamer::HandleSessionEndGracePeriodExpired()
{
	UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Grace period expired with no reconnect - treating this as a real session end, clearing cached chunks"));

	// bKeepCurrentChunk=false: nobody is coming back, so there's nothing to
	// protect - delete everything and reclaim the disk space.
	ClearCachedChunks(/*bKeepCurrentChunk=*/false);
}

void UDLCLevelStreamer::UnloadChunk(int32 ChunkId, bool bDeleteFromDisk)
{
	if (!DLCManager)
	{
		return;
	}

	if (DLCManager->IsChunkMounted(ChunkId))
	{
		DLCManager->UnmountChunk(ChunkId);
		UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Unmounted chunk %d"), ChunkId);
	}

	if (bDeleteFromDisk)
	{
		DLCManager->DeleteChunk(ChunkId);
		UE_LOG(LogTemp, Log, TEXT("DLCLevelStreamer: Deleted chunk %d from disk"), ChunkId);
	}
}
