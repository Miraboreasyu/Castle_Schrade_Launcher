# 资源诊断结果与标题预加载实验

适用 MHW:I 15.23 / Steam build 15539686。2026-11-16。

## 最新日志的计时

用户提供 resource-profile 日志，ready=723881562，native quest-entry=723892359，跨度10797ms。以下是互不重叠的观察区间；不是逐函数CPU/I/O耗时。

|区间|耗时|
|---|---:|
|ready→首次aLogo PREPARE|3344ms|
|Logo初始准备→进入常驻资源阶段|47ms|
|aLogo PREPARE state3：常驻资源准备|1047ms|
|其后基础GUI准备→aLogo EXECUTE|109ms|
|aLogo EXECUTE→字体请求|1281ms|
|字体请求→字体就绪子状态|31ms|
|字体就绪子状态→首次aTitle PREPARE|610ms|
|aTitle PREPARE→aTitle EXECUTE|687ms|
|aTitle EXECUTE→选择存档|766ms|
|选择存档→任务进入状态|2875ms|

标题主ticket从首次观测723888046到准备state2的723888593约547ms。基础GUI ticket等待约94ms，字体约31ms。pending只是扫描遇到的首个未就绪项，不能把title_st101连续出现的375ms归因于它自身，更不能据此确定磁盘或解压线程瓶颈。

本次建房请求在723889531被接受，到任务进入状态为2828ms；上一份有效第二版日志相应为390ms。两份日志总跨度10.797s与8.953s的差异主要落在会话就绪阶段。选择存档之前本次为7.922s，上次为8.516s；不能把本次总时间增加解释成资源诊断开销，也不能据此做严格受控归因。

上述任务进入状态不是任务地图加载完并可操作，当前日志仍未覆盖后半段。

## 本轮反编译核对

通过项目stdio MCP查询141B7BFE0及其调用关系、141AD8D20、141B80780、sMhWwiseManager::LoadRequestInfo::ctor，再用Ghidra只读反编译核对，未修改数据库。

- 141B7BFE0是sResidentData初始化状态机，实际状态字段+38；aLogo以mode=3调用。若普通常驻资源完成标志尚未设置，state1仍会提交463项及补充资源，state2等ticket（字段+3C），state3绑定引用，state4继续初始化；mode=3跳过本地化212项分支。若bootstrap完成标志尚未设置，state8组装6项普通资源和13项按语言处理的资源；state9等ticket（字段+44），state10绑定资源引用，state11返回完成。哪些已在此前初始化，当前日志尚不能确定。本次1047ms不能当作固定延迟删除，新版补充这条状态机与上述两个ticket的只读采样；大批次超过256项时会标记partial=1。
- aLogo EXECUTE state16/substate2调用141B20670。它把语音语言请求加入Wwise队列、清除音频就绪标志，并未在该函数中固定Sleep。之后请求切换到aTitle。现有610ms区间还包含区域交接、清理/构造等工作，不能直接省去。
- 14025DA80的state0才组装标题资源。141AD8D20(gui,ticket,5)主要枚举GUI组5的rGUI与存在的rGUISoundParam，仅登记ticket。组5在后面的地图/任务分支之前返回；不设置GUI单元状态。路径可能根据系统选项选择变体，因此原生标题仍需重新登记它自己的完整批次。
- 141B80780分配ticket，141B80C70(...,0,2,0,0)提交原生普通异步加载。141B81180(manager,&id)请求state7回收；141B80CB0随后释放资源引用及ticket块。完成的ticket不会仅因加载完成自动归还其资源引用。
- rScheduler::DTI::Instance=1451C3B00，rWwiseContainer::DTI::Instance=1451C46F8，已从Ghidra符号确认。新增调用的五个函数入口字节也逐一核对。

原始反编译记录位于本聊天work/resident-init.log、prefetch-audit.log、prefetch-abi.log、prefetch-symbols.log。

## 实验实现

在原有aLogo EXECUTE回调首次state0时，分配一个独立原生普通ticket，提前登记GUI组5，以及固定的ui/save_icon/save_pos和sound/env/title/title_envb，然后以原版flags=2提交。此时基础GUI已准备完成，后续还有约1.3s存档/系统初始化，可尝试重叠后台资源加载。

不预选随机标题场景，不提前加载依赖存档进度的场景，不调用完整aTitle PREPARE，不改变原生资源ready位或ticket更新循环。原生aTitle仍请求自己的完整批次并等待真实完成。提前ticket保持引用，到第一次aTitle EXECUTE时才通过原生release接口请求回收。回收前核对manager、ticket分配标志和首个块身份；身份变化时丢弃本地句柄，避免回收其他所有者的ticket。

加速上限受本次整个标题ticket约0.55s约束；这里只覆盖其中一部分，实际收益可能更小、为零或因竞争而回退。现阶段不声称已经加速。此实验也不能解释或优化日志前3344ms和后续任务场景加载。

新增代码在src/startup_prefetch.h，调用入口在原有Logo/title回调；没有增加注入位置。第二版固定等待清理保留。INI仍只有原有三个设置。

## 验证与测试

MSVC构建、现有verify_source.py、git diff --check通过。实际预加载实现的内存夹具测试覆盖ticket不足、提交失败回收、flags=2与普通通道、重复调用不重复提交、完成后的单次释放、所有权变化不释放其他ticket，以及采样不修改ticket。实际profiler夹具测试继续通过。新增原生调用仍需实机验证。

测试包：Castle_Schrade_Launcher-15.23-title-prefetch.zip，仅含nativePC/plugins/auto_quest_start.dll。关闭游戏后覆盖，保留INI。可用Castle_Schrade_Launcher-15.23-resource-profile.zip回退到本次已运行的诊断版；timer-v2.zip是已有有效加速版。

下一次提供完整日志，尤其title-prefetch、resident-bootstrap与title-assets行。判断预加载是否在标题准备前完成，以及原生标题ticket的初始ready数量是否增加。比较ready→选择存档这一段可降低会话波动的干扰，同时记录进入地图可操作的实际总时间。相同缓存条件下对照数次比单次总耗时更有意义。
