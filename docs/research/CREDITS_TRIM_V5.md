# 制作人员表资源裁剪实验 v5

2026-11-16，MHW:I 15.23 / Steam build 15539686。

## v4实机结论

场景请求确已省略，index=6；标题ticket从120变成119项，首次采样117项ready，到下一准备状态约15ms；任务入口scheduler=null、fallback=0。整个标题PREPARE阶段172ms，上次v3为562ms。

ready→任务入口本次11109ms，上次10406ms。字体就绪→首次标题PREPARE本次1234ms，上次610ms；建房接受→任务入口本次3047ms，上次2125ms。总跨度受这些区间波动影响，不能用总时间增加否定已观测到的标题批次缩短，也不能据此声称总启动速度已经提高。

## 新的资源裁剪审计

通过项目stdio MCP查找title_news、staffRollDataSlugger及credit logo路径的消费者，再用Ghidra只读反编译核对标题初始化、GUI虚表和更新函数。

1. 新闻不是孤立的预加载。主标题GUI初始化141F00600会以flags=1获取common/title/title_news；uGUITitleNews初始化14055D8B0还会获取新闻数据与按语言选择的title_news文本。单删PREPARE请求会让这些消费者改为同步加载。本版保留新闻及相应GUI。
2. title_envb在14025CF70标题初始化中被同步获取并绑定cWwiseContainer；同样不适合单删准备请求。
3. uGUIStaffRollSimple由14025CF70无条件创建，title+2A0保存实例。其初始化14055AF30建立多组界面节点，但不获取staffRollDataSlugger。14055B950更新仅在GUI活动标志满足条件时继续，并在播放阶段获取制作人员表数据。成功自动进任务路径不进入主状态19（0x19），该状态才会激活title+2A0的制作人员表GUI。
4. 标题主ticket额外请求staffRollDataSlugger及一批ui/credit/tex/logo/logo...纹理，GUI组5还登记ui/credit/credit及声音参数。其他制作人员表实现141EBE460、140AD18C0也有自己的资源ticket请求流程。

因此这次采用配套裁剪：制作人员表GUI预载、数据和Logo纹理请求，以及首次启动时对应GUI单元的创建一起处理。只减少显式ticket条目而保留所有消费对象，可能无法减少实际加载成本。

## 实现

- 保留v4场景裁剪及已验证的GUI提前加载。
- 两个新增调用门都安装成功后才启用本组裁剪；提前加载时排除ui/credit路径，避免先把它及依赖载入缓存。原样本29个GUI路径，通常会变成28个。
- 14025DD2E是标题PREPARE最后的submit调用。在提交前，仅对当前首次自动启动title拥有、配置存档已占用、状态尚未提交且所有条目resource指针均为空的ticket过滤ui/credit前缀和staffRollDataSlugger。先完整验证，随后在各块内保序压紧条目并更新数量；保留链表及其他请求。已开始加载、无效或有环的ticket不修改。
- 保存被移除条目的精确DTI和路径，最多128项。记录removed和kept数量；数量取决于实际文件、DLC和其他mod，不硬编码“必须删除多少项”。
- 14025D67E是初始StaffRollSimple的newUnit调用。仅在首次自动启动范围省略它，原生调用者本来就处理空返回值，并保持title+2A0为空。其他调用按原参数转发。
- 如果首次标题后来进入制作人员表状态19，用保存的原始DTI/路径重新提交原生异步ticket，等待真正完成，再通过原生newUnit、addBottom和引用赋值接口创建该GUI。等待期间只压住对应标题子状态；全局资源、GUI和网络更新继续运行。实例可用后才回收临时ticket并执行原来的制作人员表状态。
- 后续正常返回标题时，首次启动条件已失效，原生请求和GUI创建按原路径执行。

没有新增INI设置，没有删除磁盘文件，没有修改资源ready标志或伪造ticket完成，没有改动常驻数据构建预算。

## 本轮额外发现

常驻构建函数141373E40确有每次处理约101条的分页限制，涉及CraftElement索引构建，属于分帧工作而非固定显示计时。本版按用户指定方向只做资源裁剪，没有调整这个预算。标题PREPARE中的141ADE8D0实际在请求加载画面调度器，不是等待所有注册GUI实例，因此缺少未使用的制作人员表实例不会单凭这个检查卡住。

## 验证与交付

MSVC构建、verify_source.py、git diff --check通过。实际代码夹具测试覆盖跨块稳定过滤、保留新闻和存档图标、拒绝已提交/已发出资源和循环链表、原始DTI/路径备份、初始空GUI返回和正常参数转发、手动恢复ticket/GUI及引用回收、提交失败清理。

用真实hook.cpp在测试进程执行两个x64调用门，确认submit的寄存器参数及第五/第六栈参数保持正确，GUI创建调用按条件省略或恰好转发一次。既有场景调用门和预加载测试继续通过。没有向游戏进程写入测试代码。

v5及制作人员表恢复路径尚未实机验证。removed是移除的请求条目数，不等于同样数量的新磁盘读取被消除；依赖和缓存复用仍需实测。

交付Castle_Schrade_Launcher-15.23-credits-trim-v5.zip，仅含nativePC/plugins/auto_quest_start.dll。关闭游戏后覆盖，保留INI；回退使用已验证的no-title-scene-v4.zip。

下一份完整日志重点：

- credits-trim request/creation gates=1/1 armed=1。
- title-prefetch gui-paths及总条目数。
- credits-trim removed/kept及initial StaffRollSimple creation omitted。
- 任务入口widget是否为空、title-assets条目数、预加载耗时及字体→标题交接区间。

原始反编译资料：本聊天work/title-extra-resources.log、optional-title-ui-complete.log、credits-trim-sites.log、credits-null-widget-audit.log、resident-build-budget.log。
