// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CombatLogger.generated.h"

UENUM(BlueprintType)
enum class EBossTactic : uint8
{
    None            UMETA(DisplayName = "None"),
    CounterLow      UMETA(DisplayName = "Counter Low"),
    CounterHigh     UMETA(DisplayName = "Counter High"),
    Reposition      UMETA(DisplayName = "Reposition"),
    AggressivePress UMETA(DisplayName = "Aggressive Press"),
    DefensiveWait   UMETA(DisplayName = "Defensive Wait")
};

UCLASS()
class ECHO_API ACombatLogger : public AActor
{
    GENERATED_BODY()

public:
    ACombatLogger();
    virtual void Tick(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable, Category = "Combat")
    void AddAction(const FString& Action);

    UFUNCTION(BlueprintCallable, Category = "Combat|AI")
    EBossTactic GetCurrentTactic() const { return CurrentTactic; }

protected:
    virtual void BeginPlay() override;

private:
    static EBossTactic ParseTactic(const FString& ResponseText);
    void SendToIGI();

    UPROPERTY(EditAnywhere, Category = "Combat")
    int32 MaxActions = 10;

    UPROPERTY(EditAnywhere, Category = "Combat")
    float SendInterval = 4.0f;

    UPROPERTY(EditAnywhere, Category = "Combat")
    bool bAutoSeedTestActions = true;

    UPROPERTY(VisibleAnywhere, Category = "Combat")
    TArray<FString> ActionBuffer;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Combat|AI", meta = (AllowPrivateAccess = "true"))
    FString LastIGIResponse;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Combat|AI", meta = (AllowPrivateAccess = "true"))
    EBossTactic CurrentTactic = EBossTactic::None;

    float TimeSinceSend = 0.0f;
    bool bPendingRequest = false;
    bool bIGICoreLoaded = false;
};
