// DLCSaveGame.h - Tiny save file that remembers which DLC chunk was loaded,
// so a Pixel Streaming browser reload (which restarts the whole game process)
// can resume the player back into their last level instead of dumping them
// at the startup menu.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "DLCSaveGame.generated.h"

UCLASS()
class BEAN_TRAILERS_API UDLCSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	// -1 = no chunk loaded (player was on the base/startup map)
	UPROPERTY()
	int32 LastChunkId = -1;
};
