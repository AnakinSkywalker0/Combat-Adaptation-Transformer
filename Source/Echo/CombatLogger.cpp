// Copyright Epic Games, Inc. All Rights Reserved.

#include "CombatLogger.h"

#include "Async/Async.h"
#include "Modules/ModuleManager.h"

#include "IGIGPT.h"
#include "IGIModule.h"

EBossTactic ACombatLogger::ParseTactic(const FString& ResponseText)
{
    const FString Normalized = ResponseText.TrimStartAndEnd().ToUpper();

    if (Normalized.Contains(TEXT("COUNTER_LOW")))
    {
        return EBossTactic::CounterLow;
    }
    if (Normalized.Contains(TEXT("COUNTER_HIGH")))
    {
        return EBossTactic::CounterHigh;
    }
    if (Normalized.Contains(TEXT("REPOSITION")))
    {
        return EBossTactic::Reposition;
    }
    if (Normalized.Contains(TEXT("AGGRESSIVE_PRESS")))
    {
        return EBossTactic::AggressivePress;
    }
    if (Normalized.Contains(TEXT("DEFENSIVE_WAIT")))
    {
        return EBossTactic::DefensiveWait;
    }

    return EBossTactic::None;
}

ACombatLogger::ACombatLogger()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ACombatLogger::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogTemp, Log, TEXT("CombatLogger active"));

    if (bAutoSeedTestActions && ActionBuffer.Num() == 0)
    {
        AddAction(TEXT("low_attack"));
        AddAction(TEXT("block"));
        AddAction(TEXT("dodge_back"));
        UE_LOG(LogTemp, Log, TEXT("CombatLogger seeded test actions"));
    }
}

void ACombatLogger::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    TimeSinceSend += DeltaSeconds;
    if (!bPendingRequest && ActionBuffer.Num() > 0 && TimeSinceSend >= SendInterval)
    {
        TimeSinceSend = 0.0f;
        SendToIGI();
    }
}

void ACombatLogger::AddAction(const FString& Action)
{
    ActionBuffer.Add(Action);
    if (ActionBuffer.Num() > MaxActions)
    {
        ActionBuffer.RemoveAt(0);
    }
}

void ACombatLogger::SendToIGI()
{
    bPendingRequest = true;

    const FString SystemPrompt = TEXT(
        "You are a combat AI boss. Analyse the player pattern and respond with ONE action.\n"
        "Actions: COUNTER_LOW, COUNTER_HIGH, REPOSITION, AGGRESSIVE_PRESS, DEFENSIVE_WAIT");

    const FString UserPrompt = FString::Printf(
        TEXT("Player last actions: %s"),
        *FString::Join(ActionBuffer, TEXT(", ")));

    const FString AssistantPrompt;

    // Keep inference work off the game thread.
    Async(EAsyncExecution::ThreadPool, [this, SystemPrompt, UserPrompt, AssistantPrompt]()
    {
        FString ResultText;

        if (FIGIModule* Module = FModuleManager::GetModulePtr<FIGIModule>("IGI"))
        {
            if (!bIGICoreLoaded)
            {
                bIGICoreLoaded = Module->LoadIGICore();
            }

            if (!bIGICoreLoaded)
            {
                AsyncTask(ENamedThreads::GameThread, [this]()
                {
                    UE_LOG(LogTemp, Error, TEXT("IGI core failed to load"));
                    bPendingRequest = false;
                });
                return;
            }

            if (FIGIGPT* GPT = Module->GetGPT())
            {
                ResultText = GPT->Evaluate(SystemPrompt, UserPrompt, AssistantPrompt);
            }
        }
        else
        {
            AsyncTask(ENamedThreads::GameThread, [this]()
            {
                UE_LOG(LogTemp, Error, TEXT("IGI module not loaded"));
                bPendingRequest = false;
            });
            return;
        }

        AsyncTask(ENamedThreads::GameThread, [this, ResultText]()
        {
            LastIGIResponse = ResultText;
            CurrentTactic = ParseTactic(ResultText);
            UE_LOG(LogTemp, Log, TEXT("IGI response: %s"), *ResultText);
            UE_LOG(LogTemp, Log, TEXT("Mapped tactic: %s"),
                *StaticEnum<EBossTactic>()->GetNameStringByValue(static_cast<int64>(CurrentTactic)));
            bPendingRequest = false;
        });
    });
}
