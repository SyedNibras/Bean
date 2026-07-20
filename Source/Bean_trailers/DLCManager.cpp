// DLCManager.cpp - Implementation of PAK download, mount, and load system

#include "DLCManager.h"
#include "DLCSaveGame.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformFileManager.h"
#include "IPlatformFilePak.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Pawn.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"

UDLCManager::UDLCManager()
{
}

// ==================== SESSION PERSISTENCE ====================

namespace DLCSessionSave
{
	static const FString SlotName = TEXT("DLCSession");
	static const int32 UserIndex = 0;
}

void UDLCManager::SaveLastChunkId(int32 ChunkId) const
{
	UDLCSaveGame* SaveGameInstance = Cast<UDLCSaveGame>(UGameplayStatics::CreateSaveGameObject(UDLCSaveGame::StaticClass()));
	if (!SaveGameInstance)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: SaveLastChunkId - failed to create save game object"));
		return;
	}

	SaveGameInstance->LastChunkId = ChunkId;

	if (UGameplayStatics::SaveGameToSlot(SaveGameInstance, DLCSessionSave::SlotName, DLCSessionSave::UserIndex))
	{
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Saved session state - last chunk = %d"), ChunkId);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("DLCManager: Failed to save session state to disk"));
	}
}

int32 UDLCManager::LoadLastChunkId() const
{
	if (!UGameplayStatics::DoesSaveGameExist(DLCSessionSave::SlotName, DLCSessionSave::UserIndex))
	{
		return -1;
	}

	if (UDLCSaveGame* SaveGameInstance = Cast<UDLCSaveGame>(UGameplayStatics::LoadGameFromSlot(DLCSessionSave::SlotName, DLCSessionSave::UserIndex)))
	{
		return SaveGameInstance->LastChunkId;
	}

	return -1;
}

// ==================== DOWNLOAD ====================

void UDLCManager::DownloadChunk(int32 ChunkId, const FString& URL)
{
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Starting download of Chunk %d from %s"), ChunkId, *URL);
	
	// Check if already downloading
	if (ActiveRequests.Contains(ChunkId))
	{
		UE_LOG(LogTemp, Warning, TEXT("DLCManager: Chunk %d is already downloading"), ChunkId);
		return;
	}
	
	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(URL);
	HttpRequest->SetVerb(TEXT("GET"));
	
	// Note: UE 5.6 HTTP module doesn't support OnRequestProgress callback
	// Progress will jump from 0% to 100% on completion
	// For large files, consider implementing chunked downloading
	
	// Bind completion callback
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[WeakThis = TWeakObjectPtr<UDLCManager>(this), ChunkId](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
		{
			if (UDLCManager* StrongThis = WeakThis.Get())
			{
				StrongThis->OnHttpRequestComplete(Request, Response, bSuccess, ChunkId);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("DLCManager: Object destroyed during download of Chunk %d"), ChunkId);
			}
		});
	
	// Store request for cancellation
	ActiveRequests.Add(ChunkId, HttpRequest);
	DownloadProgressMap.Add(ChunkId, 0.0f);
	
	// Broadcast initial progress
	OnDownloadProgress.Broadcast(ChunkId, 0.0f, 0);
	
	// Start download
	HttpRequest->ProcessRequest();
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: HTTP request started for Chunk %d"), ChunkId);
}


void UDLCManager::CancelDownload(int32 ChunkId)
{
	if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>* RequestPtr = ActiveRequests.Find(ChunkId))
	{
		(*RequestPtr)->CancelRequest();
		ActiveRequests.Remove(ChunkId);
		DownloadProgressMap.Remove(ChunkId);
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Cancelled download of Chunk %d"), ChunkId);
	}
}

float UDLCManager::GetDownloadProgress(int32 ChunkId) const
{
	const float* Progress = DownloadProgressMap.Find(ChunkId);
	return Progress ? *Progress : 0.0f;
}

void UDLCManager::OnHttpRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess, int32 ChunkId)
{
	// Remove from active requests
	ActiveRequests.Remove(ChunkId);
	
	// Broadcast 100% progress on completion
	if (bSuccess && Response.IsValid())
	{
		int64 ContentLength = Response->GetContent().Num();
		OnDownloadProgress.Broadcast(ChunkId, 1.0f, ContentLength);
	}

	
	if (!bSuccess || !Response.IsValid())
	{
		FString ErrorMsg = TEXT("HTTP request failed");
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Download failed for Chunk %d - %s"), ChunkId, *ErrorMsg);
		OnDownloadComplete.Broadcast(ChunkId, false, ErrorMsg);
		return;
	}
	
	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		FString ErrorMsg = FString::Printf(TEXT("HTTP Error %d"), ResponseCode);
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Download failed for Chunk %d - %s"), ChunkId, *ErrorMsg);
		OnDownloadComplete.Broadcast(ChunkId, false, ErrorMsg);
		return;
	}
	
	// Get the downloaded data
	const TArray<uint8>& Content = Response->GetContent();
	
	if (Content.Num() == 0)
	{
		FString ErrorMsg = TEXT("Downloaded content is empty");
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Download failed for Chunk %d - %s"), ChunkId, *ErrorMsg);
		OnDownloadComplete.Broadcast(ChunkId, false, ErrorMsg);
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Downloaded %d bytes for Chunk %d"), Content.Num(), ChunkId);
	
	// Get version from chunk info
	FString ChunkVersion;
	for (const FDLCChunkInfo& Info : AvailableChunks)
	{
		if (Info.ChunkId == ChunkId)
		{
			ChunkVersion = Info.Version;
			break;
		}
	}
	
	// Save to file
	if (SaveChunkToFile(ChunkId, Content, ChunkVersion))
	{
		DownloadProgressMap.Add(ChunkId, 1.0f);
		
		// Update chunk info if exists
		for (FDLCChunkInfo& Info : AvailableChunks)
		{
			if (Info.ChunkId == ChunkId)
			{
				Info.bIsDownloaded = true;
				Info.LocalVersion = ChunkVersion;
				Info.bNeedsUpdate = false;
				break;
			}
		}
		
		UE_LOG(LogTemp, Log, TEXT("DLCManager: ✅ Chunk %d download complete and saved (v%s)"), ChunkId, *ChunkVersion);
		OnDownloadComplete.Broadcast(ChunkId, true, TEXT(""));

		// Try mounting with a few retries. A file that was JUST written to disk can
		// briefly fail to Mount() (Windows/AV can hold a short-lived lock/scan on a
		// freshly-created file) even though nothing is actually wrong with it.
		// Retrying after a short delay instead of giving up immediately fixes that.
		TSharedPtr<int32> AttemptsLeft = MakeShared<int32>(5);
		TWeakObjectPtr<UDLCManager> WeakThis(this);
		TSharedPtr<FTickerDelegate> MountAttemptDelegate = MakeShared<FTickerDelegate>();

		*MountAttemptDelegate = FTickerDelegate::CreateLambda(
			[WeakThis, ChunkId, AttemptsLeft](float DeltaTime) -> bool
			{
				UDLCManager* StrongThis = WeakThis.Get();
				if (!StrongThis)
				{
					return false; // object gone, stop retrying
				}

				if (StrongThis->MountChunk(ChunkId))
				{
					StrongThis->OnLevelReady.Broadcast(ChunkId, true);
					return false; // success, stop ticking
				}

				(*AttemptsLeft)--;
				if (*AttemptsLeft <= 0)
				{
					UE_LOG(LogTemp, Error, TEXT("DLCManager: Giving up mounting Chunk %d after retries"), ChunkId);
					StrongThis->OnLevelReady.Broadcast(ChunkId, false);
					return false; // out of attempts, stop ticking
				}

				UE_LOG(LogTemp, Warning, TEXT("DLCManager: Mount attempt failed for Chunk %d, retrying in 0.5s (%d attempts left)"), ChunkId, *AttemptsLeft);
				return true; // keep ticking, will be called again after the interval
			});

		FTSTicker::GetCoreTicker().AddTicker(*MountAttemptDelegate, 0.5f);
	}
	else
	{
		FString ErrorMsg = TEXT("Failed to save PAK file");
		UE_LOG(LogTemp, Error, TEXT("DLCManager: %s for Chunk %d"), *ErrorMsg, ChunkId);
		OnDownloadComplete.Broadcast(ChunkId, false, ErrorMsg);
	}
}

bool UDLCManager::SaveChunkToFile(int32 ChunkId, const TArray<uint8>& Data, const FString& Version)
{
	FString FilePath = GetChunkFilePath(ChunkId);
	
	// Ensure directory exists
	FString Directory = FPaths::GetPath(FilePath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	
	if (!PlatformFile.DirectoryExists(*Directory))
	{
		PlatformFile.CreateDirectoryTree(*Directory);
	}
	
	// Save the file
	bool bSaved = FFileHelper::SaveArrayToFile(Data, *FilePath);
	
	if (bSaved)
	{
		// Also save version file
		SaveLocalVersion(ChunkId, Version);
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Saved PAK to %s (v%s)"), *FilePath, *Version);
	}
	
	return bSaved;
}

// ==================== MOUNT ====================

bool UDLCManager::MountChunk(int32 ChunkId)
{
	FString PakFilePath = GetChunkFilePath(ChunkId);
	
	// Check if file exists
	if (!FPaths::FileExists(PakFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Cannot mount Chunk %d - PAK file not found at %s"), ChunkId, *PakFilePath);
		OnMountComplete.Broadcast(ChunkId, false);
		return false;
	}
	
	// Check if already mounted
	if (MountedChunks.Contains(ChunkId))
	{
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Chunk %d already mounted"), ChunkId);
		OnMountComplete.Broadcast(ChunkId, true);
		return true;
	}
	
	// Get PAK platform file
	FPakPlatformFile* PakFileMgr = static_cast<FPakPlatformFile*>(
		FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile")));
	
	if (!PakFileMgr)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: PAK file manager not available! Make sure UsePak is enabled."));
		OnMountComplete.Broadcast(ChunkId, false);
		return false;
	}
	
	// Mount the PAK file with high priority
	int32 MountPriority = 1000 + ChunkId;
	
	// CRITICAL FIX FOR SHIPPING: Use absolute paths
	FString AbsolutePakPath = FPaths::ConvertRelativePathToFull(PakFilePath);
	
	// Two different explicit mount-point overrides (ProjectDir and
	// ProjectContentDir) both failed to make the map resolve, even though the
	// math looked correct against "UnrealPak -list" output. Rather than guess a
	// third override, pass nullptr and let the pak use its OWN embedded mount
	// point - the one UnrealPak recorded inside the pak itself at cook time,
	// which is guaranteed to match how this specific chunk was staged.
	UE_LOG(LogTemp, Warning, TEXT("DLCManager: Mounting PAK (embedded mount point)"));
	UE_LOG(LogTemp, Warning, TEXT("  - Absolute File: %s"), *AbsolutePakPath);
	UE_LOG(LogTemp, Warning, TEXT("  - Priority: %d"), MountPriority);
	
	bool bMounted = PakFileMgr->Mount(*AbsolutePakPath, MountPriority, nullptr);
	
	if (bMounted)
	{
		MountedChunks.Add(ChunkId);
		
		// CRITICAL FIX: Tell the Asset Registry to scan the new mount point
		// Without this, the AsyncLoadingThread might crash with "Serialization Error" because it doesn't know where the assets are.
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		
		TArray<FString> ScanPaths;
		ScanPaths.Add(TEXT("/Game/")); // Scan game root (includes the new mount)
		AssetRegistry.ScanPathsSynchronous(ScanPaths, true);
		
		// Update chunk info status for UI
		for (FDLCChunkInfo& Info : AvailableChunks)
		{
			if (Info.ChunkId == ChunkId)
			{
				Info.bIsMounted = true;
				break;
			}
		}
		
		UE_LOG(LogTemp, Warning, TEXT("DLCManager: ✅ Successfully mounted Chunk %d and synchronized Asset Registry"), ChunkId);
		
		// Log all currently mounted PAKs, and for each one, what mount point it
		// actually registered - this tells us the REAL embedded mount point
		// instead of us having to guess it.
		TArray<FString> MountedFiles;
		PakFileMgr->GetMountedPakFilenames(MountedFiles);
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Currently mounted PAKs (%d):"), MountedFiles.Num());
		for (const FString& MountedFile : MountedFiles)
		{
			UE_LOG(LogTemp, Log, TEXT("  - %s"), *MountedFile);
		}
		
		// DIAGNOSTIC: directly verify (through the platform file abstraction,
		// which includes the pak VFS) whether the expected map file is visible
		// right now, immediately after mount. This removes all guesswork - if
		// this prints "NOT FOUND", the problem is the mount/pak itself, not
		// something downstream in package loading.
		FString ExpectedDiskPath = FPaths::ProjectContentDir() / TEXT("Chunk01/Map/L_BeanTrailer.umap");
		bool bDiagFound = IFileManager::Get().FileExists(*ExpectedDiskPath);
		UE_LOG(LogTemp, Warning, TEXT("DLCManager: DIAGNOSTIC - Checking %s -> %s"),
			*ExpectedDiskPath, bDiagFound ? TEXT("FOUND") : TEXT("NOT FOUND"));
		
		OnMountComplete.Broadcast(ChunkId, true);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: ❌ Failed to mount PAK file for Chunk %d"), ChunkId);
		UE_LOG(LogTemp, Error, TEXT("  - Possible causes:"));
		UE_LOG(LogTemp, Error, TEXT("    1. PAK file is corrupted or incomplete"));
		UE_LOG(LogTemp, Error, TEXT("    2. PAK was created with different engine version"));
		UE_LOG(LogTemp, Error, TEXT("    3. PAK signature mismatch (if signing enabled)"));
		OnMountComplete.Broadcast(ChunkId, false);
	}
	
	return bMounted;
}

bool UDLCManager::UnmountChunk(int32 ChunkId)
{
	if (!MountedChunks.Contains(ChunkId))
	{
		return false;
	}
	
	FString PakFilePath = GetChunkFilePath(ChunkId);
	
	FPakPlatformFile* PakFileMgr = static_cast<FPakPlatformFile*>(
		FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile")));
	
	if (PakFileMgr && PakFileMgr->Unmount(*PakFilePath))
	{
		MountedChunks.Remove(ChunkId);
		
		for (FDLCChunkInfo& Info : AvailableChunks)
		{
			if (Info.ChunkId == ChunkId)
			{
				Info.bIsMounted = false;
				break;
			}
		}
		
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Unmounted Chunk %d"), ChunkId);
		return true;
	}
	
	return false;
}

// ==================== LOAD ====================

void UDLCManager::LoadLevelFromChunk(const FString& LevelPath)
{
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Loading level %s"), *LevelPath);
	
	UWorld* World = GEngine->GetWorldFromContextObject(this, EGetWorldErrorMode::LogAndReturnNull);
	if (World)
	{
		// Safety: Flush any pending async loads before transitioning to a level that was just mounted
		FlushAsyncLoading();
		UGameplayStatics::OpenLevel(World, FName(*LevelPath));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Cannot load level - no valid world context"));
	}
}

// ==================== STATUS ====================

bool UDLCManager::IsChunkDownloaded(int32 ChunkId) const
{
	return FPaths::FileExists(GetChunkFilePath(ChunkId));
}

bool UDLCManager::IsChunkMounted(int32 ChunkId) const
{
	return MountedChunks.Contains(ChunkId);
}

FString UDLCManager::GetChunkFilePath(int32 ChunkId) const
{
	FString DLCDir = GetDLCDirectory();
	FString FileName = FString::Printf(TEXT("pakchunk%d-Windows.pak"), ChunkId);
	return FPaths::Combine(DLCDir, FileName);
}

FString UDLCManager::GetDLCDirectory() const
{
#if PLATFORM_ANDROID
	// Android: Use persistent download directory
	return FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("DLC"));
#else
	// Windows/Other: Check if we're in a packaged build or editor
	#if WITH_EDITOR
		// In Editor: Use Saved/DLC folder (project's Saved directory)
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DLC"));
	#else
		// Packaged Build: Save directly into Content/Paks, the SAME folder pakchunk0
		// lives in. This is a relative path built from ProjectContentDir(), so it
		// resolves correctly no matter where the game is installed (not hardcoded
		// to this dev machine's path) — it will always land in
		// "<InstallFolder>\Confi\Content\Paks".
		//
		// NOTE: If the game is installed under a protected folder (e.g. Program Files),
		// Content/Paks may be read-only for a normal user and the save will fail.
		// For a Desktop/Documents/custom install folder (like your current build
		// location) this is fine and writable.
		FString DLCDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Paks"));

		UE_LOG(LogTemp, Log, TEXT("DLCManager: DLC directory for packaged build: %s"), *DLCDir);
		return FPaths::ConvertRelativePathToFull(DLCDir);
	#endif
#endif
}

TArray<FDLCChunkInfo> UDLCManager::GetAvailableChunks() const
{
	return AvailableChunks;
}

bool UDLCManager::DeleteChunk(int32 ChunkId)
{
	// Unmount first if mounted
	if (MountedChunks.Contains(ChunkId))
	{
		UnmountChunk(ChunkId);
	}
	
	FString FilePath = GetChunkFilePath(ChunkId);
	FString VersionPath = FilePath + TEXT(".version");
	
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	bool bDeleted = false;
	
	if (FPaths::FileExists(FilePath))
	{
		bDeleted = PlatformFile.DeleteFile(*FilePath);
	}
	
	// Also delete version file
	if (FPaths::FileExists(VersionPath))
	{
		PlatformFile.DeleteFile(*VersionPath);
	}
	
	if (bDeleted)
	{
		// Update chunk info
		for (FDLCChunkInfo& Info : AvailableChunks)
		{
			if (Info.ChunkId == ChunkId)
			{
				Info.bIsDownloaded = false;
				Info.LocalVersion.Empty();
				break;
			}
		}
		
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Deleted Chunk %d"), ChunkId);
	}
	
	return bDeleted;
}

// ==================== MANIFEST ====================

void UDLCManager::LoadManifestFromURL(const FString& ManifestURL)
{
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Loading manifest from %s"), *ManifestURL);
	
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(ManifestURL);
	HttpRequest->SetVerb(TEXT("GET"));
	
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[WeakThis = TWeakObjectPtr<UDLCManager>(this)](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
		{
			// CRITICAL FIX: Ensure object is still valid before accessing members
			if (!WeakThis.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("DLCManager: Object destroyed before manifest load completed"));
				return;
			}

			if (!bSuccess || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				UE_LOG(LogTemp, Error, TEXT("DLCManager: Failed to load manifest"));
				return;
			}
			
			FString JsonContent = Response->GetContentAsString();
			TSharedPtr<FJsonObject> JsonObject;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
			
			if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
			{
				UE_LOG(LogTemp, Error, TEXT("DLCManager: Failed to parse manifest JSON"));
				return;
			}
			
			// Parse chunks array
			const TArray<TSharedPtr<FJsonValue>>* ChunksArray;
			if (JsonObject->TryGetArrayField(TEXT("chunks"), ChunksArray))
			{
				WeakThis->AvailableChunks.Empty();
				
				for (const TSharedPtr<FJsonValue>& ChunkValue : *ChunksArray)
				{
					TSharedPtr<FJsonObject> ChunkObj = ChunkValue->AsObject();
					if (!ChunkObj.IsValid()) continue;
					
					FDLCChunkInfo ChunkInfo;
					ChunkInfo.ChunkId = ChunkObj->GetIntegerField(TEXT("id"));
					ChunkInfo.Name = ChunkObj->GetStringField(TEXT("name"));
					ChunkInfo.Version = ChunkObj->GetStringField(TEXT("version"));
					ChunkInfo.SizeBytes = ChunkObj->GetNumberField(TEXT("size"));
					ChunkInfo.WindowsURL = ChunkObj->GetStringField(TEXT("windows_url"));
					ChunkInfo.AndroidURL = ChunkObj->GetStringField(TEXT("android_url"));
					ChunkInfo.LevelPath = ChunkObj->GetStringField(TEXT("level_path"));
					
					// Check if already downloaded
					ChunkInfo.bIsDownloaded = WeakThis->IsChunkDownloaded(ChunkInfo.ChunkId);
					ChunkInfo.bIsMounted = WeakThis->IsChunkMounted(ChunkInfo.ChunkId);
					
					// Check local version and compare
					ChunkInfo.LocalVersion = WeakThis->LoadLocalVersion(ChunkInfo.ChunkId);
					ChunkInfo.bNeedsUpdate = ChunkInfo.bIsDownloaded && 
						!ChunkInfo.Version.IsEmpty() && 
						ChunkInfo.Version != ChunkInfo.LocalVersion;
					
					WeakThis->AvailableChunks.Add(ChunkInfo);
					
					UE_LOG(LogTemp, Log, TEXT("DLCManager: Found chunk %d: %s (server v%s, local v%s, update=%d)"), 
						ChunkInfo.ChunkId, *ChunkInfo.Name, *ChunkInfo.Version, *ChunkInfo.LocalVersion, ChunkInfo.bNeedsUpdate);
				}
				
				UE_LOG(LogTemp, Log, TEXT("DLCManager: ✅ Loaded %d chunks from manifest"), 
					WeakThis->AvailableChunks.Num());
			}
		});
	
	HttpRequest->ProcessRequest();
}

void UDLCManager::AddChunkInfo(const FDLCChunkInfo& ChunkInfo)
{
	// Remove existing if present
	AvailableChunks.RemoveAll([&](const FDLCChunkInfo& Info) {
		return Info.ChunkId == ChunkInfo.ChunkId;
	});
	
	FDLCChunkInfo NewInfo = ChunkInfo;
	NewInfo.bIsDownloaded = IsChunkDownloaded(ChunkInfo.ChunkId);
	NewInfo.bIsMounted = IsChunkMounted(ChunkInfo.ChunkId);
	
	AvailableChunks.Add(NewInfo);
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Added chunk %d: %s"), ChunkInfo.ChunkId, *ChunkInfo.Name);
}

// ==================== VERSION SYSTEM ====================

FString UDLCManager::LoadLocalVersion(int32 ChunkId) const
{
	FString VersionPath = GetChunkFilePath(ChunkId) + TEXT(".version");
	FString Version;
	
	if (FPaths::FileExists(VersionPath))
	{
		FFileHelper::LoadFileToString(Version, *VersionPath);
		Version.TrimStartAndEndInline();
	}
	
	return Version;
}

bool UDLCManager::SaveLocalVersion(int32 ChunkId, const FString& Version)
{
	FString VersionPath = GetChunkFilePath(ChunkId) + TEXT(".version");
	return FFileHelper::SaveStringToFile(Version, *VersionPath);
}

bool UDLCManager::NeedsUpdate(int32 ChunkId) const
{
	// Not downloaded = needs "update" (download)
	if (!IsChunkDownloaded(ChunkId))
	{
		return true;
	}
	
	// Find server version
	FString ServerVersion;
	for (const FDLCChunkInfo& Info : AvailableChunks)
	{
		if (Info.ChunkId == ChunkId)
		{
			ServerVersion = Info.Version;
			break;
		}
	}
	
	// Get local version
	FString LocalVer = LoadLocalVersion(ChunkId);
	
	// Compare versions
	return !ServerVersion.IsEmpty() && ServerVersion != LocalVer;
}

FString UDLCManager::GetLocalVersion(int32 ChunkId) const
{
	return LoadLocalVersion(ChunkId);
}

bool UDLCManager::CheckAndDownloadChunk(int32 ChunkId)
{
	UE_LOG(LogTemp, Warning, TEXT("DLCManager: CheckAndDownloadChunk called for ChunkId=%d, AvailableChunks.Num()=%d"), ChunkId, AvailableChunks.Num());
	
	// Find chunk info
	const FDLCChunkInfo* ChunkInfo = nullptr;
	for (const FDLCChunkInfo& Info : AvailableChunks)
	{
		UE_LOG(LogTemp, Log, TEXT("DLCManager: - Checking chunk %d (%s)"), Info.ChunkId, *Info.Name);
		if (Info.ChunkId == ChunkId)
		{
			ChunkInfo = &Info;
			break;
		}
	}
	
	if (!ChunkInfo)
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: ❌ Chunk %d not found in available chunks! Manifest may not be loaded."), ChunkId);
		return false;
	}
	
	// Check if update needed
	if (!NeedsUpdate(ChunkId))
	{
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Chunk %d is up-to-date (v%s), no download needed"), 
			ChunkId, *LoadLocalVersion(ChunkId));
		return false;
	}
	
	// Get platform-specific URL
#if PLATFORM_ANDROID
	FString URL = ChunkInfo->AndroidURL;
#else
	FString URL = ChunkInfo->WindowsURL;
#endif
	
	if (URL.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: No URL configured for Chunk %d on this platform"), ChunkId);
		return false;
	}
	
	// Start download
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Starting download of Chunk %d (v%s -> v%s)"),
		ChunkId, *LoadLocalVersion(ChunkId), *ChunkInfo->Version);
	
	DownloadChunk(ChunkId, URL);
	return true;
}

void UDLCManager::PrepareLevel(int32 ChunkId)
{
	// Must happen before anything else: this is called from the hub/startup
	// map before we ever switch to a chunk map, so this is the correct moment
	// to remember "home". No-ops after the first successful capture.
	CaptureStartupMapIfNeeded();
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Preparing level for Chunk %d"), ChunkId);
	
	// If already mounted, we're ready
	if (IsChunkMounted(ChunkId))
	{
		UE_LOG(LogTemp, Log, TEXT("DLCManager: Chunk %d already mounted, level ready"), ChunkId);
		OnLevelReady.Broadcast(ChunkId, true);
		return;
	}
	
	// If downloaded but not mounted, mount it
	if (IsChunkDownloaded(ChunkId) && !NeedsUpdate(ChunkId))
	{
		bool bMounted = MountChunk(ChunkId);
		OnLevelReady.Broadcast(ChunkId, bMounted);
		return;
	}
	
	// Need to download - bind to download complete event
	// The UI should bind to OnDownloadComplete and OnLevelReady
	bool bStartedDownload = CheckAndDownloadChunk(ChunkId);
	
	if (!bStartedDownload)
	{
		// Already up to date but not mounted? Try mounting
		if (IsChunkDownloaded(ChunkId))
		{
			bool bMounted = MountChunk(ChunkId);
			OnLevelReady.Broadcast(ChunkId, bMounted);
		}
		else
		{
			OnLevelReady.Broadcast(ChunkId, false);
		}
	}
	// Otherwise download in progress, OnDownloadComplete will handle it
}

// ==================== STORAGE MANAGEMENT ====================

void UDLCManager::ClearAllDownloads()
{
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Clearing all downloaded chunks"));
	
	// Get all chunk IDs that are downloaded
	TArray<int32> ChunksToDelete;
	for (const FDLCChunkInfo& Info : AvailableChunks)
	{
		if (Info.bIsDownloaded)
		{
			ChunksToDelete.Add(Info.ChunkId);
		}
	}
	
	// Also scan DLC directory for any orphan files.
	// SAFETY: in a packaged build, GetDLCDirectory() returns the SAME folder
	// pakchunk0-Windows.pak lives in (see GetDLCDirectory() above), so a bare
	// "*.pak" wildcard here would also delete the base game pak - exactly the
	// "pak0 disappeared" bug from before. Explicitly skip pakchunk0.
	FString DLCDir = GetDLCDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	
	if (PlatformFile.DirectoryExists(*DLCDir))
	{
		TArray<FString> Files;
		PlatformFile.FindFiles(Files, *DLCDir, TEXT("*.pak"));
		
		for (const FString& File : Files)
		{
			FString FileName = FPaths::GetCleanFilename(File);
			if (FileName.StartsWith(TEXT("pakchunk0")))
			{
				UE_LOG(LogTemp, Warning, TEXT("DLCManager: Skipping %s - base game pak, never deleted"), *FileName);
				continue;
			}
			
			PlatformFile.DeleteFile(*File);
			
			// Also delete version file
			FString VersionFile = File + TEXT(".version");
			if (FPaths::FileExists(VersionFile))
			{
				PlatformFile.DeleteFile(*VersionFile);
			}
		}
	}
	
	// Update chunk infos
	for (FDLCChunkInfo& Info : AvailableChunks)
	{
		Info.bIsDownloaded = false;
		Info.bIsMounted = false;
		Info.LocalVersion.Empty();
	}
	
	MountedChunks.Empty();
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: ✅ Cleared all DLC downloads"));
}

// ==================== STARTUP / RETURN ====================

void UDLCManager::CaptureStartupMapIfNeeded()
{
	if (bStartupMapCaptured)
	{
		return;
	}
	
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World)
	{
		// Not available yet - harmless, we'll just try again on the next call
		return;
	}
	
	FString MapName = World->GetMapName();
	MapName.RemoveFromStart(World->StreamingLevelsPrefix); // strips PIE's "UEDPIE_0_" prefix in-editor
	
	StartupMapName = FName(*MapName);
	bStartupMapCaptured = true;
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Captured startup map '%s'"), *StartupMapName.ToString());
}

void UDLCManager::ReturnToStartupMap()
{
	if (!bStartupMapCaptured || StartupMapName.IsNone())
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Cannot return - startup map was never captured (PrepareLevel must run at least once from the hub map first)"));
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("DLCManager: Returning to startup map '%s' - cleaning up all downloaded chunks first"), *StartupMapName.ToString());
	
	// Clean up every chunk we know about, no matter which map is currently
	// loaded. DeleteChunk() already unmounts first, then removes the .pak and
	// .version files - and it only ever touches THIS chunk's specific
	// filename (pakchunkN-Windows.pak), so pakchunk0 is never at risk here.
	TArray<int32> ChunkIdsToClean;
	for (const FDLCChunkInfo& Info : AvailableChunks)
	{
		if (Info.ChunkId != 0) // never touch the base game pak
		{
			ChunkIdsToClean.Add(Info.ChunkId);
		}
	}
	
	for (int32 ChunkId : ChunkIdsToClean)
	{
		if (IsChunkDownloaded(ChunkId) || IsChunkMounted(ChunkId))
		{
			DeleteChunk(ChunkId);
		}
	}
	
	MountedChunks.Empty();
	
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (World)
	{
		FlushAsyncLoading();
		UGameplayStatics::OpenLevel(World, StartupMapName);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("DLCManager: Cannot return to startup map - no valid world context"));
	}
}

void UDLCManager::CleanupDownloadedChunks(int32 ChunkIdToKeep)
{
	TArray<FDLCChunkInfo> Downloaded = GetDownloadedChunks();

	UE_LOG(LogTemp, Log, TEXT("DLCManager: CleanupDownloadedChunks - %d chunk(s) on disk, keeping chunk %d"),
		Downloaded.Num(), ChunkIdToKeep);

	for (const FDLCChunkInfo& Info : Downloaded)
	{
		if (Info.ChunkId == ChunkIdToKeep)
		{
			// Protected: this is the chunk the player is currently in (or
			// could still resume into after a reload) - don't touch it.
			continue;
		}

		// DeleteChunk() already unmounts first if needed, then removes the
		// .pak and .version files - only for this specific ChunkId.
		DeleteChunk(Info.ChunkId);
	}
}

FDLCStorageInfo UDLCManager::GetStorageInfo() const
{
	FDLCStorageInfo Info;
	
	for (const FDLCChunkInfo& Chunk : AvailableChunks)
	{
		if (IsChunkDownloaded(Chunk.ChunkId))
		{
			Info.DownloadedCount++;
			Info.TotalSizeBytes += GetChunkFileSize(Chunk.ChunkId);
		}
	}
	
	Info.TotalSizeFormatted = FormatBytes(Info.TotalSizeBytes);
	
	return Info;
}

TArray<FDLCChunkInfo> UDLCManager::GetDownloadedChunks() const
{
	TArray<FDLCChunkInfo> Downloaded;
	
	for (const FDLCChunkInfo& Info : AvailableChunks)
	{
		if (IsChunkDownloaded(Info.ChunkId))
		{
			FDLCChunkInfo ChunkCopy = Info;
			ChunkCopy.bIsDownloaded = true;
			ChunkCopy.LocalVersion = LoadLocalVersion(Info.ChunkId);
			ChunkCopy.SizeBytes = GetChunkFileSize(Info.ChunkId);
			Downloaded.Add(ChunkCopy);
		}
	}
	
	return Downloaded;
}

int64 UDLCManager::GetChunkFileSize(int32 ChunkId) const
{
	FString FilePath = GetChunkFilePath(ChunkId);
	
	if (FPaths::FileExists(FilePath))
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		return PlatformFile.FileSize(*FilePath);
	}
	
	return 0;
}

FString UDLCManager::FormatBytes(int64 Bytes)
{
	if (Bytes >= 1073741824) // GB
	{
		return FString::Printf(TEXT("%.2f GB"), (double)Bytes / 1073741824.0);
	}
	else if (Bytes >= 1048576) // MB
	{
		return FString::Printf(TEXT("%.2f MB"), (double)Bytes / 1048576.0);
	}
	else if (Bytes >= 1024) // KB
	{
		return FString::Printf(TEXT("%.2f KB"), (double)Bytes / 1024.0);
	}
	else
	{
		return FString::Printf(TEXT("%lld bytes"), Bytes);
	}
}
