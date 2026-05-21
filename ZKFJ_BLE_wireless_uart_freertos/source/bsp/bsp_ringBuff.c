/*
 * bsp_ringBuff.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "bsp_ringBuff.h"

#include <string.h>

// 计算已用空间
static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size)
{
    if (head >= tail)
    {
        return head - tail;
    }
    return (uint32_t)(size - (tail - head));
}

static uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t size)
{
    return (uint32_t)(size - 1U - ring_used(head, tail, size));
}

static uint32_t ring_advance(const bsp_ringBuff_t *rb, uint32_t index, uint32_t delta)
{
    if (rb->mask != 0U)
    {
        return (uint32_t)((index + delta) & rb->mask);
    }
    return (uint32_t)((index + delta) % rb->size);
}

// 初始化环形缓冲区
void BSP_RingBuff_Init(bsp_ringBuff_t *rb, uint8_t *storage, uint32_t size)
{
    if ((rb == NULL) || (storage == NULL) || (size < 2U))
    {
        return;
    }

    rb->buffer = storage;
    rb->size   = size;
    rb->head   = 0U;
    rb->tail   = 0U;
    rb->mask   = (((size & (size - 1U)) == 0U) ? (size - 1U) : 0U);
}

// 计算已用空间
uint32_t BSP_RingBuff_Used(const bsp_ringBuff_t *rb)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->size < 2U))
    {
        return 0U;
    }

    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    return ring_used(head, tail, rb->size);
}

// 计算空闲空间
uint32_t BSP_RingBuff_Free(const bsp_ringBuff_t *rb)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->size < 2U))
    {
        return 0U;
    }

    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    return ring_free(head, tail, rb->size);
}

// 入队
bool BSP_RingBuff_Push(bsp_ringBuff_t *rb, const uint8_t *data, uint32_t length)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->size < 2U) || (data == NULL) || (length == 0U))
    {
        return false;
    }

    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    if (length > ring_free(head, tail, rb->size))
    {
        return false;
    }

    uint32_t headToEnd = (uint32_t)(rb->size - head);
    uint32_t firstLen  = (length < headToEnd) ? length : headToEnd;
    memcpy(&rb->buffer[head], &data[0], firstLen);
    if (firstLen < length)
    {
        memcpy(&rb->buffer[0], &data[firstLen], length - firstLen);
    }

    rb->head = ring_advance(rb, head, length);
    return true;
}

// 查看队头数据
uint32_t BSP_RingBuff_Peek(const bsp_ringBuff_t *rb, uint8_t *out, uint32_t length)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->size < 2U) || (out == NULL) || (length == 0U))
    {
        return 0U;
    }

    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t used = ring_used(head, tail, rb->size);
    if (used == 0U)
    {
        return 0U;
    }

    uint32_t toRead   = (length < used) ? length : used;
    uint32_t tailToEnd = (uint32_t)(rb->size - tail);
    uint32_t firstLen  = (toRead < tailToEnd) ? toRead : tailToEnd;
    memcpy(&out[0], &rb->buffer[tail], firstLen);
    if (firstLen < toRead)
    {
        memcpy(&out[firstLen], &rb->buffer[0], toRead - firstLen);
    }

    return toRead;
}

// 丢弃队头数据
bool BSP_RingBuff_Discard(bsp_ringBuff_t *rb, uint32_t length)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->size < 2U) || (length == 0U))
    {
        return false;
    }

    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t used = ring_used(head, tail, rb->size);
    if (length > used)
    {
        return false;
    }

    rb->tail = ring_advance(rb, tail, length);
    return true;
}

// 出队
uint32_t BSP_RingBuff_Pop(bsp_ringBuff_t *rb, uint8_t *out, uint32_t length)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->size < 2U) || (out == NULL) || (length == 0U))
    {
        return 0U;
    }

    uint32_t readLen = BSP_RingBuff_Peek(rb, out, length);
    if (readLen == 0U)
    {
        return 0U;
    }

    (void)BSP_RingBuff_Discard(rb, readLen);
    return readLen;
}

