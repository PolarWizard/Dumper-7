#pragma once

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "Managers/StructManager.h"
#include "Managers/MemberManager.h"
#include "Wrappers/StructWrapper.h"
#include "Wrappers/MemberWrappers.h"
#include "Wrappers/EnumWrapper.h"
#include "Managers/PackageManager.h"

#include "Generator.h"

namespace fs = std::filesystem;

/*
* Generates a Rust crate ("RustSDK") for UE5 x64 games.
*
* The crate's runtime (src/rt/*.rs) is hand-written Rust in Generator/RustRuntime/rt, embedded into the DLL as
* RCDATA resources (Generator/RustRuntime/RustRuntime.rc) and written out as-is. Everything else is generated here.
*/
class RustGenerator
{
private:
    friend class Generator;

private:
    using StreamType = std::ofstream;

    /* Hands out unique identifiers by appending '_' on collision */
    class NameScope
    {
    private:
        std::unordered_set<std::string> UsedNames;

    public:
        NameScope() = default;
        NameScope(std::initializer_list<std::string> ReservedNames) : UsedNames(ReservedNames) {}

    public:
        std::string Add(std::string Name);
    };

public:
    static inline PredefinedMemberLookupMapType PredefinedMembers;

    static inline std::string MainFolderName = "RustSDK";
    static inline std::string SubfolderName = "src";

    static inline fs::path MainFolder;
    static inline fs::path Subfolder;

private:
    /* PackageIndex -> Rust module name / folder name */
    static inline std::unordered_map<int32, std::string> PackageModuleNames;
    static inline std::unordered_map<int32, std::string> PackageFolderNames;

    /* Placeholder types for properties the generator doesn't know, name -> (size, alignment) */
    static inline std::unordered_map<std::string, std::pair<int32, int32>> UnknownProperties;

private:
    static std::string GetStructPath(const StructWrapper& Struct);
    static std::string GetEnumPath(const EnumWrapper& Enum);
    static std::string GetRustType(UEProperty Property);
    static std::string GetDelegateSignature(UEFunction SignatureFunction);
    static std::string GetPredefinedMemberType(const PropertyWrapper& Member);

    /* OutFieldNames: raw member name -> emitted field name */
    static void GenerateStruct(const StructWrapper& Struct, StreamType& StructFile, StreamType& ParamFile, StreamType& FunctionFile, int32 PackageIndex, NameScope& ParamStructNames, const std::string& StructNameOverride = std::string(), std::unordered_map<std::string, std::string>* OutFieldNames = nullptr);
    static std::string GenerateFunction(const FunctionWrapper& Func, bool bHasStaticClass, NameScope& MethodNames, StreamType& ParamFile, int32 PackageIndex, NameScope& ParamStructNames);
    static void GenerateEnum(const EnumWrapper& Enum, StreamType& StructFile);

    static void InitPackageNames();
    static bool WriteRuntimeFiles();

    static void GenerateCargoToml(StreamType& CargoToml);
    static void GenerateConfig(StreamType& Config);
    static void GenerateBasic(StreamType& Basic);
    static void GeneratePropertyFixup(StreamType& PropertyFixup);

public:
    /* The Rust SDK targets UE5 (large world coordinates) on 64-bit only */
    static bool IsSupported();

    static void Generate();

    static void InitPredefinedMembers();
    static void InitPredefinedFunctions() { }
};
