#include <algorithm>
#include <array>
#include <cctype>
#include <format>

#include "Unreal/ObjectArray.h"
#include "Generators/RustGenerator.h"
#include "Wrappers/MemberWrappers.h"
#include "Managers/MemberManager.h"
#include "SharedPredefinedMembers.h"
#include "Encoding/UnicodeNames.h"
#include "Platform.h"

#include "../Settings.h"

#include <Windows.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace
{
	/* Strict and reserved keywords of Rust 2024 */
	const std::unordered_set<std::string> RustKeywords = {
		"as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "false", "fn", "for",
		"if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static",
		"struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while", "abstract", "become", "box", "do",
		"final", "gen", "macro", "override", "priv", "try", "typeof", "unsized", "virtual", "yield",
	};

	/* Decodes the first code point of a UTF-8 string */
	char32_t GetFirstCodePoint(const std::string& Str)
	{
		const unsigned char Lead = static_cast<unsigned char>(Str[0]);

		const int32 Length = Lead < 0x80 ? 1 : Lead < 0xE0 ? 2 : Lead < 0xF0 ? 3 : 4;
		char32_t CodePoint = Length == 1 ? Lead : Lead & (0x7F >> Length);

		for (int32 i = 1; i < Length && i < static_cast<int32>(Str.size()); i++)
			CodePoint = (CodePoint << 6) | (static_cast<unsigned char>(Str[i]) & 0x3F);

		return CodePoint;
	}

	/* Turns a valid C++ identifier into a valid Rust identifier */
	std::string MakeRustIdent(const std::string& Name)
	{
		if (Name.empty() || Name == "_")
			return "__";

		/* MSVC accepts names like "２nd" (fullwidth digit first), Rust requires XID_Start or '_' */
		const char32_t FirstChar = GetFirstCodePoint(Name);

		if (FirstChar != '_' && !IsUnicodeCharXIDStart(FirstChar))
			return "_" + Name;

		if (!RustKeywords.contains(Name))
			return Name;

		/* These can't be raw identifiers */
		if (Name == "self" || Name == "Self" || Name == "super" || Name == "crate")
			return Name + "_";

		return "r#" + Name;
	}

	std::string MakeRustStringLiteral(const std::string& Str)
	{
		std::string Out = "\"";

		for (const char C : Str)
		{
			if (C == '"' || C == '\\')
				Out += '\\';

			Out += C;
		}

		return Out + '"';
	}

	std::string GetIntegerType(int32 Size, bool bIsSigned)
	{
		switch (Size)
		{
		case 1: return bIsSigned ? "i8" : "u8";
		case 2: return bIsSigned ? "i16" : "u16";
		case 4: return bIsSigned ? "i32" : "u32";
		case 8: return bIsSigned ? "i64" : "u64";
		default: return std::format("[u8; 0x{:X}]", Size);
		}
	}

	/* Alignment of the Rust type emitted for this property */
	int32 GetRustAlignment(UEProperty Property)
	{
		if (Property.IsA(EClassCastFlags::StructProperty))
		{
			if (UEStruct Struct = Property.Cast<UEStructProperty>().GetUnderlayingStruct())
				return StructWrapper(Struct).GetAlignment();
		}

		return Property.GetAlignment();
	}

	int32 Align(int32 Value, int32 Alignment)
	{
		return Alignment > 1 ? ((Value + Alignment - 1) / Alignment) * Alignment : Value;
	}
}

std::string RustGenerator::NameScope::Add(std::string Name)
{
	while (!UsedNames.insert(Name).second)
		Name += '_';

	return Name;
}

bool RustGenerator::IsSupported()
{
	return Settings::Internal::bUseLargeWorldCoordinates && !Platform::Is32Bit();
}

std::string RustGenerator::GetStructPath(const StructWrapper& Struct)
{
	const int32 PackageIndex = Struct.GetUnrealStruct().GetPackageIndex();

	return std::format("crate::{}::{}", PackageModuleNames[PackageIndex], MakeRustIdent(Struct.GetUniqueName().first));
}

std::string RustGenerator::GetEnumPath(const EnumWrapper& Enum)
{
	const int32 PackageIndex = Enum.GetUnrealEnum().GetPackageIndex();

	return std::format("crate::{}::{}", PackageModuleNames[PackageIndex], MakeRustIdent(Enum.GetUniqueName().first));
}

std::string RustGenerator::GetRustType(UEProperty Property)
{
	static const std::string UObjectPath = GetStructPath(ObjectArray::FindClassFast("Object"));
	static const std::string UClassPath = GetStructPath(ObjectArray::FindClassFast("Class"));

	auto GetClassPath = [](UEClass Class) -> std::string
	{
		return Class ? GetStructPath(Class) : UObjectPath;
	};

	auto [Class, FieldClass] = Property.GetClass();

	const EClassCastFlags Flags = Class ? Class.GetCastFlags() : FieldClass.GetCastFlags();

	/* Same order of checks as CppGenerator::GetMemberTypeStringWithoutConst, some cast-flags include others */
	if (Flags & EClassCastFlags::ByteProperty)
	{
		if (UEEnum Enum = Property.Cast<UEByteProperty>().GetEnum())
		{
			EnumWrapper WrappedEnum(Enum);

			if (WrappedEnum.GetUnderlyingTypeSize() == sizeof(uint8))
				return GetEnumPath(WrappedEnum);

			/* TEnumAsByte of a wider enum (C++: T1ByteEnum<E>) */
			return std::format("rt::TFixedSizeEnum<{}, {}>", GetEnumPath(WrappedEnum), GetIntegerType(sizeof(uint8), WrappedEnum.IsUnderlyingTypeSigned()));
		}

		return "u8";
	}
	else if (Flags & EClassCastFlags::UInt16Property)
	{
		return "u16";
	}
	else if (Flags & EClassCastFlags::UInt32Property)
	{
		return "u32";
	}
	else if (Flags & EClassCastFlags::UInt64Property)
	{
		return "u64";
	}
	else if (Flags & EClassCastFlags::Int8Property)
	{
		return "i8";
	}
	else if (Flags & EClassCastFlags::Int16Property)
	{
		return "i16";
	}
	else if (Flags & EClassCastFlags::IntProperty)
	{
		return "i32";
	}
	else if (Flags & EClassCastFlags::Int64Property)
	{
		return "i64";
	}
	else if (Flags & EClassCastFlags::FloatProperty)
	{
		return "f32";
	}
	else if (Flags & EClassCastFlags::DoubleProperty)
	{
		return "f64";
	}
	else if (Flags & EClassCastFlags::ClassProperty)
	{
		if (Property.HasPropertyFlags(EPropertyFlags::UObjectWrapper))
			return std::format("rt::TSubclassOf<{}>", GetClassPath(Property.Cast<UEClassProperty>().GetMetaClass()));

		return "*mut " + UClassPath;
	}
	else if (Flags & EClassCastFlags::NameProperty)
	{
		return "rt::FName";
	}
	else if (Flags & EClassCastFlags::StrProperty)
	{
		return "rt::FString";
	}
	else if (Flags & EClassCastFlags::TextProperty)
	{
		return "rt::FText";
	}
	else if (Flags & EClassCastFlags::BoolProperty)
	{
		return Property.Cast<UEBoolProperty>().IsNativeBool() ? "bool" : GetIntegerType(Property.GetSize(), false);
	}
	else if (Flags & EClassCastFlags::StructProperty)
	{
		return GetStructPath(Property.Cast<UEStructProperty>().GetUnderlayingStruct());
	}
	else if (Flags & EClassCastFlags::ArrayProperty)
	{
		return std::format("rt::TArray<{}>", GetRustType(Property.Cast<UEArrayProperty>().GetInnerProperty()));
	}
	else if (Flags & EClassCastFlags::WeakObjectProperty)
	{
		return std::format("rt::TWeakObjectPtr<{}>", GetClassPath(Property.Cast<UEWeakObjectProperty>().GetPropertyClass()));
	}
	else if (Flags & EClassCastFlags::LazyObjectProperty)
	{
		return std::format("rt::TLazyObjectPtr<{}>", GetClassPath(Property.Cast<UELazyObjectProperty>().GetPropertyClass()));
	}
	else if (Flags & EClassCastFlags::SoftClassProperty)
	{
		return std::format("rt::TSoftClassPtr<{}>", GetClassPath(Property.Cast<UESoftClassProperty>().GetPropertyClass()));
	}
	else if (Flags & EClassCastFlags::SoftObjectProperty)
	{
		return std::format("rt::TSoftObjectPtr<{}>", GetClassPath(Property.Cast<UESoftObjectProperty>().GetPropertyClass()));
	}
	else if (Flags & EClassCastFlags::ObjectProperty)
	{
		return "*mut " + GetClassPath(Property.Cast<UEObjectProperty>().GetPropertyClass());
	}
	else if (Settings::EngineCore::bEnableEncryptedObjectPropertySupport && Flags & EClassCastFlags::ObjectPropertyBase)
	{
		return std::format("rt::TEncryptedObjPtr<{}>", GetClassPath(Property.Cast<UEObjectProperty>().GetPropertyClass()));
	}
	else if (Flags & EClassCastFlags::MapProperty)
	{
		UEMapProperty MapProperty = Property.Cast<UEMapProperty>();

		return std::format("rt::TMap<{}, {}>", GetRustType(MapProperty.GetKeyProperty()), GetRustType(MapProperty.GetValueProperty()));
	}
	else if (Flags & EClassCastFlags::SetProperty)
	{
		return std::format("rt::TSet<{}>", GetRustType(Property.Cast<UESetProperty>().GetElementProperty()));
	}
	else if (Flags & EClassCastFlags::EnumProperty)
	{
		UEEnumProperty EnumProperty = Property.Cast<UEEnumProperty>();

		if (UEEnum Enum = EnumProperty.GetEnum())
		{
			EnumWrapper WrappedEnum(Enum);

			if (WrappedEnum.GetUnderlyingTypeSize() == Property.GetSize())
				return GetEnumPath(WrappedEnum);

			/* A mismatching size would shift every following member, so store the enum with the property's size */
			const std::string Storage = GetIntegerType(Property.GetSize(), WrappedEnum.IsUnderlyingTypeSigned());

			if (Storage.starts_with('['))
				return Storage;

			return std::format("rt::TFixedSizeEnum<{}, {}>", GetEnumPath(WrappedEnum), Storage);
		}

		return GetRustType(EnumProperty.GetUnderlayingProperty());
	}
	else if (Flags & EClassCastFlags::InterfaceProperty)
	{
		return std::format("rt::TScriptInterface<{}>", GetClassPath(Property.Cast<UEInterfaceProperty>().GetPropertyClass()));
	}
	else if (Flags & EClassCastFlags::DelegateProperty)
	{
		return std::format("rt::TDelegate<{}>", GetDelegateSignature(Property.Cast<UEDelegateProperty>().GetSignatureFunction()));
	}
	else if (Flags & EClassCastFlags::MulticastInlineDelegateProperty)
	{
		return std::format("rt::TMulticastInlineDelegate<{}>", GetDelegateSignature(Property.Cast<UEMulticastInlineDelegateProperty>().GetSignatureFunction()));
	}
	else if (Flags & EClassCastFlags::FieldPathProperty)
	{
		if (Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty)
			return "*mut " + GetClassPath(Property.Cast<UEObjectProperty>().GetPropertyClass());

		/* The field classes generated in basic.rs, other property classes fall back to their base */
		static const std::unordered_set<std::string> BasicFieldClasses = {
			"FField", "FProperty", "FByteProperty", "FBoolProperty", "FObjectPropertyBase", "FClassProperty", "FStructProperty", "FArrayProperty",
			"FDelegateProperty", "FMapProperty", "FSetProperty", "FEnumProperty", "FFieldPathProperty", "FOptionalProperty",
		};

		const UEFFieldClass FieldClass = Property.Cast<UEFieldPathProperty>().GetFieldClass();
		std::string FieldClassName = FieldClass ? FieldClass.GetCppName() : "FField";

		if (!BasicFieldClasses.contains(FieldClassName))
			FieldClassName = FieldClassName.ends_with("Property") ? "FProperty" : "FField";

		return std::format("rt::TFieldPath<crate::basic::{}>", FieldClassName);
	}
	else if (Flags & EClassCastFlags::OptionalProperty)
	{
		UEProperty ValueProperty = Property.Cast<UEOptionalProperty>().GetValueProperty();

		/* Check if there is an additional 'bool' flag in the TOptional to check if the value is set */
		if (Property.GetSize() > ValueProperty.GetSize())
			return std::format("rt::TOptional<{}>", GetRustType(ValueProperty));

		return std::format("rt::TOptionalNoFlag<{}>", GetRustType(ValueProperty));
	}
	else if (Flags & EClassCastFlags::Utf8StrProperty)
	{
		return "rt::FUtf8String";
	}
	else if (Flags & EClassCastFlags::AnsiStrProperty)
	{
		return "rt::FAnsiString";
	}

	/* Unknown property: a byte blob of the right size and alignment, defined in PropertyFixup.rs */
	const std::string Name = (Class ? Class.GetCppName() : FieldClass.GetCppName()) + "_";
	UnknownProperties[Name] = { Property.GetSize(), Property.GetAlignment() };

	return "crate::PropertyFixup::" + Name;
}

std::string RustGenerator::GetDelegateSignature(UEFunction SignatureFunction)
{
	if (!SignatureFunction)
		return "fn()";

	std::string Params;
	std::string ReturnType;

	MemberManager Members = StructWrapper(SignatureFunction).GetMembers();

	for (const PropertyWrapper& Param : Members.IterateMembers())
	{
		if (!Param.HasPropertyFlags(EPropertyFlags::Parm))
			continue;

		std::string Type = GetRustType(Param.GetUnrealProperty());

		if (Param.GetArrayDim() > 1)
			Type = std::format("[{}; 0x{:X}]", Type, Param.GetArrayDim());

		if (Param.IsReturnParam())
		{
			ReturnType = " -> " + Type;
			continue;
		}

		/* Same passing rules as the generated UFunction wrappers */
		const bool bIsConst = Param.HasPropertyFlags(EPropertyFlags::ConstParm);
		const bool bIsRef = Param.HasPropertyFlags(EPropertyFlags::ReferenceParm);
		const bool bIsOut = bIsRef || Param.HasPropertyFlags(EPropertyFlags::OutParm);
		const bool bIsMoveType = Param.IsType(EClassCastFlags::StructProperty | EClassCastFlags::ArrayProperty | EClassCastFlags::StrProperty | EClassCastFlags::TextProperty | EClassCastFlags::MapProperty | EClassCastFlags::SetProperty);

		if (bIsOut && !(bIsRef && bIsConst))
			Type = "&mut " + Type;
		else if (bIsRef || bIsMoveType)
			Type = "&" + Type;

		Params += (Params.empty() ? "" : ", ") + Type;
	}

	return std::format("fn({}){}", Params, ReturnType);
}

std::string RustGenerator::GetPredefinedMemberType(const PropertyWrapper& Member)
{
	/* Translates the C++ types used by InitCorePredefinedMembers */
	static const std::unordered_map<std::string, std::string> Translations = {
		{ "void*", "*mut ::core::ffi::c_void" },
		{ "int16", "i16" },
		{ "int32", "i32" },
		{ "EObjectFlags", "rt::EObjectFlags" },
		{ "EFunctionFlags", "rt::EFunctionFlags" },
		{ "enum class EClassCastFlags", "rt::EClassCastFlags" },
		{ "class FName", "rt::FName" },
		{ "class FField*", "*mut crate::basic::FField" },
		{ "struct FStructBaseChain", "rt::FStructBaseChain" },
		{ "uint8*", "*mut u8" },
		{ "class UObject*", "*mut " + GetStructPath(ObjectArray::FindClassFast("Object")) },
		{ "class UClass*", "*mut " + GetStructPath(ObjectArray::FindClassFast("Class")) },
		{ "class UField*", "*mut " + GetStructPath(ObjectArray::FindClassFast("Field")) },
		{ "class UStruct*", "*mut " + GetStructPath(ObjectArray::FindClassFast("Struct") ? ObjectArray::FindClassFast("Struct") : ObjectArray::FindClassFast("struct")) },
		{ "class TArray<class TPair<class FName, int64>>", "rt::TArray<rt::TPair<rt::FName, i64>>" },
		{ "TMap<class FName, uint8*>", "rt::TMap<rt::FName, *mut u8>" },
	};

	const std::string CppType = Member.GetType();

	/* Typed like the C++ SDK's FNativeFuncPtr */
	if (Member.GetName() == "ExecFunction")
		return "rt::FNativeFuncPtr";

	if (auto It = Translations.find(CppType); It != Translations.end())
		return It->second;

	if (CppType == "class TArray<class AActor*>")
	{
		if (UEClass Actor = ObjectArray::FindClassFast("Actor"))
			return std::format("rt::TArray<*mut {}>", GetStructPath(Actor));
	}

	if (CppType == "UScriptStruct*")
	{
		if (UEClass ScriptStruct = ObjectArray::FindClassFast("ScriptStruct"))
			return "*mut " + GetStructPath(ScriptStruct);
	}

	/* Unknown C++ type, keep the layout */
	return GetIntegerType(Member.GetSize(), false);
}

void RustGenerator::GenerateStruct(const StructWrapper& Struct, StreamType& StructFile, StreamType& ParamFile, StreamType& FunctionFile, int32 PackageIndex, NameScope& ParamStructNames, const std::string& StructNameOverride, std::unordered_map<std::string, std::string>* OutFieldNames)
{
	if (!Struct.IsValid())
		return;

	const bool bIsFunction = Struct.IsFunction();
	const bool bIsClass = Struct.IsClass();
	const bool bIsInterface = Struct.IsInterface();

	const std::string Name = StructNameOverride.empty() ? MakeRustIdent(Struct.GetUniqueName().first) : StructNameOverride;

	const StructWrapper Super = Struct.GetSuper();

	/* UFunctions have a Super, but parameter structs don't inherit from each other. Interfaces don't inherit from UObject in the SDK. */
	const bool bHasSuper = Super.IsValid() && !bIsFunction && !bIsInterface;
	const bool bIsReusingTrailingPaddingFromSuper = bHasSuper && Super.HasReusedTrailingPadding();

	const int32 StructSize = Struct.GetSize();
	const int32 SuperSize = bHasSuper ? Super.GetSize() : 0x0;
	const int32 Alignment = (std::max)(Struct.GetAlignment(), 0x1);

	NameScope FieldNames;
	NameScope MethodNames = { "StaticClass", "StaticName", "GetDefaultObj", "AsUObject" };

	std::string Fields;
	std::string Bitfields;
	std::string Assertions;

	int32 PrevPropertyEnd = 0x0;

	if (bHasSuper)
	{
		FieldNames.Add("Super");

		if (bIsReusingTrailingPaddingFromSuper)
		{
			/* The super's trailing padding is used by our first members, so only embed its unaligned bytes */
			PrevPropertyEnd = Super.GetUnalignedSize();
			Fields += std::format("    pub Super: rt::TailPadded<{}, 0x{:04X}>,\n", GetStructPath(Super), PrevPropertyEnd);
		}
		else
		{
			PrevPropertyEnd = SuperSize;
			Fields += std::format("    pub Super: {},\n", GetStructPath(Super));
		}
	}

	auto AddPadding = [&](int32 Offset, int32 Size, const char* Reason)
	{
		Fields += std::format("    pub {}: [u8; 0x{:X}], // 0x{:04X}(0x{:04X})({})\n", FieldNames.Add(std::format("Pad_{:X}", Offset)), Size, Offset, Size, Reason);
	};

	int32 BitfieldOffset = -1;
	std::string BitfieldName;

	MemberManager Members = Struct.GetMembers();

	for (const PropertyWrapper& Member : Members.IterateMembers())
	{
		if (Member.IsStatic() || Member.IsZeroSizedMember())
			continue;

		const int32 Offset = Member.GetOffset();
		const int32 Size = Member.GetSize();
		const int32 ArrayDim = (std::max)(Member.GetArrayDim(), 0x1);
		const std::string MemberName = Member.GetName();

		if (Member.IsBitField())
		{
			/* One byte-array per bitfield group, plus a getter/setter per bit */
			if (Offset != BitfieldOffset)
			{
				if (Offset < PrevPropertyEnd)
				{
					Fields += std::format("    // Skipped overlapping bitfield '{}' at 0x{:04X}\n", MemberName, Offset);
					continue;
				}

				if (Offset > PrevPropertyEnd)
					AddPadding(PrevPropertyEnd, Offset - PrevPropertyEnd, "Fixing Size After Last Property [ Dumper-7 ]");

				BitfieldOffset = Offset;
				BitfieldName = FieldNames.Add(std::format("BitField_{:X}", Offset));
				PrevPropertyEnd = Offset + Size;

				Fields += std::format("    pub {}: [u8; 0x{:X}], // 0x{:04X}(0x{:04X})\n", BitfieldName, Size, Offset, Size);
				Assertions += std::format("    assert!(::core::mem::offset_of!({}, {}) == 0x{:04X});\n", Name, BitfieldName, Offset);
			}

			const int32 ByteIndex = Member.GetBitIndex() / 8;

			if (ByteIndex >= Size)
				continue;

			Bitfields += std::format("    {}[{}] & 0x{:02X} => {}, {}; // {}\n", BitfieldName, ByteIndex, Member.GetFieldMask(),
				MethodNames.Add(MakeRustIdent(MemberName)), MethodNames.Add("set_" + MemberName), Member.GetFlagsOrCustomComment());

			continue;
		}

		BitfieldOffset = -1;

		if (Offset < PrevPropertyEnd)
		{
			Fields += std::format("    // Skipped overlapping member '{}' at 0x{:04X}\n", MemberName, Offset);
			continue;
		}

		if (Offset > PrevPropertyEnd)
			AddPadding(PrevPropertyEnd, Offset - PrevPropertyEnd, "Fixing Size After Last Property [ Dumper-7 ]");

		/* A misaligned member would be moved by the Rust compiler, keep its bytes instead */
		const bool bIsMisaligned = Member.IsUnrealProperty() && Offset % (std::max)(GetRustAlignment(Member.GetUnrealProperty()), 0x1) != 0;

		std::string Type;

		if (!Member.IsUnrealProperty())
		{
			Type = GetPredefinedMemberType(Member);
		}
		else if (bIsMisaligned)
		{
			Type = std::format("[u8; 0x{:X}]", Size);
		}
		else
		{
			Type = GetRustType(Member.GetUnrealProperty());
		}

		if (ArrayDim > 1)
			Type = std::format("[{}; 0x{:X}]", Type, ArrayDim);

		const std::string FieldName = FieldNames.Add(MakeRustIdent(MemberName));

		/* Parameters stored as raw bytes can't be passed through a typed wrapper argument */
		if (OutFieldNames && !bIsMisaligned)
			(*OutFieldNames)[MemberName] = FieldName;

		Fields += std::format("    pub {}: {}, // 0x{:04X}(0x{:04X})({})\n", FieldName, Type, Offset, Size * ArrayDim, Member.GetFlagsOrCustomComment());
		Assertions += std::format("    assert!(::core::mem::offset_of!({}, {}) == 0x{:04X});\n", Name, FieldName, Offset);

		PrevPropertyEnd = Offset + (Size * ArrayDim);
	}

	const int32 UnalignedSize = Struct.GetUnalignedSize();

	if (UnalignedSize > PrevPropertyEnd)
		AddPadding(PrevPropertyEnd, UnalignedSize - PrevPropertyEnd, "Fixing Struct Size After Last Property [ Dumper-7 ]");

	/* An empty C++ struct takes one byte, and UE assumes that when laying out members of this type */
	const bool bNeedsSizeByte = !bIsInterface && PrevPropertyEnd == 0x0 && UnalignedSize == 0x0;

	if (bNeedsSizeByte)
		AddPadding(0x0, 0x1, "Empty Struct [ Dumper-7 ]");

	const int32 ExpectedSize = bNeedsSizeByte ? Align(0x1, Alignment) : (bIsInterface ? 0x0 : StructSize);

	/* UClasses and UFunction parameters are never copied by value, script structs are plain data */
	const bool bIsPlainData = !bIsClass && !bIsFunction;

	const bool bHasStaticClass = bIsClass && Struct.IsUnrealStruct() && (Super.IsValid() || Struct.IsExactClassUObject());

	std::string Output = std::format(R"(
// {}
// 0x{:04X} (0x{:04X} - 0x{:04X})
#[repr(C, align(0x{:02X}))]{}
pub struct {} {{
{}}}
)", Struct.GetFullName(), StructSize - SuperSize, StructSize, SuperSize, Alignment, bIsPlainData ? "\n#[derive(Clone, Copy)]" : "", Name, Fields);

	if (bHasSuper)
		Output += std::format("rt::inherit!({} => {}{});\n", Name, GetStructPath(Super), (bIsReusingTrailingPaddingFromSuper && !bIsClass) ? ", const" : "");

	if (bHasStaticClass)
	{
		/* rt::ClassCache re-validates the cached class on every call, so Blueprint classes need no special handling */
		if (Struct.GetUniqueName().second)
		{
			Output += std::format("rt::uclass!({}, {});\n", Name, MakeRustStringLiteral(Struct.GetRawName()));
		}
		else
		{
			Output += std::format("rt::uclass!({}, full {}, {});\n", Name, MakeRustStringLiteral(Struct.GetFullName()), MakeRustStringLiteral(Struct.GetRawName()));
		}
	}

	if (bIsInterface && Struct.IsUnrealStruct())
		Output += std::format("rt::uinterface!({});\n", Name);

	if (!Bitfields.empty())
		Output += std::format("rt::bitfields!({} {{\n{}}});\n", Name, Bitfields);

	Output += std::format(R"(#[cfg(feature = "layout-checks")]
const _: () = {{
    assert!(::core::mem::size_of::<{0}>() == 0x{1:04X});
    assert!(::core::mem::align_of::<{0}>() == 0x{2:02X});
{3}}};
)", Name, ExpectedSize, Alignment, Assertions);

	StructFile << Output;

	/* Functions of UClasses (param structs of UFunctions are generated from here too) */
	if (!bIsClass || !Members.HasFunctions())
		return;

	std::string Functions;

	for (const FunctionWrapper& Func : Members.IterateFunctions())
	{
		/* The function is no callable function, but instead just the signature of a TDelegate or TMulticastInlineDelegate */
		if (Func.IsPredefined() || (Func.GetFunctionFlags() & EFunctionFlags::Delegate))
			continue;

		Functions += GenerateFunction(Func, bHasStaticClass, MethodNames, ParamFile, PackageIndex, ParamStructNames);
	}

	if (!Functions.empty())
		FunctionFile << std::format("\nimpl {} {{{}}}\n", GetStructPath(Struct), Functions);
}

std::string RustGenerator::GenerateFunction(const FunctionWrapper& Func, bool bHasStaticClass, NameScope& MethodNames, StreamType& ParamFile, int32 PackageIndex, NameScope& ParamStructNames)
{
	const bool bIsStatic = Func.IsStatic();

	/* Static functions are called on the class default object, found through StaticClass() */
	if (bIsStatic && !bHasStaticClass)
		return "";

	UEFunction UnrealFunc = Func.GetUnrealFunction();

	const bool bHasParamStruct = Func.GetParamStructSize() > 0x0;
	const std::string ParamStructName = ParamStructNames.Add(MakeRustIdent(Func.GetParamStructName()));
	const std::string ParamStructPath = std::format("crate::{}::params::{}", PackageModuleNames[PackageIndex], ParamStructName);

	std::unordered_map<std::string, std::string> ParamFieldNames;

	if (bHasParamStruct)
		GenerateStruct(Func.AsStruct(), ParamFile, ParamFile, ParamFile, PackageIndex, ParamStructNames, ParamStructName, &ParamFieldNames);

	NameScope ParamNames = { "Parms", "rt", "__FUNC", "__Out" };

	std::string Params;
	std::string ReturnType;
	std::string ReturnValue;
	std::string CopyIn;
	std::string CopyOut;
	std::string ParamDocs;

	MemberManager FuncParams = Func.GetMembers();

	for (const PropertyWrapper& Param : FuncParams.IterateMembers())
	{
		if (!Param.HasPropertyFlags(EPropertyFlags::Parm))
			continue;

		auto FieldIt = ParamFieldNames.find(Param.GetName());

		if (FieldIt == ParamFieldNames.end())
			continue;

		const std::string& Field = FieldIt->second;

		std::string Type = GetRustType(Param.GetUnrealProperty());

		if (Param.GetArrayDim() > 1)
			Type = std::format("[{}; 0x{:X}]", Type, Param.GetArrayDim());

		if (Param.IsReturnParam())
		{
			ReturnType = " -> " + Type;
			ReturnValue = std::format("        Parms.{}\n", Field);
			ParamDocs += std::format("    /// - returns `{}` ({})\n", Type, Param.GetFlagsOrCustomComment());
			continue;
		}

		const std::string Name = ParamNames.Add(MakeRustIdent(Param.GetName()));
		ParamDocs += std::format("    /// - `{}`: `{}` ({})\n", Name, Type, Param.GetFlagsOrCustomComment());

		const bool bIsConst = Param.HasPropertyFlags(EPropertyFlags::ConstParm);
		const bool bIsRef = Param.HasPropertyFlags(EPropertyFlags::ReferenceParm);
		const bool bIsOut = bIsRef || Param.HasPropertyFlags(EPropertyFlags::OutParm);
		const bool bIsMoveType = Param.IsType(EClassCastFlags::StructProperty | EClassCastFlags::ArrayProperty | EClassCastFlags::StrProperty | EClassCastFlags::TextProperty | EClassCastFlags::MapProperty | EClassCastFlags::SetProperty);

		if (bIsOut && !bIsRef)
		{
			/* Out-parameter (C++: T*) */
			Params += std::format(", {}: ::core::option::Option<&mut {}>", Name, Type);
			CopyOut += std::format("        if let ::core::option::Option::Some(__Out) = {} {{ *__Out = Parms.{}; }}\n", Name, Field);
		}
		else if (bIsRef && !bIsConst)
		{
			/* In/out reference (C++: T&) */
			Params += std::format(", {}: &mut {}", Name, Type);
			CopyIn += std::format("        Parms.{} = *{};\n", Field, Name);
			CopyOut += std::format("        *{} = Parms.{};\n", Name, Field);
		}
		else if (bIsRef || bIsMoveType)
		{
			/* Read-only reference (C++: const T&) */
			Params += std::format(", {}: &{}", Name, Type);
			CopyIn += std::format("        Parms.{} = *{};\n", Field, Name);
		}
		else
		{
			Params += std::format(", {}: {}", Name, Type);
			CopyIn += std::format("        Parms.{} = {};\n", Field, Name);
		}
	}

	if (!Params.empty() && bIsStatic)
		Params = Params.substr(2); // drop the leading ", "

	const std::string Receiver = bIsStatic ? "" : "&mut self";
	const std::string ParmsPtr = bHasParamStruct ? std::format("(&mut Parms as *mut {}).cast()", ParamStructPath) : "::core::ptr::null_mut()";
	const std::string Native = Func.HasFunctionFlag(EFunctionFlags::Native) ? "true" : "false";

	const std::string Call = bIsStatic
		? std::format("rt::call_static(Self::StaticClass(), &__FUNC, {}, {})", ParmsPtr, Native)
		: std::format("rt::call_ufunction((self as *mut Self).cast(), &__FUNC, {}, {})", ParmsPtr, Native);

	if (!ParamDocs.empty())
		ParamDocs = "    ///\n    /// Parameters:\n" + ParamDocs;

	return std::format(R"(
    /// {}
    /// ({})
{}    pub fn {}({}{}){} {{
        static __FUNC: rt::FuncCache = rt::FuncCache::new({}, {});
{}{}        unsafe {{ {} }};
{}{}    }}
)", UnrealFunc.GetFullName(), Func.StringifyFlags(), ParamDocs, MethodNames.Add(MakeRustIdent(Func.GetName())), Receiver, Params, ReturnType
  , MakeRustStringLiteral(UnrealFunc.GetOuter().GetName()), MakeRustStringLiteral(UnrealFunc.GetName())
  , bHasParamStruct ? std::format("        let mut Parms: {} = unsafe {{ ::core::mem::zeroed() }};\n", ParamStructPath) : "", CopyIn
  , Call, CopyOut, ReturnValue);
}

void RustGenerator::GenerateEnum(const EnumWrapper& Enum, StreamType& StructFile)
{
	if (!Enum.IsValid())
		return;

	const uint8 UnderlyingSize = Enum.GetUnderlyingTypeSize();
	const bool bIsValidSize = UnderlyingSize == 1 || UnderlyingSize == 2 || UnderlyingSize == 4 || UnderlyingSize == 8;
	const std::string UnderlyingType = bIsValidSize ? GetIntegerType(UnderlyingSize, Enum.IsUnderlyingTypeSigned()) : "u8";

	NameScope ValueNames = { "__VARIANTS" };
	std::string Values;

	for (const EnumCollisionInfo& Info : Enum.GetMembers())
		Values += std::format("        {} = 0x{:X}u64,\n", ValueNames.Add(MakeRustIdent(Info.GetUniqueName())), Info.GetValue());

	StructFile << std::format(R"(
rt::ue_enum! {{
    /// {}
    {}: {} {{
{}    }}
}}
)", Enum.GetFullName(), MakeRustIdent(Enum.GetUniqueName().first), UnderlyingType, Values);
}

void RustGenerator::InitPackageNames()
{
	NameScope ModuleNames = { "rt", "config", "PropertyFixup", "core", "std", "alloc" };
	NameScope FolderNames = { "rt" };

	for (PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
	{
		const int32 PackageIndex = Package.GetIndex();

		const std::string ModuleName = ModuleNames.Add(MakeRustIdent(Package.GetName()));
		const std::string Unescaped = ModuleName.starts_with("r#") ? ModuleName.substr(2) : ModuleName;

		/* Folders must be ASCII and unique on case-insensitive file systems */
		const bool bIsAscii = std::all_of(Unescaped.begin(), Unescaped.end(), [](char C) { return C > 0 && C < 0x7F; });

		std::string FolderName = bIsAscii ? Unescaped : std::format("Package_{}", PackageIndex);
		std::string LowerFolderName = FolderName;
		std::transform(LowerFolderName.begin(), LowerFolderName.end(), LowerFolderName.begin(), [](char C) { return static_cast<char>(std::tolower(static_cast<unsigned char>(C))); });

		const std::string UniqueLower = FolderNames.Add(LowerFolderName);

		if (UniqueLower != LowerFolderName)
			FolderName += std::string(UniqueLower.size() - LowerFolderName.size(), '_');

		PackageModuleNames[PackageIndex] = ModuleName;
		PackageFolderNames[PackageIndex] = FolderName;
	}
}

bool RustGenerator::WriteRuntimeFiles()
{
	struct RuntimeFile
	{
		const wchar_t* ResourceName;
		const char* FileName;
	};

	/* Must match Generator/RustRuntime/RustRuntime.rc */
	static constexpr std::array<RuntimeFile, 7> RuntimeFiles = { {
		{ L"RT_MOD", "mod.rs" },
		{ L"RT_CONTAINERS", "containers.rs" },
		{ L"RT_FFI", "ffi.rs" },
		{ L"RT_MATH", "math.rs" },
		{ L"RT_OBJECT", "object.rs" },
		{ L"RT_STRINGS", "strings.rs" },
		{ L"RT_TYPES", "types.rs" },
	} };

	const HMODULE ThisModule = reinterpret_cast<HMODULE>(&__ImageBase);
	const fs::path RuntimeFolder = Subfolder / "rt";

	fs::create_directories(RuntimeFolder);

	for (const RuntimeFile& File : RuntimeFiles)
	{
		const HRSRC Resource = FindResourceW(ThisModule, File.ResourceName, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
		const HGLOBAL LoadedResource = Resource ? LoadResource(ThisModule, Resource) : nullptr;
		const void* Data = LoadedResource ? LockResource(LoadedResource) : nullptr;

		if (!Data)
		{
			std::cerr << "RustSDK: couldn't load the embedded runtime file '" << File.FileName << "' (was the DLL mapped without its resources?)\n";
			return false;
		}

		StreamType OutFile(RuntimeFolder / File.FileName, std::ios::binary);
		OutFile.write(static_cast<const char*>(Data), SizeofResource(ThisModule, Resource));
	}

	return true;
}

void RustGenerator::GenerateCargoToml(StreamType& CargoToml)
{
	CargoToml << std::format(R"(# Rust SDK for {} ({}), generated by Dumper-7.
[package]
name = "sdk"
version = "0.1.0"
edition = "2024"
rust-version = "1.85"

[lib]
path = "src/lib.rs"

[features]
default = ["layout-checks"]
# Compile-time checks that every struct matches the game's layout. Turn off with
# `default-features = false` to build faster, or if a single odd struct fails to compile.
layout-checks = []
)", Settings::Generator::GameName, Settings::Generator::GameVersion);
}

void RustGenerator::GenerateConfig(StreamType& Config)
{
	const auto& GObjectsLayout = Off::FUObjectArray::ChunkedFixedLayout;

	auto GetMemberOffset = [](const char* ClassName, const char* MemberName) -> int32
	{
		if (UEClass Class = ObjectArray::FindClassFast(ClassName))
		{
			if (UEProperty Member = Class.FindMember(MemberName))
				return Member.GetOffset();
		}

		return -1;
	};

	auto GetClassPathOr = [](const char* ClassName, const std::string& Fallback) -> std::string
	{
		UEClass Class = ObjectArray::FindClassFast(ClassName);
		return Class ? GetStructPath(Class) : Fallback;
	};

	const std::string UObjectPath = GetStructPath(ObjectArray::FindClassFast("Object"));
	const UEStruct SoftObjectPath = ObjectArray::FindObjectFast<UEStruct>("SoftObjectPath");

	std::string NameMode = "NamePool";

	if (Off::InSDK::Name::AppendNameToString != 0x0)
		NameMode = Off::InSDK::Name::bIsAppendStringInlinedAndUsed ? "InlinedAppendString" : "AppendString";

	/* Things the Rust SDK can't do automatically become compile errors with instructions */
	std::string Errors;

	if (!Off::FUObjectArray::bIsChunked || Off::InSDK::ObjArray::ChunkSize <= 0)
		Errors += "compile_error!(\"This game uses a non-chunked GObjects array, which the Rust SDK doesn't support.\");\n";

	if (!Settings::Internal::bUseNamePool && Off::InSDK::Name::AppendNameToString == 0x0)
		Errors += "compile_error!(\"This game has neither FName::AppendString nor an FNamePool, which the Rust SDK doesn't support.\");\n";

	if (!Off::InSDK::Name::bIsUsingAppendStringOverToString)
		Errors += "compile_error!(\"Dumper-7 only found FName::ToString, which allocates on every call and would leak memory. Find AppendString and set APPEND_STRING manually.\");\n";

	std::string DecryptObjects = "objects";

	if (!ObjectArray::DecryptionLambdaStr.empty())
	{
		std::string CommentedLambda = "// " + ObjectArray::DecryptionLambdaStr;

		for (size_t Pos = 0; (Pos = CommentedLambda.find('\n', Pos)) != std::string::npos; Pos += 4)
			CommentedLambda.replace(Pos, 1, "\n// ");

		Errors += std::format("// GObjects are encrypted in this game. Dumper-7 used this C++ decryption, port it to `decrypt_objects` below:\n{}\ncompile_error!(\"Port the GObjects decryption to `decrypt_objects` in config.rs, then remove this line.\");\n", CommentedLambda);
	}

	Config << std::format(R"(//! Values Dumper-7 found for {} ({}).
//!
//! `image_base` and `decrypt_objects` may be edited, everything else is used by `rt` as-is.

use crate::rt;

pub const GAME_NAME: &str = {};
pub const GAME_VERSION: &str = {};

{}
// GObjects (FChunkedFixedUObjectArray)
pub const GOBJECTS: usize = 0x{:X};
pub const GOBJECTS_OBJECTS: usize = 0x{:X};
pub const GOBJECTS_NUM_ELEMENTS: usize = 0x{:X};
pub const GOBJECTS_NUM_CHUNKS: usize = 0x{:X};
pub const GOBJECTS_CHUNK_SIZE: i32 = 0x{:X};
pub const GOBJECTS_ITEM_SIZE: usize = 0x{:X};
pub const GOBJECTS_ITEM_OBJECT_OFFSET: usize = 0x{:X};

// FName
pub const FNAME_SIZE: usize = 0x{:X};
pub const FNAME_NUMBER_OFFSET: isize = {};
pub const FNAME_DISPLAY_INDEX_OFFSET: isize = {};
pub const NAME_MODE: rt::NameMode = rt::NameMode::{};
pub const APPEND_STRING: usize = 0x{:X};
pub const GET_NAME_ENTRY: usize = 0x{:X};
pub const GNAMES: usize = 0x{:X};
pub const CASE_PRESERVING_NAME: bool = {};
pub const OUTLINE_NUMBER_NAME: bool = {};
pub const NAMEPOOL_BLOCK_OFFSET_BITS: i32 = 0x{:X};
pub const NAMEPOOL_ENTRY_STRIDE: usize = 0x{:X};
pub const NAMEPOOL_CURRENT_BLOCK: usize = 0x{:X};
pub const NAMEPOOL_CURRENT_BYTE_CURSOR: usize = 0x{:X};
pub const NAMEPOOL_BLOCKS: usize = 0x{:X};
pub const NAMEENTRY_HEADER_OFFSET: usize = 0x{:X};
pub const NAMEENTRY_STRING_OFFSET: usize = 0x{:X};

// Functions and globals
pub const PROCESS_EVENT_INDEX: usize = 0x{:X};
pub const GWORLD: usize = 0x{:X};
pub const ENGINE_GAME_VIEWPORT_OFFSET: isize = {};
pub const VIEWPORT_WORLD_OFFSET: isize = {};

// Sizes of engine types
pub const TEXT_SIZE: usize = 0x{:X};
pub const TEXT_DATA_OFFSET: usize = 0x{:X};
pub const TEXT_DATA_STRING_OFFSET: usize = 0x{:X};
pub const DELEGATE_SIZE: usize = 0x{:X};
pub const MULTICAST_INLINE_DELEGATE_SIZE: usize = 0x{:X};
pub const WEAK_POINTER_HAS_TAG: bool = {};

// Types the runtime uses
pub type UObject = {};
pub type UField = {};
pub type UStruct = {};
pub type UClass = {};
pub type UFunction = {};
pub type UEngine = {};
pub type UWorld = {};
pub type FSoftObjectPath = {};

/// Base address of the game module.
pub fn image_base() -> usize {{
    #[link(name = "kernel32")]
    unsafe extern "C" {{
        fn GetModuleHandleW(module_name: *const u16) -> *mut ::core::ffi::c_void;
    }}

    unsafe {{ GetModuleHandleW(::core::ptr::null()) as usize }}
}}

/// Decrypts the pointer to the GObjects chunk table (most games don't encrypt it).
pub fn decrypt_objects(objects: *mut u8) -> *mut u8 {{
    {}
}}
)"
	, Settings::Generator::GameName, Settings::Generator::GameVersion
	, MakeRustStringLiteral(Settings::Generator::GameName), MakeRustStringLiteral(Settings::Generator::GameVersion)
	, Errors
	, (std::max)(Off::InSDK::ObjArray::GObjects, 0x0), (std::max)(GObjectsLayout.ObjectsOffset, 0x0), (std::max)(GObjectsLayout.NumElementsOffset, 0x0), (std::max)(GObjectsLayout.NumChunksOffset, 0x0)
	, (std::max)(Off::InSDK::ObjArray::ChunkSize, 0x1), (std::max)(Off::InSDK::ObjArray::FUObjectItemSize, 0x0), (std::max)(Off::InSDK::ObjArray::FUObjectItemInitialOffset, 0x0)
	, Off::InSDK::Name::FNameSize
	, Settings::Internal::bUseOutlineNumberName ? -1 : Off::FName::Number
	, Settings::Internal::bUseCasePreservingName ? (Off::FName::Number == 4 ? 8 : 4) : -1
	, NameMode
	, (std::max)(Off::InSDK::Name::AppendNameToString, 0x0), (std::max)(Off::InSDK::Name::GetNameEntryFromName, 0x0), (std::max)(Off::InSDK::NameArray::GNames, 0x0)
	, Settings::Internal::bUseCasePreservingName, Settings::Internal::bUseOutlineNumberName
	, Off::InSDK::NameArray::FNamePoolBlockOffsetBits, Off::InSDK::NameArray::FNameEntryStride
	, (std::max)(Off::NameArray::MaxChunkIndex, 0x0), (std::max)(Off::NameArray::ByteCursor, 0x0), (std::max)(Off::NameArray::ChunksStart, 0x0)
	, (std::max)(Off::FNameEntry::NamePool::HeaderOffset, 0x0), (std::max)(Off::FNameEntry::NamePool::StringOffset, 0x0)
	, Off::InSDK::ProcessEvent::PEIndex
	, Settings::CppGenerator::bForceNoGWorldInSDK ? 0x0 : (std::max)(Off::InSDK::World::GWorld, 0x0)
	, GetMemberOffset("Engine", "GameViewport"), GetMemberOffset("GameViewportClient", "World")
	, Off::InSDK::Text::TextSize, Off::InSDK::Text::TextDatOffset, Off::InSDK::Text::InTextDataStringOffset
	, PropertySizes::DelegateProperty, PropertySizes::MulticastInlineDelegateProperty
	, !Settings::Internal::bIsWeakObjectPtrWithoutTag
	, UObjectPath
	, GetClassPathOr("Field", UObjectPath)
	, ObjectArray::FindClassFast("Struct") ? GetStructPath(ObjectArray::FindClassFast("Struct")) : GetClassPathOr("struct", UObjectPath)
	, GetClassPathOr("Class", UObjectPath)
	, GetClassPathOr("Function", UObjectPath)
	, GetClassPathOr("Engine", UObjectPath)
	, GetClassPathOr("World", UObjectPath)
	, SoftObjectPath ? GetStructPath(SoftObjectPath) : "rt::FSoftObjectPathFallback"
	, DecryptObjects);

	auto GetScriptStructPath = [](const char* StructName) -> std::string
	{
		const UEStruct Struct = ObjectArray::FindObjectFast<UEStruct>(StructName);
		return Struct ? GetStructPath(Struct) : "()";
	};

	Config << std::format(R"(
/// Offset of `UObject::ProcessEvent` from the image base (for hooking).
pub const PROCESS_EVENT: usize = 0x{:X};

// More types the runtime uses
pub type UGameEngine = {};
pub type UKismetStringLibrary = {};
pub type FVector = {};
pub type FVector2D = {};
pub type FRotator = {};

/// Every call into game code goes through this macro (like `InSDKUtils::CallGameFunction` in the C++ SDK).
/// Replace the body to route calls through your own trampoline.
macro_rules! call_game_function {{
    ($function:expr $(, $argument:expr)* $(,)?) => {{
        $function($($argument),*)
    }};
}}
pub(crate) use call_game_function;
)"
	, (std::max)(Off::InSDK::ProcessEvent::PEOffset, 0x0)
	, GetClassPathOr("GameEngine", UObjectPath)
	, GetClassPathOr("KismetStringLibrary", UObjectPath)
	, GetScriptStructPath("Vector"), GetScriptStructPath("Vector2D"), GetScriptStructPath("Rotator"));
}

void RustGenerator::GenerateBasic(StreamType& Basic)
{
	struct BasicMember
	{
		std::string Name;
		std::string Type;
		int32 Offset;
		int32 Size;
		int32 Alignment = 0x1;
	};

	/* Returns the struct's size, which is where child structs start */
	auto GenerateBasicStruct = [&Basic](const std::string& Name, const std::string& Super, int32 SuperSize, std::vector<BasicMember> Members, int32 MinSize = 0x0, const std::string& Extra = std::string(), bool bIsPlainData = false) -> int32
	{
		std::sort(Members.begin(), Members.end(), [](const BasicMember& L, const BasicMember& R) { return L.Offset < R.Offset; });

		std::string Fields = Super.empty() ? "" : std::format("    pub Super: {},\n", Super);
		std::string Assertions;
		int32 PrevMemberEnd = SuperSize;

		auto AddPadding = [&](int32 Offset, int32 Size)
		{
			Fields += std::format("    pub Pad_{:X}: [u8; 0x{:X}],\n", Offset, Size);
		};

		for (const BasicMember& Member : Members)
		{
			/* Offsets Dumper-7 couldn't find, that overlap the previous member, or that Rust would move to align them */
			if (Member.Offset < PrevMemberEnd || Member.Offset % Member.Alignment != 0)
			{
				Fields += std::format("    // Skipped '{}': offset 0x{:X} is unknown, overlaps or is misaligned\n", Member.Name, Member.Offset);
				continue;
			}

			if (Member.Offset > PrevMemberEnd)
				AddPadding(PrevMemberEnd, Member.Offset - PrevMemberEnd);

			Fields += std::format("    pub {}: {}, // 0x{:04X}(0x{:04X})\n", Member.Name, Member.Type, Member.Offset, Member.Size);
			Assertions += std::format("    assert!(::core::mem::offset_of!({}, {}) == 0x{:04X});\n", Name, Member.Name, Member.Offset);

			PrevMemberEnd = Member.Offset + Member.Size;
		}

		if (MinSize > PrevMemberEnd)
		{
			AddPadding(PrevMemberEnd, MinSize - PrevMemberEnd);
			PrevMemberEnd = MinSize;
		}

		const int32 Size = Align(PrevMemberEnd, sizeof(void*));

		Basic << std::format("\n/// {}\n#[repr(C, align(0x08))]{}\npub struct {} {{\n{}}}\n", Name, bIsPlainData ? "\n#[derive(Clone, Copy)]" : "", Name, Fields);

		if (!Super.empty())
			Basic << std::format("rt::inherit!({} => {});\n", Name, Super);

		Basic << Extra;
		Basic << std::format("#[cfg(feature = \"layout-checks\")]\nconst _: () = {{\n    assert!(::core::mem::size_of::<{}>() == 0x{:04X});\n{}}};\n", Name, Size, Assertions);

		return Size;
	};

	const std::string ClassPtr = "*mut " + GetStructPath(ObjectArray::FindClassFast("Class"));
	const std::string StructPtr = "*mut " + GetStructPath(ObjectArray::FindClassFast("Struct") ? ObjectArray::FindClassFast("Struct") : ObjectArray::FindClassFast("struct"));
	const std::string EnumPtr = "*mut " + GetStructPath(ObjectArray::FindClassFast("Enum"));
	const std::string FunctionPtr = "*mut " + GetStructPath(ObjectArray::FindClassFast("Function"));
	const std::string VoidPtr = "*mut ::core::ffi::c_void";
	const int32 NameSize = Off::InSDK::Name::FNameSize;
	const int32 PtrSize = sizeof(void*);

	Basic << "//! Reflection types with offsets Dumper-7 found for this game (the C++ SDK's Basic.hpp).\n\nuse crate::rt;\n";

	GenerateBasicStruct("FFieldClass", "", 0x0, {
		{ "Name", "rt::FName", Off::FFieldClass::Name, NameSize },
		{ "Id", "u64", Off::FFieldClass::Id, 0x8 },
		{ "CastFlags", "rt::EClassCastFlags", Off::FFieldClass::CastFlags, 0x8 },
		{ "ClassFlags", "rt::EClassFlags", Off::FFieldClass::ClassFlags, 0x4 },
		{ "SuperClass", "*mut FFieldClass", Off::FFieldClass::SuperClass, PtrSize },
	});

	/* Container is either an FField* or a UObject*, told apart by a bool or by the lowest bit of the pointer */
	const int32 FieldVariantSize = Settings::Internal::bUseMaskForFieldOwner ? 0x8 : 0x10;

	if (Settings::Internal::bUseMaskForFieldOwner)
	{
		GenerateBasicStruct("FFieldVariant", "", 0x0, { { "Container", VoidPtr, 0x0, PtrSize } }, 0x0, "impl FFieldVariant {\n    pub const UObjectMask: u64 = 0x1;\n}\n");
	}
	else
	{
		GenerateBasicStruct("FFieldVariant", "", 0x0, { { "Container", VoidPtr, 0x0, PtrSize }, { "bIsUObject", "bool", 0x8, 0x1 } });
	}

	const int32 FieldSize = GenerateBasicStruct("FField", "", 0x0, {
		{ "VTable", VoidPtr, Off::FField::Vft, PtrSize },
		{ "ClassPrivate", "*mut FFieldClass", Off::FField::Class, PtrSize },
		{ "Owner", "FFieldVariant", Off::FField::Owner, FieldVariantSize },
		{ "Next", "*mut FField", Off::FField::Next, PtrSize },
		{ "Name", "rt::FName", Off::FField::Name, NameSize },
		{ "ObjFlags", "rt::EObjectFlags", Off::FField::Flags, 0x4 },
	});

	const int32 PropertySize = GenerateBasicStruct("FProperty", "FField", FieldSize, {
		{ "ArrayDim", Settings::Internal::bUseUint8ArrayDim ? "u8" : "i32", Off::Property::ArrayDim, Settings::Internal::bUseUint8ArrayDim ? 0x1 : 0x4 },
		{ "ElementSize", "i32", Off::Property::ElementSize, 0x4 },
		{ "PropertyFlags", "rt::EPropertyFlags", Off::Property::PropertyFlags, 0x8 },
		{ "Offset", "i32", Off::Property::Offset_Internal, 0x4 },
	}, Off::InSDK::Properties::PropertySize);

	GenerateBasicStruct("FByteProperty", "FProperty", PropertySize, { { "Enum", EnumPtr, Off::ByteProperty::Enum, PtrSize } });

	GenerateBasicStruct("FBoolProperty", "FProperty", PropertySize, {
		{ "FieldSize", "u8", Off::BoolProperty::Base, 0x1 },
		{ "ByteOffset", "u8", Off::BoolProperty::Base + 0x1, 0x1 },
		{ "ByteMask", "u8", Off::BoolProperty::Base + 0x2, 0x1 },
		{ "FieldMask", "u8", Off::BoolProperty::Base + 0x3, 0x1 },
	});

	const int32 ObjectPropertyBaseSize = GenerateBasicStruct("FObjectPropertyBase", "FProperty", PropertySize, { { "PropertyClass", ClassPtr, Off::ObjectProperty::PropertyClass, PtrSize } });

	GenerateBasicStruct("FClassProperty", "FObjectPropertyBase", ObjectPropertyBaseSize, { { "MetaClass", ClassPtr, Off::ClassProperty::MetaClass, PtrSize } });
	GenerateBasicStruct("FStructProperty", "FProperty", PropertySize, { { "Struct", StructPtr, Off::StructProperty::Struct, PtrSize } });
	GenerateBasicStruct("FArrayProperty", "FProperty", PropertySize, { { "InnerProperty", "*mut FProperty", Off::ArrayProperty::Inner, PtrSize } });
	GenerateBasicStruct("FDelegateProperty", "FProperty", PropertySize, { { "SignatureFunction", FunctionPtr, Off::DelegateProperty::SignatureFunction, PtrSize } });

	GenerateBasicStruct("FMapProperty", "FProperty", PropertySize, {
		{ "KeyProperty", "*mut FProperty", Off::MapProperty::Base, PtrSize },
		{ "ValueProperty", "*mut FProperty", Off::MapProperty::Base + PtrSize, PtrSize },
	});

	GenerateBasicStruct("FSetProperty", "FProperty", PropertySize, { { "ElementProperty", "*mut FProperty", Off::SetProperty::ElementProp, PtrSize } });

	GenerateBasicStruct("FEnumProperty", "FProperty", PropertySize, {
		{ "UnderlayingProperty", "*mut FProperty", Off::EnumProperty::Base, PtrSize },
		{ "Enum", EnumPtr, Off::EnumProperty::Base + PtrSize, PtrSize },
	});

	GenerateBasicStruct("FFieldPathProperty", "FProperty", PropertySize, { { "FieldClass", "*mut FFieldClass", Off::FieldPathProperty::FieldClass, PtrSize } });
	GenerateBasicStruct("FOptionalProperty", "FProperty", PropertySize, { { "ValueProperty", "*mut FProperty", Off::OptionalProperty::ValueProperty, PtrSize } });

	/* Same layout as the C++ SDK's FFieldPath, editor builds have two more members */
	const bool bIsWithEditorOnlyData = PropertySizes::FieldPathProperty > 0x20;
	const int32 EditorOnlySize = bIsWithEditorOnlyData ? PtrSize + 0x4 : 0x0;

	std::vector<BasicMember> FieldPathMembers = {
		{ "ResolvedField", "*mut FField", 0x0, PtrSize, PtrSize },
		{ "ResolvedOwner", "rt::TWeakObjectPtr<" + StructPtr.substr(5) + ">", PtrSize + EditorOnlySize, 0x8, 0x4 },
		{ "Path", "rt::TArray<rt::FName>", PtrSize + EditorOnlySize + 0x8, 0x10, PtrSize },
	};

	if (bIsWithEditorOnlyData)
	{
		FieldPathMembers.push_back({ "InitialFieldClass", "*mut FFieldClass", PtrSize, PtrSize, PtrSize });
		FieldPathMembers.push_back({ "FieldPathSerialNumber", "i32", PtrSize * 2, 0x4, 0x4 });
	}

	GenerateBasicStruct("FFieldPath", "", 0x0, FieldPathMembers, PropertySizes::FieldPathProperty, "", true);
}

void RustGenerator::GeneratePropertyFixup(StreamType& PropertyFixup)
{
	PropertyFixup << "//! Placeholder types for properties Dumper-7 doesn't know. They only keep the layout correct.\n";

	for (const auto& [Name, SizeAndAlignment] : UnknownProperties)
	{
		const auto [Size, Alignment] = SizeAndAlignment;

		PropertyFixup << std::format("\n#[repr(C, align(0x{:02X}))]\n#[derive(Clone, Copy)]\npub struct {}(pub [u8; 0x{:X}]);\n", (std::max)(Alignment, 0x1), Name, Size);
	}
}

void RustGenerator::Generate()
{
	InitPackageNames();

	if (!WriteRuntimeFiles())
		return;

	StreamType LibRs(Subfolder / "lib.rs");

	LibRs << std::format(R"(//! Rust SDK for {} ({}), generated by Dumper-7.
//!
//! Every type mirrors game memory. Turning a raw game pointer into a reference
//! (`unsafe {{ &mut *ptr }}`) is the one unsafe step: do it on the game thread,
//! while the object is alive. See `rt` for object lookup, names and containers.
#![allow(non_snake_case, non_camel_case_types, non_upper_case_globals, dead_code, unused_imports, unused_macros, unused_unsafe, unused_mut, clippy::all)]
#![allow(uncommon_codepoints, confusable_idents, mixed_script_confusables)]

pub mod rt;
pub mod config;
pub mod basic;
pub mod PropertyFixup;

)", Settings::Generator::GameName, Settings::Generator::GameVersion);

	StreamType Basic(Subfolder / "basic.rs");
	GenerateBasic(Basic);

	for (PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
	{
		if (Package.IsEmpty())
			continue;

		const int32 PackageIndex = Package.GetIndex();
		const std::string& ModuleName = PackageModuleNames[PackageIndex];
		const std::string& FolderName = PackageFolderNames[PackageIndex];

		const fs::path PackageFolder = Subfolder / FolderName;
		fs::create_directories(PackageFolder);

		LibRs << std::format("#[path = \"{}/mod.rs\"]\npub mod {};\n", FolderName, ModuleName);

		StreamType TypesFile(PackageFolder / "mod.rs");
		StreamType ParamsFile;
		StreamType FunctionsFile;

		TypesFile << std::format("//! Package {}. Generated by Dumper-7.\n\nuse crate::rt;\n", Package.GetName());

		/* Only UClasses have functions */
		if (Package.HasClasses())
		{
			ParamsFile.open(PackageFolder / "params.rs");
			FunctionsFile.open(PackageFolder / "functions.rs");

			ParamsFile << std::format("//! Parameter structs of package {}. Generated by Dumper-7.\n\nuse crate::rt;\n", Package.GetName());
			FunctionsFile << std::format("//! UFunction wrappers of package {}. Generated by Dumper-7.\n\nuse crate::rt;\n", Package.GetName());

			TypesFile << "\npub mod params;\nmod functions;\n";
		}

		NameScope ParamStructNames;

		for (int32 EnumIdx : Package.GetEnums())
			GenerateEnum(ObjectArray::GetByIndex<UEEnum>(EnumIdx), TypesFile);

		DependencyManager::OnVisitCallbackType GenerateStructCallback = [&](int32 Index) -> void
		{
			GenerateStruct(ObjectArray::GetByIndex<UEStruct>(Index), TypesFile, ParamsFile, FunctionsFile, PackageIndex, ParamStructNames);
		};

		if (Package.HasStructs())
			Package.GetSortedStructs().VisitAllNodesWithCallback(GenerateStructCallback);

		if (Package.HasClasses())
			Package.GetSortedClasses().VisitAllNodesWithCallback(GenerateStructCallback);
	}

	StreamType Config(Subfolder / "config.rs");
	GenerateConfig(Config);

	/* Written last, GetRustType collects unknown properties while generating the packages */
	StreamType PropertyFixup(Subfolder / "PropertyFixup.rs");
	GeneratePropertyFixup(PropertyFixup);

	StreamType CargoToml(MainFolder / "Cargo.toml");
	GenerateCargoToml(CargoToml);
}

void RustGenerator::InitPredefinedMembers()
{
	InitCorePredefinedMembers(PredefinedMembers);

	/* Same as CppGenerator::InitPredefinedMembers */
	const UEStruct FInstancedStruct = ObjectArray::FindStructFast("InstancedStruct");
	if (FInstancedStruct && FInstancedStruct.GetStructSize() >= 0x10)
	{
		PredefinedMembers[FInstancedStruct.GetIndex()].Members =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "UScriptStruct*", .Name = "ScriptStruct", .Offset = Off::FInstancedStruct::ScriptStruct, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint8*", .Name = "StructMemory", .Offset = Off::FInstancedStruct::StructMemory, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			}
		};
	}

	for (auto& [Index, Predefs] : PredefinedMembers)
		std::sort(Predefs.Members.begin(), Predefs.Members.end(), ComparePredefinedMembers);
}
