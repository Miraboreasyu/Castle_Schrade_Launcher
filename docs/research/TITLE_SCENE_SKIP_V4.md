# 标题场景请求裁剪实验 v4

2026-11-16，MHW:I 15.23 / Steam build 15539686。

## v3实机结果

本次日志已证实提前加载真正执行：727351453提交，29个GUI路径加save_pos、title_envb共31项；727351640观测全部ready，跨度187ms；比首次标题PREPARE早1735ms完成。

原生标题ticket第一次采样是109/120项ready，随后116/120，最后完成。预加载请求按原生接口回收，未出现所有权变化。本次标题批次从727353390到727353828约438ms，整个PREPARE阶段562ms。

早前相同st101场景的未预加载诊断样本，标题批次约547ms，整个PREPARE阶段687ms；对应差值109ms、125ms。样本缓存、调度等条件不受控，不能把全部差值归因于预加载，也不能把109项初始ready全算作31项请求直接带来的独占收益。

本次ready→任务入口10406ms，ready→选择存档8234ms，建房接受→任务入口2125ms。仍未覆盖后续任务地图加载到可操作的区间。

## 能否直接不加载标题场景

进一步通过项目MCP查询14025DA80、14025EF60及标题状态机，并用Ghidra只读反编译和代码引用核对：

- 14025DA80 PREPARE state0根据进度随机选择一个标题场景，写title+390，再在14025DBDE调用141B80820，把对应rScheduler路径加入主ticket。
- 14025EF60才同步获取该资源，创建并绑定uMhScheduler，保存到title+398并启动后续标题背景流程。它的直接代码调用来自14025E0D0的state5。
- 目前已经实测成功的auto_quest_start路径是0→3→4→F→13/18→11；state4的原生内容检查完成后，mod选择存档并直接进入F，没有进入5。因此这个成功快进路径虽然预先加载了标题场景，却没有执行该场景调度器的创建。
- 普通标题或启动失败后若进入state5，14025EF60仍保留LoadResource(...,flags=1)同步请求；后续正常返回标题的PREPARE也仍可正常请求场景。此回退路径本轮未实机验证。

因此可以做一个局限于首次自动启动的“省略场景请求”实验，无需先固定或提前加载某个基础场景，也无需把整个标题初始化替换掉。

不能从首个pending路径断言438ms全是这个资源自身的I/O；裁剪目标是整批中对应的场景及其依赖加载部分。

## 实现范围

新增src/startup_scene_skip.h，在14025DBDE这条CALL设置上下文调用门。仅当：mod启用、尚未进入首次aTitle EXECUTE、配置存档已占用、RDI所属title处于PREPARE state0、ticket号与owner一致、DTI是rScheduler、路径是当前选择表中的stage/title路径时，省略该次加入资源请求。

没有修改资源ready位、ticket状态、场景索引或随机数结果；没有删除磁盘资源。其他情况通过相同原生141B80820接口转发原来的四个参数和返回值。该CALL之后不使用其RAX返回值，已从汇编核对。

覆盖字节E8 3D 2C 92 01，经Ghidra核对确实指向141B80820。原CALL是相对寻址，不能直接复制到远处跳板；上下文回调负责调用转发，displaced区域使用五个NOP。新位置若不匹配或安装失败，保留v3功能并记录gate unavailable。

保留v3已验证的31项预加载和此前计时优化；只增加一个场景请求调用门。正常返回标题时g_startupLogoDone已为true，调用门会转发原生请求。配置存档未占用时也转发。

日志：

- `title-scene: request gate installed`：调用门安装成功。
- `title-scene: omitted startup scene request index=...`：场景请求确实从本次ticket中省略。
- `title-scene: quest-entry scheduler=... fallback=0`：到任务入口时的调度器指针及是否进入过state5。
- 若出现state5，记录fallback，正常执行原生同步加载/创建函数。

## 验证与交付

MSVC构建、verify_source.py、git diff --check通过。实际实现的夹具测试覆盖启用/禁用、首次之后、未占用存档、错误ticket/DTI/路径/index、普通请求的参数与返回值转发、title与存档数据保持原样、fallback日志。

额外用本项目真实hook.cpp生成并执行测试进程中的x64跳板：省略时原CALL执行零次，转发时恰好执行一次，返回值正确。没有向游戏进程写入测试代码。

尚未实机验证v4。包Castle_Schrade_Launcher-15.23-no-title-scene-v4.zip仅含nativePC/plugins/auto_quest_start.dll，关闭游戏后覆盖，保留INI。回退用已成功提交预加载的title-prefetch-v3.zip。

下一轮验证场景省略日志、标题ticket条目数（原样本120项，省略一项后通常应为119项）、标题批次等待、到任务入口时scheduler/fallback，以及地图可操作的实际总时间。

原始核对记录：本聊天work/title-scene-choice.log、title-scene-consumers.log、title-no-scene-audit.log、title-scene-gate-check.log。
