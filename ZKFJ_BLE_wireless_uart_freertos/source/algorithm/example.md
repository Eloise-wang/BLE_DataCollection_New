# 上位机 ↔ 设备 命令协议（简化版，BLE/UART 都能用）

你只需要记住 4 件事：
- SOF 固定 0xA5
- 每条命令都带长度 LEN
- 每条命令都带 CRC16（防误触发）
- task_id 用 8 字节（TaskId64）
- RequestHistory 回数据时，增加 TOTAL_BYTES（总有效数据长度），上位机知道何时结束
- 协议层放在 source/protocol/，专门做“组帧/验帧/分发”
- 协议层 → 任务层：用一个简单的“命令队列”或“任务通知”把解析后的命令交给任务处理

## 1) 传输方式

- BLE 调试器里输入 HEX：实际发出去就是字节（byte[]），没问题
- UART 调试：也可以发 HEX（建议每个命令后加 `\n` 作为分隔）

## 2) 帧格式（所有命令统一）

字节序：所有多字节整数小端（Little-Endian）。

CRC16：CRC-16/CCITT-FALSE
- poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0x0000
- 校验范围：从 `CMD` 到 `PAYLOAD`（不含 SOF，不含 CRC 本身）

帧结构：
```
SOF(1)  CMD(1)  LEN(1)  PAYLOAD(N)  CRC16(2)
0xA5    xx      nn      ...         lo hi
```

LEN：PAYLOAD 的字节数（0~255）。

## 3) 应答规则（设备回上位机）

设备收到命令后会回一个“应答帧”，格式同上面一致，只是：
- CMD = 原 CMD + 0x80（例如请求 0x02，应答就是 0x82）
- PAYLOAD 固定 1 字节：STATUS

STATUS：
- 0x00 OK
- 0x01 CRC_ERROR
- 0x02 LEN_ERROR
- 0x03 CMD_UNSUPPORTED
- 0x04 PARAM_ERROR
- 0x05 BUSY
- 0x06 NOT_FOUND
- 0x07 STORAGE_ERROR

## 4) task_id（任务ID）

- task_id 使用 8 字节 TaskId64
- 上位机如果有 16 字节 UUID，可取 UUID 的低 8 字节作为 TaskId64

## 5) 命令列表（请求 CMD）

### CMD=0x01 初测（SelfTest）
PAYLOAD：
```
TASK_ID(8)
```

### CMD=0x02 开始采集（StartCollect）
PAYLOAD：
```
TASK_ID(8)
START_TIME_EPOCH_S(4)   // 上位机下发 Unix 秒；0 表示设备只用相对时间
DURATION_S(4)           // 采集总时长（秒）；0 表示直到 StopCollect
PERIOD_MS(4)            // 采样周期（毫秒），例如 1000 或 30000
```

### CMD=0x03 停止采集（StopCollect，提前结束）
PAYLOAD：
```
TASK_ID(8)
```
说明：停止产生新记录，存储任务可以把队列里已有数据写完。

### CMD=0x04 请求回收数据（RequestHistory）
PAYLOAD：
```
TASK_ID(8)
OFFSET(4)      // 从 data.bin 的偏移（字节）
MAX_BYTES(2)   // 本次希望设备返回的最大字节数
```

### CMD=0x05 清理任务数据（ClearTask）
PAYLOAD：
```
TASK_ID(8)
MODE(1)        // 0=仅删 data.bin；1=删 data.bin/meta.bin/log 并删目录
```

## 6) 历史数据返回（设备 → 上位机）

设备返回历史数据时，建议用一个非常简单的数据帧（与命令帧分开，避免混淆）：
```
SOF(1)=0x5A  LEN(2)  TASK_ID(8)  OFFSET(4)  TOTAL_BYTES(4)  DATA(N)  CRC16(2)
```

字段说明：
- LEN：这一帧中，从 TASK_ID 到 DATA 的总字节数（不含 SOF，不含 CRC）
- TOTAL_BYTES：该任务 data.bin 的总有效数据字节数（上位机用它判断何时结束）
- DATA：从 data.bin 里读取的连续字节

上位机判断“传输完成”的最简单方式：
- 当 `OFFSET + DATA长度 >= TOTAL_BYTES` 时，本次任务数据回收结束

CRC16 同样用 CRC-16/CCITT-FALSE（校验范围从 LEN 到 DATA）。

## 7) 协议层结构（建议放在 source/protocol/）

```txt
source/
├── protocol/
│   ├── proto_frame.h        # 定义帧格式/CRC/常量
│   ├── proto_frame.c        # 流式解析：喂字节 → 组帧 → CRC 校验 → 输出“完整命令”
│   ├── proto_cmd_handler.h  # 命令分发接口：cmd → 调用任务层动作
│   └── proto_cmd_handler.c  # cmd 处理：Start/Stop/History/Clear → 触发任务层
```

## 8) 如何优雅实现“协议层 → 任务层”

最简单好维护的方式：协议层只做两件事：
1) 把字节流解析成“命令结构体”
2) 把命令结构体丢到一个 FreeRTOS 队列（例如 g_cmd_queue）

任务层（例如一个 Task_Protocol 或复用你已有的 BLE 主循环）从 g_cmd_queue 取命令，然后：
- CMD=0x02：调用 TASK_SetActiveTaskId(...) + TASK_SetCollectEnabled(true)
- CMD=0x03：调用 TASK_SetCollectEnabled(false)
- CMD=0x04：读取 data.bin 分段组包，通过 BLE notify 或 UART 发回上位机
- CMD=0x05：调用 APP_Storage_DeleteTask(...) 清理任务
