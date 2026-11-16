# 第二版日志分析与资源诊断版

ready=723054093；native quest-entry=723063046，跨度8953ms。

|观察区间|时间|
|---|---:|
|ready→Logo start事件|3938ms|
|Logo start事件→aLogo execute state0|937ms|
|aLogo execute state0→state16（字体请求阶段）|1297ms|
|aLogo state16→aTitle execute state0|1266ms|
|aTitle execute state0→选择存档|1078ms|
|选择存档→任务进入状态|437ms|

清除的计时余额：外层0.500s、aLogo已完成存档1.381s、标题内容提示1.500s、标题内容已完成存档1.167s，总计4.548s；相比第一版新增清除4.048s。第一版日志ready→任务进入14.594s，本次8.953s，相差5.641s，其他差异可能包括运行条件/缓存等，未做受控归因。用户感知的约30→13秒还包含日志跨度外的进程启动及任务场景加载，不能把两种计时范围混同。

state16→aTitle执行不仅包含字体，还可能包含aLogo清理、区域转换、aTitle prepare。ready→Logo start也不等于纯磁盘耗时。现有日志不足以确定这两段中的资源瓶颈。

## resource-profile诊断包

保留第二版全部加速逻辑；新增14024C4E0（aLogo prepare）与14025DA80（aTitle prepare）的入口观察hook，并在原有aLogo execute hook里观察字体准备子状态。覆盖字节分别为10和9字节，已从Ghidra只读数据库核对，均为完整且无RIP相对寻址的指令。

仅观察owner自己的ticket：logo、base-gui、font、title-assets、title-quest-data。每个ticket最多每50ms采样一次，在变化时将观测时间、ticket状态、已实例化请求数、ready数及第一个未就绪路径写入固定大小内存缓冲。最多256条，超出计数；资源块最多8块，每块最多32项，未扫描完时partial=1。首次到达任务进入状态后统一输出，避免逐资源同步文件I/O影响加载。

resource-profile行的外层时间是输出时间，分析应使用行内at=的采样时间。采样无法提供精确的逐资源I/O耗时；issued是ticket条目已获得资源指针数（可能缓存命中），不是新读盘次数。pending是扫描遇到的首个未就绪条目，不一定是唯一或最慢资源。state转换只给出约50ms分辨率的提交/完成观测。

该版没有改变资源加载顺序或并发策略，不预期进一步加速。观察回调本身仍有少量采样开销。若启动失败未进入任务，内存缓冲不会自动输出。

验证：MSVC构建、现有源码检查、git diff --check通过；实际profiling函数的内存夹具测试通过，包括排队/部分就绪/全部就绪、重复抑制、缓冲上限及夹具状态保持。尚未实机验证新hook。

安装：关闭游戏，将Castle_Schrade_Launcher-15.23-resource-profile.zip覆盖到游戏目录，保持INI；跑一次后提供完整auto_quest_start.log，尤其尾部resource-profile段。包内仅DLL。回退为已验证的timer-v2.zip。
