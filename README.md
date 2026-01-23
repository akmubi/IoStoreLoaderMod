# IoStoreLoaderMod (UE4SS)

**IoStoreLoaderMod** is a native **UE4SS C++ mod** that enables loading **IoStore-based assets** (`.pak` / `.utoc` / `.ucas`) at runtime for **Hi-Fi-RUSH**, including mods packaged as:

* IoStore-only containers (`.utoc` + `.ucas*`)
* Pak-only containers (`.pak`)
* Full containers (`.pak` + `.utoc` + `.ucas*`)

It mounts containers once the game's normal pak mounting has run, then (optionally) spawns a **Blueprint mod actor** for each mounted container when the game's PlayerController begins play (`HbkPlayerControllerBP_C.ReceiveBeginPlay`).

## Features

* ✅ Runtime mounting of **IoStore containers** (`.utoc` / `.ucas`)
* ✅ Runtime mounting of **Pak containers**, including automatic IoStore environment mounting
* ✅ Priority-based mount order (override control)
* ✅ Supports mods with **multiple IoStore containers per folder**
* ✅ **Blueprint ModActor auto-spawn**

## Requirements

* **UE4SS 3.0.1** (C++ mods enabled)
* Mods must be **properly cooked** for the Hi-FI RUSH (i.e. UE 4.27, IoStore enabled)

## Installation
### 1) Install UE4SS
This mod requires **UE4SS version 3.0.1** (⚠ other UE4SS versions are not supported and may not work correctly).
* Download UE4SS v3.0.1 from the official UE4SS releases
* Install UE4SS into the game directory (`C:\Program Files (x86)\Steam\steamapps\common\Hi-Fi RUSH\Hibiki\Binaries\Win64`)
* Make sure UE4SS is working before continuing
* ❗ Add Hi-Fi RUSH support (if you haven't done it already) by copying `UE4SS_Signatures` and `UE4SS-settings.ini` into the game directory.

### 2) Install IoStoreLoaderMod
1) Copy the mod folder into:
```
Mods/IoStoreLoaderMod/
```

Required structure:
```
Mods/
  IoStoreLoaderMod/
    dlls/
      main.dll
```

3) Enable the mod in `Mods/mods.txt` (**put it at the very top**):
```
IoStoreLoaderMod : 1
```

Make sure the name **exactly matches** the mod folder name.

## Where to place your mods

Each mod is a **folder** inside `Mods/IoStoreLoaderMod/`:
```
Mods/
  IoStoreLoaderMod/
    dlls/
      main.dll
    FooMod/
      FooMod.pak
      FooMod.utoc
      FooMod.ucas
    BarMod/
      BarMod.pak
      BarMod.utoc
      BarMod.ucas
```

* The folder name is used only for sorting / grouping.
* What actually mounts are the container files inside the folder.

## Mount Order & Override Rules
### When does mounting happen?
IoStoreLoaderMod waits until the game calls Unreal's `MountAllPakFiles` once.  
After that first call completes, it mounts user mods **one time**.

### How are mod folders ordered?
1. It scans all subfolders in `Mods/IoStoreLoaderMod/` (excluding `dlls/`).
2. It sorts folders **alphabetically**.
3. Each folder is assigned a base mount order `order_base = 200 + folder_index`.

### How are files ordered inside a folder?
- If the folder contains **any `.pak` files**:
- All `.pak` files in that folder are mounted (sorted alphabetically by filename)
- Each one gets:
 ```
 order = order_base + pak_index
 ```
- Otherwise, if there are **no `.pak` files** but there are `.utoc` files:
- All `.utoc` files are mounted (sorted alphabetically by filename)
- Each one gets:
 ```
 order = order_base + utoc_index
 ```

### Who "wins" if two mods replace the same asset?
Higher mount order overrides lower mount order.

Practical rule:
- Folders later in the alphabet mount later (higher order) -> override earlier folders.
- Within the same folder, later file names override earlier file names.

Example:
- `A_Mod\aaa.pak` mounts before `A_Mod\zzz.pak`
- `Z_Mod\something.pak` mounts after both -> overrides both

---

## Blueprint Mod Spawning (ModActor)

IoStoreLoaderMod can auto-spawn a Blueprint entry actor for each mounted container.

### When does spawning happen?
Spawning is triggered when `HbkPlayerControllerBP_C.ReceiveBeginPlay` is called.

### What gets spawned?
For each mounted container, the loader queues a class path:
```
/Game/Mods/<BaseName>/ModActor.ModActor_C
```

Where `<BaseName>` is derived from the **container filename**, not the folder name:
- For `.pak` files:
  - `SomeName.pak` -> `<BaseName> = SomeName`
- For `.utoc` files:
  - Intended: `SomeName.utoc`-> `<BaseName> = SomeName`  

So if you ship `ExampleMod.pak`, the loader will try to spawn:
```
/Game/Mods/ExampleMod/ModActor.ModActor_C
```

## Troubleshooting

### Mod does not mount

* Check UE4SS logs
* Verify `load_order.txt` path is correct
* Ensure the mod is cooked for the **same engine version (4.27)** with **UseIoStore** enabled
* Confirm `<BaseName>` matches the container filename (e.g. `ExampleMod.pak` -> `ExampleMod`)
* Remember: Live View shows loaded objects; the class won't appear until it's actually loaded/spawned.

## Disclaimer

This mod performs **runtime hooks and memory patching**.
It is intended for **modding and research purposes only**.

Use at your own risk.
