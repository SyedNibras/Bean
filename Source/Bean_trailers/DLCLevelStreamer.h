// DLCLevelStreamer.h - Ties UDLCManager to actual level travel
// Handles: registering Hostinger chunk URLs, downloading+mounting on demand,
// opening the level once ready, and unmounting the previous chunk to free memory.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "TimerManager.h"
#include "DLCManager.h"
#include "DLCLevelStreamer.generated.h"

UCLASS(Blueprintable, BlueprintType)
class BEAN_TRAILERS_API UDLCLevelStreamer : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Call once at startup (e.g. from GameInstance::Init).
	 * @param InDLCManager      Your existing DLC manager instance
	 * @param InWorldContext    Any valid world-context object (GameInstance is fine) - needed for OpenLevel
	 * @param InBaseLevelName   Name of your Chunk 0 / startup map
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Streaming")
	void Initialize(UDLCManager* InDLCManager, UObject* InWorldContext, FName InBaseLevelName);

	/**
	 * Registers Level 1 (chunk 1) and Level 2 (chunk 2) with their Hostinger PAK URLs.
	 * Edit the URLs / level paths below to match your actual hosting + content paths.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Streaming")
	void RegisterHostingerChunks();

	/**
	 * Downloads (if needed) + mounts + opens the level for the given chunk.
	 * Safe to call even if the chunk is already downloaded - it will just mount + travel.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Streaming")
	void TravelToChunkLevel(int32 ChunkId);

	/**
	 * Unmounts whatever chunk is currently loaded and returns to the base (Chunk 0) map.
	 * Call this from your "Exit to Main Menu" / "Back to Lobby" button.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Streaming")
	void ReturnToBaseLevel();

	/**
	 * Currently mounted/loaded chunk, or -1 if on the base level.
	 * Reads from DLCManager (not a local variable) because DLCLevelStreamer
	 * itself gets destroyed and recreated on every level travel - see the
	 * comment on DLCManager::GetCurrentLoadedChunkId().
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Streaming")
	int32 GetCurrentChunkId() const { return DLCManager ? DLCManager->GetCurrentLoadedChunkId() : -1; }

	/**
	 * Reclaims disk space by deleting cached chunk PAKs. Does NOT navigate
	 * anywhere and does NOT affect what's currently mounted/loaded in memory.
	 *
	 * @param bKeepCurrentChunk  If true (default), the chunk the player is
	 *        currently in (or could resume into) is left untouched - only
	 *        OTHER cached chunks are deleted. Safe to call mid-session.
	 *        If false, EVERYTHING is deleted with no exception - only call
	 *        this when the Pixel Streaming session has truly ended (not a
	 *        reload), since a subsequent resume attempt would then fail.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Session")
	void ClearCachedChunks(bool bKeepCurrentChunk = true);

	/**
	 * Call this from Blueprint when the Pixel Streaming "On All Connections
	 * Closed" delegate fires. Does NOT clean up immediately - starts a grace
	 * period timer instead, because a browser reload also momentarily drops
	 * to zero connections before reconnecting. If a connection comes back
	 * before the timer expires (see NotifyConnectionRestored), cleanup is
	 * cancelled. If the timer runs out with no reconnect, that's treated as
	 * a real session end and ClearCachedChunks(false) runs automatically.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Session")
	void NotifyAllConnectionsClosed();

	/**
	 * Call this from Blueprint when Pixel Streaming reports a new/restored
	 * connection (e.g. "On New Connection"). Cancels any pending cleanup
	 * timer started by NotifyAllConnectionsClosed() - this is what happens
	 * on an accidental reload, so the cache survives.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Session")
	void NotifyConnectionRestored();

	/**
	 * How long to wait, after all connections close, before treating it as
	 * a real session end and clearing the cache. Long enough to cover a
	 * browser reload's reconnect, short enough not to waste disk space
	 * needlessly if the session really is over. Tune to your setup.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLC|Session")
	float SessionEndGracePeriodSeconds = 20.0f;

protected:
	UPROPERTY()
	UDLCManager* DLCManager = nullptr;

	TWeakObjectPtr<UObject> WorldContext;
	FName BaseLevelName;

	int32 PendingChunkId = -1;
	// NOTE: CurrentLoadedChunkId used to live here as a local int32, but this
	// object is destroyed and recreated on every level travel (it's usually
	// owned by a PlayerController), so that state was silently lost every
	// time OpenLevel ran - which is why ReturnToBaseLevel() was never
	// actually deleting the PAK. It now lives on DLCManager instead, which
	// persists for the whole GameInstance. See DLCManager::CurrentLoadedChunkId.

	UFUNCTION()
	void HandleLevelReady(int32 ChunkId, bool bSuccess);

	// Fired by the grace-period timer if no connection came back in time -
	// this is the point where we treat it as a real session end.
	void HandleSessionEndGracePeriodExpired();

	// Set while NotifyAllConnectionsClosed()'s grace-period timer is running;
	// used by NotifyConnectionRestored() to cancel it.
	FTimerHandle SessionEndTimerHandle;

	// Unmounts a chunk (frees the memory/VRAM its assets were using) and,
	// optionally, also deletes its PAK from disk. Internal callers
	// (HandleLevelReady, ReturnToBaseLevel) now always pass false so that
	// switching between chunks - or going back to the base map - just frees
	// memory and leaves the PAK cached on disk for a fast re-visit later.
	// Actual disk deletion only happens via DLCManager::CleanupDownloadedChunks(),
	// exposed here through ClearCachedChunks().
	void UnloadChunk(int32 ChunkId, bool bDeleteFromDisk);
};
