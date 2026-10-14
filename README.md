# 内置塞尔达 Amiibo / NTAG215 模拟

当前源码版本：`NTAG215 embedded-zelda flash-log-v9-paper-listener-recovery`。

## v9：对照 1013.md / paper_name_plate 优化（2026-10-13）

- 默认采用 FDEL=5、MRT=04、512/fc 步进，配置后读回验证；保留 64/fc、02 对照配置。I²C 保持 400 kHz。
- 增加 AUTH0/PROT、静态/动态锁及块锁、CFGLCK 场生命周期和 CC OTP 行为，保留原始 Amiibo UID/数据与 PWD/PACK 恒屏蔽。
- 保留 SELECT 与 RXE 合并时的首帧及错误位，修正同轮激活后拒绝帧时的自动应答恢复；Idle 不读取无效负载或计算软件 CRC。
- 检查关键 I/O，失败停止分发并在无场时自动恢复；hook 版本为 3，新增失败阶段诊断。
- 完整说明、与 1013.md 的逐项对照、验证和限制见 [1013-对照优化说明](docs/1013-对照优化说明.md)。后面的版本章节保留历史验证记录。

```powershell
pwsh -NoProfile -File tools/run_host_tests.ps1
pwsh -NoProfile -File tools/build_firmware.ps1
```

9 组协议断言、721 项实际监听状态机检查（含 100 个 I/O 故障注入点）、FlashLog 与串口测试通过。固件身份与结果见 `.build-tmp/paper-v9/build-manifest.json`；产物为同目录 `Emulation.ino.bin`。已烧录到 COM7，四段写入校验及 v9 启动/串口诊断通过，计时配置读回一致；记录见 `logs/1013-v9-com7-deployment.json`。Switch 识别率仍需实机验证。

回退文件：`backups/pre-paper-name-plate-v9/`、`backups/pre-1013-protocol/`。默认构建使用本地 `dependencies/M5Unit-NFC`。

## READ 循环与计时优化（2026-10-08）

- 根据分享会话关注的响应延迟，优化尾页 READ 的读取循环：初始偏移只取模一次，之后递增游标并在 540 字节边界回绕；保留密码/PACK 屏蔽和返回数据。
- `TX_START_US` / `HANDLE_US` 从回调入口开始计时，包含日志批次初始化；原先首条命令的这部分开销未计入。新旧固件的耗时不能直接视为同一起点比较，仍不代表射频 FDT。
- 新增每个合法起始页读取完整 540 字节的测试，验证回绕、秘密字段屏蔽、输出缓冲边界和零长度读取。
- 保持 I2C 时钟、FIFO 预装大小、接收计时和协议策略不变。本次属于局部 CPU 路径优化，尚不能证明解决 Switch 识别问题，需实机对照扫描。
- 修改前文件保存在 `backups/pre-read-timing-optimization/`。下方 v5 的固件与构建记录属于此前版本，不包含本次修改。

验证：全部 5 组 C++14 编译期测试（MSVC）、FlashLog 宿主测试与串口测试通过。MSVC 命令记录在 `.build-tmp/host-tests/protocol-tests.cmd`；测试代码有常量条件及整数收窄警告。ESP32 编译器因 Windows 报“无效的系统 DLL 重定位”无法启动，本次没有生成或烧录新固件。

## v5：代码排查优化（2026-10-08）

- 启动监听与维护后恢复共用初始化流程。配置目标模式、`emu.begin` 或接收计时配置失败时，记录失败步骤并停止本轮监听，在无场时每隔至少 1 秒重试；恢复成功刷新屏幕底部提示。总线初始化失败仍沿用原有停机行为。
- Flash 日志保存前计算本条记录长度，额外预留 8 KiB 文件系统操作空间；先回收遗留的 `nfc-pending.tmp`，空间仍不足时从最旧日志开始清理，只处理 `/log` 内严格匹配文件名的普通文件，始终保留最新一份完整日志。无法腾出足够空间时返回失败并保留 RAM 记录。预留空间不替代实际写入、刷新和改名的错误检查。
- 日志生成回调会执行两次（计数与写入），必须不产生副作用。空间清理可能发生在新文件写入成功之前，因此空间紧张时历史日志可能少于 8 份。
- 命令缓存条目由 32 字节降为 24 字节，256 条合计节省 2048 字节静态 RAM；用编译期断言保持大小约束。
- FAST_READ 的秘密字段屏蔽只遍历交集，最多清零 6 字节，保持原有读取结果及射频参数。
- 串口脚本逐条发送命令；`logcat` 必须收到 FILE/END 标记才视为完成。键盘输入逐键读取，不再阻塞串口接收；串口异常、响应超时或导出中断均报告失败。

源码备份：`backups/pre-review-optimization/`。该目录保留本轮修改前文件。

验证命令：

```powershell
pwsh -File tools/run_tests.ps1       # 5 组 ESP32-S3 编译期测试，含初始化失败与重试
pwsh -File tools/run_host_tests.ps1  # MSVC：实际 FlashLog + 内存文件系统故障模拟，并运行串口测试
pwsh -File tests/serial_capture_test.ps1  # 单独运行串口响应测试，无需设备
```

Flash 测试覆盖空间回收、最新日志保护、遗留临时文件、删除/写入/改名失败与重试、8 份日志轮转和超大记录拒绝；串口测试覆盖分段标记、延迟响应、长时间传输、缺失结束标记和设备错误。测试不能替代实机射频、Flash 断电和串口设备验证。

本轮上述测试全部通过，`m5stack:esp32:m5stack_cardputer` 完整编译通过：程序 666791 字节（50%），静态 RAM 36244 字节（11%），比 v4 减少 2048 字节。固件为 `.build-tmp/flash-log-v5/Emulation.ino.bin`，构建日志为 `.build-tmp/flash-log-v5-build.log`，修改差异为 `.build-tmp/optimization-v5.diff`。尚未烧录设备。

## v4：协议修正、激活诊断与恢复

- PWD/PACK 经 READ、FAST_READ 始终返回零，包括认证成功之后；PACK 只通过成功的 PWD_AUTH 应答返回。
- 单条命令的 `AUTH_MATCH` 与会话的 `AUTH_NOW` 分开记录；离场清除会话授权，但保留本批认证/写密码页统计，直到下一批日志开始。
- 命令处理移入无硬件依赖的 `main/NtagCommands.h`，使用真实生产函数测试认证、读取、写入、边界和应答格式。
- 新增激活阶段转换、停留时间及按阶段分组的服务耗时统计；只在 RAM 中记录，不增加场内 I2C 读取。
- 恢复监听失败时停止调用 NFC 更新，每隔至少 1 秒检查无外部场后重试；保留串口诊断和导出功能。
- 自动会话保存连续失败 3 次后暂停，保留 RAM 记录；串口 `save` 可手动保存并重新开启自动尝试。无场检测最小间隔为 100 ms。
- 保留 400 kHz、128 字节预装、60 ms 补充时限和 540 字节缓冲，避免同时改变多项射频变量。

修改前备份：`backups/pre-flash-log-v4/`。源码修改不涉及系统安装的 NFC 库。

启动时从 `main/AmiiboData.h` 加载用户提供的黄昏公主塞尔达 540 字节数据，UID 与原始文件一致。运行时写入仅保留在 RAM，重启恢复内置数据。

原始固件备份：`backups/pre-embedded-amiibo/`。

## Bruce 对照优化

- 对照 BruceDevices/firmware 的 ST25R3916.cpp（2026-09-28 获取）。其 Amiibo 认证使用 UID 派生密码，匹配后返回 PACK 8080。
- 本项目仅在已知 Amiibo 的 PWD 与 PACK 六字节全部为零时，于启动 RAM 副本中恢复认证字段。内置原始数组不变；显式密码/PACK 不覆盖。错误密码仍 NAK 04，运行时改写密码/PACK 后立即使用新值。
- 跨密码页的 FAST_READ 使用复用缓冲区、批量复制和清零，避免每次分配 508 字节栈缓冲及逐字节判断；普通读取继续直接发送内存。
- 保留 MRT=02、4-bit 无 CRC ACK/NAK、场内不绘屏/打印、尾页回绕和隐藏密码页。
- Bruce 的 TXE 等待不能直接套用：当前库集中读取/缓存 IRQ，直接读取会消费状态机中断。本版不修改全局库，SENT 仍只表示发送命令成功。

修改前备份：`backups/pre-bruce-auth/`。

## v3：长帧发送与内部 Flash 日志

- 参考 ST25R3916 RFAL 的 FIFO 分段补充机制独立实现发送：先装入最多 128 字节并启动射频发送，再按 FIFO 剩余空间补充。短帧仍一次装入；长帧无需等待全部数据通过 I2C 后才启动。
- FAST_READ 支持所有合法页范围（0x00～0x86），包括 512/516/540 字节响应。密码和 PACK 页继续返回零，READ 尾页回绕不变。
- 补充数据时检查 FIFO 空、上溢/下溢、总时限和 I/O 失败。只读取 FIFO 状态，不读取会清除的中断寄存器，保留库的 IRQ 状态机。
- 未把“已装入 FIFO”当成发送完成或 Switch 已接收；日志仍明确标为 `SENT=queued only`。
- 命令缓存扩大至 256 条，状态变化缓存 64 条；溢出时保留最近记录，并报告丢弃数量。日志不记录 PWD_AUTH 密码、写入内容、PACK 和完整 dump。

修改前备份：`backups/pre-flash-log-v3/`。未修改系统安装的 M5Unit-NFC 库。

### Flash 中的 /log

Arduino 固件使用内部 Flash 的 LittleFS，无需 SD 卡。默认分区方案的 `spiffs` 分区被用于 LittleFS；无需上传文件系统镜像。

启动先尝试挂载已有 LittleFS。仅在扫描确认**整个分区均为擦除态 FF**后才自动初始化；已有 SPIFFS/损坏的非空分区不会自动格式化，串口会显示原因，屏幕显示 `ERROR (serial)`。不要为取日志执行整片擦除。

文件示例：`/log/nfc-00000001.txt`，按编号递增，重启后继续编号。每次启动保存 BOOT 记录，之后保存 NFC 会话记录；最多保留最近 8 份完整日志。文件先写 `.tmp`，成功后改名 `.txt`，写入失败保留 RAM 记录重试。时间为本次开机后的毫秒数，BOOT 随机标识区分不同开机。

通信时仅记录 RAM。状态回到 Off/None 且空闲至少 1 秒后检查外部射频场，再暂停监听、写文件并恢复目标模式和接收计时配置。自动保存尝试间隔至少 5 秒；连续失败 3 次后停止自动尝试，因此扫描后请移开 Switch 并等待约 5 秒；若库正在等待会话超时，可能需要更久。Flash 写入或串口导出期间监听暂停，完成后可再次扫描。

日志保留最新命令、状态变化、认证匹配结果、响应长度、发送错误、回调处理耗时及 FIFO 开始发送耗时。RAM 记录在落盘前断电会丢失。分区不足、未挂载或写入失败不会假报保存成功。

### 编译、上传与取日志

默认硬件仍为 UnitNFC（ST25R3916 I2C），板型 Cardputer，默认分区布局保持不变。

**I2C 时钟**：UnitNFC 走 I2C，提高总线时钟可以减少 I2C 操作耗时，但实际识别收益仍需对比验证。当前由 `Emulation.cpp` 的 `NFC_I2C_CLOCK` 常量显式设为 **400000**，不依赖库的默认配置。若出现 I2C 通信错误（串口 `endTransmission` 报错），可将该常量改为 `100000` 做对照验证。

```powershell
arduino-cli compile --jobs 8 --fqbn m5stack:esp32:m5stack_cardputer .
pwsh -File tools/run_tests.ps1   # 宿主机 constexpr 测试，需先跑通再上传
```

1. 上传后确认串口版本为 `flash-log-v5`，屏幕显示 `Log: /log (Flash)`。
2. 确认串口出现 `NFC I2C clock: requested 400000, active 400000`，且 BOOT 记录里的 `BUS_CLOCK` 与之一致（这些字段表示软件配置；实际总线频率需测量）。
3. 使用 Switch 扫描，移开后等约 5 秒；串口应显示 `Flash log: saved /log/nfc-xxxxxxxx.txt`。
4. 串口监视器使用 115200，命令结尾选择换行。发送 `logs` 列出文件，发送 `logcat` 输出最新文件，或指定 `logcat nfc-00000002.txt`。
5. `diag` 输出当前保留的 RAM 诊断，包括尚未落盘的记录；`save` 手动保存待写日志，并重启自动保存尝试。无场且暂停监听后执行；恢复失败时仍可读取诊断。

6. 复制从 `==== FILE ... ====` 到 `==== END NFC diagnostic ====` 的内容用于排查；如最新文件只有 `EVENT=BOOT`，先扫描并等待会话日志落盘。

**基线采集**：每次只改一个时序参数，保持主机、位置和距离一致，改动前后各扫描 10 次。保存每次完整日志及是否识别的结果；手机对照使用同一版固件。

### 如何把日志导出到电脑

日志只在设备内部 Flash。**导出只有一条通道：串口。** 本固件不读 SD 卡、也没有 USB 大容量存储，所以必须通过串口命令把内容读出来。三种方式，按方便程度排列：

#### 方式一：手工复制（单份日志、临时排查）

1. 关闭 Arduino IDE 的串口监视器（或任何占用该端口的程序）。**同一端口只能被一个程序占用**，否则打开时报 `Access denied`。
2. 用任意串口终端打开该端口：**115200、8N1、结尾换行（CR 或 LF 都可以）**。可选：Arduino IDE 监视器、`arduino-cli monitor -p COMx -c baudrate=115200`、PuTTY、MobaXterm。
3. 发送 `logs` 查看现有文件与剩余空间，输出形如：
   ```
   FLASH_LOG=saved TOTAL=1048576 USED=21632
   /log/nfc-00000003.txt 2411 bytes
   /log/nfc-00000002.txt 3180 bytes
   ```
4. 发送 `logcat`（默认最新一份）或 `logcat nfc-00000002.txt`（指定文件，**必须写全 16 个字符的文件名**，`nfc-2.txt` 这种简写不被接受）。
5. 终端会输出以标记包起来的内容，把这两行之间的全部文字复制出来：
   ```
   ==== FILE /log/nfc-00000003.txt ====
   ...日志正文...
   ==== END NFC diagnostic ====
   ```
   **这两行标记是最可靠的抓取边界**，建议连标记一起复制，后续可自动切分。
6. 提示：Arduino IDE 监视器建议关闭"自动滚动"，否则滚屏时难以选中；输出较长（单份日志可达几万字符，命令记录最多 256 行）时不要误以为卡死，等它输出完。

> 如果输出里出现 `Log not found`，说明文件名写错了或该文件已被轮转删除（只保留最近 8 份）。如果出现 `FLASH_LOG=<错误信息>`，说明文件系统未就绪，看屏幕底行与启动日志。

#### 方式二：脚本抓取（需要多份日志、要留档时用）

```powershell
# 抓取一份最新日志到 dump.txt
pwsh -File tools/serial_capture.ps1 -Port COM5 -Command logcat -ExitAfter 3000 -Out dump.txt

# 交互式：打开后自己在窗口里敲 logs / logcat，Ctrl+C 结束
pwsh -File tools/serial_capture.ps1 -Port COM5 -Out dump.txt

# 把一个含多份日志的抓取文件切成单独文件（自动识别 ==== FILE ... ==== 标记）
pwsh -File tools/serial_split.ps1 -In dump.txt -OutDir docs/baseline
```

- `-Port` 省略时自动选择第一个串口；多串口时请显式指定。
- `-ExitAfter 3000` 表示响应接收过程允许最多 3 秒无新数据，并在所有命令完成后退出；不是整份日志的总时限。`logcat` 收到结束标记后即可完成，缺少结束标记时超时报错。省略此参数时命令完成后继续交互监听，默认空闲超时为 3 秒。
- `-ResponseTimeout 30000` 为等待首个有效导出 FILE 标记（其他命令为首批数据）的时限，默认 30 秒；设备持续处于射频场内时可适当加大。`-Seconds` 可设置整个抓取过程的硬时限，命令未完成就到期会报错并保留已抓取内容。
- `serial_split.ps1` 按日志编号命名输出文件（`nfc-00000003.txt`），已存在时默认跳过，可用 `-Force` 覆盖。

#### 方式三：手动重定向（不想用脚本）

任何能重定向输出的终端都可以，例如：
```powershell
arduino-cli monitor -p COM5 -c baudrate=115200 > dump.txt
```
缺点是会一直抓取、需要手动 Ctrl+C，且包含从打开端口起的全部回显噪声。

#### 相关注意点

- **读取期间监听暂停**：`logcat` 会先暂停 NFC 再恢复，所以导出时不要同时用 Switch 扫描。
- **日志内容不含密码与写入数据**：命令记录只保留命令码与页号，`PWD_AUTH` 的密码、写入内容、PACK 和完整 dump 都不写入文件。唯一例外是长按 A 打印到**串口**的完整内存，那部分**不会**进 Flash 日志。
- **时间戳是本次开机后的毫秒数**（`UPTIME_MS`、每条记录的起始数字），不是墙上时间；用 BOOT 随机标识 `BOOT=` 区分不同的开机批次。
- **要更多历史**：只有 8 份。需要长期留存就每次抓完及时复制走，或建立 `docs/baseline/` 目录按日期归档。
- 串口脚本仍需设备验证；使用前确认串口名与 .NET `SerialPort` 可用。`tools/run_tests.ps1` 已在本机执行通过。


`tools/run_tests.ps1` 自动发现 `tests/*_test.cpp`。全部 FAST_READ 页范围的断言已分组，在 GCC 默认 constexpr 运算预算内执行。

长按 A 输出 RAM 诊断及变化页，并尝试保存诊断（变化页仅串口显示，不写入 Flash），成功自动落盘后仍可查看上一批诊断。单击 A 仍会输出含密码页的全部内存，**不会写入日志文件**。

日志字段：

- `AUTH_MATCH=-1/0/1`：本条命令未比较密码／密码错误／密码匹配。
- `PWD_AUTH_SEEN`／`AUTH_OK`：本批收到的 PWD_AUTH 次数／其中密码匹配的次数；`WROTE_PWD_PAGE`：本批改写过密码页或 PACK 页的次数；`AUTH_NOW`：导出时当前会话是否已认证（0/1）。离场只清除此状态，前三项仍保留本批历史。这四个字段用于判断读卡器究竟走到了哪一步，无需逐行翻命令记录。
- `RESUME_FAIL`／`RESUME_STEP`：日志落盘后重启监听失败的次数与最后失败的步骤。`RESUME_STEP` 取值 `configureEmulationMode`／`emu.begin`／`configure_listener_timing`／`recovery`。非零表示曾出现该故障，故障不再导致停机（见下）。
- `TX_START_US`：进入回调至发送命令返回；`HANDLE_US`：回调整体耗时。均不包含进入回调之前的延迟，也不代表实测射频首位时刻。
- `TX_RESULT=0`：已入队；1：无发送；2：准备失败；3：FIFO 状态读取失败；4：FIFO 错误；5：补充前 FIFO 已空；6：补充超时；7：补充写入失败。
- `MAX_SERVICE_US`：**本批次**最大的 NFC 更新耗时；`MAX_SERVICE_US_SESSION`：本次开机以来的峰值（不受新批次清零影响）。`COMMAND_DROPPED`、`STATE_DROPPED`：RAM 环形缓存覆盖的条数。
- `BUS_CLOCK`：配置的 I2C 时钟；BOOT 在总线初始化后保存，便于对比固件参数，但不替代总线测量。
- `READY_TO_ACTIVE`／`READY_TO_OFF`：主循环观察到的 Ready→Active／Ready→Off 次数。
- `ACTIVE_TO_OFF`／`ACTIVE_OFF_NO_CMD`：观察到 Active→Off 的次数／其中未进入应用命令回调的次数。不能区分读卡器没发命令与底层丢帧。
- `MAX_READY_MS`／`MAX_ACTIVE_MS`：本批已结束的对应状态区间的最大采样时长；逐条 `PREV_STATE`／`PREV_MS` 表示前一采样状态及停留时间。
- `MAX_SERVICE_US_BY_STATE`：按调用更新前的状态分组，包含 Units.update 和 emu_a.update；不代表射频收发时延。
- `RECOVERY_PENDING`：是否等待恢复；`LISTENER_RUNNING`：软件是否允许更新监听。Flash 保存已暂停监听，因此会话文件中的 `LISTENER_RUNNING=0` 是正常的保存现场。
- `SAVE_FAILURES`／`AUTO_SAVE`：此前连续会话保存失败数／自动保存是否暂停；当前成功保存会清零失败计数。
- `LISTENER_TRACE`：是否编译启用库的串口 trace。

### 维护失败不再停机

写日志需要先暂停监听再恢复。恢复目标模式、重新 `begin` 或重配接收计时失败时不调用 `failStop()`；停止调用 NFC 更新，并在无外部场时每隔至少 1 秒重试。串口仍可执行 `diag`、`logs`、`logcat`。失败时尝试单独保存 `EVENT=RESUME_FAIL`；自动恢复重试本身不写 Flash。失败计数直到下一份会话诊断成功落盘后才清零。

### 密码页的可见性

密码页 0x85 与 PACK 的两个字节在 READ/FAST_READ 中始终返回零，认证成功也不例外；PWD_AUTH 成功后直接返回 PACK。屏蔽范围为 532..537，0x86 的后两字节是 RFUI，动态锁位位于 0x82。依据：[NXP 数据手册](https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf) §8.8、§10.2–10.3、表 8。

### 对“Active 但没有命令”的进一步诊断

先导出完整会话日志，查看 `READY_TO_OFF`、`ACTIVE_OFF_NO_CMD` 和各状态服务耗时。它们只描述主循环看到的阶段，不能据此认定 FWT 超时、天线故障或密码问题。

若仍无法定位，本机已安装的 NFC 库支持可选串口 trace，可对整个编译添加宏（默认不开启）：

```powershell
arduino-cli compile --jobs 8 --fqbn m5stack:esp32:m5stack_cardputer --build-property 'compiler.cpp.extra_flags=-DM5_UNIT_NFC_EMULATION_TRACE' .
```

此开关应同时传给应用和库的编译，不能只在 `.ino` 中定义。扫描时保留实时串口的 `emulation trace`、`ready_irq`、`PTA` 和 `A ERR` 行；底层 trace 仅输出串口，不会自动进入 Flash 日志。库在退出场时打印 trace，可能影响紧接着的轮询，所以仅用于诊断对照；未启用的构建不产生这部分输出。

原生 ESP-IDF 示例路径依赖上级仓库，当前工作目录不完整；新增 Flash 日志面向已配置的 Arduino 固件，原生 IDF 不启用此存储功能。

## 搜索与对照依据

- [Flipper 官方 NFC-A HAL](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/targets/f7/furi_hal/furi_hal_nfc_iso14443a.c)：监听初始化设置 MASK_RX_TIMER=02；普通帧发送等待 TXE。
- [Momentum NTAG/Ultralight listener](https://github.com/Next-Flip/Momentum-Firmware/blob/dev/lib/nfc/protocols/mf_ultralight/mf_ultralight_listener.c)：校验密码，成功后回复配置中的 PACK。
- [Bruce 官方 ST25R3916 驱动](https://github.com/BruceDevices/firmware/blob/main/src/modules/rfid/ST25R3916.cpp)：同样设置 MASK_RX_TIMER=02，并处理 GET_VERSION、READ、FAST_READ、PWD_AUTH。
- [pixl.js](https://github.com/solosky/pixl.js/blob/main/fw/application/src/ntag/ntag_emu_v2.c)：使用 nRF52 内置 NFC；其固定认证回复不作为本版默认行为。
- [nfc-emu](https://github.com/solosky/nfc-emu)：支持 NTAG215/TagMo 的另一开源模拟器；本次未移植其硬件实现。

仅按协议行为和寄存器配置独立修改当前项目，没有复制外部固件实现。
未移植 Flipper 的透明模式发送或 SPI 中断架构；I2C 响应延迟已通过提高总线时钟与显式配置降低，但仍未实测确认是否满足读卡器的帧等待时间。

## 限制与回退（v8 及以前）

FAST_READ 已支持完整 540 字节流式装载，但尚未进行硬件射频验证；I2C 调度或读卡器超时仍可能影响识别。
**Switch 识别和故障恢复仍需实机验证**；本版已撤回认证后公开密码的错误行为。首次实测请保留完整日志，并优先确认 Switch 是否识别。
锁定配置会保存，但不完整执行真实芯片写保护、AUTH0/PROT 或 AUTHLIM。
READ_SIG 未配置签名时返回 NAK，可通过派生类 set_signature() 配置匹配 UID 的 32 字节签名；计数器仍固定为零。编译通过不能替代手机和 Switch 实测。

上轮应用代码备份：`backups/pre-listener-timing/Emulation.cpp.bak`。
上轮库原文件备份：库目录 `unit_ST25R3916.hpp.ntag215.bak`、`.cpp.ntag215.bak`。
`tools/install_irq_observer.py` 仅保留用于历史回退，不要在当前版本运行。

## 历史优化项与 v4 更正

| 项 | 内容 |
|---|---|
| A1 | 显式设置 `NFC_I2C_CLOCK = 400000`；串口与 BOOT 记录均输出配置时钟，默认值取决于所用库版本 |
| E1 | 原建议错误，v4 已纠正：认证前后 READ/FAST_READ 均屏蔽 PWD/PACK；离场仅清会话授权 |
| B1 | 恢复监听失败不再 `failStop()`，改为记录 `RESUME_FAIL`/`RESUME_STEP` + 自动重试 + 单独落盘 `EVENT=RESUME_FAIL` |
| B7 | `MAX_SERVICE_US` 明确为本批最大值，新增不受批次清零影响的 `MAX_SERVICE_US_SESSION` |
| E2 | 汇总行新增 `PWD_AUTH_SEEN`/`AUTH_OK`/`WROTE_PWD_PAGE`/`AUTH_NOW` |
| E3 | 编译期测试入口为 `tools/run_tests.ps1`；基线步骤与日志判读见本文 |
| D1 | **原判定有误**：该处并无重复保存。实际修正为按键落盘失败时不再打印上一次的文件名（会被误读为成功） |

## 验证

`tests/amiibo_auth_test.cpp` 使用 C++14 编译期断言验证 UID 字节映射、显式/部分凭据保留、真实 dump 的改动范围与重复初始化行为。
`tests/nfc_protocol_test.cpp` 验证全部 135 个页的密码恒屏蔽与读取回绕、全部 9180 个有效 FAST_READ 页范围（含单页）只屏蔽 532..537 这 6 个字节、1～540 字节逐长度 FIFO 装载、两种排空速度、计时回绕及 10 类故障路径。该测试模拟 FIFO，不能证明实机的射频时序。
参考源码、许可证及最近文件修订记录保存在 `references/`。主机测试及编译无法证明 Switch 射频交互成功，仍需实机扫描。
`tests/ntag_commands_test.cpp` 覆盖正确/错误认证、认证后密码仍隐藏、离场保留统计、密码改写、锁位单向置位、UID 保留、命令边界与缓冲容量。
`tests/listener_diagnostics_test.cpp` 使用本次实机日志中的时间序列验证激活诊断，并验证批次重置、回调与状态采样顺序及计时回绕。
四组编译期测试已在本机通过。最终源码已通过 `m5stack:esp32:m5stack_cardputer` 完整编译：程序 666159 字节（50%），静态 RAM 38292 字节（11%）。固件位于 `.build-tmp/flash-log-v4/Emulation.ino.bin`，编译日志保存在 `.build-tmp/flash-log-v4-build.log`。尚未烧写，实机识别仍需另外验证。

## 2026-09-30 GitHub 对照记录

- [ST25R3916 RFAL](https://github.com/stm32duino/ST25R3916/blob/main/src/rfal_rfst25r3916.cpp)：核对 FIFO 深度、分段补充和发送未完成时的错误处理。本项目使用 FIFO 状态轮询以避免消费 M5 库的中断缓存，没有替换成 RFAL 驱动。
- [Pixl.js NTAG 模拟](https://github.com/solosky/pixl.js/blob/main/fw/application/src/ntag/ntag_emu_v2.c)：核对 GET_VERSION、READ、FAST_READ、PWD_AUTH。其固定认证成功和 nRF 原生 NFC 架构没有照搬；当前继续校验密码。
- [Flipper NTAG listener](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/lib/nfc/protocols/mf_ultralight/mf_ultralight_listener.c)：核对秘密页屏蔽和边界行为。检索时其 FAST_READ 实现最多 64 页，因此不把 Flipper 当作完整 540 字节发送的依据。
- [Bruce 当前驱动](https://github.com/BruceDevices/firmware/blob/main/src/modules/rfid/ST25R3916.cpp)：当前主分支改为 RFAL fork，文件注明尚未接入卡模拟；上一轮保存的 Bruce 实现仅作为历史参考。

检索到的固定版本源码和来源保存在 `references/2026-09-30/`；仅按协议和芯片操作行为独立实现，没有复制外部固件函数。

新增 `tests/nfc_protocol_test.cpp` 使用 C++14 编译期执行生产读取/发送辅助代码：验证全部 65536 对页参数、读取回绕、秘密页屏蔽、1～540 字节逐长度 FIFO 装载、两种排空速度、计时回绕及 10 类故障路径。该测试模拟 FIFO，不能证明实机的射频时序。原认证测试继续保留。


### M5Unit-NFC 0.1.2 派生类适配

`main/Ntag215Emulator.h` 提供独立的 `amiibo::Ntag215Emulator<Unit>`，继承
`EmulationLayerA` 并重写 `receive_callback()`；`Emulation.cpp` 中的派生类仅追加 RAM 诊断。
PWD_AUTH/PACK、READ_CNT、READ_SIG、GET_VERSION、READ/FAST_READ、WRITE 在标签层处理；
合法 HLTA 委托基类。不能直接把 READ/WRITE 交回通用基类，否则会丢失秘密字段屏蔽、
尾页回绕和 4-bit ACK/NAK。未知命令返回 NAK，避免基类未知命令串口输出。

- 不修改系统库：HALT/WUPA bitrate、RX 缓冲/边界检查、RF 活动超时和 millis 回绕由本地 0.1.2 负责。
- 显式保留 10 秒无活动超时；begin/end/update 离开 Active 时清除认证状态。
- 最多 128 字节的数据响应调用受保护的 transmit()；更长响应保留 FIFO 流式补充。
  transmit() 返回值只表示提交结果，参数 4 不代表适配器保证 4 ms 内完成射频发送。
- ACK/NAK 仍用无 CRC 的 4-bit 发送，发送失败与数据响应一样执行停止清理。
- READ_SIG 缺失签名时返回 NAK，不再返回全零。取得原标签匹配 UID 的签名后，在 setup 中调用
  `emu_a.set_signature(signature)`；数组必须有 32 字节并在模拟期间保持有效。540 字节 image 不包含此独立数据。
- AUTH0/PROT/完整永久锁规则、AUTHLIM、真实计数器仍未实现；本次适配不宣称完整 NTAG215 协议兼容。
- 应用日志只写 RAM，底层可选串口 trace 仍仅用于诊断；启用后不能承诺没有时序影响。

验证：命令测试覆盖签名缺失 NAK 与非零签名原样返回，保留认证、读取和 FIFO 回归测试。
实际 RF 时序与 Switch 成功率仍需硬件验证。


### v7：READY / ACTIVE 无命令的 RAM 射频诊断

构建必须使用项目内 `dependencies/M5Unit-NFC`（来源为本机 0.1.2，原文件 SHA256 记录在
`upstream-sha256.json`）。系统安装库未修改。Arduino CLI 命令：

```powershell
& 'F:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe' compile --config-file 'C:/Users/17036/.arduinoIDE/arduino-cli.yaml' --jobs 8 --fqbn m5stack:esp32:m5stack_cardputer --library 'F:/0.MyProject/M5adv/Emulation/dependencies/M5Unit-NFC' --build-path 'F:/0.MyProject/M5adv/Emulation/build' .
```

请勿直接用未选择本地库的 Arduino IDE 构建；缺少 `NfcDiagnosticHook.h` 会明确失败。
不启用 `M5_UNIT_NFC_EMULATION_TRACE` 串口模式；热路径已知的接收错误及超时打印改为 RAM 事件。

- `RF`：256 条微秒时间戳事件，包含已有 IRQ/PTA/bitrate 读取结果、底层状态 trace、FIFO 大小/读取结果、接收拒绝和超时。
  只记录元数据，不保存命令负载或密码。没有为诊断增加任何寄存器读取。
- `IRQ_OBSERVED`：各 IRQ 位在成功读取中出现的次数；硬件位可能合并事件，此数字不是精确 RF 帧数。
- `IRQ_READ_FAILURES / FIFO_READ_FAILURES / RX_REJECTED / EXPIRY_COUNT`：区分总线失败、帧拒绝及无活动超时。
- `MAX_UNITS_US / MAX_LISTENER_US / MAX_POLL_GAP_US / SERVICE_GE_2MS`：区分 IRQ 轮询、状态机处理和服务间隔；维护暂停间隔不计入。
- `RAM_HOOK_VERSION / RAM_HOOK_CALLS`：确认诊断钩子实际接入；调用数包括初始化，RF 会话环仅记录监听期间事件。
- `SNAPSHOT=MAINTENANCE_PAUSED`：说明导出/写 Flash 时监听已暂停，避免误读 `LISTENER_RUNNING=0`。
- 状态环由 64 扩为 256；`RF_DROPPED / STATE_DROPPED` 明确标记覆盖数量，汇总计数保留。
- Active 收帧读取失败、部分读取或缓冲截断时先清 FIFO/恢复接收，再回到 Idle/Halt，不把残缺帧交给命令处理。

这些时间戳来自软件观察，不能视为实际 RF 起止时间或 FDT 测量。
运行数次扫描、移开读卡器并等至少 5 秒，再读取 `logs` / `logcat`。
升级仅写入固件相关区域，不擦除日志分区；保留最近 8 份文件的轮换策略仍然生效。


### v8：参考 RFAL 的 Active 接收路径优化

参考 [stm32duino/ST25R3916 RFAL](https://github.com/stm32duino/ST25R3916/blob/main/src/rfal_rfst25r3916.cpp)
的 `RFAL_LM_STATE_ACTIVE_A/ACTIVE_Ax`（2026-10-09 查阅），并对照
[M5Unit-NFC adapter](https://github.com/m5stack/M5Unit-NFC/blob/main/src/nfc/layer/a/emulation_layer_a_ST25R3916.cpp)。
仅独立实现局部改动，未替换驱动或复制 RFAL 源文件。

- RFAL 同样优先处理 EOF，保留该行为。新增 `ACTIVE_EOF_RXE`，记录 Active 中因场消失而未分发的 RXE；该计数不代表一定收到完整有效命令。
- Active 在 RXE 后读取一次 FIFO 状态，再通过新增 `readFIFOExact()` 读取已验证长度，省去旧 `readFIFO()` 内部的重复状态读取。原 API 及其他协议路径保留。
- `readFIFOExact()` 要求调用方先验证状态、确保缓冲容量，且中间没有其他 FIFO 消费者；传输失败不分发任何部分数据。
- `NfcRxFrame.h` 在去掉两字节 CRC 后检查负载容量；拒绝不完整字节、短帧及超限长度。256 字节缓冲可容纳 258 字节含 CRC 帧。
- 无效帧直接清 FIFO/恢复接收，不再为了诊断读取无效负载。避免额外总线开销和秘密数据记录。
- 新增 `RX_ACCEPTED` 累计值及事件 12/13；hook 版本升级到 2，旧日志仍可依据各自图例阅读。
- 保持 I²C 400 kHz、RX mask 和现有发送路径。预计减少一次状态读取的总线开销；具体改善幅度及 Switch 成功率必须通过实测确认。

验证：7 组编译期测试通过，包括 0..600 字节边界、1..7 个不完整位、GET_VERSION/WRITE 负载、FIFO 硬件上限及计数跨环覆盖/清零。
AUTH0/PROT/完整锁规则、AUTHLIM 和真实计数器仍未补全，签名仍需原标签数据；本版本聚焦最近 RF 日志暴露的接收路径。
