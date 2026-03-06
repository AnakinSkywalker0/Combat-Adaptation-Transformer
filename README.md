# CAT - Combat Adaptation Transformer

CAT is a real-time adaptive boss AI prototype for Unreal Engine 5.5.

It observes player combat actions, runs local NVIGI inference, and maps model output into tactical boss responses such as:
- `COUNTER_LOW`
- `COUNTER_HIGH`
- `REPOSITION`
- `AGGRESSIVE_PRESS`
- `DEFENSIVE_WAIT`

## What CAT Does

- Tracks recent player actions in a rolling combat buffer.
- Builds short prompts from live gameplay context.
- Calls NVIGI GPT inference asynchronously (off the game thread).
- Converts raw model text into game-ready tactics (`EBossTactic`).
- Exposes the mapped tactic so boss behavior logic can consume it.

## Tech Stack

- Unreal Engine 5.5
- C++ gameplay module (`Echo`)
- Custom UE plugin: `Plugins/IGI`
- NVIDIA NVIGI SDK (local inference)
- Nemotron Mini 4B GGUF model

## Current Status

- IGI plugin integrated and build-stable in UE5.
- Inference pipeline working end-to-end.
- `CombatLogger` receives responses and maps them to tactics.
- Next step: plug `CurrentTactic` into boss Behavior Tree / state logic.

## Repository Notes

This repository does **not** include large generated/cached folders (`Binaries`, `Intermediate`, `Saved`, etc).

NVIGI SDK pack and model binaries may be excluded from version control due to size/licensing. If missing, place them locally at:

`Plugins/IGI/ThirdParty/nvigi_pack/`

## Quick Start (Windows)

1. Install Unreal Engine 5.5 and Visual Studio 2022 (Desktop C++).
2. Clone this repo.
3. Ensure NVIGI SDK + model files exist under:
   `Plugins/IGI/ThirdParty/nvigi_pack/`
4. Right-click `Echo.uproject` -> **Generate Visual Studio project files**.
5. Open `Echo.sln`.
6. Set config to `Development Editor` + `Win64`.
7. Build project `Echo`.
8. Open `Echo.uproject` in Unreal Editor and run PIE.

## Core Files

- `Source/Echo/CombatLogger.h`
- `Source/Echo/CombatLogger.cpp`
- `Plugins/IGI/Source/IGI/Private/IGICore.cpp`
- `Plugins/IGI/Source/IGI/Private/IGIGPT.cpp`
- `Plugins/IGI/Source/IGI/Private/IGIModule.cpp`

## Vision

CAT aims to make boss fights non-scripted and replay-resistant by enabling opponents to learn and adapt during combat, not between patches.
