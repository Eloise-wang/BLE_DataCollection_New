/*
 * bsp_ringBuff.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef BSP_BSP_RINGBUFF_H_
#define BSP_BSP_RINGBUFF_H_

#include <stdbool.h>
#include <stdint.h>

// 环形缓冲区结构体
typedef struct
{
    uint8_t *buffer;// 环形缓冲区存储空间
    uint32_t size;// 环形缓冲区大小
    volatile uint32_t head;// 队头指针
    volatile uint32_t tail;// 队尾指针
    uint32_t mask;
} bsp_ringBuff_t;


// 初始化环形缓冲区
void BSP_RingBuff_Init(bsp_ringBuff_t *rb, uint8_t *storage, uint32_t size);

// 计算已用空间
uint32_t BSP_RingBuff_Used(const bsp_ringBuff_t *rb);
// 计算空闲空间
uint32_t BSP_RingBuff_Free(const bsp_ringBuff_t *rb);

// 入队
bool BSP_RingBuff_Push(bsp_ringBuff_t *rb, const uint8_t *data, uint32_t length);
// 查看队头数据
uint32_t BSP_RingBuff_Peek(const bsp_ringBuff_t *rb, uint8_t *out, uint32_t length);
// 丢弃队头数据
bool BSP_RingBuff_Discard(bsp_ringBuff_t *rb, uint32_t length);
// 出队
uint32_t BSP_RingBuff_Pop(bsp_ringBuff_t *rb, uint8_t *out, uint32_t length);

#endif /* BSP_BSP_RINGBUFF_H_ */

