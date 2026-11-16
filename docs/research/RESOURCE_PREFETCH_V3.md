# 预加载v2日志与GUI Hook兼容修正

2026-11-16，MHW:I 15.23 / Steam build 15539686。

## 本次日志结论

关键行：`title-prefetch skipped=code-mismatch rva=1AD8D20 actual=E9AE8148FE4889742418`。

141AD8D20原本应以`48 89 5C 24 10`开头，现在前五字节被E9相对跳转覆盖，目标为13FF60ED3。其后五字节仍与原版下一条保存RSI的指令相符。已确认入口被Hook；v2没有提交任何预加载ticket。v1笼统的precondition失败也可能来自这个同一检查，但旧日志不足以单独证明。

只读检查已安装插件发现GuildCardView.dll包含同一RVA，且其日志记录安装了资源生命周期Hook，属于候选来源。尝试读取运行中跳板时游戏进程已退出，未确认跳板处理函数归属，不把候选当作最终归属。没有修改、卸载或停用其他mod，也没有向游戏进程写入内存。

ready=726548937，选择存档=726558062，任务入口=726560328：总跨度11391ms，选择存档前9125ms，建房接受后2219ms。

|区间|本次|上一份v2前日志|
|---|---:|---:|
|ready→首次Logo PREPARE|3781ms|3609ms|
|常驻主ticket state2→state3|485ms|500ms|
|常驻初始化state4→state5|672ms|454ms|
|标题主ticket首次观察→PREPARE state2|1063ms|531ms|
|整个标题PREPARE→首次EXECUTE|1203ms|688ms|
|建房接受→任务入口|2219ms|2735ms|

本次原生随机标题场景是title_st108_02，上次为title_st102。标题批次等待变长与场景选择同时发生，但pending只报告首个未就绪项，不能把差值完全归因于单一资源。当前日志不能提供预加载收益的实测对照，因为两版预加载都没有实际提交。

## 常驻ticket计数核对

通过MCP查询141B7CD80、141B7C7D0、141B808D0、141B80E90，并用Ghidra只读反编译核对。普通ticket块以+C08空指针为结束；每块32项。除固定463项外，141B7CD80还枚举quest/bui并调用141B7C7D0加入动态remData资源。因此不能把固定数组463项视为整个批次总数。

本次1024/1024且partial=1代表已扫描32块，上限之外仍有块；不是总数，也不是观察到了1024项全部加载后仍无故等待。状态进入3才是原生整批完成信号。本轮保留采样边界，没有进一步扩大扫描成本。未发现支持直接删除这段真实完成等待的证据。

## v3实现

预加载不再调用141AD8D20。改为读取gui+15000处组5的已注册资源描述符表（每项24字节：unit DTI、路径指针、flags），直接通过仍经字节核对的141B80820把明确的rGUI路径加入自己的ticket。rGUI::DTI::Instance=1451C4608，已从MCP数据符号核对。

仅预加载具有终止符且长度有效的固定路径；flags bit3涉及系统选项变体的条目跳过。声音参数、变体路径以及其他mod追加行为继续在正常标题准备中执行。此预加载子集还包含原先的save_pos和title_envb。提交日志带gui-paths数量和mode=registered-list。

字节校验现在覆盖实际调用的四个原生ticket接口：分配、加入资源、提交、回收。141AD8D20已不在调用和校验列表中。没有调用它的未知跳板，没有还原或覆盖它的代码。正常标题仍走已有的完整加载流程，因此其他mod的Hook能在原来的调用时机执行。

保留v2的有界重试、详细失败原因、引用持有/原生回收、原生完整标题ticket就绪检查和有效的计时优化。没有新增hook位置。

## 验证与测试

MSVC构建、verify_source.py、git diff --check通过。实际实现的夹具测试覆盖：GUI入口被改成E9后预加载所需接口校验仍通过；注册表中的正常路径加入rGUI请求；变体、空指针、短路径、无终止符和异常计数被排除；描述符保持原样。此前原生ticket生命周期、相邻VirtualQuery区域、失败原因及profiler测试继续通过。

尚未实机验证v3是否提交成功、缓存复用比例和实际收益。这个版本是对已确认的Hook兼容问题的修正，不能提前宣称加速。

交付Castle_Schrade_Launcher-15.23-title-prefetch-v3.zip，仅含nativePC/plugins/auto_quest_start.dll。关闭游戏后覆盖，保留INI。下一次重点看title-prefetch submitted、gui-paths、提前ticket是否在标题PREPARE前就绪，以及title-assets最初的ready数量；同时比较ready→选择存档时间及地图可操作总时间。

原始资料：本聊天work/resident-ticket-size.log、resource-queue.log、prefetch-audit.log；本轮只读插件入口常量扫描脚本find_gui_hook_owner.py。
