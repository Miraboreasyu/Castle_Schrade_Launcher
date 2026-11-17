# 常驻构建预算实验 v1

2026-11-17，基于已实测的 v1.1.0，适用 MHW:I 15.23 / Steam build 15539686。

复用现有 aLogo PREPARE 钩子。在首次自动启动、Logo 准备状态3且 resident 状态4时，在原生更新之前额外调用 `141B7BFE0(resident, 3)`。每次更新最多增加7次调用，QPC计时达到2ms后停止额外推进；原生调用不可中断，因此这是两次调用之间检查的软预算。

每次调用前重新检查 resident 状态，离开4立即停止。其内部仍按原来的顺序调用四组构建器及最终初始化；不写游标、完成标志或ticket，不删除资源，不创建工作线程，不新增hook。原生Logo更新随后正常执行。

仅自动集会和自动任务模式启用；disabled、title_screen_only及返回标题后不启用。函数入口字节不匹配或计时器不可用时保持原生预算。绑定首次Logo和resident实例，实例变化时停止额外推进。

日志在已有resource-profile缓冲内记录，到标题/任务入口统一输出：

- `resident-budget: begin extra-limit=7 soft-budget-us=2000`
- `resident-budget: finished state=5 wall-ms=... extra-calls=... updates=... extra-work-ms=... max-slice-ms=...`

wall-ms从首次观察到state4计算至观察到离开state4；若最后一次原生常规更新完成，则在下一次钩子观察到，所以可能包含一个采样间隔。extra-work-ms仅统计额外调用部分，不是整段CPU时间。max-slice-ms用来发现单次不可中断调用导致的预算超出。

对比原有 `resident-bootstrap state=4 → state=5` 跨度，以及相同模式、相同存档下从启动到地图可操作的时间。此前任务模式样本该阶段为687ms；不能直接当作所有hub启动的基准。

MSVC x64构建、源码检查与现有模式夹具通过。增加的针对性检查覆盖7次调用上限、软时间预算、完成即停止，以及不推进资源等待等其他阶段。游戏内收益待用户实测。本实验未发布到GitHub Release。

交付测试包：DLL位于nativePC/plugins，随包README、quest_id.csv及许可证；不包含INI，以保留用户当前配置。回退可覆盖正式v1.1.0的DLL。

## 首次实机日志

用户2026-11-17提供日志：ready=29775187，任务入口=29783625，任务模式、slot0、quest66845。构建到达原生state5，额外调用36次，分布于6次更新；wall-ms=72.982，extra-work-ms=7.648，max-slice-ms=5.537。后续bootstrap为19/19、base GUI为47/47，标题主ticket首次采样58/58就绪；抵达任务入口。

与此前v5任务模式样本比较：构建阶段约687ms降至73ms，减少约614ms（约89%）；旧值来自GetTickCount64阶段采样，新值来自QPC，精度不同。ready到任务入口9.297秒降至8.438秒，减少859ms；其中早期未覆盖区间也缩短265ms，建房到入口反而增加172ms，所以总差值不能全部归因于本补丁。地图可操作时间日志仍未覆盖。

max-slice超过2ms符合软预算设计：单次原生调用不可中断，在返回后才检查时间。额外工作总计7.648ms不包含每帧正常原生调用的CPU时间。无需为了再压缩剩余约73ms立刻扩大预算。

日志未单独采样resident state5而直接看到8，符合额外调用到达5后原生常规更新继续推进的设计；完成记录已明确state=5，后续19项真实资源等待仍执行。字体子状态2至标题PREPARE依旧468ms，是后续独立测量候选。

原始附件：`C:\Users\Administrator\.codex\attachments\d534a812-e080-4ec5-8751-719e029ffe51\已粘贴的文本.txt`。这份日志确认预算路径生效，不代表所有存档和后续游戏行为已经验证。
