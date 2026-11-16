# 初始化深挖与第二版实验

适用：MHW:I 15.23 / Steam build 15539686。2026-11-16。

## 结论修正

第一版仅覆盖aLogo/aTitle外层过渡计时，不能据此判断初始化已无明显加速空间。本轮沿实际启动链找到更深层的1.5秒计时。存在真实可尝试的优化点；用户此前提供的日志不能证明这些点每个都被执行，也不能把各路径的1.5秒简单相加。

## 反编译证据

通过项目stdio MCP查询1402999D0、14029A400、14029B040、141B7BFE0、140299D10以及14029A410附近函数，随后用Ghidra只读反编译核对。

1. 标题主菜单14025F5D0的B4=0启动user+1F980上的内容检查，B4=1等待其完成。1402999D0给内容对象+8写1.5f，置+50=5并显示消息。14029AD00的state5做DLC相关工作，state6单独消耗这1.5秒，然后才调用1402992B0并遍历三个占用存档执行14134F850。这是后续处理之前的固定门槛。
2. 另一分支14029A400启动byte+1554状态机，14029B040的byte-state1也写+8=1.5f；byte-state2只扣计时，到零关闭消息并继续内容处理。
3. 140CDCE80是存档操作分发器：op0/6→140CDEBE0，op1→140CDD800，op2→140CDD440，op3/4→140CDCEE0，op5→140CDE1F0。已检查的流程分别保存+38=30.0f错误超时和+3C=1.5f最短等待。进入结果阶段后，同时检查+3C与user+1F8D0的忙状态，并根据user+1F8D4处理成功、失败或重试。最短等待与实际I/O是两个条件。
4. aLogo state5内14024EA60会请求op1，部分分支请求op4；state7请求op3。启动状态机另有op2/op0等调用。故这组等待确实在启动链上，具体命中数量仍取决于存档状态与实际路径。
5. 内容控制器14029AFA0在controller+C=2时轮询controller+10上的存档操作，可能再受上述最短等待约束。

原始反编译日志：本聊天work/init-deep.log、work/init-deep2.log、work/init-deep3.log。没有修改Ghidra工程或SQLite注释。

## 第二版实现

沿用第一版的现有hook，无新增代码注入位置：

- 首次aLogo启动的明确存档轮询分支：仅当操作进入结果阶段、I/O非busy且结果为成功时，清除剩余+3C最短等待。op5暂不覆盖。
- aTitle state4/B4=1：清除内容检查state6或alternate byte-state2的+8计时；检查其嵌套存档操作是否满足上述成功条件。
- 第一版外层过渡归零保留。
- 记录aLogo主状态、aTitle主状态/B4，以及内容控制器状态变化。只在状态变化或消除正数计时时写日志。
- 30秒错误超时、请求提交、存档结果、资源ready、错误路径及DLC处理继续由原生代码执行。

## 验证与使用

MSVC编译通过；现有源码回归检查与git diff --check通过。另将实际计时辅助函数抽出编译运行内存夹具测试：覆盖busy、重试结果、尚未进入结果阶段、六种支持操作、op5排除、30秒超时字段保持、负值/NaN计时不修改。游戏回调顺序、UI动画交互以及整体节省量仍需实机验证。

测试包Castle_Schrade_Launcher-15.23-timer-v2.zip仅包含nativePC/plugins/auto_quest_start.dll；关闭游戏后覆盖，保留现有INI。回退上一版使用timer-trial.zip。

日志重点：

- title content message minimum：标题内容检查的固定等待。
- aLogo completed save minimum：aLogo阶段I/O已成功后的剩余等待。
- title content completed save minimum：内容检查嵌套存档操作已成功后的剩余等待。

removed数值只代表清除的计时余额，不直接等于总启动时间缩短量。比较相同存档、同等冷热启动条件下ready→native quest-entry state reached的总耗时。此前第一版该值为14.594秒；它也不包含随后任务场景加载到可操作的时间。
