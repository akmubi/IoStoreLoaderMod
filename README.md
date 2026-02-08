# IoStoreLoaderMod

A **UE4SS C++ mod** that loads custom assets (`.pak`, `.utoc`, `.ucas` files) into **Hi-Fi RUSH** at runtime.

## What it does

- ✅ Loads `.pak`, `.utoc`, and `.ucas` mod files at runtime
- ✅ Auto-spawns Blueprint ModActors when the game starts
- ✅ Works with Steam, Epic Games Store, and Xbox versions
- ✅ Supports multiple mods with automatic load ordering

## Requirements

- **UE4SS 3.0.1** with C++ mods enabled
- Mods must be cooked for **UE 4.27** with **IoStore enabled**

## Installation
### Steam/Epic Games

1. Download [IoStoreLoaderBundle.Steam.Epic.zip](https://github.com/akmubi/IoStoreLoaderMod/releases/latest/download/IoStoreLoaderBundle.Steam.Epic.zip)
2. Extract to: `<Game>/Hibiki/Binaries/Win64/`

### Xbox (Microsoft Store)
1. Download [IoStoreLoaderBundle.Xbox.zip](https://github.com/akmubi/IoStoreLoaderMod/releases/latest/download/IoStoreLoaderBundle.Xbox.zip)
2. Extract to: `<Game>/Hibiki/Binaries/WinGDK/`

## Adding mods

Place each mod in its own folder inside `Mods/IoStoreLoaderMod/`:

```
Mods/
  IoStoreLoaderMod/
    dlls/
      main.dll
    ExampleMod/
      ExampleMod.pak
      ExampleMod.utoc
      ExampleMod.ucas
    AnotherMod/
      AnotherMod.pak
```

**To disable a mod:** Move its folder into `Mods/IoStoreLoaderMod/disabled/`

## Load order

Mods load in **alphabetical order** by folder name:
- `A_FirstMod` loads first (order 200)
- `Z_LastMod` loads last (order 201, 202, etc.)
- **Later mods override earlier ones**

To control which mod wins, rename folders:
```
01_BaseMod/
02_Override/
```

## Blueprint ModActor spawning

For each mounted container, the loader automatically spawns:
```
/Game/Mods/<ContainerName>/ModActor.ModActor_C
```

Example: `ExampleMod.pak` spawns `/Game/Mods/ExampleMod/ModActor.ModActor_C`

### Supported custom events
- `HbkPrintToModLoader` - Print to console
- `HbkConstructPersistentObject` - Create persistent objects

## Troubleshooting

**Mod doesn't load:**
- Check UE4SS console for errors
- Verify mod is cooked for UE 4.27 with IoStore enabled
- Ensure container filename matches the expected ModActor path
- Confirm the mod is not in the `disabled/` folder

**ModActor doesn't spawn:**
- Blueprint class must exist at `/Game/Mods/<ContainerName>/ModActor`
- Class name must be `ModActor_C`
- Check UE4SS Live View to verify assets loaded

## Platform support

Works on:
- ✅ Steam
- ✅ Epic Games Store
- ✅ Xbox (Microsoft Store)

## Disclaimer

This mod hooks engine functions and patches memory. **Use at your own risk.**  
Intended for modding and research purposes only.
