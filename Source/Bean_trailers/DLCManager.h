// DLCManager.h - Handles downloading, mounting, and loading PAK files at runtime
// For UE 5.6 with chunked content delivery

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "DLCManager.generated.h"

// Delegate for download progress updates
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDLCDownloadProgress, int32, ChunkId, float, Progress, int64, BytesReceived);

// Delegate for download completion
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDLCDownloadComplete, int32, ChunkId, bool, bSuccess, const FString&, ErrorMessage);

// Delegate for mount completion
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDLCMountComplete, int32, ChunkId, bool, bSuccess);

// Struct to hold DLC chunk information
USTRUCT(BlueprintType)
struct FDLCChunkInfo
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	int32 ChunkId = 0;
	
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	FString Name;
	
	// Server version (from manifest)
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	FString Version;
	
	// Local downloaded version
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	FString LocalVersion;
	
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	int64 SizeBytes = 0;
	
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	FString WindowsURL;
	
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	FString AndroidURL;
	
	UPROPERTY(BlueprintReadWrite, Category = "DLC")
	FString LevelPath;
	
	// Check if this chunk is downloaded
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	bool bIsDownloaded = false;
	
	// Check if this chunk is mounted
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	bool bIsMounted = false;
	
	// Check if an update is available
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	bool bNeedsUpdate = false;
};

// Storage info for UI
USTRUCT(BlueprintType)
struct FDLCStorageInfo
{
	GENERATED_BODY()
	
	// Total size of downloaded DLC in bytes
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	int64 TotalSizeBytes = 0;
	
	// Number of downloaded chunks
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	int32 DownloadedCount = 0;
	
	// Human-readable size string (e.g., "256 MB")
	UPROPERTY(BlueprintReadOnly, Category = "DLC")
	FString TotalSizeFormatted;
};

/**
 * DLC Manager - Downloads, mounts, and loads PAK files at runtime
 * Use this to download levels from your server and load them dynamically
 *
 * This is a GameInstanceSubsystem: it is created automatically when the game
 * starts, lives for the whole lifetime of the GameInstance (i.e. across every
 * level/map load), and there is always exactly one instance. In Blueprint,
 * get it anywhere with: "Get Game Instance" -> "Get Subsystem" (Class: DLC
 * Manager). No more spawning/placing it as an Actor.
 */
UCLASS(BlueprintType)
class BEAN_TRAILERS_API UDLCManager : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UDLCManager();
	
	// ==================== DOWNLOAD ====================
	
	/**
	 * Download a PAK chunk from a URL
	 * @param ChunkId - The chunk ID (1, 2, 3, etc.)
	 * @param URL - Full URL to the PAK file on your server
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Download")
	void DownloadChunk(int32 ChunkId, const FString& URL);
	
	/**
	 * Cancel an active download
	 * @param ChunkId - The chunk ID to cancel
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Download")
	void CancelDownload(int32 ChunkId);
	
	/**
	 * Get the download progress for a chunk (0.0 to 1.0)
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Download")
	float GetDownloadProgress(int32 ChunkId) const;
	
	// ==================== MOUNT ====================
	
	/**
	 * Mount a downloaded PAK file so its contents become available
	 * @param ChunkId - The chunk ID to mount
	 * @return True if mount was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Mount")
	bool MountChunk(int32 ChunkId);
	
	/**
	 * Unmount a PAK file
	 * @param ChunkId - The chunk ID to unmount
	 * @return True if unmount was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Mount")
	bool UnmountChunk(int32 ChunkId);
	
	// ==================== LOAD ====================
	
	/**
	 * Load a level from a mounted chunk
	 * @param LevelPath - The full level path (e.g., "/Game/Level/Living_Room")
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Load")
	void LoadLevelFromChunk(const FString& LevelPath);
	
	// ==================== STATUS ====================
	
	/**
	 * Check if a chunk is already downloaded
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Status")
	bool IsChunkDownloaded(int32 ChunkId) const;
	
	/**
	 * Check if a chunk is currently mounted
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Status")
	bool IsChunkMounted(int32 ChunkId) const;
	
	/**
	 * Get the path where a chunk's PAK file is stored
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Status")
	FString GetChunkFilePath(int32 ChunkId) const;
	
	/**
	 * Get all available chunk infos (from manifest)
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Status")
	TArray<FDLCChunkInfo> GetAvailableChunks() const;
	
	/**
	 * Delete a downloaded chunk file
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Storage")
	bool DeleteChunk(int32 ChunkId);
	
	/**
	 * Delete all downloaded chunks (clear storage)
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Storage")
	void ClearAllDownloads();

	/**
	 * Deletes every downloaded chunk PAK from disk (unmounting first if
	 * still mounted), EXCEPT the one passed as ChunkIdToKeep - use this to
	 * protect the chunk the player is currently in / can still resume into.
	 * Pass -1 to delete everything with no exception (true session end,
	 * e.g. Pixel Streaming link disconnected for good - not a reload).
	 *
	 * Unlike ReturnToStartupMap(), this does NOT navigate anywhere - it only
	 * reclaims disk space. Call it from wherever your project detects the
	 * streaming session has actually ended (this is infrastructure-specific;
	 * Pixel Streaming's JS/signalling layer typically exposes a disconnect
	 * event you'd forward into the game, e.g. via a console command or a
	 * custom HTTP/WebSocket hook - there's no engine-side "session ended"
	 * event by default).
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Storage")
	void CleanupDownloadedChunks(int32 ChunkIdToKeep = -1);
	
	/**
	 * Get storage info (total size, count)
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Storage")
	FDLCStorageInfo GetStorageInfo() const;
	
	/**
	 * Get list of downloaded chunks (for storage UI)
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Storage")
	TArray<FDLCChunkInfo> GetDownloadedChunks() const;
	
	// ==================== VERSION SYSTEM ====================
	
	/**
	 * Check if a chunk needs an update (version mismatch)
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Version")
	bool NeedsUpdate(int32 ChunkId) const;
	
	/**
	 * Get the local version of a downloaded chunk
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Version")
	FString GetLocalVersion(int32 ChunkId) const;
	
	/**
	 * Check and download chunk if needed (handles version check automatically)
	 * Returns true if download was started, false if already up-to-date
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Download")
	bool CheckAndDownloadChunk(int32 ChunkId);
	
	/**
	 * Prepare a level for loading - downloads if needed, mounts if needed
	 * When ready, broadcasts OnLevelReady
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Load")
	void PrepareLevel(int32 ChunkId);
	
	// ==================== MANIFEST ====================
	
	/**
	 * Load DLC manifest from URL (JSON file listing all available chunks)
	 * @param ManifestURL - URL to your manifest.json file
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Manifest")
	void LoadManifestFromURL(const FString& ManifestURL);
	
	/**
	 * Add a chunk manually (without using manifest)
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Manifest")
	void AddChunkInfo(const FDLCChunkInfo& ChunkInfo);
	
	// ==================== STARTUP / RETURN ====================
	
	/**
	 * Call this from ANY level (any DLC chunk map) to return to the map the
	 * game originally started on. Deletes every downloaded chunk (unmounts +
	 * removes the .pak and .version files) before opening the startup map, so
	 * the player always comes back to a clean state.
	 */
	UFUNCTION(BlueprintCallable, Category = "DLC|Startup")
	void ReturnToStartupMap();
	
	/**
	 * The map that was loaded when the game started, captured automatically
	 * the first time PrepareLevel() is called. Empty ("None") if no DLC
	 * activity has happened yet.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Startup")
	FName GetStartupMapName() const { return StartupMapName; }
	
	// ==================== CURRENT CHUNK TRACKING ====================
	// This lives here (not in DLCLevelStreamer) because DLCManager is a
	// GameInstanceSubsystem and survives level travel. DLCLevelStreamer is a
	// plain UObject usually owned by a PlayerController/Actor, which gets
	// destroyed and recreated on every hard level transition (OpenLevel) -
	// so any state stored there is lost the moment you travel to a new map.

	/**
	 * The chunk currently loaded/mounted (if any). -1 if on the base/startup map.
	 * Kept here so it survives level travel instead of living on
	 * DLCLevelStreamer, which gets destroyed and recreated on every OpenLevel.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Status")
	int32 GetCurrentLoadedChunkId() const { return CurrentLoadedChunkId; }

	/** Called internally by DLCLevelStreamer to update the tracked chunk. */
	UFUNCTION(BlueprintCallable, Category = "DLC|Status")
	void SetCurrentLoadedChunkId(int32 ChunkId) { CurrentLoadedChunkId = ChunkId; SaveLastChunkId(ChunkId); }

	// ==================== SESSION PERSISTENCE ====================
	// Unlike CurrentLoadedChunkId above (which only survives level travel
	// within the same running game process), these two persist to disk -
	// so they survive the whole process restarting, e.g. a Pixel Streaming
	// browser tab being reloaded. SetCurrentLoadedChunkId() already calls
	// SaveLastChunkId() for you, so you normally only need LoadLastChunkId()
	// on startup to check whether a session should be resumed.

	/** Writes ChunkId (or -1 for "none") to a small save file on disk. */
	UFUNCTION(BlueprintCallable, Category = "DLC|Session")
	void SaveLastChunkId(int32 ChunkId) const;

	/** Reads back the chunk saved by SaveLastChunkId(). -1 if none saved. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DLC|Session")
	int32 LoadLastChunkId() const;
	
	// ==================== DELEGATES ====================
	
	/** Called during download with progress updates */
	UPROPERTY(BlueprintAssignable, Category = "DLC|Events")
	FOnDLCDownloadProgress OnDownloadProgress;
	
	/** Called when a download completes (success or failure) */
	UPROPERTY(BlueprintAssignable, Category = "DLC|Events")
	FOnDLCDownloadComplete OnDownloadComplete;
	
	/** Called when a chunk is mounted */
	UPROPERTY(BlueprintAssignable, Category = "DLC|Events")
	FOnDLCMountComplete OnMountComplete;
	
	/** Called when a level is ready to load (downloaded + mounted) */
	UPROPERTY(BlueprintAssignable, Category = "DLC|Events")
	FOnDLCMountComplete OnLevelReady;

protected:
	// Get the DLC storage directory (platform-specific)
	FString GetDLCDirectory() const;
	
	// HTTP request completion handler
	void OnHttpRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess, int32 ChunkId);
	
	// Save downloaded data to file
	bool SaveChunkToFile(int32 ChunkId, const TArray<uint8>& Data, const FString& Version);
	
	// Load version info from meta file
	FString LoadLocalVersion(int32 ChunkId) const;
	
	// Save version info to meta file
	bool SaveLocalVersion(int32 ChunkId, const FString& Version);
	
	// Get file size for a chunk
	int64 GetChunkFileSize(int32 ChunkId) const;
	
	// Format bytes as human-readable string
	static FString FormatBytes(int64 Bytes);

private:
	// Available DLC chunks (from manifest or manually added)
	UPROPERTY()
	TArray<FDLCChunkInfo> AvailableChunks;
	
	// Active HTTP requests (for cancellation)
	TMap<int32, TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>> ActiveRequests;
	
	// Download progress tracking
	TMap<int32, float> DownloadProgressMap;
	
	// Mounted chunk IDs
	TSet<int32> MountedChunks;
	
	// The map the game started on (captured the first time PrepareLevel() runs)
	UPROPERTY()
	FName StartupMapName;
	
	bool bStartupMapCaptured = false;
	
	// Currently loaded/mounted chunk, tracked here (not in DLCLevelStreamer)
	// because this subsystem persists across level travel. -1 = base map.
	int32 CurrentLoadedChunkId = -1;
	
	// Records StartupMapName from the current world the first time it's called;
	// does nothing on later calls. Safe to call every time PrepareLevel() runs.
	void CaptureStartupMapIfNeeded();
};
