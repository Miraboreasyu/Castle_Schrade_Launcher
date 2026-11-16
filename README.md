# 虚黑城启动器 / Castle Schrade Launcher

Skip the intro. Start the hunt.

A Monster Hunter: World — Iceborne **15.23 / Steam build 15539686 (Windows x64)** plugin that skips opening splash screens, selects a configured save slot, and enters a chosen quest. Gathering-session settings are inherited from the selected save, including privacy preferences. Compatible with IN-Q; no modified IN-Q or bridge is required.

## Installation

Install Stracker's Loader with support for `nativePC/plugins` first (not included). Close the game and copy the package's `nativePC` folder into the game directory. Edit `nativePC/plugins/auto_quest_start.ini`, then start the game.

```ini
[auto_quest_start]
enabled=1
save_slot=0
quest_id=66845
```

- `enabled`: `1` enables the mod; `0` disables all hooks, memory patches and splash skipping. Restart after changes.
- `save_slot`: `0`, `1`, or `2` selects the first, second, or third slot. Empty slots do not fall back to another character.
- `quest_id`: numeric quest ID; `66845` is Fade to Black. See [quest_ids.csv](quest_ids.csv). Choose a quest your character can normally accept.

**Do not rename the DLL or INI.** The public name is Castle Schrade Launcher; the installed files retain their original names. Full bilingual configuration instructions: [CONFIGURATION.md](CONFIGURATION.md).

IN-Q is independently configured and optional. This plugin does not customize equipment, items or meals. Session settings are changed and saved inside the game, not in this INI. To uninstall, remove the two installed mod files.

## Build

Requires Visual Studio 2022 C++ Build Tools / Windows SDK (x64) and Python 3.9+ for packaging and source checks.

```bat
build.bat
python tests\verify_source.py
python export_release.py
```

The build script detects Visual Studio via `vswhere`, or uses an existing developer command prompt. No Ghidra installation, game binary, external checkout or modified Better-IN-Q source is required to build. Packaging writes `dist/Castle_Schrade_Launcher-15.23.zip`: standard `nativePC/plugins` installation layout plus root README (configuration guide) and quest list. License notices are appended to the packaged README.

## Scope and compatibility

The plugin uses version-specific in-memory patches and native game initialization, save selection and session creation. It does not patch the executable on disk or directly edit the save file. Normal game activity can still save your progress. Keep save backups as with any mod.

Only the configured startup path is supported. This is not a generic task launcher UI or a promise that every quest ID is valid. Other game versions or mods changing the same sites may be incompatible. Logs are written beside the DLL as `auto_quest_start.log`; disabled mode does not write logs.

The pre-cleanup startup, splash skipping, saved-session settings and IN-Q compatibility were confirmed in-game by the author. The source-cleanup revision is build- and source-checked, but requires a fresh in-game regression test before replacing that tested release.

## 中文说明

跳过开场 Splash Screen，自动选择指定存档并直接进入任务；集会沿用该存档保存的设置，兼容 IN-Q。

配置只有 `enabled`、`save_slot`、`quest_id` 三项。存档编号从 **0** 开始；修改配置后重启游戏。停用请设 `enabled=0`，不要通过删除 INI 停用（缺省为启用）。详细说明见 [CONFIGURATION.md](CONFIGURATION.md)。

清理版只保留当前已用的启动路径，不含旧装备、道具、猫饭自定义、BIQ bridge 或其他实验接口。旧版本已实测正常；本次清理版仍需重新进行游戏内回归验证。

## License and credits

MIT. The hook implementation and task reference table derive from [Better-IN-Q](https://github.com/Miraboreasyu/Better-IN-Q). Original copyright and license notices are retained in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
