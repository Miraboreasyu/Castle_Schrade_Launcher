# 预加载第一轮日志与前置检查修正

2026-11-16，MHW:I 15.23 / Steam build 15539686。

## 实机结果

用户本次日志：ready=725739687，native quest-entry=725750750，跨度11063ms。选择存档=725747968，故ready→选择存档为8281ms；建房请求接受=725748015，其后等待到任务入口为2735ms。此前诊断版对应为10797ms、7922ms、2828ms。两次不具备受控对照条件。

关键行是 `title-prefetch skipped=precondition`。本次没有提交任何预加载ticket，不能据此判断提前加载是否有效，也不能把标题批次的变化归因于预加载。

|观察区间|耗时|
|---|---:|
|ready→首次Logo PREPARE|3609ms|
|常驻主批次state2→state3|500ms|
|常驻资源初始化state4→state5|454ms|
|常驻bootstrap批次state9→state10|15ms|
|字体准备state1→state2|31ms|
|字体准备state2→首次标题PREPARE|656ms|
|标题主ticket首次观察→PREPARE state2|531ms|
|整个标题PREPARE→首次标题EXECUTE|688ms|

常驻主批次在日志中是issued=256/256、partial=1：仅扫描前256项。即使ready=256，仍不能称整批完成；后续原生状态3才证明ticket完成。原生代码组装463项及补充资源，本次确实执行了这条大批次。state4调用若干表/索引初始化子状态机，454ms并不是已证实可删除的显示计时。

标题场景本次是title_st102，上次为title_st101，再次说明不能把st101硬编码为唯一预加载目标。新版仍由原生选择和加载场景。

## 前置检查核对

通过项目stdio MCP查询sMhGUI构造函数与常驻数据初始化函数，随后用Ghidra只读反编译核对。141ACFE10全局构造中写入gui+15000的标题组表指针，并将gui+15008计数设为29。该列表在Logo执行前已由全局构造建立；无需提前构造或执行整个aTitle。

上一版的组合条件包含 `Readable(manager, 0xA250)`。当前Readable实现只接受单个VirtualQuery区域覆盖整个范围；一个原生分配若跨越相邻区域，会被这一额外检查排除，即使实际需要访问的字段都可读。此限制与游戏原生接口没有对应依赖，已移除，改为逐项检查manager头部、描述符及通道字段。

这是已复现的检查缺陷，但本次日志只有一个合并后的precondition结果，不能证明该缺陷就是这次游戏运行中唯一或实际命中的原因。代码字节被其他mod改写、读取条件或GUI状态等其余原因仍需新增明细日志来区分。

## v2修改

- 前置条件分别记录manager-null、manager-head/descriptor/channels-unreadable、gui-null、gui-group-unreadable/empty/count-invalid、gui-table-null/unreadable，并附manager、gui与计数。代码不匹配时输出函数RVA和实际入口十字节。
- 原版只在Logo state0检查一次，并立即锁定为已尝试。现在对可变输入条件在Logo执行期间最多每100ms重试；到字体请求阶段结束尝试窗口，防止过晚提交影响字体和标题切换。同一原因只记录一次；成功提交或代码不匹配后不再重复尝试。
- 保持独立普通原生ticket、flags=2、原生标题完整就绪检查及回收生命周期。代码入口校验继续有效，不绕过未知改写。
- resident-main的采样上限从8块提高到32块（最多1024项），其他ticket仍为8块；便于观察常驻批次后半段。超过边界仍标partial。
- 本轮未增加其他加速行为或新的hook地址，保留第二版有效的计时清理。

## 验证与交付

MSVC编译、源码检查、git diff --check通过。实际前置检查夹具使用Windows VirtualAlloc/VirtualProtect构造相邻VirtualQuery区域，验证旧整块检查拒绝、新字段检查通过；另覆盖GUI列表未就绪、计数异常、不可读列表、重试窗口结束与代码入口变更。入口字节测试以此前Ghidra输出为输入，而不是复制比较器自己的预期数组。实际ticket生命周期与profiler夹具测试继续通过。

尚未实机验证v2是否成功提交及是否改善耗时。

包名Castle_Schrade_Launcher-15.23-title-prefetch-v2.zip，仅含nativePC/plugins/auto_quest_start.dll。关闭游戏后覆盖，保留INI。回退可用已运行的resource-profile.zip或有效的timer-v2.zip。

下一次日志首先看title-prefetch submitted，或具体deferred/skipped原因。若成功，检查提前ticket在标题准备前的ready数量，以及原生title-assets的初始ready数量；结合ready→选择存档时间，判断缓存是否被复用及是否有净收益。

相关原始反编译记录：本聊天work/gui-prefetch-init.log、prefetch-blocker.log；前次接口核对记录仍见prefetch-audit.log、prefetch-abi.log、prefetch-symbols.log。
