## CRC计算

CRC 计算总结（对应你这个工程的命令帧/数据帧）

#### 1) CRC 类型与参数（CRC16）

在该工程里，协议用的 CRC16 来自硬件 CRC 模块配置（见 bsp_crc.c ）：

- 多项式： 0x1021
- 初值（seed）： 0xFFFF
- reflectIn： false
- reflectOut： false
- xorOut（complementChecksum）： false （等价于 xorOut=0）
- 输出宽度：16 bit
- 输出顺序： 发送时按小端放到帧尾 （CRC low byte 在前，high byte 在后）
  这就是常说的 CRC-16/CCITT-FALSE（init=0xFFFF） 这一类（无反射、无异或）。

#### 2) CRC 覆盖哪些字节？

##### 命令帧（SOF=0xA5）

CRC 计算输入是：

- CMD (1 byte) + LEN (1 byte) + PAYLOAD (LEN bytes)
  注意： 不包含帧头 SOF=0xA5 。

代码依据： proto_frame.c 里校验时用的是 cmd+len+payload 。

##### 数据帧（SOF=0x5A）/ 状态帧（SOF=0x5B）

CRC 计算输入是：

- LEN(2 bytes, 小端) + PAYLOAD(payload_len bytes)
  同样： 不包含 SOF（0x5A/0x5B） 。

代码依据： proto_frame.c （ crc = crc16(&out[1], 2 + payload_len) ）。

#### 3) 发送时 CRC 怎么放？

- CRC16 计算结果是 16bit： crc
- 帧尾附加两个字节： crc_lo = crc & 0xFF ， crc_hi = (crc >> 8) & 0xFF
- 也就是 小端 ： lo hi
  举例：如果算出来 CRC=0x29A8，则帧尾是： A8 29 。

#### 4) 一个最短可验证例子（ACK）

对 0x02 START_COLLECT 的 OK ACK：

- ACK cmd = 0x02 + 0x80 = 0x82
- len=1
- status=0
  CRC 输入为： 82 01 00
   算出的 CRC=0xAA97 → 帧尾 97 AA
   完整 ACK 帧： A5 82 01 00 97 AA

如果你用上位机算 CRC，能和这个对上，就说明 CRC 实现方向没错。