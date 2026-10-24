# MantellaQuestChecks

An SKSE plugin for [Mantella](https://github.com/art-from-the-machine/Mantella). It answers the checks behind Mantella's quest tags: has this quest reached this step, is this NPC dead, and so on.

Mantella reads the `code_N` conditions of bio template tags and turns them into a list of checks. Mantella-Spell hands that list to this plugin when a save loads, and asks for the answers at the start of every conversation and every time the player speaks. The plugin answers the whole list in one go, in about one frame, using the game's own condition functions. All the logic (comparing, combining, deciding what NPCs are told) is in Mantella.

One DLL works with Skyrim SE, AE and VR. It needs SKSE and the Address Library for SKSE Plugins.

## Papyrus

```papyrus
Scriptname MantellaQuestChecks Hidden

Bool Function SetCheckList(String version, String[] functions, String[] subjects, String[] arguments) Global Native
String Function GetListVersion() Global Native
Float[] Function Evaluate() Global Native
```

`Evaluate` returns one number per check. Two reserved values mark a check whose subject doesn't exist in this game (`-9999901`) or isn't loaded right now, such as an NPC far away (`-9999902`).

## Building

The DLL is built by GitHub Actions on every push (`.github/workflows/build.yml`); download it from the run's Artifacts. To build locally you need VS 2022 with the C++ workload and [xmake](https://xmake.io) 3.0+:

```bat
git clone --recurse-submodules <this repository>
xmake config --mode=releasedbg
xmake build
```

The output is in `build/windows/x64/releasedbg/`.

## License

The source code in this repository is MIT licensed (see `LICENSE`). The built DLL statically links [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR) (`lib/commonlibsse-ng`), which is GPL-3.0 with a modding exception, so the DLL is distributed under those terms.

Started from the [commonlibsse-ng-template](https://github.com/libxse/commonlibsse-ng-template).
