# UnityBridge

Windows Unity games built with IL2CPP, on an iPhone: the **Unity engine runs natively** (Unity's own ARM64 Android
player, loaded through a Husk-style Android translation layer), and **only the game's code** (`GameAssembly.dll`,
x86-64) is translated to ARM64 at run time.

Research project, nothing runs yet. Status, findings and next steps: [docs/STATUS.md](docs/STATUS.md).

## Idea

```
┌──────────────────────────────────────────────────────────────┐
│ libunity.so (ARM64, Android, IL2CPP variant, same version)   │  native: rendering, physics, audio
│   └─ loaded by a Husk-style loader (bionic → Darwin, JNI HLE)│
├──────────────── il2cpp_* API (228 functions) ────────────────┤
│ libil2cpp.so stand-in: forwards each il2cpp_* call           │  ARM64 ↔ x64 thunks
├──────────────────────────────────────────────────────────────┤
│ GameAssembly.dll (x86-64 PE) under FEXCore, in process       │  translated: game code only
│   └─ mini Win32 (≈250 functions: kernel32, baselib, …)       │
└──────────────────────────────────────────────────────────────┘
```

Compared with running the whole Windows game under Wine (as [Madeira](https://github.com/willfaust/Madeira) does),
the engine is not translated and no Windows is needed. The cost: the game's D3D11 shaders must be converted, and the
Android player of the game's exact Unity version is required.

## Rules

- No game files, saves, screenshots or Unity player binaries in this repository, its releases or CI artifacts.
  Unity's player is proprietary; users supply it themselves.
- Game code runs only on the device, inside the engine, never on a build machine.

## License

GPL-3.0-or-later (see `LICENSE`). Parts are expected to come from
[Husk](https://github.com/Leviidev/Husk) (GPL-2.0-or-later) and from FEX
([upstream](https://github.com/FEX-Emu/FEX) MIT; Madeira's fork changes GPL-3.0-or-later).
