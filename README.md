# IoStoreLoaderMod (UE4SS)

**IoStoreLoaderMod** is a native **UE4SS C++ mod** that enables loading **IoStore-based assets** (`.utoc` / `.ucas`) at runtime for **Hi-Fi-RUSH**, including mods packaged as:

* IoStore-only containers (`.utoc` + `.ucas*`)
* Hybrid containers (`.pak` + `.utoc` + `.ucas*`)

The mod integrates directly with Unreal's **FIoDispatcher** and **FPakPlatformFile** systems, allowing custom asset containers to be mounted with deterministic priority and override behavior.

---

## Features

* ✅ Runtime mounting of **IoStore containers** (`.utoc` / `.ucas`)
* ✅ Runtime mounting of **Pak containers**, including automatic IoStore environment mounting
* ✅ Priority-based mount order (override control)
* ✅ Supports mods with **multiple IoStore containers per folder**
* ❌ Actor spawning from mounted assets (**not implemented yet**)

---

## Requirements

* **UE4SS 3.0.1** (C++ mods enabled)
* Windows x64
* Mods must be **properly cooked** for the Hi-FI RUSH (i.e. UE 4.27, IoStore enabled)

---

## Mod Folder Structure

Example layout:

```
UE4SS/
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

---

## Configuration: `load_order.txt`

Defines which containers to load and in what order.

### Format

```
# priority   relative_path
200 MyWidgetMod
100 Paks/AnotherCoolMod
```

### Rules

* **priority**
  Higher numbers override lower ones
  (internally combined with a base offset i.e. 200)

* **relative_path**
  Path relative to the mod root
  (`Mods/IoStoreLoaderMod/`)

### How paths are resolved

Given:

```
Paks/ExampleMod
```

If the folder contains:

```
ExampleMod.utoc
ExampleMod.ucas
```

The loader mounts:

```
Mods/IoStoreLoaderMod/Paks/ExampleMod/ExampleMod
```

If a folder contains multiple IoStore pairs:

```
Mod1.utoc / Mod1.ucas
Mod2.utoc / Mod2.ucas
```

Each base is mounted independently.

---

## Mounting Behavior

The mod uses a **best-available strategy**:

1. **If `.pak` exists**
   * Calls `FPakPlatformFile::Mount`
   * Automatically mounts the associated IoStore environment
   * Ensures proper asset registry + override behavior

2. **If `.pak` does NOT exist**
   * Calls `FIoDispatcher::Mount` directly
   * Requires `.utoc` + at least one `.ucas*`

---

## Troubleshooting

### Mod does not mount

* Check UE4SS logs
* Verify `load_order.txt` path is correct
* Ensure the mod is cooked for the **same engine version (4.27)** with **UseIoStore** enabled

---

## Disclaimer

This mod performs **runtime hooks and memory patching**.
It is intended for **modding and research purposes only**.

Use at your own risk.
