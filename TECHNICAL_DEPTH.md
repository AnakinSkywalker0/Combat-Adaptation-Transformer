# CAT — Combat Adaptation Transformer: Technical Depth Document

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Repository Layout](#2-repository-layout)
3. [Architecture Overview](#3-architecture-overview)
4. [Module: Echo (Game Module)](#4-module-echo-game-module)
   - 4.1 [Echo.Build.cs](#41-echobuildcs)
   - 4.2 [Echo.h / Echo.cpp](#42-echoh--echocpp)
   - 4.3 [CombatLogger — Header](#43-combatlogger--header)
   - 4.4 [CombatLogger — Implementation](#44-combatlogger--implementation)
5. [Plugin: IGI (In-Game Inference)](#5-plugin-igi-in-game-inference)
   - 5.1 [IGI.uplugin](#51-igiuplugin)
   - 5.2 [IGI.Build.cs](#52-igibuildcs)
   - 5.3 [IGILog.h](#53-igilogh)
   - 5.4 [IGICore — Header & Implementation](#54-igicore--header--implementation)
   - 5.5 [IGIModule — Header & Implementation](#55-igimodule--header--implementation)
   - 5.6 [FIGIGPT — Header & Implementation](#56-figigpt--header--implementation)
   - 5.7 [IGIBlueprintLibrary — Header & Implementation](#57-igiblueprintlibrary--header--implementation)
6. [NVIGI SDK Integration](#6-nvigi-sdk-integration)
7. [End-to-End Data Flow](#7-end-to-end-data-flow)
8. [Threading Model](#8-threading-model)
9. [Tactic Mapping Logic](#9-tactic-mapping-logic)
10. [Build System & Configuration](#10-build-system--configuration)
11. [Render Hardware Interface (RHI) Coupling](#11-render-hardware-interface-rhi-coupling)
12. [Blueprint Exposure](#12-blueprint-exposure)
13. [Extension Points & Future Work](#13-extension-points--future-work)

---

## 1. Project Overview

**CAT (Combat Adaptation Transformer)** is a real-time adaptive boss AI prototype built on **Unreal Engine 5.5**. Instead of using hand-authored behaviour trees with fixed responses, CAT feeds a rolling history of the player's combat actions into a local large-language-model (LLM) and converts the raw text response into a discrete `EBossTactic` enumeration value. That tactic value can then drive any downstream boss behaviour tree, state machine, or animation graph.

The system is intentionally non-networked and fully local: inference runs on the GPU of the player's own machine via NVIDIA's **NVIGI** (NVIDIA In-Game Inference) SDK, keeping latency predictable and avoiding external API calls.

**Key design goals:**
- Non-scripted, replay-resistant boss decisions.
- Inference entirely off the game thread; zero game-thread stalls.
- Clean separation of concerns: game code owns combat context, the IGI plugin owns SDK lifetime, and the LLM owns tactical reasoning.

---

## 2. Repository Layout

```
Combat-Adaptation-Transformer/
│
├── Echo.uproject                    # UE5 project descriptor
│
├── Source/
│   ├── Echo.Target.cs               # Game build target
│   ├── EchoEditor.Target.cs         # Editor build target
│   └── Echo/                        # Primary game C++ module
│       ├── Echo.Build.cs            # Module dependency rules
│       ├── Echo.h                   # Module header (minimal)
│       ├── Echo.cpp                 # IMPLEMENT_PRIMARY_GAME_MODULE
│       ├── CombatLogger.h           # EBossTactic enum + ACombatLogger class
│       └── CombatLogger.cpp         # Rolling buffer, timer, IGI dispatch
│
├── Plugins/
│   └── IGI/                         # Custom UE plugin wrapping NVIGI SDK
│       ├── IGI.uplugin              # Plugin descriptor
│       ├── ThirdParty/
│       │   └── nvigi_pack/          # NVIGI SDK (binaries + headers, not in VCS)
│       │       ├── include/         # nvigi.h, nvigi_ai.h, nvigi_gpt.h, …
│       │       ├── nvigi_core/include/
│       │       ├── bin/x64/         # nvigi.core.framework.dll, plugin DLLs
│       │       └── data/nvigi.models/  # Nemotron Mini 4B GGUF model file
│       └── Source/IGI/
│           ├── IGI.Build.cs         # Plugin module rules (ThirdParty paths, DLLs)
│           ├── Public/
│           │   ├── IGI.h            # Minimal public header
│           │   ├── IGIModule.h      # FIGIModule interface + nvigi type forwards
│           │   ├── IGIGPT.h         # FIGIGPT class (Pimpl)
│           │   └── IGIBlueprintLibrary.h  # UIGIGPTEvaluateAsync async BP node
│           └── Private/
│               ├── IGILog.h         # LogIGISDK category + IGILogCallback
│               ├── IGICore.h        # FIGICore (DLL loader + nvigi init)
│               ├── IGICore.cpp
│               ├── IGIModule.cpp    # FIGIModule lifecycle + Pimpl::Impl
│               ├── IGIGPT.cpp       # FIGIGPT::Impl (inference execution)
│               └── IGIBlueprintLibrary.cpp  # Async BP action implementation
│
├── Config/
│   ├── DefaultEngine.ini            # DX12 RHI, ray-tracing, Lumen settings
│   ├── DefaultGame.ini
│   ├── DefaultEditor.ini
│   └── DefaultInput.ini
│
└── Content/                         # UE assets (meshes, levels, animations)
```

---

## 3. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          GAME THREAD                                    │
│                                                                         │
│  ┌──────────────────────────────────────────────────────┐               │
│  │  ACombatLogger  (AActor, ticks every frame)          │               │
│  │                                                      │               │
│  │  ActionBuffer: TArray<FString>  ← AddAction()        │               │
│  │  TimeSinceSend: float                                │               │
│  │  CurrentTactic: EBossTactic   ← read by boss logic   │               │
│  │  bPendingRequest: bool        ← guards re-entry      │               │
│  │                                                      │               │
│  │  Tick() ──► every SendInterval seconds ──► SendToIGI()              │
│  └─────────────────────┬────────────────────────────────┘               │
│                        │ Async(ThreadPool)                              │
└────────────────────────┼────────────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────────────┐
│                     THREAD POOL WORKER                                  │
│                                                                         │
│  1. FModuleManager::GetModulePtr<FIGIModule>("IGI")                     │
│  2. Module->LoadIGICore()   (idempotent after first call)               │
│  3. Module->GetGPT()        (lazy-creates FIGIGPT)                      │
│  4. GPT->Evaluate(System, User, "")                                     │
│       └─► FIGIGPT::Impl::Evaluate()                                     │
│              └─► nvigi evaluateAsync() + condition_variable wait        │
│                       │                                                 │
│                       ▼                                                 │
│              Completion callback fires (NVIGI internal thread)          │
│              → strips <JSON> noise                                      │
│              → accumulates token stream into gptOutput                  │
│              → signals condition_variable                               │
│                                                                         │
│  5. ResultText returned to lambda                                        │
│  6. AsyncTask(GameThread, …)                                            │
└─────────────────────────────────────────────────────────────────────────┘
                         │ AsyncTask → GameThread
┌────────────────────────▼────────────────────────────────────────────────┐
│                         GAME THREAD (resume)                            │
│                                                                         │
│  LastIGIResponse = ResultText                                           │
│  CurrentTactic   = ParseTactic(ResultText)  ← keyword scan             │
│  bPendingRequest = false                                                │
│                                                                         │
│  Boss Behavior Tree / State Machine reads CurrentTactic                 │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Module: Echo (Game Module)

### 4.1 `Echo.Build.cs`

`Source/Echo/Echo.Build.cs` is the **Unreal Build Tool (UBT)** module definition for the game code. It:

- Sets `PCHUsage = UseExplicitOrSharedPCHs` (Unity/PCH compilation).
- Adds public dependencies: `Core`, `CoreUObject`, `Engine`, `InputCore`, `EnhancedInput`, and **`IGI`** (the custom plugin module). Adding `IGI` here makes the public headers in `Plugins/IGI/Source/IGI/Public/` visible to game code, and ensures the plugin is linked.

### 4.2 `Echo.h` / `Echo.cpp`

Minimal boilerplate: `Echo.h` is an empty minimal header; `Echo.cpp` calls `IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Echo, "Echo")` which registers the module with UE's module manager. No custom `IModuleInterface` overrides are needed for the game module itself.

### 4.3 `CombatLogger` — Header

**File:** `Source/Echo/CombatLogger.h`

#### `EBossTactic` enum

```cpp
UENUM(BlueprintType)
enum class EBossTactic : uint8
{
    None, CounterLow, CounterHigh,
    Reposition, AggressivePress, DefensiveWait
};
```

A `BlueprintType` enum exposed to both C++ and Blueprint. Each value maps to a distinct keyword the LLM is instructed to produce. The `uint8` backing type keeps storage minimal and makes the enum compatible with UE's reflection system.

#### `ACombatLogger` class

| Member | Kind | Purpose |
|---|---|---|
| `MaxActions` | `UPROPERTY EditAnywhere int32` | Rolling-buffer cap (default 10). Older actions are dropped as new ones arrive. |
| `SendInterval` | `UPROPERTY EditAnywhere float` | Seconds between inference requests (default 4.0 s). Prevents request flooding. |
| `bAutoSeedTestActions` | `UPROPERTY EditAnywhere bool` | When true, pre-populates the buffer with `low_attack / block / dodge_back` in `BeginPlay` for quick testing without a full combat setup. |
| `ActionBuffer` | `UPROPERTY VisibleAnywhere TArray<FString>` | The sliding window of recent player action strings. Serialised and visible in the Details panel for inspection. |
| `LastIGIResponse` | `UPROPERTY BlueprintReadOnly FString` | Raw text returned by the model, exposed for debugging in Blueprint. |
| `CurrentTactic` | `UPROPERTY BlueprintReadOnly EBossTactic` | The most recently decoded tactic. Boss logic reads this value. |
| `TimeSinceSend` | `float` | Accumulated delta time since the last `SendToIGI()` call. Not a `UPROPERTY` — transient runtime state. |
| `bPendingRequest` | `bool` | Guards against concurrent inference calls. Set `true` before dispatch; `false` on the game thread once a response arrives. |
| `bIGICoreLoaded` | `bool` | Cached flag so `LoadIGICore()` is only called once per `ACombatLogger` instance even if it returns early on the first tick. |

**Public API:**

- `AddAction(const FString& Action)` — Blueprint-callable; appends an action string to the buffer, evicting the oldest entry if the buffer is full.
- `GetCurrentTactic()` — Inline Blueprint-callable getter; returns `CurrentTactic` for boss logic to consume.

**Private helpers:**

- `ParseTactic(const FString& ResponseText)` — Static pure function; converts raw LLM text to an `EBossTactic` value.
- `SendToIGI()` — Orchestrates the async inference dispatch.

### 4.4 `CombatLogger` — Implementation

**File:** `Source/Echo/CombatLogger.cpp`

#### `ParseTactic`

Performs a case-insensitive keyword scan on the trimmed response text. Match order is not functionally significant here because `COUNTER_LOW` and `COUNTER_HIGH` are distinct strings with no substring overlap; the ordering is retained as a defensive convention that would matter if shorter aliases were ever introduced. Falls through to `EBossTactic::None` if no keyword is found.

#### `BeginPlay`

If `bAutoSeedTestActions` is true and the buffer is empty, three canonical test action strings are pushed. This allows placing the actor in the level and immediately seeing end-to-end inference on the first tick without requiring connected player input.

#### `Tick`

Accumulates `DeltaSeconds` into `TimeSinceSend`. When the interval elapses, `bPendingRequest` is false, and the buffer is non-empty, `SendToIGI()` is called and `TimeSinceSend` is reset. Importantly, `bPendingRequest` prevents a second call before the first inference completes, keeping latency predictable regardless of how long the model takes.

#### `SendToIGI`

1. Sets `bPendingRequest = true` immediately (on game thread).
2. Builds two prompt strings:
   - **System prompt**: Hard-coded instruction telling the model it is a combat AI and must respond with exactly one of the five action tokens.
   - **User prompt**: `"Player last actions: low_attack, block, dodge_back, …"` — the `ActionBuffer` joined with `, `.
   - **Assistant prompt**: Empty (no few-shot seeding).
3. Captures the prompts and `this` by value into a `ThreadPool` lambda via `Async(EAsyncExecution::ThreadPool, …)`.
4. Inside the lambda: acquires the `IGI` module pointer; calls `LoadIGICore()` if not yet done; calls `GetGPT()` to obtain the `FIGIGPT` handle; calls `GPT->Evaluate(…)`.
5. On error at any step, marshals `bPendingRequest = false` back to the game thread via `AsyncTask(ENamedThreads::GameThread, …)` and returns early.
6. On success, marshals `LastIGIResponse`, `CurrentTactic`, and log output back to the game thread.

---

## 5. Plugin: IGI (In-Game Inference)

The `IGI` plugin is a self-contained UE plugin that encapsulates all interaction with the NVIGI SDK. Game code depends only on `IGIModule.h`, `IGIGPT.h`, and `IGIBlueprintLibrary.h` — the NVIGI headers are entirely private to the plugin.

### 5.1 `IGI.uplugin`

```json
{
  "FriendlyName": "IGI",
  "Description": "NVIDIA In-Game Inference for ECHO",
  "Category": "AI",
  "EnabledByDefault": true,
  "CanContainContent": false,
  "Modules": [{ "Name": "IGI", "Type": "Runtime", "LoadingPhase": "Default" }]
}
```

`Type: Runtime` means the module loads in both game and editor contexts. `LoadingPhase: Default` means it loads after the engine core but before gameplay begin-play.

### 5.2 `IGI.Build.cs`

**File:** `Plugins/IGI/Source/IGI/IGI.Build.cs`

Key decisions:

| Setting | Value | Reason |
|---|---|---|
| `PublicIncludePaths` | `nvigi_pack/include`, `nvigi_pack/nvigi_core/include` | Exposes NVIGI headers to plugin private code without leaking to game code. |
| `PrivateIncludePaths` | UE Engine `D3D12RHI/Private` (and `/Windows` subdirectory) | Required to include `d3dx12.h` helper used by `nvigi_d3d12.h`. |
| `AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12")` | Links `d3d12.lib` / `dxgi.lib` | NVIGI's D3D12 backend requires the raw D3D12 API. |
| `RuntimeDependencies.Add(…dll)` | `nvigi.core.framework.dll`, `nvigi.plugin.gpt.ggml.cuda.dll` | Instructs UBT to stage these DLLs alongside the game binary so they are found at runtime. |
| `D3D12RHI` in `PublicDependencyModuleNames` | — | Gives access to `ID3D12DynamicRHI` for extracting the D3D12 device and command queue from UE's RHI abstraction. |

### 5.3 `IGILog.h`

**File:** `Plugins/IGI/Source/IGI/Private/IGILog.h`

Defines a `DEFINE_LOG_CATEGORY_STATIC(LogIGISDK, …)` local to the plugin translation units. Also defines `IGILogCallback`, a C-style function pointer passed to `nvigi::Preferences::logMessageCallback`. This callback bridges NVIGI's internal log output into UE's structured log system (`UE_LOG`), translating NVIGI `LogType` values to UE `Warning`, `Error`, or default `Log` severity. NVIGI log messages contain trailing newlines which are stripped with `TrimEndInline()`.

### 5.4 `IGICore` — Header & Implementation

**Files:** `Private/IGICore.h`, `Private/IGICore.cpp`

`FIGICore` owns the lifetime of the NVIGI core DLL handle and the four function pointers extracted from it.

#### Construction (`FIGICore(FString IGICoreLibraryPath)`)

1. `FPlatformProcess::GetDllHandle(…)` — loads `nvigi.core.framework.dll` at runtime. Returns `nullptr` on failure; logs `Fatal` and marks `bInitialized = false`.
2. Four `GetDllExport` calls extract: `nvigiInit`, `nvigiShutdown`, `nvigiLoadInterface`, `nvigiUnloadInterface`. Missing any one is a Fatal error.
3. Fills `nvigi::Preferences`:
   - `showConsole = false` — suppresses NVIGI's own console window.
   - `logLevel = eDefault`.
   - `utf8PathsToPlugins` — points to `bin/x64/` so NVIGI can find `nvigi.plugin.gpt.ggml.cuda.dll` and any other plugin DLLs.
   - `utf8PathToLogsAndData` — redirects NVIGI internal logs to UE's `Saved/Logs/` directory.
   - `logMessageCallback = IGILogCallback` — routes NVIGI logs through UE's log system.
4. Calls `(*Ptr_nvigiInit)(Pref, &IGIRequirements, nvigi::kSDKVersion)`. Sets `bInitialized = (InitResult == kResultOk)`.

#### Destruction

Frees the DLL handle. `nvigiShutdown` is not explicitly called; the DLL unload acts as implicit shutdown. This is a known gap — see [Section 13](#13-extension-points--future-work) for the recommended fix.

#### `LoadInterface` / `UnloadInterface`

Thin wrappers around the extracted function pointers. `LoadInterface` passes `DummyInterface.getVersion()` to enable NVIGI's interface versioning check.

### 5.5 `IGIModule` — Header & Implementation

**Files:** `Public/IGIModule.h`, `Private/IGIModule.cpp`

`FIGIModule` is the `IModuleInterface` implementation registered with UE's module manager. It uses a **Pimpl** (`TPimplPtr<Impl>`) to keep NVIGI headers out of the public header.

#### `FIGIModule::Impl` (inner class in `IGIModule.cpp`)

Holds:
- `TUniquePtr<FIGICore> Core` — loaded/unloaded on demand.
- `TUniquePtr<FIGIGPT> GPT` — lazily created in `GetGPT()`.
- `FCriticalSection CS` — guards `Core`, `GPT`, and path strings against concurrent access from the thread pool.
- `FString IGICoreLibraryPath` — resolved absolute path to the DLL.
- `FString IGIModelsPath` — resolved path to the model data directory.

`StartupModule()` resolves both paths from the plugin base directory (using `IPluginManager`). It does **not** load the DLL yet — loading is deferred to the first `LoadIGICore()` call.

`LoadIGICore()` takes the critical section, constructs `FIGICore`, and checks `IsInitialized()`. Idempotent: if `Core` already exists it is replaced (though in practice it's only called once per session).

`GetGPT(FIGIModule* module)` lazily constructs `FIGIGPT` if not already alive. All calls are serialised by `CS`.

#### Public API on `FIGIModule`

| Method | Description |
|---|---|
| `LoadIGICore()` | Delegates to `Impl`; logs result; returns bool. |
| `UnloadIGICore()` | Resets `GPT` then `Core`; called from `ShutdownModule`. |
| `LoadIGIFeature(Feature, Interface, Path)` | Delegates to `FIGICore::LoadInterface`; logs result; returns `nvigi::Result`. |
| `UnloadIGIFeature(Feature, Interface)` | Delegates to `FIGICore::UnloadInterface`; logs result. |
| `GetModelsPath()` | Returns the resolved path to the model data directory. |
| `GetGPT()` | Returns a non-owning pointer to the lazily-created `FIGIGPT`. |
| `GetIGIStatusString(Result)` | Free function; maps `nvigi::Result` codes to human-readable strings for all 17 documented error codes. |

#### nvigi type forwards in `IGIModule.h`

```cpp
namespace nvigi {
    using Result = uint32_t;
    struct InferenceInterface;
    struct alignas(8) PluginID;
    struct alignas(8) CudaParameters;
}
```

These forward declarations allow game code to include `IGIModule.h` without pulling in the full NVIGI SDK headers.

### 5.6 `FIGIGPT` — Header & Implementation

**Files:** `Public/IGIGPT.h`, `Private/IGIGPT.cpp`

`FIGIGPT` wraps the NVIGI GPT inference pipeline, again using Pimpl to keep SDK types private.

#### `FIGIGPT::Impl` construction

1. `LoadIGIFeature(SelectedGPTBackendId(), &GPTInterface)` — loads `nvigi.plugin.gpt.ggml.d3d12`. The backend is hardcoded to the D3D12 variant (`nvigi::plugin::gpt::ggml::d3d12::kId`) to avoid CUDA backend aborts in the UE editor; a CUDA path is a possible future alternative (see Section 13).
2. Fills `nvigi::CommonCreationParameters`:
   - `utf8PathToModels` — path to the GGUF model directory.
   - `numThreads = 1` — optimal for a single concurrent inference.
   - `vramBudgetMB = 4096` — safe budget for RTX 4060 class hardware.
   - `modelGUID = "{8E31808B-C182-4016-9ED8-64804FF5B40D}"` — the GUID for Nemotron Mini 4B GGUF.
3. Extracts `ID3D12Device*` and `ID3D12CommandQueue*` from UE's `ID3D12DynamicRHI` interface and chains them as `nvigi::D3D12Parameters`.
4. Calls `GPTInterface->createInstance(params, &GPTInstance)`.

If any step fails, `GPTInstance` is left `nullptr` and subsequent `Evaluate()` calls are no-ops that return an empty string.

#### `FIGIGPT::Impl::Evaluate`

```
Input : SystemPrompt, UserPrompt, AssistantPrompt (FString)
Output: FString (raw model response)
```

1. Guard: returns empty if `GPTInstance == nullptr`.
2. Takes `FCriticalSection CS` (serialises calls).
3. Defines `BasicCallbackCtx` with a mutex, condition variable, atomic state, and `FString gptOutput`.
4. Defines `completionCallback` (lambda passed to NVIGI): extracts `nvigi::InferenceDataText` from the output slot, accumulates token text into `gptOutput` while skipping any `<JSON>` prefixed tokens (internal NVIGI metadata), then notifies the condition variable.
5. Converts prompt strings to UTF-8 via `StringCast<UTF8CHAR>` and wraps them in `nvigi::InferenceDataTextSTLHelper`.
6. Builds `TArray<nvigi::InferenceDataSlot>` with user slot always present; system and assistant slots added only when non-empty.
7. Sets `nvigi::GPTRuntimeParameters`:
   - `seed = -1` (random, for response variety).
   - `tokensToPredict = 48` (short response expected; keeps latency low).
   - `interactive = false` (single-shot, not a dialogue).
8. Calls `instance->evaluateAsync(&gptCtx)` — dispatches inference to NVIGI's internal thread.
9. Blocks the worker thread on the condition variable until the callback fires with a non-pending state.
10. Returns the accumulated `FString`.

#### Key constant: `GGUF_MODEL_MINITRON`

```cpp
constexpr const char* const GGUF_MODEL_MINITRON{ "{8E31808B-C182-4016-9ED8-64804FF5B40D}" };
```

This GUID identifies the Nemotron Mini 4B GGUF model in NVIGI's model registry. To switch models, change this GUID and supply the corresponding GGUF file under `data/nvigi.models/`.

### 5.7 `IGIBlueprintLibrary` — Header & Implementation

**Files:** `Public/IGIBlueprintLibrary.h`, `Private/IGIBlueprintLibrary.cpp`

`UIGIGPTEvaluateAsync` is a **Blueprint Async Action** (`UBlueprintAsyncActionBase`) that exposes a single-call, event-driven GPT query to Blueprints. It is an alternative invocation path to the C++ `ACombatLogger` — useful for Blueprint-driven prototypes or editor utilities.

```
BP usage:
  GPTEvaluateAsync(System, User, Assistant)
       └─► OnResponse pin fires once with FString Response
```

- `IsRunning` is a `static std::atomic<bool>` preventing concurrent Blueprint-triggered requests.
- `Activate()` trims all prompts, logs the user prompt, then dispatches to `ENamedThreads::AnyBackgroundHiPriTask` (higher priority than the ThreadPool used by `CombatLogger`).
- The result is marshalled back to the game thread via `AsyncTask(ENamedThreads::GameThread, …)` which broadcasts `OnResponse`.
- `AddToRoot()` / `RemoveFromRoot()` prevent the `UObject` from being garbage-collected while the async task is in flight.

---

## 6. NVIGI SDK Integration

NVIGI (NVIDIA In-Game Inference) is NVIDIA's SDK for running quantised LLMs locally on RTX hardware. It is structured around:

| Layer | Description |
|---|---|
| **Core framework** | `nvigi.core.framework.dll` — the host that manages plugin discovery and the `nvigiInit` / `nvigiShutdown` / `nvigiLoadInterface` / `nvigiUnloadInterface` API. |
| **Plugins** | Feature DLLs (e.g. `nvigi.plugin.gpt.ggml.d3d12.dll`, CUDA variant) that implement `nvigi::InferenceInterface`. |
| **Model data** | GGUF quantised model files placed in the models directory specified at init time. |

### SDK function-pointer loading

Rather than linking against an import library, `FIGICore` manually loads the core DLL via `FPlatformProcess::GetDllHandle` and resolves the four entry points with `GetDllExport`. This approach:

- Avoids hard linker dependencies — the game runs even if the NVIGI SDK is absent (the module simply reports `bInitialized = false`).
- Matches the NVIGI SDK's own recommended integration pattern.

### Inference interface lifecycle

```
nvigiInit()                ← FIGICore constructor
    └─► LoadInterface()    ← FIGIGPT::Impl constructor (per-model)
            └─► createInstance()   ← model weights loaded into VRAM
                    └─► evaluateAsync() × N   ← per inference call
            └─► destroyInstance()  ← FIGIGPT::Impl destructor
    └─► UnloadInterface()  ← FIGIGPT::Impl destructor
nvigiShutdown()            ← not yet called explicitly; DLL unload is
                              implicit shutdown (known gap — see Section 13)
```

---

## 7. End-to-End Data Flow

```
Player presses attack button
        │
        ▼
ACharacter / APlayerController (not yet wired in prototype)
        │  AddAction("low_attack")
        ▼
ACombatLogger::ActionBuffer   [low_attack, block, dodge_back, …]
        │  (rolling window, max 10 entries)
        │
        │  Every SendInterval seconds (default 4 s)
        ▼
ACombatLogger::SendToIGI()
        │
        │  SystemPrompt = "You are a combat AI boss…"
        │  UserPrompt   = "Player last actions: low_attack, block, …"
        │
        │  Async(ThreadPool)
        ▼
FIGIModule::LoadIGICore()   [once per session]
FIGIModule::GetGPT()        [lazy creation]
FIGIGPT::Evaluate(System, User, "")
        │
        │  evaluateAsync → NVIGI GGML D3D12 backend
        │  Nemotron Mini 4B processes prompt
        │  Token stream: "COUNTER_LOW"
        │
        │  AsyncTask(GameThread)
        ▼
ACombatLogger::ParseTactic("COUNTER_LOW")
        │
        ▼
CurrentTactic = EBossTactic::CounterLow
        │
        ▼
[Boss Behavior Tree / State Machine reads GetCurrentTactic()]
```

---

## 8. Threading Model

CAT uses three distinct execution contexts:

| Context | Used for |
|---|---|
| **Game Thread** | Tick, AddAction, ActionBuffer mutation, CurrentTactic write-back, log output |
| **UE Thread Pool** (`EAsyncExecution::ThreadPool`) | Module/GPT acquisition, `FIGIGPT::Evaluate` call |
| **NVIGI Internal Thread** | Token generation, `completionCallback` invocation |

**Synchronisation primitives:**

| Primitive | Location | Guards |
|---|---|---|
| `bool bPendingRequest` (game thread only) | `ACombatLogger` | Prevents a second dispatch while one is in flight |
| `FCriticalSection CS` | `FIGIModule::Impl` | `Core`, `GPT` pointers |
| `FCriticalSection CS` | `FIGIGPT::Impl` | Serialises concurrent `Evaluate()` calls |
| `std::mutex` + `std::condition_variable` | `BasicCallbackCtx` (stack in `Evaluate`) | Synchronises UE thread pool with NVIGI callback thread |
| `std::atomic<bool> IsRunning` | `UIGIGPTEvaluateAsync` | Prevents concurrent Blueprint-triggered requests |

**Important:** `bPendingRequest` is written from both the game thread (set `true` before dispatch) and marshalled writes back via `AsyncTask(GameThread, …)` (set `false` after response). Because these writes always happen on the game thread, no additional lock is needed.

---

## 9. Tactic Mapping Logic

`ACombatLogger::ParseTactic` performs a **keyword scan** on the uppercased, trimmed model response:

| Keyword scanned | Maps to |
|---|---|
| `COUNTER_LOW` | `EBossTactic::CounterLow` — punish low attacks with a low counter |
| `COUNTER_HIGH` | `EBossTactic::CounterHigh` — punish high/overhead attacks |
| `REPOSITION` | `EBossTactic::Reposition` — move away to reset engagement |
| `AGGRESSIVE_PRESS` | `EBossTactic::AggressivePress` — press the attack on a passive player |
| `DEFENSIVE_WAIT` | `EBossTactic::DefensiveWait` — hold position, bait player into attacking |
| *(none match)* | `EBossTactic::None` — model produced unexpected output; boss does nothing |

The system prompt constrains the model to produce exactly one of these tokens. The scan is intentionally `Contains`-based rather than an exact match so that minor model verbosity (e.g. `"I recommend COUNTER_LOW."`) is tolerated.

`tokensToPredict = 48` is large enough to fit any of the keywords with a few words of preamble, while staying short enough (< 100 ms on target hardware) that the 4-second polling interval never feels stale.

---

## 10. Build System & Configuration

### Unreal Build Tool targets

| File | Type | Notes |
|---|---|---|
| `Echo.Target.cs` | Game (shipping / development) | `TargetType.Game`, `BuildSettingsVersion.V5`, UE 5.5 include order |
| `EchoEditor.Target.cs` | Editor | Adds editor-only modules |

### Engine configuration highlights (`DefaultEngine.ini`)

| Setting | Value | Impact |
|---|---|---|
| `DefaultGraphicsRHI` | `DefaultGraphicsRHI_DX12` | Forces DirectX 12; required for NVIGI D3D12 backend |
| `r.RayTracing` | `True` | Enables hardware ray tracing (RTX required) |
| `r.DynamicGlobalIlluminationMethod` | `1` (Lumen) | Lumen GI |
| `r.ReflectionMethod` | `1` (Lumen) | Lumen reflections |
| `r.Shadow.Virtual.Enable` | `1` | Virtual Shadow Maps |
| `D3D12TargetedShaderFormats` | `PCD3D_SM6` | Shader Model 6 for mesh shaders and other SM6 features |
| `GameDefaultMap` | `OpenWorld` template | Default level on PIE |

The D3D12 requirement is load-bearing: `FIGIGPT::Impl` checks `GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12` and sets `GPTInstance = nullptr` if the condition fails, so running under Vulkan or D3D11 will silently disable inference.

### Third-party SDK placement

```
Plugins/IGI/ThirdParty/nvigi_pack/
├── include/               ← nvigi.h, nvigi_ai.h, nvigi_gpt.h, nvigi_struct.h, …
├── nvigi_core/include/    ← nvigi_types.h, nvigi_stl_helpers.h, …
├── bin/x64/               ← nvigi.core.framework.dll, nvigi.plugin.gpt.ggml.d3d12.dll
└── data/nvigi.models/     ← Nemotron Mini 4B .gguf model file
```

The `copy_nvigi_pack_here.txt` placeholder in `ThirdParty/` signals to contributors where the SDK pack must be placed before building.

---

## 11. Render Hardware Interface (RHI) Coupling

NVIGI's D3D12 backend requires a live `ID3D12Device*` and `ID3D12CommandQueue*`. `FIGIGPT::Impl` extracts these from UE's RHI abstraction:

```cpp
ID3D12DynamicRHI* RHI = static_cast<ID3D12DynamicRHI*>(GDynamicRHI);
ID3D12CommandQueue* CmdQ  = RHI->RHIGetCommandQueue();
ID3D12Device*      Device = RHI->RHIGetDevice(0);
```

`GDynamicRHI` is UE's global RHI singleton. The cast is guarded by `GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12`. This coupling means:

- **NVIGI inference shares the same D3D12 device as UE's renderer.** GPU resource contention is possible during heavy rendering frames. The `vramBudgetMB = 4096` cap mitigates this.
- **The FIGIGPT instance must be created after the RHI is initialised.** Because `FIGIGPT` is lazily constructed on the first `GetGPT()` call (which happens inside a thread-pool task triggered from `Tick`), the RHI is guaranteed to be alive by that point.
- **Editor vs. standalone:** Both contexts use D3D12 as long as the engine INI forces it, so inference works identically in PIE and packaged builds.

---

## 12. Blueprint Exposure

Two parallel paths exist for triggering inference from Blueprint:

### Path A — `ACombatLogger` (autonomous, polled)

Place `ACombatLogger` in the level. It self-manages its timer and dispatches automatically. Blueprint can:
- Call `AddAction(FString)` to push actions.
- Read `GetCurrentTactic()` to drive boss logic.
- Inspect `LastIGIResponse` (BlueprintReadOnly) for debugging.

### Path B — `UIGIGPTEvaluateAsync` (manual, event-driven)

```
[Call GPTEvaluateAsync]──► SystemPrompt, UserPrompt, AssistantPrompt
                               │
                         [OnResponse]──► FString Response
```

Useful when the caller wants direct control over when inference happens and wants to supply arbitrary prompts, not just the combat buffer. The `static IsRunning` prevents overlapping calls.

---

## 13. Extension Points & Future Work

| Area | Current State | Suggested Extension |
|---|---|---|
| **Input wiring** | `bAutoSeedTestActions` seeds dummy data; real player input not connected | Add `AddAction()` calls in `APlayerCharacter` animation notifies or `UAbilityTask` |
| **Tactic consumption** | `GetCurrentTactic()` is exposed but not yet wired to a Behavior Tree | Add a BT Service or BTDecorator that reads `CurrentTactic` and sets a Blackboard key |
| **Model switching** | `GGUF_MODEL_MINITRON` GUID is hardcoded | Expose `ModelGUID` as a config variable in `IGIGPT` or `ACombatLogger` |
| **Prompt engineering** | System prompt is a literal string in `CombatLogger.cpp` | Move prompts to a `UDataAsset` for designer-friendly editing |
| **Response parsing** | `Contains`-based keyword scan | Replace with structured JSON output if model supports it, or add a confidence-weighted multi-keyword scan |
| **CUDA backend** | D3D12 preferred to avoid editor aborts | Re-evaluate CUDA path once NVIGI SDK matures; CUDA may offer lower latency on non-D3D12 contexts |
| **Multi-boss support** | Single `ACombatLogger` actor | Each boss instance can own its own `ACombatLogger` or share one via a manager actor; `bPendingRequest` per-instance already handles this correctly |
| **Persistence / learning** | Tactics are stateless per-session | Log tactic history to disk; seed future sessions with the outcome to implement cross-session adaptation |
| **nvigiShutdown** | DLL unload acts as implicit shutdown | Call `(*Ptr_nvigiShutdown)()` explicitly in `FIGICore::~FIGICore` for clean SDK teardown |
