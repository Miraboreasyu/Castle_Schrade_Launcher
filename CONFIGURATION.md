# 虚黑城启动器 / Castle Schrade Launcher — INI 配置说明

## 中文

### 配置文件位置

编辑游戏目录下的 `nativePC/plugins/auto_quest_start.ini`，与 mod 的 DLL 放在同一个目录。虽然 mod 名称为“虚黑城启动器”，配置文件仍必须保留原名 `auto_quest_start.ini`。

请先退出游戏，用记事本打开文件，修改后保存，再重新启动游戏。配置不会在游戏运行中自动刷新。

### 默认配置

```ini
[auto_quest_start]
enabled=1
title_screen_only=0
title_only_hub=1
rehook_on_return=0
save_slot=0
quest_id=66845
```

请保留 `[auto_quest_start]` 分节名称和各配置项的英文拼写，只修改等号右侧的数字。以 `;` 开头的行是说明注释，不影响设置。建议用 UTF-8 保存，避免误存为 `.ini.txt`。

### 标题、集会与任务模式

`title_only_hub=1` 会自动选择 `save_slot`，按存档设置正常创建集会，并使用原生据点/集会入口。保留等待缩短、资源提前加载以及场景/制作人员表裁剪；不应用任务重定向，`quest_id` 和 `rehook_on_return` 不控制任务跳转。

自动建房时也会应用该存档的开始区域选项；选择沿用上次区域时交给原生存档区域回退。剧情强制入口和区域解锁检查仍由游戏处理。日志 `hub-area` 记录存档选项、临时目的地与上次区域，该区域修复已通过用户游戏内验证。

自动集会和自动任务模式均启用首次常驻构建预算优化及字形按需生成，无需新增 INI 项。`title_screen_only=1` 不启用这两项优化。包内默认仍为自动集会；要复现自动黑龙任务测试，设置 `title_only_hub=0`。

优先级为 `title_screen_only` → `title_only_hub` → 自动任务模式。两者都为1时停在标题；两者都为0时自动进配置任务。旧 `title_only` 仍作为 `title_screen_only` 缺省值兼容，显式的新键优先。

自动进集会示例：

```ini
title_screen_only=0
title_only_hub=1
```

下表的 `rehook_on_return` 行为适用于 `title_only_hub=0`：

|title_screen_only|rehook_on_return|首次启动|返回标题后手动选档|
|---:|---:|---|---|
|0|1|自动选择配置存档并进入配置任务|使用玩家选中的存档进入配置任务|
|0|0|自动选择配置存档并进入配置任务|恢复原生流程，正常进入该存档的据点/集会区域|
|1|任意|保留完整原生加载与手动选档|原生存档入口，不自动进入任务|

`title_screen_only=1` 优先级更高，只缩短已识别的显示/过渡等待和存档成功后的最短等待，并沿用现有原生 Logo 完成流程跳过开场展示。自动选档、自动建房、任务重定向、资源提前加载和场景/制作人员表裁剪均不启用。真实加载与 I/O 完成条件不会被跳过。此时 `save_slot`、`quest_id` 和 `rehook_on_return` 不替玩家作选择。

`rehook_on_return=0` 不影响首次自动进任务；在下一次进入标题准备阶段恢复原生存档入口。后续由玩家选档，按该存档进度及原生据点/集会逻辑进入游戏，新角色教程等原生规则仍保留。

`rehook_on_return=1` 保留旧版行为：返回标题后不自动选档，但手动选择存档后仍进入 `quest_id` 对应任务。

随包配置默认 `title_screen_only=0`、`title_only_hub=1`，`rehook_on_return=0`；如仍有旧 `title_only`，则用于前者的缺省值。只首次自动进任务可在两个模式均为0时设置 `rehook_on_return=0`；只想快速到标题后手动开始请设 `title_screen_only=1`。修改后重启游戏。

模式开关和存档开始区域修复已通过用户游戏内验证；构建与模式夹具检查补充覆盖配置和补丁边界。

未填写模式项时，保留兼容旧版的任务模式缺省值；删除 INI 不等于停用。

### enabled：总开关

- `enabled=1`：启用 mod，包括跳过开场 Splash Screen、自动选择存档并进入指定任务。
- `enabled=0`：停用整个 mod，不执行上述功能。此时也不会跳过开场。

修改总开关后同样需要重启游戏。

### save_slot：选择哪个存档

- `save_slot=0`：第一个存档位。
- `save_slot=1`：第二个存档位。
- `save_slot=2`：第三个存档位。

这里从 **0** 开始编号，不是从 1 开始。只会使用指定槽位；如果指定槽位为空，不会自动切换到其他存档。请填写 0、1 或 2，其他数值会回退到 0。

### quest_id：启动后进入哪个任务

填写任务的数字 ID，而不是任务名称。默认 `quest_id=66845` 对应“传说中的黑龙”（Fade to Black）。

请在包内的 `quest_id.csv` 中查找任务名称，并把对应的“任务ID”填入此处。任务表仅供查询，不需要放进插件目录。

例如：使用第二个存档，启动后进入黑龙任务：

```ini
[auto_quest_start]
enabled=1
title_screen_only=0
title_only_hub=0
save_slot=1
quest_id=66845
```

请选择该角色能够正常承接的任务。任务 ID 数值通过检查不代表任务一定存在或角色已经满足接取条件；不要随意填写数字。

### 集会与 IN-Q

集会设置沿用该存档中保存的设置，包括隐私偏好；请在游戏内修改并正常保存，无需在此 INI 中添加集会字段。

IN-Q 使用它自己的配置。本 mod 的公开配置共六项，无需额外添加 IN-Q 相关配置项。

### 常见问题

**改了配置没有变化？** 确认已经退出并重新启动游戏，改的是游戏插件目录里的配置，而不是下载目录中的副本；确认文件名没有变成 `auto_quest_start.ini.txt`。

**选了第二个存档却不对？** 第二个存档对应 `save_slot=1`；第三个对应 `save_slot=2`。

**想临时恢复正常启动？** 将 `enabled` 改为 `0`，保存并重启游戏。不要删除配置文件来停用，因为缺省总开关是启用。

---

## English

### Configuration file

Edit `nativePC/plugins/auto_quest_start.ini` inside your game directory, next to the mod DLL. The mod is named **Castle Schrade Launcher**, but the configuration file must still be named `auto_quest_start.ini`.

Close the game, edit the file with a text editor, save it, and restart the game. Configuration changes are not applied live.

### Default configuration

```ini
[auto_quest_start]
enabled=1
title_screen_only=0
title_only_hub=1
rehook_on_return=0
save_slot=0
quest_id=66845
```

Keep the section name `[auto_quest_start]` and the setting names unchanged. Edit only the numbers after `=`. Lines beginning with `;` are comments. Save as UTF-8 and make sure the filename does not end in `.ini.txt`.

### New mode switches

The shipped defaults are `title_screen_only=0`, `title_only_hub=1`, and `rehook_on_return=0`. The old `title_only` key is a fallback for title_screen_only; an explicit new key wins.

- `title_screen_only=0, title_only_hub=1` automatically selects the configured save and creates its saved session, then uses native hub/progression entry. Prefetching and resource trimming stay enabled. Quest redirection is disabled, including on return to title. The screen-only mode has higher priority.

- `title_screen_only=1` shortens known fixed waits and retains the native Logo-completion skip. Native resource loading, manual save selection and native game entry are preserved. Automatic save/session actions, quest redirection, prefetching and resource trimming are disabled. This mode overrides `rehook_on_return`; it does not use save_slot or quest_id to choose for the player.
- `title_screen_only=0, title_only_hub=0, rehook_on_return=0` keeps the first automatic startup, then restores native save-entry instructions on the next title visit. Later manual save selections follow the selected character's normal hub/progression rules.
- `title_screen_only=0, title_only_hub=0, rehook_on_return=1` keeps the previous behavior. Later title visits allow manual save selection, then redirect that selected character to the configured quest.

Real asynchronous readiness and I/O completion remain native. Restart after edits. Existing custom configurations can add the two keys without replacing save_slot or quest_id. Mode switches and saved start-location routing have been verified in-game by the user; build and fixture checks cover configuration and patch boundaries.

### enabled — Master switch

- `1`: Enable the mod, including splash-screen skipping, automatic save selection, and automatic quest entry.
- `0`: Disable the entire mod, including splash-screen skipping.

Restart the game after changing this setting.

### save_slot — Save selection

- `0`: First save slot.
- `1`: Second save slot.
- `2`: Third save slot.

Slot numbering starts at **0**. The mod uses only the selected slot; an empty slot does not trigger automatic selection of another character. Use 0, 1, or 2; other values fall back to slot 0.

### quest_id — Quest selection

Enter the numeric quest ID, not its name. The default, `66845`, is **Fade to Black**.

Look up your quest in the included `quest_id.csv` and copy its ID into this setting. The CSV is a reference list and does not need to be installed in the plugins folder.

Example: use the second save slot and enter Fade to Black:

```ini
[auto_quest_start]
enabled=1
title_screen_only=0
title_only_hub=0
save_slot=1
quest_id=66845
```

Choose a quest that your character can normally accept. A numeric ID passing validation does not guarantee that the quest exists or that the character meets its requirements.

### Session settings and IN-Q

Gathering-session settings, including privacy preferences, are inherited from the selected save. Change and save those settings in the game; do not add session fields to this INI.

IN-Q keeps its own configuration. No additional IN-Q settings are required in this mod's INI. The public configuration consists of six options.

### Troubleshooting

If a change has no effect, restart the game, verify that you edited the installed configuration rather than a downloaded copy, and check that the filename is not `auto_quest_start.ini.txt`.

To temporarily restore normal startup, set `enabled=0`, save, and restart. Do not delete the configuration to disable the mod: the default master-switch value is enabled.
