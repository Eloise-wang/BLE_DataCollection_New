# Debug 命令

通过 BLE 发送十六进制帧格式来查询 Flash 中存储的任务信息。所有 Debug 命令（`0xE0-0xEF`）**无需 CRC 校验**，CRC 字段可以填任意值。

**帧格式**：`A5 <CMD> <LEN> [PAYLOAD...] <CRC_LO> <CRC_HI>`

---

## 1. 列出所有任务 ID（0xE0）

查询 Flash 中存储的所有任务 ID。

**请求帧**：
```
A5 E0 00 00 00
          └── CRC（任意值）
```

**UART 输出示例**：
```
[DBG] === Task List ===
  [0] CB6713C5D2AFC3FE
  [1] 1234567890ABCDEF
[DBG] === 2 tasks ===
```

---

## 2. 查询指定任务详情（0xE1）

查询某个 task_id 的详细信息（是否存在、各区字节数、记录条数）。

**请求帧**（task_id 为 8 字节小端）：
```
A5 E1 08 <task_id(8字节)> 00 00
```

**查询 CB6713C5D2AFC3FE 示例**：
```
A5 E1 08 FE C3 AF D2 C5 13 67 CB 00 00
```

**UART 输出示例**：
```
[DBG] === Task CB6713C5D2AFC3FE ===
  found=1
  data_bytes=20000
  pre_bytes=0
  meta_bytes=20
  record_size=20 version=1
  start_ts=... sample_ms=... duration_ms=...
  record_count=1000
[DBG] ===================
```

---

## 3. 快速查询记录条数（0xE2）

查询某个 task_id 的数据记录条数（精简版，比 0xE1 更快）。

**请求帧**：
```
A5 E2 08 <task_id(8字节)> 00 00
```

**查询 CB6713C5D2AFC3FE 示例**：
```
A5 E2 08 FE C3 AF D2 C5 13 67 CB 00 00
```

**UART 输出示例**：
```
[DBG] task=CB6713C5D2AFC3FE rec_count=1000
```

---

## 常用 task_id 小端字节对照

| task_id（十六进制） | 小端字节序（HEX） |
|--------------------|-------------------|
| CB6713C5D2AFC3FE | FE C3 AF D2 C5 13 67 CB |
| 1234567890ABCDEF | EF CD AB 90 78 56 34 12 |
| 0000000000000001 | 01 00 00 00 00 00 00 00 |
