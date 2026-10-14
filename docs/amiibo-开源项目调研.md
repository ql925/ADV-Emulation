# 面向 Cardputer ADV + M5Unit-NFC 的 Amiibo 模拟开源项目调研

日期：2026-10-13。目标硬件：M5Stack Cardputer ADV（ESP32-S3）+ Unit NFC U216（ST25R3916，**I²C**，地址 0x50），软件栈 `m5stack/M5Unit-NFC` 的 `EmulationLayerA`。

本机克隆目录：`.build-tmp/amiibo-research/`（14 个仓库，供后续 `git diff` / 源码对照）。上游库副本：`.build-tmp/upstream-m5unit-nfc-latest/`。

---

## 1. 结论摘要

**最重要的一条**：`ciniml/paper_name_plate` 证明**同一颗芯片、同一条 I²C 总线**上的 Type 2 Tag（NTAG216 类）模拟**已在实机被手机读和写验证通过**。这说明当前工程卡住的原因在软件时序，不在芯片或 I²C 方案本身。

但**没有任何仓库**给出「Cardputer ADV + Unit NFC U216 模拟 NTAG215 被某读卡器识别」的实测记录——包括硬件完全相同的 `silasgubi/CardputerADV-NFC`。当前工程的日志仍是这组硬件上最接近的实测数据。

---

## 2. 分层清单

### 2.1 第一层：直接对应本工程硬件

| 项目 | 硬件/总线 | 模拟能力 | 实机验证 | 许可 | 最后提交 |
|---|---|---|---|---|---|
| [silasgubi/CardputerADV-NFC](https://github.com/silasgubi/CardputerADV-NFC) | Cardputer ADV + U216，**I²C** | NFC-A，NTAG2xx/Ultralight | **无证据** | MIT | 2026-07-14 |
| [ciniml/paper_name_plate](https://github.com/ciniml/paper_name_plate) | PaperMono ESP32-S3 + ST25R3916，**I²C** | Type 2 / NTAG216 全命令集 | **是——手机读+写** | **无许可证** | 2026-09-06 |
| [7h30th3r0n3/Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project) | Cardputer ADV + Cap CC1101，**SPI** | UID + 完整 ISO-DEP 中继 + Classic 尝试 | 仅测试计划 | MIT | 2026-10-09 |

`CardputerADV-NFC` 的 `src/nfc_reader.cpp:93-147` 与本工程走同一条库路径（`cfg.emulation = true` → `emu_a.begin(epicc, mem, size)` → `emu_a.update()`），可直接作为「库的正确用法」对照，但作者是单次提交投递，**没有跑通证据**。

`paper_name_plate` 的 `src/t2t_emu.rs` 是本调研中最有价值的单文件：它是一个独立重写的 ST25R3916 Type 2 Tag 模拟器，状态机**明确声明对齐 `emulation_layer_a_ST25R3916.cpp`**，并额外给出两条硬信息：

- `DESIGN.md:118`：「I²C 总线启动 100 kHz → bring-up 后拉到 400 kHz **（因为标签模拟的响应期限）**」——明确说明 100 kHz 不够用。
- `t2t_emu.rs:254-260`：完全掉电的 Off 状态**在该板上不上报 `I_eon`**，所以他们保持振荡器开启。

**注意**：该仓库无 LICENSE 文件、无 SPDX 头，默认保留所有权利。**只可学习寄存器序列与设计，不可拷贝代码**。

### 2.2 第二层：NTAG215 语义与访问控制参考

| 项目 | 价值 | 许可 | 活跃度 |
|---|---|---|---|
| [RfidResearchGroup/ChameleonUltra](https://github.com/RfidResearchGroup/ChameleonUltra) | **最完整的 NTAG 语义参考** | GPL-3.0 | 极活跃（2026-10-07，508+ PR） |
| [solosky/pixl.js](https://github.com/solosky/pixl.js) | Amiibo 模拟实际做法 | GPL-2.0 | 2026-01-16 |
| [bettse/weebo](https://github.com/bettse/weebo) | amiitool 加密实现 | GPL-3.0 | 2026-08-01 |
| [cmdwtf/Odiin](https://github.com/cmdwtf/Odiin) | 反面教材 | ISC/0BSD | **已死**（2023-07） |

`ChameleonUltra` 的 `firmware/application/src/rfid/nfctag/hf/nfc_mf0_ntag.c` 实现了本工程尚未实现的全部策略，可作为**规格书**逐条对照：

- PWD/PACK 页屏蔽（`:604-617`）——认证后仍屏蔽，符合 NXP 行为
- AUTH0/PROT 门控（`:271-294`）
- AUTHLIM 计数（`:1023-1051`）
- 静态/动态锁位、CFGLCK（`:716-872`）
- 第 2 页锁位 OR 合并、第 3 页 OTP OR 合并（`:926-937`）
- GET_VERSION 与 READ_SIG 的 CRC 处理（`:391-415`）

**GPL-3.0 是传染性许可**：可读、可按算法独立重写，**不可拷贝代码进本工程**。

`Odiin` 是反面教材：`PWD_AUTH` **接受任意密码**，`READ_SIG` 返回硬编码假签名，**零访问控制**，PWD/PACK 通过 READ 直接泄漏。虽为 ISC 许可（可安全拷贝），但实现过于残缺，不值得借鉴。

### 2.3 第三层：数据、密钥与公式

| 项目 | 提供 | 许可 |
|---|---|---|
| [Lanjelin/AmiiboConverter](https://github.com/Lanjelin/AmiiboConverter) | PWD 公式、UID 随机化、.nfc 格式 | GPL-3.0 |
| [AmiiboDB/Amiibo](https://github.com/AmiiboDB/Amiibo) | .bin/.nfc 数据库 | 无 |
| [HiddenRamblings/TagMo](https://github.com/HiddenRamblings/TagMo) | 安卓端实现 | GPL-3.0 |

`AmiiboConverter` 本身**不含加密代码**，它依赖 `pyamiibo` 包（`pyproject.toml:26`）。

### 2.4 第四层：ST25R3916 驱动 / I²C 时序

| 项目 | 价值 | 许可 |
|---|---|---|
| [hydrabus/hydrafw_hydranfc_shield_v2](https://github.com/hydrabus/hydrafw_hydranfc_shield_v2) | **完整 RFAL + ST25R3916 listen 模式** | 见仓库 |
| [wilson-elechouse/ST25R3916](https://github.com/wilson-elechouse/ST25R3916) | ESP32 I²C 路径实测文档 | **ST MIX MYLIBERTY（非开源）** |
| [stm32duino/ST25R3916](https://github.com/stm32duino/ST25R3916) | RFAL Arduino 封装 | NOASSERTION |
| [mcqn/nfc-st25r3916](https://github.com/mcqn/nfc-st25r3916) | 硬件设计（仅读卡器） | CERN-OHL-S-2.0 |

`ST25R3916-esp32/docs/i2c_validation_results.md:194` 给出独立佐证：**100 kHz 下 MF1 数据块写入无法可靠完成；400 kHz 下通过**。该仓库**不是开源许可**，只可阅读文档。

---

## 3. 关键技术结论：FDT 与硬件自动应答

这是本次调研中**最重要、也最能解释当前困境**的一条。

### 3.1 芯片把最紧的时序放进了硬件

ST25R3916 有 NFC-A **被动目标自动应答**机制：UID/ATQA/SAK 写入 PT_MEMORY 后，芯片**自行**应答 REQA/WUPA、防冲突和 SELECT，完全不经过主机。

本工程已正确使用（`dependencies/M5Unit-NFC/src/nfc/layer/a/emulation_layer_a_ST25R3916.cpp`）：

- `:277` `goto_off()` → `clear_bit_register8(..., d_106_ac_a)` —— **开启**自动应答
- `:357` `goto_active()` → `set_bit_register8(..., d_106_ac_a)` —— **关闭**自动应答，交给应用
- `:371` `goto_halt()` → 重新开启
- `:183-184` `load_config()` → `writePtMemoryA()` 写入 15 字节 PT 内存

`hydrafw` 的 RFAL 实现走同一条路（`rfal_rfst25r3916.c:3276-3459` `rfalListenStart`，`:3295` `autoResp` 位运算，`:3339` `st25r3916WritePTMem`）。**两套独立实现一致，可确认这是正确架构。**

### 3.2 时序预算

| 项 | 数值 |
|---|---|
| FDT(PICC listen, n=9) | `RFAL_FDT_LISTEN_NFCA_POLLER = 1172` / 13.56 MHz = **86.4 µs** |
| 单次 I²C 寄存器写（400 kHz） | ~50 µs |
| 单次 I²C 寄存器读（400 kHz） | ~75 µs |

**一次 I²C 事务就已经超过 FDT。** 若应用命令需要软件应答，6~7 次总线操作合计约 570 µs，是 FDT 的 6.6 倍。

**但这不是致命问题**，原因有两点：

1. **REQA/防冲突/SELECT/SAK/HLTA 由硬件应答**，不经软件。最紧的那一段已经是硬件。
2. 应用命令（READ/FAST_READ/PWD_AUTH/…）受读卡器的 **FWT/超时**约束，而非 86.4 µs FDT。FWT 量级是毫秒到秒。

**真正的风险**是主机在两次轮询之间「失联」太久。本工程日志（`logs/com7-v7-rf-latest-20261009.txt:366`）显示：

```
MAX_UNITS_US=586 MAX_LISTENER_US=2872 MAX_POLL_GAP_US=3445 SERVICE_GE_2MS=7
```

`MAX_POLL_GAP_US=3445` —— **3.4 ms 的轮询空档**。对照 `Evil-M5Project` 的经验（`:50520-50521`、`:50689-50692`）：他们**仅仅因为在轮询循环里重绘一次屏幕**，就导致读卡器错过 ATS 后的第一帧、ISO-DEP 激活中止。该固件随后把重绘节流到约 5 次/秒。

`Evil-M5Project` 对 FDT 的自我说明（`src/nfc_mfemul.h:30-33`）：

> "What CANNOT be validated without a real reader on hardware is the ISO14443-A Frame-Delay-Time (~86 us): our response is driven from firmware over SPI (nfc_emul_poll -> handle -> TX), and if that round trip exceeds FDT a strict reader may abort after nt."

**注意该固件用的是 SPI，不是 I²C。** 本工程的 I²C 每次事务成本更高。

### 3.3 nRF52 的做法无法移植

`ChameleonUltra` 依赖 nRF52 NFCT 外设的硬件帧延迟定时器，并在软件慢时**主动放宽硬件期限**（`nfc_seos.c:750-753`）：

```c
// Increase maximum frame delay
// Without this, if the command took too long,
//  the nRF firmware will just... never respond
NRF_NFCT->FRAMEDELAYMAX = 135600; // 10ms
```

**ST25R3916 没有等价寄存器**（`fdel<3:0>` 只是静态 4 bit 修正，MRT 是接收屏蔽计时器）。这条技巧不可移植。可移植的是**架构**：把 FDT 关键路径全部推给硬件自动应答，让软件离开关键路径——本工程已经这样做了。

---

## 4. 对本工程的具体建议

按「证据强度 × 可操作性」排序。

### P0：`READ_SIG` 不应返回 NAK

**当前行为**（`main/NtagCommands.h:86-87`）：

```cpp
case 0x3C:
    return rx_len == 2 && rx[1] == 0 && signature ? data_reply(signature, 32) : nak_reply();
```

未配置签名时返回 NAK。**真实 NTAG215 永远不会 NAK 这条命令。** 所有开源实现的处理方式：

| 实现 | READ_SIG 行为 |
|---|---|
| `pixl.js` `ntag_emu_v2.c:52-55,167-170` | 恒返回 32 字节**全零**，从不 NAK |
| `AmiiboConverter.py:144` | 写入 32 个零 |
| `Odiin` `nxp_ntag21x.h:153-159` | 假常量 `D3 C0 D3 00 C0 01 BA BE` ×4 |
| `ChameleonUltra` `nfc_mf0_ntag.c:1430-1433` | 独立 32 字节字段，默认全零 |

**建议**：默认返回 32 字节全零，保留 `set_signature()` 以便日后填入真标签签名。NAK 是协议偏离，严格的读卡器可能当作硬失败。

### P0：压缩 3.4 ms 轮询空档

这是当前最可疑的失败原因。方向：

- 检查 `loop()` 中是否有阻塞式绘制/串口输出落在 `emu_a.update()` 路径上
- 绘制与日志改为节流或移出轮询
- 用日志里的 `MAX_POLL_GAP_US` 作为回归指标

### P1：`Off` 状态保持振荡器

`paper_name_plate` `t2t_emu.rs:254-260` 记录：完全掉电（`en=0`）的 Off 状态**在该板上不上报 `I_eon`**，导致无法感知外部场。本工程 `goto_off()` 的 `en` 处理值得对照检查。

### P1：UID 第 3 字节避开 0x88

`weebo/weebo_common.c:25`：

```c
UID[3] |= 0x01; // To avoid forbidden 0x88 value
```

0x88 是 ISO14443-A 级联标志字节，出现在 UID 第 3 字节会破坏防冲突。本工程若随机化 UID 需注意（当前保留原始 UID，暂不受影响）。

### P2：补齐 AUTH0/PROT/AUTHLIM

本工程当前存储但不强制执行。**读路径不依赖这些**，故优先级低；若要协议完整性，按 `ChameleonUltra nfc_mf0_ntag.c` 的行为表独立实现（勿拷贝 GPL 代码）。

### P2：FAST_READ 边界

`pixl.js` 的 FAST_READ **完全没有边界检查**（`ntag_emu_v2.c:173-189`），会泄漏 PWD/PACK 与陈旧缓冲。本工程已实现完整 540 字节范围校验，**不要退回**。

---

## 5. 已核对的数据（本工程 Zelda dump）

`main/AmiiboData.h`，UID `04 4E 40 22 1F 3E 80`：

| 页 | 偏移 | 值 | 字段 |
|---|---|---|---|
| 0 | 0x000 | `04 4E 40 82` | UID0-2 + BCC0 |
| 1 | 0x004 | `22 1F 3E 80` | UID3-6 |
| 2 | 0x008 | `83 48 0F E0` | BCC1, internal, 静态锁 |
| 3 | 0x00C | `F1 10 FF EE` | Capability Container |
| 130 | 0x208 | `01 00 0F BD` | 动态锁 |
| 131 | 0x20C | `00 00 00 04` | CFG0，**AUTH0=4** |
| 132 | 0x210 | `5F 00 00 00` | CFG1，ACCESS=0x5F |
| 133 | 0x214 | `00 00 00 00` | PWD（被屏蔽） |
| 134 | 0x218 | `00 00 00 00` | PACK（被屏蔽） |

**AUTH0=0x04** 是 Amiibo 的定义性属性：密码保护从第 4 页起（即全部用户区）。`ACCESS=0x5F` 的 PROT 位为 0，故**读不保护、写需要 PWD_AUTH**——这正是 Switch 发起认证的原因。

### PWD / PACK

四个独立实现一致（`AmiiboConverter.py:169-172`、`pixl.js amiibo_helper.c:74-77`、`weebo_common.c:7-12`、`main/AmiiboAuth.h:16-20`）：

```
PWD[0] = 0xAA ^ UID[1] ^ UID[3]
PWD[1] = 0x55 ^ UID[2] ^ UID[4]
PWD[2] = 0xAA ^ UID[3] ^ UID[5]
PWD[3] = 0x55 ^ UID[4] ^ UID[6]
PACK   = 0x80 0x80
```

对本工程 UID：**PWD = `C6 0A B6 CA`，PACK = `80 80`**。`main/AmiiboAuth.h:11-22` 的恢复逻辑正确（仅在全零时恢复，保留显式凭据）。

### BCC 规则

```
BCC0 = 0x88 ^ UID0 ^ UID1 ^ UID2   （存于偏移 3）
BCC1 = UID3 ^ UID4 ^ UID5 ^ UID6   （存于偏移 8）
```

`pixl.js` 在 nRF52 上由硬件重算 BCC（`hal_nfc_t2t.c:358-391`），存储值不参与发送。本工程走 PT_MEMORY 路径，**BCC 必须正确写入**。

---

## 6. 关于密钥与「unfix」

- **密钥不在任何开源仓库中。** 仅有指纹可校验：`key_retail.bin` MD5 `45fd53569f5765eef9c337bd5172f937`；`unfixed-info.bin` / `locked-secret.bin` 的 SHA256 见 `pyamiibo/keys.py:13-16`。
- 「unfixed / fixed」指的是**两把主密钥**（data / tag），不是标签状态。`key_retail.bin` = 两者拼接（160 字节）。
- **「unfix」不是密码学操作**，而是「换 UID → 重新打包 → 清锁位」：新 UID 会带动 PWD/PACK 重算，两个 HMAC 与 AES-CTR 全部重算。
- `pixl.js` 与 `weebo` 都**没有** `unfix` 符号，且 `nfc3d_amiibo_copy_app_data`（写计数递增）在两者中都是**死代码**。社区所谓 unfix 的实现方式就是**随机化 UID**（`pixl.js docs/en/04-Using-Firmware.md:19`）。

**本工程保留原始 UID，无需密钥，也不需要 unfix。**

---

## 7. 许可注意事项

| 许可 | 项目 | 可否拷贝代码 |
|---|---|---|
| MIT | `M5Unit-NFC`、`CardputerADV-NFC`、`Evil-M5Project` | 可以 |
| ISC/0BSD | `Odiin` | 可以（但实现残缺） |
| GPL-2.0 / GPL-3.0 | `pixl.js`、`weebo`、`ChameleonUltra`、`AmiiboConverter` | **仅可读，须独立重写** |
| **无许可证** | `paper_name_plate` | **不可拷贝** |
| **ST MIX MYLIBERTY** | `ST25R3916-esp32` | **非开源**，仅可读文档 |
| CERN-OHL-S-2.0 | `nfc-st25r3916` | 硬件许可 |

---

## 8. 检索方法与覆盖

通过 GitHub API 检索了 30+ 组关键词与 7 个 topic（`amiibo`、`amiibo-emulation`、`ntag215`、`st25r3916`、`nfc-emulation`、`card-emulation`、`iso14443a`），克隆 14 个仓库逐一读源码。深度分析了其中 9 个。

**未发现**任何仓库同时满足：ST25R3916 + I²C + Amiibo/NTAG215 + 实机验证。

**局限**：所有克隆为 `--depth 1`，提交计数不可用，仅最后提交日期可信；`weebo` 的空中命令处理委托给 Flipper 固件的 `mf_ultralight_listener.c`，该文件不在克隆中，未作推断。
