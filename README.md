# 虚黑城启动器 / Castle Schrade Launcher

MHW: Iceborne **15.23 / Steam build 15539686，Windows x64** 启动加速插件。跳过开场展示和已识别的固定等待；支持停在标题、自动进入集会或自动进入指定任务。

## 安装与默认行为

先安装支持 `nativePC/plugins` 的 Stracker's Loader（不随包提供）。退出游戏，把压缩包内的 `nativePC` 文件夹复制到游戏根目录。

默认自动选择第一个存档，沿用该存档的集会设置与开始区域。修改 `nativePC/plugins/auto_quest_start.ini` 后需要重启游戏。

```ini
[auto_quest_start]
enabled=1
title_screen_only=0
title_only_hub=1
rehook_on_return=0
save_slot=0
quest_id=66845
dx11_fast=1
dx12_fast=1
```

|配置|作用|
|---|---|
|`enabled`|`0` 完全停用，不安装 hook 或修改游戏内存；`1` 启用。|
|`title_screen_only`|`1` 只加速到标题，完整保留资源加载与手动选档。优先级最高。|
|`title_only_hub`|`1` 自动选档建房，按存档的开始区域设置进入据点/集会；保留预加载和资源裁剪。|
|`rehook_on_return`|仅自动任务模式有效：`0` 返回标题后手动选档正常进入据点/集会；`1` 仍重定向到指定任务。返回标题后不会自动选档。|
|`save_slot`|`0 / 1 / 2` 对应三个存档槽；空槽不会改选其他角色。|
|`quest_id`|仅自动任务模式有效；`66845` 为传说中的黑龙。查阅包内 `quest_id.csv`。|
|`dx11_fast`|实验性 DX11 启动加速：`1` 启用后台异步 + 按需编译着色器与延迟创建遮挡查询池（需提前预加载 `dinput8.dll`，如安装了 SharpPluginLoader / `winmm.dll`）；`0` 不安装任何 DX11 加速 hook。|
|`dx12_fast`|实验性 DX12 加载加速：`1` 启用 64MB Placed Heap 纹理与缓冲区子分配器（仅装 Stracker's Loader 即可生效）；`0` 不安装任何 DX12 加速 hook。|

## 三种模式

|模式|title_screen_only|title_only_hub|首次启动|
|---|---:|---:|---|
|标题|1|任意|缩短等待、跳过开场展示，手动选档，完整加载|
|集会（默认）|0|1|自动选档、按存档设置建房和进入区域|
|任务|0|0|自动选档、建房并进入 `quest_id` 指定任务|

开始区域使用游戏内保存的选项；选择沿用上次区域时使用原生回退。剧情强制入口与区域解锁规则仍由游戏处理。集会隐私等设置在游戏内修改并保存。

真实资源加载和 I/O 就绪条件仍保留。自动模式提前请求部分启动资源，并裁剪启动标题场景与制作人员表资源；返回标题时恢复需要的原生资源路径。只到标题模式不启用预加载或裁剪。

自动模式还包含多项启动与进入区域加速：提高首次常驻索引构建预算；在 `sMhMain::ctor` 与 `aLogo` 阶段将批量字形预热改为原生按需生成并跳过同语种重复刷新；三槽存档 Blowfish 并行解密与标题无脏字节冗余重写跳过；在空闲窗口提前并行预取本地化常驻资源与通用 `aPlayable`（任务表与 HUD）资源。此外提供两项可通过 INI 独立开关的实验性图形底层加速：`dx12_fast`（64MB `ID3D12Heap` Placed Resource 纹理与缓冲区子分配器，仅装 Stracker's Loader 即可生效）与 `dx11_fast`（后台异步 + 按需编译着色器与按需创建 `ID3D11Predicate` 查询池；由于游戏在初始化 DX11 之后才动态加载 `dinput8.dll`，`dx11_fast` 需配合进程启动期提前预加载 `dinput8.dll` 的环境——例如安装了 SharpPluginLoader `winmm.dll` 并启用 `PreloadDLLs`——才能拦截启动期着色器构建，仅装 Stracker's Loader 时自动无害跳过）。在测试机样本中，完整插件包环境下进入月辰集会（`ready -> stage-init`）中位数：DX12 由 12.73s 降至 11.34s，DX11（含 SPL 预加载）由 18.36s 降至 11.08s；仅安装 Stracker's Loader + 本插件的纯净环境下 DX12 `ready -> stage-init` 为 8.88–9.62s（从 Steam 启动到月辰集会画面淡入完成约 14.76–15.74s）。这些是特定硬件样本对比，不是固定性能保证；若遇驱动兼容或显存异常，可将 `dx11_fast` 或 `dx12_fast` 设为 `0` 回退至原生图形路径。

不要重命名 DLL 或 INI；不要通过删除 INI 停用插件。日志位于 DLL 旁的 `auto_quest_start.log`，其中 `hub-area` 记录存档开始区域选项。卸载时删除两个插件文件。

## 验证与兼容性

启动加速、资源裁剪、模式开关与按存档开始区域进入集会已经用户游戏内验证。构建与模式夹具检查补充覆盖配置、补丁边界和返回标题恢复；不表示所有任务和剧情进度组合均已验证。请选用角色可以正常承接的任务。

使用特定版本的内存 hook，不修改磁盘上的游戏程序或直接编辑存档。其他游戏版本或修改相同位置的 mod 可能不兼容。兼容 SharpPluginLoader（自动进集会/任务模式下仍可正常初始化 D3D 面板并呼出 UI）。IN-Q 可选，独立配置；本插件不管理装备、道具或猫饭。

## English

Startup acceleration for MHW: Iceborne 15.23 (Steam build 15539686), Windows x64. Install Stracker's Loader first, then copy `nativePC` into the game directory. Restart after editing the INI.

The shipped configuration automatically selects save slot 0 and creates a session using that save's lobby settings and start-location preference. Native story and area-unlock checks remain in control.

- `title_screen_only=1`: shorten known waits and skip opening presentation, retain full loading and manual save selection. Overrides hub mode.
- `title_screen_only=0, title_only_hub=1`: automatic save/session startup into the saved start area, with resource prefetching and trimming.
- Both switches `0`: automatic entry into `quest_id`. Only in this mode does `rehook_on_return` control whether later manual save selections redirect to that quest.
- `enabled=0` disables the entire plugin. `save_slot` is zero-based; empty slots do not fall back to another character.

The release includes `quest_id.csv` as a reference; it does not need to be installed. Do not rename the DLL or INI. Compatible with SharpPluginLoader (its D3D UI overlay initializes normally even when skipping the Title Screen into Hub/Quest). IN-Q is optional and independently configured. Full configuration details are in [CONFIGURATION.md](CONFIGURATION.md).

Automatic modes also accelerate resident-index construction, defer exhaustive glyph prewarming in both `sMhMain::ctor` and `aLogo` (skipping duplicate same-language refreshes), parallelize 3-slot Blowfish save decryption while skipping redundant bit-identical title re-saves, and overlap localized-resident plus universal `aPlayable` (quest table + HUD) prefetches during idle startup windows. Two experimental graphics-API toggles are also included (`dx12_fast` for 64 MB `ID3D12Heap` placed-resource texture and buffer sub-allocators; `dx11_fast` for background/on-demand DX11 shader compilation and lazy `ID3D11Predicate` creation); set either to `0` to skip installing its hooks entirely. Note that MHW initializes DX11 *before* dynamically loading `dinput8.dll`: `dx12_fast` and all shared startup optimizations work with Stracker's Loader alone (`ready -> stage-init` 8.88–9.62s / 14.76–15.74s Steam-launch-to-Hub in clean-install DX12 samples; 11.34s with a full modpack), whereas `dx11_fast`'s pre-logo shader/predicate hooks only intercept early DX11 init when `dinput8.dll` is preloaded at process start (e.g., alongside SharpPluginLoader `winmm.dll` with `PreloadDLLs`, where `ready -> stage-init` dropped from 18.36s to 11.08s; otherwise it is a harmless no-op). Results vary by hardware, and all optimizations are disabled in title-screen-only mode.

## 构建与打包 / Build

Visual Studio 2022 C++ Build Tools / Windows SDK (x64), Python 3.9+:

```bat
build.bat
python tests\verify_source.py
python tests\verify_modes.py
python export_release.py
```

无需 Ghidra、游戏二进制或 IN-Q 源码。发布包包含 DLL、INI、README、配置说明、`quest_id.csv` 和许可证。仓库中的任务表源文件仍为 `quest_ids.csv`，打包时统一命名为 `quest_id.csv`。实现和调查记录见 [docs/research](docs/research)。

## License and credits

MIT. Hook implementation and quest reference derive from Better-IN-Q (Miraboreasyu/Better-IN-Q on GitHub). Copyright and license notices are retained in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
