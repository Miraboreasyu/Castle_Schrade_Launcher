# 启动、区域预取与 DX11/DX12 图形底层加速记录 (v1.3.0)

2026-11-19，MHW:I 15.23（Steam build 15539686）。在 v1.2.0 基础上扩展全链路启动与进入据点/任务剖析、通用预取、存档解包优化，以及实验性 DX11/DX12 图形底层加速。

## 1. 通用启动与进入区域加速

- **`sMhMain::ctor` 字形预热省略与同语种重复刷新跳过**（`src/startup_font.h`）：
  - 拦截构造期 `0x141AEE62D -> 0x141AD9800` 的首次字体刷新，与 `aLogo` 阶段共用按需字形缓存；若 `aLogo 0x16` 目标语言与构造期已绑定语言相同，直接省略重复缓存清空与重绑。
- **三槽存档并行解密与无脏字节重写跳过**（`src/startup_save_fast.h`）：
  - `0x141B8E2A0`：使用 Windows ThreadPool 并行执行 3 个存档槽的 Blowfish 解密（`0x141B8D620`）与校验（`0x141B8E9D0`），主线程顺序执行 `MtMemoryStream`/`MtByteReader` 解包（`~150 ms` 解密 + `~7 ms` 解包）。
  - `aLogo` 初始读档启用原生静默标志 `op1 + 0xC = 1`；`aTitle` 内容检查（`0x14029A0AB -> 0x14029A1A0`）在 DLC 与领取计数无变化且 `sUserData` 全量摘要一致时，按原生分支跳过冗余 11.28 MB 重写。
- **本地化常驻与通用 `aPlayable` 预取重叠**（`src/startup_prefetch.h`）：
  - 在 `aLogo` 空闲窗口提前提交 212 项本地化常驻表（`0x1450111C0`）与 Group 0 GUI；
  - 在 `aTitle 0x13` 等待建房期间单次触发 `sQuest::LoadAllQuests`（`0x141B5AB10` `0 -> 1`）并提前提交 Group 1 HUD 与通用 `aPlayable` 资源（不预测具体地图/集会）。
  - **关键修复**：`sTicketManager::addFolder`（`0x141B808D0`）第 6 参数为 8 字节过滤回调指针 `uintptr_t`；若误声明为 `uint8_t` 会保留栈高 7 字节残值并误调用 `0x141B80800` 覆写 `0x1451D0868`，导致退回标题或切图时 `sCollision` 清理崩溃。已修正并通过页面只读断点实机验证。

## 2. 实验性 DX11 启动加速 (`dx11_fast`)

- 配置项 `dx11_fast=1`（设为 `0` 时完全不安装以下 6 个 hook）：
  - **跳过无效 PSO 缓存加载**：`0x141AED52D -> 0x1423AD040` 在 DX11 下直接跳过 `PSOCache` 与 `PSOCacheBackground` 同步读取。
  - **延迟 + 单后台线程编译着色器**：拦截 `0x1423D7655 -> 0x1423D4B90`，因 `FUN_14263EDA0` 会在 `FUN_1423D7600` 末尾释放非 VS 字节码，先将 8,912 个有效 DX11 着色器字节码快照至临时缓冲区（跳过 1,513 个 `__d3d12_binary`），由 1 个后台线程在 `SRWLOCK` 保护下编译（约 `2.2s` 完成并立即释放快照内存）；`SetShaders`（`0x14259A030`）按需优先编译当前绘制所需着色器，`0x1423D4DD0` 负责析构同步。
  - **按需创建 `ID3D11Predicate` 查询池**：`initForD3D11`（`0x1423A47F1`）清零 32,768 个槽位并跳过启动期同步创建，改为在 `0x1423A2B29` 首次使用时无锁按需创建。

## 3. 实验性 DX12 加载加速 (`dx12_fast`)

- 实机诊断确认 DX12 在标题后进入据点比 DX11 慢约 `1.4s` 的主要原因是 `CreateCommittedResource` 逐个向 WDDM 申请数千个默认堆纹理与缓冲区（单次约 `0.46ms`）。
- 配置项 `dx12_fast=1`（设为 `0` 时完全不安装以下 6 个 hook）：
  - **纹理 Placed Heap 池**（`D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES`，64 MB 块）：拦截 `0x1424E8265`（创建）与 `0x1424E9286`（`~Texture12` 释放），使用 `param_1[5]` bit-63 标记句柄自动绕过逐资源 `mpResidencyManager` 开销，释放块经 8 帧 GPU 隔离队列后合并回收。
  - **缓冲区 Placed Heap 池**（`D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS`，64 MB 块）：拦截 IB `0x1424E6722`、VB `0x1424E89A2`、SB `0x1424E6F9E` 与基类释放 `0x14255397F`；保持 `param_1[5] == 0` 以兼容原生 SRV/UAV/上传路径，清零 `[bufObj + 0x0C]` 跳过逐资源驻留调用，并通过映射表与 8 帧隔离队列回收。

## 4. 实机基准中位数（Ryzen 7 9800X3D + Radeon + NVMe）

### 月辰集会区域 (`0x132`, `title_only_hub=1`)

| 指标 | DX11 优化前 | DX11 `dx11_fast=1` | DX12 优化前 | DX12 `dx12_fast=1` |
|---|---:|---:|---:|---:|
| `preLogo` | `8,328 ms` | `2,640 ms` | `2,797 ms` | `3,110 ms` |
| `ready -> 0x11` | `13,750 ms` | `7,515 ms` | `7,781 ms` | `7,469 ms` |
| `0x11 -> stage-init` | `3,703 ms` | `3,563 ms` | `4,953 ms` | `3,875 ms` (min `3,656 ms`) |
| `ready -> stage-init` | `18,360 ms` | `11,078 ms` | `12,734 ms` | `11,344 ms` (min `11,094 ms`) |
| Steam 启动到画面淡入完成 | `25.61 s` | `17.95 s` | `19.71 s` | `18.43 s` |

### 虚黑城任务 (`66845` / `0x1A1`, 含未修改的原生 `2.000s` 同步等待)

| 指标 | DX11 `dx11_fast=1` | DX12 优化前 | DX12 `dx12_fast=1` |
|---|---:|---:|---:|
| `ready -> stage-init` | `13,977 ms` (min `13,500`) | `14,610 ms` | `13,336 ms` (min `12,625`) |
| Steam 启动到画面淡入完成 | `21.04 s` | `21.78 s` | `20.27 s` (min `19.40 s`) |

用户实机验证 DX12 与 DX11 进入游戏、切图及返回标题均正常。
