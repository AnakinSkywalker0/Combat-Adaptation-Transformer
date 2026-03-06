using UnrealBuildTool;
using System.IO;

public class IGI : ModuleRules
{
    public IGI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects",
            "RHI",
            "D3D12RHI"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "RenderCore"
        });

        string ThirdPartyPath = Path.Combine(ModuleDirectory, "../../ThirdParty/nvigi_pack");
        PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "include"));
        PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "nvigi_core/include"));

        // D3D12 helper headers (d3dx12.h) live in the engine D3D12RHI private folder
        string D3D12PrivatePath = Path.Combine(EngineDirectory, "Source/Runtime/D3D12RHI/Private");
        PrivateIncludePaths.Add(D3D12PrivatePath);
        PrivateIncludePaths.Add(Path.Combine(D3D12PrivatePath, "Windows"));
        AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");

        string BinPath = Path.Combine(ThirdPartyPath, "bin/x64");
        RuntimeDependencies.Add(Path.Combine(BinPath, "nvigi.core.framework.dll"));
        RuntimeDependencies.Add(Path.Combine(BinPath, "nvigi.plugin.gpt.ggml.cuda.dll"));
    }
}
