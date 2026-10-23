# Using the Rust SDK to create a simple DLL project

The Rust SDK is generated next to the C++ SDK (`C:\Dumper-7\GameVersion-GameName\RustSDK`) for UE5 x64 games.
Its API mirrors the C++ SDK: the same classes, members and functions, and the same helpers
(`UObject::FindObject`, `IsA`, `StaticClass`, `UWorld::GetWorld`, `TArray::Num`, ...).

## Cargo project setup
The SDK is an edition 2024 crate and needs Rust 1.85 or newer.
1. Create a library crate: `cargo new --lib my_mod`
2. Make it a DLL and add the SDK in `Cargo.toml`:
   ```toml
   [lib]
   crate-type = ["cdylib"]

   [dependencies]
   sdk = { path = "C:/Dumper-7/GameVersion-GameName/RustSDK" }
   ```
3. Add a `DllMain` and a `main_thread` function to `src/lib.rs` (see code [here](#code))
4. Build with `cargo build --release`. The DLL is in `target/release/my_mod.dll`.

## Compile times and layout checks
- The SDK is one crate. The first build takes about a minute; after that only your own code is rebuilt.
- Every struct is checked at compile time against the offsets and sizes the game reported. If a check fails,
  `cargo build` stops with an `assert!` error pointing at the struct. \
  To build anyway, or to build faster, turn the checks off:
  ```toml
  sdk = { path = "...", default-features = false }
  ```
- If the build fails for another reason, read the [Issues](README.md#issues) part of the [ReadMe](README.md).

## Safety
Every SDK type mirrors memory owned by the game.
- Objects are raw pointers (`*mut AActor`). Turning one into a reference (`unsafe { ptr.as_mut() }`) is the one
  unsafe step: only do it on the game thread and while the object is alive.
- UFunction wrappers take `&mut self`, so the compiler knows the game may change the object during the call.
- Strings and arrays the game returns are never freed (like the C++ SDK).

## Using the SDK
### 1. Retrieving instances of classes/structs to manipulate them
   - FindObject, used to find an object by its name
     ```rust
     use sdk::rt::EClassCastFlags;
     use sdk::CoreUObject::UObject;

     let obj1 = UObject::FindObject("ClassName PackageName.Outer1.Outer2.ObjectName", EClassCastFlags::None);
     let obj2 = UObject::FindObjectFast("ObjectName", EClassCastFlags::None);

     let obj3 = UObject::FindObjectFast("StructName", EClassCastFlags::Struct); // Finds a UStruct
     ```
   - Static functions / global variables, used to retrieve class instances from static variables
     ```rust
     /* UWorld::GetWorld() replaces GWorld, no offset required */
     let world = UWorld::GetWorld();

     let my_controller = unsafe {
         let game_instance = &mut *(*world).OwningGameInstance;
         let local_player = &mut **game_instance.LocalPlayers.get(0).unwrap();
         local_player.PlayerController
     };
     ```
### 2. Calling functions
  - Non-static functions
    ```rust
    let my_controller = unsafe { &mut *magic_func_to_get_player_controller() };

    let (mut out_x, mut out_y) = (0.0, 0.0);
    my_controller.GetMousePosition(Some(&mut out_x), Some(&mut out_y));
    ```
  - Static functions
    ```rust
    /* static functions do not require an instance, they are automatically called using their DefaultObject */
    /* sdk::fstring! is the Rust version of L"..." */
    let my_new_name = UKismetStringLibrary::Conv_StringToName(&sdk::fstring!("DemoNetDriver"));
    ```
### 3. Checking a UObject's type
  - With EClassCastFlags
    ```rust
    /* Limited to base types, but is the fastest option */
    let is_actor = obj.IsA(EClassCastFlags::Actor);
    ```
  - With `*mut UClass`
    ```rust
    /* Ideal for native classes. Use `StaticName()` for Blueprint classes instead */
    let is_specific_actor = obj.IsA(ASomeSpecificActor::StaticClass());
    ```
  - With `FName` (class name)
    ```rust
    /* Works for every class */
    let is_specific_actor = obj.IsA(ASomeSpecificActor_C::StaticName());
    ```
### 4. Casting
  UnrealEngine heavily relies on inheritance and often uses pointers to a base class, which are later assigned addresses to \
  instances of child classes. `Cast` checks the type (`IsA`) and returns `None` if it doesn't match.
  ```rust
  let pawn = unsafe { &mut *my_controller.Pawn };

  if let Some(my_game_pawn) = pawn.Cast::<AGameSpecificPawn>() {
      my_game_pawn.GameSpecificVariable = 30;
  }
  ```
  Child classes deref to their parent, so inherited members and functions can be used directly
  (`my_game_pawn.K2_GetActorLocation()`, `my_game_pawn.GetName()`).

### Differences from the C++ SDK
| C++ SDK | Rust SDK |
|---|---|
| `UObject::GObjects->Num()` | `UObject::GObjects.Num()` |
| `static_cast<APawn*>(Actor)` | `actor.Cast::<APawn>()` (checked) or `ptr.cast::<APawn>()` (unchecked) |
| `UObject::FindObject<UClass>(...)` | `UObject::FindObject(...).cast::<UClass>()` |
| `TSubclassOf<ABase> Base = Derived;` (implicit, checked) | `derived.Cast::<ABase>()` (unchecked); `.into()` converts from/to `*mut UClass` |
| `L"text"` | `sdk::fstring!("text")` |
| `UClass::GetFunction(FuncName)` | `UClass::FindFunction(&FuncName)` |
| `Flags & EFlags::Value` (contains all) | `Flags.HasFlags(EFlags::Value)` (`use sdk::rt::UEnum;`); `&` is bitwise |
| `FAllocatedString(Size)` / `TAllocatedArray<T>(Size)` | `rt::FAllocatedString::new(Size)` / `rt::TAllocatedArray::<T>::new(Size)`, then `.Get()` |
| Out-parameters `T*` | `Option<&mut T>` |
| `InSDKUtils::CallGameFunction` / `GetImageBase` setting | `call_game_function!` / `image_base` in `RustSDK/src/config.rs` |

## Code
### DllMain and main_thread
```rust
use std::ffi::c_void;

#[link(name = "kernel32")]
unsafe extern "C" {
    fn AllocConsole() -> i32;
}

fn main_thread() {
    /* Code to open a console window */
    unsafe { AllocConsole() };

    // Your code here
}

#[unsafe(no_mangle)]
extern "C" fn DllMain(_module: *mut c_void, reason: u32, _reserved: *mut c_void) -> i32 {
    const DLL_PROCESS_ATTACH: u32 = 1;

    if reason == DLL_PROCESS_ATTACH {
        std::thread::spawn(main_thread);
    }

    1
}
```
### Example program that enables the UnrealEngine console
```rust
use sdk::rt::EClassCastFlags;
use sdk::CoreUObject::UObject;
use sdk::Engine::{APawn, UEngine, UGameplayStatics, UInputSettings, UKismetStringLibrary, UWorld};

fn main_thread() {
    /* Code to open a console window */
    unsafe { AllocConsole() };

    /* Functions returning "static" instances */
    let engine = UEngine::GetEngine();
    let world = UWorld::GetWorld();

    /* Getting the PlayerController, World, OwningGameInstance, ... should all be checked not to be null! */
    let (Some(engine), Some(world)) = (unsafe { engine.as_mut() }, unsafe { world.as_mut() }) else {
        return;
    };

    /* Print the full-name of an object ("ClassName PackageName.OptionalOuter.ObjectName") */
    if let Some(console_class) = unsafe { engine.ConsoleClass.Get().as_ref() } {
        println!("{}", console_class.GetFullName());
    }

    /* Manually iterating GObjects and printing the FullName of every UObject that is a Pawn (not recommended) */
    for i in 0..UObject::GObjects.Num() {
        let Some(obj) = (unsafe { UObject::GObjects.GetByIndex(i).as_ref() }) else {
            continue;
        };

        if obj.IsDefaultObject() {
            continue;
        }

        /* Only the 'IsA' check using the cast flags is required, the other 'IsA' is redundant */
        if obj.IsA(APawn::StaticClass()) || obj.HasTypeFlag(EClassCastFlags::Pawn) {
            println!("{}", obj.GetFullName());
        }
    }

    /* You might need to loop all levels in UWorld::Levels */
    if let Some(level) = unsafe { world.PersistentLevel.as_mut() } {
        for &actor in level.Actors.iter() {
            /* Prefer using EClassCastFlags if available for your class. */
            let Some(actor) = (unsafe { actor.as_mut() }) else {
                continue;
            };

            if let Some(pawn) = actor.Cast::<APawn>() {
                // Use pawn here
                let _ = pawn;
            }
        }
    }

    /*
    * Changes the keyboard-key that's used to open the UE console
    *
    * This is a rare case of a DefaultObjects' member-variables being changed.
    * By default you do not want to use the DefaultObject, this is a rare exception.
    */
    if let Some(input_settings) = unsafe { UInputSettings::GetDefaultObj().as_mut() } {
        if let Some(console_key) = input_settings.ConsoleKeys.get_mut(0) {
            console_key.KeyName = UKismetStringLibrary::Conv_StringToName(&sdk::fstring!("F2"));
        }
    }

    /* Creates a new UObject of class-type specified by Engine->ConsoleClass */
    let new_object = UGameplayStatics::SpawnObject(engine.ConsoleClass.Cast(), engine.GameViewport.cast());

    /* The Object we created is a subclass of UConsole, so this cast is **safe**. */
    if let Some(viewport) = unsafe { engine.GameViewport.as_mut() } {
        viewport.ViewportConsole = new_object.cast();
    }
}
```
