using UnrealBuildTool;

public class GameInstancedAnimationGraphNiagara : ModuleRules
{
    public GameInstancedAnimationGraphNiagara(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "Niagara",
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "NiagaraCore",
                "RenderCore",
                "RHI",
                "GameInstancedAnimationGraph",
                "Slate",
                "SlateCore"
            }
        );
    }
}