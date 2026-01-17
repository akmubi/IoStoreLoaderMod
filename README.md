# IoStoreLoaderMod (UE4SS)

**IoStoreLoaderMod** is a native **UE4SS C++ mod** that enables loading **IoStore-based assets** (`.utoc` / `.ucas`) at runtime for **Hi-Fi-RUSH**, including mods packaged as:

* IoStore-only containers (`.utoc` + `.ucas*`)
* Hybrid containers (`.pak` + `.utoc` + `.ucas*`)

The mod integrates directly with Unreal's **FIoDispatcher** and **FPakPlatformFile** systems, allowing custom asset containers to be mounted with deterministic priority and override behavior.

## Features

* ✅ Runtime mounting of **IoStore containers** (`.utoc` / `.ucas`)
* ✅ Runtime mounting of **Pak containers**, including automatic IoStore environment mounting
* ✅ Priority-based mount order (override control)
* ✅ Supports mods with **multiple IoStore containers per folder**
* ❌ Actor spawning from mounted assets (**not implemented yet**)

## Requirements

* **UE4SS 3.0.1** (C++ mods enabled)
* Mods must be **properly cooked** for the Hi-FI RUSH (i.e. UE 4.27, IoStore enabled)

## Installation
### 1) Install UE4SS
This mod requires **UE4SS version 3.0.1** (⚠ other UE4SS versions are not supported and may not work correctly).
* Download UE4SS v3.0.1 from the official UE4SS releases
* Install UE4SS into the game directory (`C:\Program Files (x86)\Steam\steamapps\common\Hi-Fi RUSH\Hibiki\Binaries\Win64`)
* Make sure C++ mods are enabled and UE4SS is working correctly before proceeding
* ❗ Add Hi-Fi RUSH support (if you haven't done it already) by copying `UE4SS_Signatures` and `UE4SS-settings.ini` into the game directory.

### 2) Install IoStoreLoaderMod
1) Copy the mod folder into:
```
Mods/IoStoreLoaderMod/
```

2) Ensure the following structure exists:
```
Mods/
  IoStoreLoaderMod/
    dlls/
      main.dll
    load_order.txt
```
3) Enable the mod in `Mods/mods.txt` by adding the following line **at the very top of the file**:
```
IoStoreLoaderMod : 1
```

Make sure the name **exactly matches** the mod folder name.

## Mod Folder Structure

Example layout:

```
Mods/
  IoStoreLoaderMod/
    dlls/
      main.dll
    MyAwesomeMod/
      MyAwesomeMod.pak
      MyAwesomeMod.utoc
      MyAwesomeMod.ucas
    load_order.txt
```

### Notes

* `.pak` is optional but **recommended** when available
* `.ucas1`, `.ucas2`, etc. are supported automatically
* The mod discovers all valid IoStore bases inside the specified folders

## Configuration: `load_order.txt`

Defines which containers to load and in what order.

### Format

```
# priority   relative_path
200 MyWidgetMod
100 Paks/AnotherCoolMod
```

### Rules
* [**priority**]
  Higher numbers override lower ones
  (internally combined with a base offset i.e. 200)
* [**relative_path**]
  Path relative to the mod root
  (`Mods/IoStoreLoaderMod/`)

## Mounting Behavior
The mod uses a **best-available strategy**:
1. **If `.pak` exists**
   * Calls `FPakPlatformFile::Mount`
   * Automatically mounts the associated IoStore environment
   * Ensures proper asset registry + override behavior
2. **If `.pak` does NOT exist**
   * Calls `FIoDispatcher::Mount` directly
   * Requires `.utoc` + at least one `.ucas*`

## Troubleshooting

### Mod does not mount

* Check UE4SS logs
* Verify `load_order.txt` path is correct
* Ensure the mod is cooked for the **same engine version (4.27)** with **UseIoStore** enabled

## Disclaimer

This mod performs **runtime hooks and memory patching**.
It is intended for **modding and research purposes only**.

Use at your own risk.
