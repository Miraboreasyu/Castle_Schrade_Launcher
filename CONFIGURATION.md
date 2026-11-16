# 虚黑城启动器 / Castle Schrade Launcher — INI 配置说明

## 中文

### 配置文件位置

编辑游戏目录下的 `nativePC/plugins/auto_quest_start.ini`，与 mod 的 DLL 放在同一个目录。虽然 mod 名称为“虚黑城启动器”，配置文件仍必须保留原名 `auto_quest_start.ini`。

请先退出游戏，用记事本打开文件，修改后保存，再重新启动游戏。配置不会在游戏运行中自动刷新。

### 默认配置

```ini
[auto_quest_start]
enabled=1
save_slot=0
quest_id=66845
```

请保留 `[auto_quest_start]` 分节名称和各配置项的英文拼写，只修改等号右侧的数字。以 `;` 开头的行是说明注释，不影响设置。建议用 UTF-8 保存，避免误存为 `.ini.txt`。

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

请在单独提供的 `quest_ids.csv` 中查找任务名称，并把对应的“任务ID”填入此处。任务表仅供查询，不需要放进插件目录。

例如：使用第二个存档，启动后进入黑龙任务：

```ini
[auto_quest_start]
enabled=1
save_slot=1
quest_id=66845
```

请选择该角色能够正常承接的任务。任务 ID 数值通过检查不代表任务一定存在或角色已经满足接取条件；不要随意填写数字。

### 集会与 IN-Q

集会设置沿用该存档中保存的设置，包括隐私偏好；请在游戏内修改并正常保存，无需在此 INI 中添加集会字段。

IN-Q 使用它自己的配置。本 mod 的公开配置只有以上三个选项，无需额外添加 IN-Q 相关配置项。

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
save_slot=0
quest_id=66845
```

Keep the section name `[auto_quest_start]` and the setting names unchanged. Edit only the numbers after `=`. Lines beginning with `;` are comments. Save as UTF-8 and make sure the filename does not end in `.ini.txt`.

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

Look up your quest in the separately provided `quest_ids.csv` and copy its ID into this setting. The CSV is a reference list and does not need to be installed in the plugins folder.

Example: use the second save slot and enter Fade to Black:

```ini
[auto_quest_start]
enabled=1
save_slot=1
quest_id=66845
```

Choose a quest that your character can normally accept. A numeric ID passing validation does not guarantee that the quest exists or that the character meets its requirements.

### Session settings and IN-Q

Gathering-session settings, including privacy preferences, are inherited from the selected save. Change and save those settings in the game; do not add session fields to this INI.

IN-Q keeps its own configuration. No additional IN-Q settings are required in this mod's INI. The public configuration consists of the three options above.

### Troubleshooting

If a change has no effect, restart the game, verify that you edited the installed configuration rather than a downloaded copy, and check that the filename is not `auto_quest_start.ini.txt`.

To temporarily restore normal startup, set `enabled=0`, save, and restart. Do not delete the configuration to disable the mod: the default master-switch value is enabled.
