# M5Unit-NFC 0.1.2 修改分析与代码优化计划

日期：2026-10-10。范围：当前 Cardputer + UnitNFC I²C 的 NTAG215/Amiibo 模拟工程。此次仅分析和制定计划，不修改生产代码或烧录设备。

## 1. 对照依据

- 官方比较：[0.1.1 → 0.1.2](https://github.com/m5stack/M5Unit-NFC/compare/0.1.1...0.1.2)。已下载官方 Git 历史并实际检查 diff，而非依据 main 分支推测。
- 0.1.1：`9bb1360731e625789060dbdb2b13b1d993652a9e`。
- 0.1.2：`63c4caac1a07018ef73027e4b098d4fefe56c6f6`。
- 标签之间共涉及 50 个文件，806 行新增、166 行删除，包含示例、测试和元数据。
- 官方源码分析副本：`.build-tmp/upstream-m5unit-nfc-012-review/`。
- 本地依赖：`dependencies/M5Unit-NFC/`；当前应用版本：`flash-log-v8-rfal-rx-fastpath`。

结论：当前项目已经纳入 0.1.2 主要能力，并叠加 v7/v8 补丁。下一轮应完善失败处理和可验证性，不宜直接覆盖本地依赖再做一次升级。

## 2. 官方版本实际变化

| 变化 | 官方提交 | 对当前项目的意义 |
| --- | --- | --- |
| 给 A/F 模拟层增加 protected `transmit()`，说明派生类命令扩展方式 | `83160ce` | 便于 NTAG 专用命令扩展；不是新实现完整 NTAG215 协议，也不是新增加虚拟 receive_callback |
| 修复 NFC-A 接收缓冲越界，按容量读取并传递实际读取长度 | `6283b22` | 必须保留；但长度截断不等于完整帧有效性校验 |
| A 接收缓冲 64→256，F 接收缓冲 128→256 | `0355450` | 改善较大帧接收；不是 540 字节发送支持 |
| Halt 唤醒时重新采集 bitrate，进入 Ready 后按顺序配置 bitrate/listen mode | `a29f434` | 影响 HLTA→WUPA→重新选择的连续扫描 |
| 增加可选状态 trace | `1326ab1` | 默认关闭；启用串口输出会改变运行时序 |
| 根据 RF 活动/状态变化刷新超时，闲置返回 Off；默认 10 秒，0 可禁用 | `9389fd3` | 解决读卡器离开后状态滞留；持续活动会话不应按总时长强制终止 |
| 用 elapsed-time 判断替代绝对 deadline | `598d5a7` | 改善 millis 回绕时的检测/IRQ/FIFO 等待行为 |
| NFC-V extended read 要求返回完整已知大小的数据块 | `e4f32ad` | 防止短响应暴露旧缓冲内容；不在本工程模拟热路径 |
| 复用 M5Utility 饱和转换和奇校验工具 | `8109f37`、`be352cc` | 通用维护性改进，不应计为本工程的实测性能收益 |
| PlatformIO 的 M5UnitUnified 要求提高至 >=0.5.7 | `45df355` | Arduino library.properties 没有同步声明最低版本，构建应额外校验实际依赖 |
| 增补轮询超时测试、CoreS3 示例、identify 后 HALT 说明及 Classic 不支持模拟说明 | 见完整比较 | 不代表新增 Classic 模拟能力；不需移植到当前业务 |

关键源码：[模拟层](https://github.com/m5stack/M5Unit-NFC/blob/0.1.2/src/nfc/layer/a/emulation_layer_a.cpp)、[ST25R3916 适配器](https://github.com/m5stack/M5Unit-NFC/blob/0.1.2/src/nfc/layer/a/emulation_layer_a_ST25R3916.cpp)、[依赖声明](https://github.com/m5stack/M5Unit-NFC/blob/0.1.2/library.json)。

## 3. 当前已完成，后续应保留

1. `main/Ntag215Emulator.h` 已继承官方模拟层；仅合法 HLTA 委托基类。READ/FAST_READ/PWD_AUTH/WRITE 等继续由标签层处理，以保持秘密字段屏蔽、尾页回绕、认证和 4-bit ACK/NAK。
2. 短响应使用 protected transmit，长响应继续走 `main/NfcFifoTx.h` 的分段 FIFO；适配器忽略 timeout_ms，返回成功只代表提交，不代表 TXE 或读卡器已收到。
3. 应用显式使用 10 秒超时，离开 Active 清除认证；保持 I²C 400 kHz、128 字节预装、60 ms 补充时限。
4. v7/v8 已提供 RAM 诊断、Active 帧边界校验和 readFIFOExact，省去该路径一次重复 FIFO 状态读取。无效帧不分发，也不为诊断读取负载。
5. 与官方 src 比较，实际修改了四个已有文件：模拟层 cpp、A 适配器 cpp、unit cpp/hpp；新增 NfcDiagnosticHook.h 和 NfcRxFrame.h。另有两个 .bak 文件，不应混作功能补丁。

## 4. 优先级与实施步骤

### P0-A：先建立可追溯构建基线

目标文件：新增 `tools/build_firmware.ps1`、依赖来源清单/补丁说明；整理 README 的当前构建入口。

- 固定官方标签 SHA，同时单独记录本地补丁版本和修改文件哈希；现有 upstream-sha256.json 来自系统库拷贝，不能替代官方提交身份。
- 显式选择项目内库，记录 Arduino CLI、ESP32 core、M5UnitUnified、M5Utility、M5HAL 的实际路径与版本。对 M5UnitUnified >=0.5.7 进行构建前检查；选择经过验证的确切版本形成清单。
- 构建输出清单包含 BUILD_NAME、hook version、固件 SHA256、库路径、编译参数和产物位置。沿用当前板型和分区方案。
- 当前没有项目 Git 仓库，应先保存待改文件及哈希/差异；不要直接覆盖库。原生 ESP-IDF 的 main/idf_component.yml 仍引用上级目录，本阶段明确只交付 Arduino 构建；若要支持 IDF，单列路径和组件解析修复。

验收：从脚本生成唯一可定位的固件，构建日志证明实际选择本地库；错误依赖或 hook 版本应提前失败。

### P0-B：采集 v8 基线，定位失败阶段

当前 `logs/com7-20261009-latest-five/` 全部来自 v6。其中 8 号为会话，9～12 号仅 BOOT。会话 COMMAND_TOTAL=0、READY_TO_OFF=18、ACTIVE_OFF_NO_CMD=1。这只能提示优先检查激活/首命令接收，不能证明 v8 无效，也不能归因于密码、长帧发送或签名。

- 用可追溯 v8 构建采集 Switch 与手机各至少 10 次扫描，记录设备、位置、识别结果及完整会话日志；BOOT 文件不算扫描样本。
- 检查 RAM_HOOK_VERSION=2、RX_ACCEPTED、RX_REJECTED、ACTIVE_EOF_RXE、IRQ_READ_FAILURES、FIFO_READ_FAILURES、READY_TO_ACTIVE、COMMAND_TOTAL 及服务间隔。
- 按“未进 Active / Active 无有效帧 / 已有命令但响应失败 / 响应已提交但未识别”分类，再决定性能优化位置。
- 比较软件服务耗时与实际识别率；TX_START_US/HANDLE_US 不当作 FDT，IRQ 位出现次数不当作帧数。需要精确 RF 时序时另行抓包。

验收：每个样本可对应固件身份和完整日志，给出各失败阶段计数；收益只能以对照数据表述。

### P1-A：补齐底层 I/O 失败处理

目标：`dependencies/M5Unit-NFC/src/nfc/layer/a/emulation_layer_a_ST25R3916.cpp`，必要时联动模拟层、`main/ListenerRecovery.h` 和应用恢复入口。

已核实的问题：Idle/Halt 中读取 bitrate 失败后仍解析零初值，可能被当作 106 kbps；Idle 的 FIFO 诊断函数返回值未用于阻止后续处理；start_emulation 和多处 goto_* 忽略寄存器写入失败，可能出现软件已切状态而硬件配置未完成。

实施：

1. bitrate/PTA/FIFO 读取失败时不更新有效状态、不解析缓冲、不调用业务回调；记录既有 RAM 事件或明确失败步骤。
2. 逐步将关键配置序列变为可检查结果，首先覆盖启动、Ready、Active、Halt 和 Off。不要机械地将所有调用改成短路与：失败后必要的清理动作仍需执行。
3. 致命配置失败传播到应用恢复机制，暂停业务分发，并在确认无场后按原有间隔重试。明确 State::None/Off 与 recovery_pending 的映射，避免“返回 None 但无人重启”。
4. 清 FIFO/解除屏蔽本身失败时升级恢复，不继续假报可接收。诊断只用操作的已有结果，不额外消费 IRQ。

验收：对关键 I/O 步骤注入失败，验证无错误回调、无假成功、无无限重试；成功恢复后可再次接收，且会话认证已清除。

### P1-B：增加真实状态机/适配器回归覆盖

目标：新增宿主模拟层/适配器测试及必要的最小测试接口，接入现有测试脚本。

目前多数协议测试是 constexpr，能够验证纯逻辑，但不能证明真实寄存器调用顺序、状态切换和总线错误传播正确。

- 通过 fake Unit/Adapter 和可控时钟执行生产逻辑，避免另写一份状态机作为测试对象。
- 覆盖 HLTA→Halt→WUPA→Ready→Active 的 bitrate 更新及寄存器顺序。
- 覆盖 10 秒无活动恢复、持续通信超过 10 秒不超时、禁用超时、计时回绕，以及超时边界已有待处理 IRQ 的顺序；先确定预期再决定是否改变 update_expired 的位置。
- 覆盖 EOF 与 RXE 同时出现、FIFO 状态读取失败、payload 256/257 边界、非完整字节、传输失败，以及恢复后的首条命令。
- 验证短数据响应、长响应和 4-bit ACK/NAK 各自的调用序列与失败清理。避免业务层自行读取会清除的中断寄存器。

验收：故障用例执行实际生产路径；原有七组编译期测试、FlashLog 宿主测试和串口测试仍通过。

### P2-A：有数据后优化接收、轮询与发送

- v8 已消除 Active 的重复 FIFO 状态读取，不重复实施。若仍卡在 Ready，优先检查 Ready/Active 配置序列和轮询间隔，不能靠修改 PWD_AUTH 解决。
- 若日志表明命令稳定进入且长响应失败，再独立比较 FIFO 预装大小、补充分段及发送耗时；一次只改一个参数，并保留 400 kHz/128 字节基线。
- 如需区分“已提交”和“发送完成”，在现有集中 IRQ 读取/缓存处设计 TXE 观察，关联本次发送，处理旧 TXE、超时和离场；先做纯诊断，不能另开一条 IRQ 消费路径或在回调内盲目阻塞等待。
- 只有证据表明轮询开销占主导，再评估非阻塞补充或减少寄存器事务；IRQ 引脚/SPI 迁移涉及硬件条件，单独立项。

验收：对照识别率不下降，无 FIFO 错误回归，且目标阶段软件延迟有可重复改善。未测得收益时保留较简单的原实现。

### P2-B：按兼容性需求补全标签行为

目标：`main/NtagCommands.h`、`main/Ntag215Emulator.h` 和命令测试。

- AUTH0/PROT、静态/动态锁与配置锁、AUTHLIM、计数器分别按 NTAG215 官方资料建立行为表后实现；不要混入时序试验。
- 已有密码/PACK 屏蔽必须保留；认证状态与统计生命周期继续分离。
- READ_SIG 仍需与 UID 匹配的原标签签名；540 字节镜像不包含该独立数据。保持未配置时的明确失败行为，不以零签名假报支持。
- 当前日志没有命令回调，因此这些属于后续协议完整性工作，不能宣称可修复眼前的激活失败。

## 5. 执行顺序、验证与回退

推荐拆分为四批：①可追溯构建及 v8 基线；②I/O 失败处理及真实路径测试；③由新日志驱动的一项性能实验；④独立协议兼容补全。每批有独立差异、构建清单和回退产物。

执行代码阶段运行：

```powershell
pwsh -File tools/run_tests.ps1
pwsh -File tools/run_host_tests.ps1
```

然后按 README 指定的本地库路径完成 Cardputer 全量编译，核对内存占用与构建身份。编译器若再次出现历史记录中的 DLL 启动错误，应报告工具链阻塞，不能将宿主测试通过当作固件编译成功。

硬件验收还包括连续扫描、Halt 唤醒、离场重入、维护后恢复、日志写入失败后的继续诊断。固件回退保留现有 Flash 日志分区，不以全片擦除作为恢复步骤。

本次完成的是官方差异核实、当前代码检查和计划编写；未执行新一轮编译、测试或实机验证。以后若涉及 C# 文件，修改前检测行尾，修改后保持原行尾风格。
