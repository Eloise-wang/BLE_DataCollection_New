/*
 * filter.c
 *
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#include "filter.h"

bool filter_trimmed_mean_u16_8_drop_min_max(const uint16_t samples[FILTER_TRIMMED_MEAN_SAMPLE_COUNT],
                                            uint16_t *out_mean)
{
    if ((samples == NULL) || (out_mean == NULL))
    {
        return false;
    }

    uint16_t min_v = samples[0];
    uint16_t max_v = samples[0];
    uint32_t sum   = samples[0];

    for (uint32_t i = 1; i < FILTER_TRIMMED_MEAN_SAMPLE_COUNT; i++)
    {
        const uint16_t v = samples[i];
        sum += v;
        if (v < min_v)
        {
            min_v = v;
        }
        if (v > max_v)
        {
            max_v = v;
        }
    }

    const uint32_t trimmed_sum = sum - (uint32_t)min_v - (uint32_t)max_v;
    *out_mean                  = (uint16_t)((trimmed_sum + (FILTER_TRIMMED_MEAN_VALID_COUNT / 2U)) /
                           FILTER_TRIMMED_MEAN_VALID_COUNT);
    return true;
}

bool filter_trimmed_mean_u16_8_read_drop_min_max(filter_read_u16_fn read_fn, void *ctx, uint16_t *out_mean)
{
    if ((read_fn == NULL) || (out_mean == NULL))
    {
        return false;
    }

    uint16_t samples[FILTER_TRIMMED_MEAN_SAMPLE_COUNT];
    for (uint32_t i = 0; i < FILTER_TRIMMED_MEAN_SAMPLE_COUNT; i++)
    {
        if (!read_fn(ctx, &samples[i]))
        {
            return false;
        }
    }

    return filter_trimmed_mean_u16_8_drop_min_max(samples, out_mean);
}

