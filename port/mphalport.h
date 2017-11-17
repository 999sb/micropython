/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Armink (armink.ztl@gmail.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <rtthread.h>

static inline mp_uint_t mp_hal_ticks_us(void) {
    return rt_tick_get() * 1000;
}

static inline mp_uint_t mp_hal_ticks_ms(void) {
    return rt_tick_get() * 1000 / RT_TICK_PER_SECOND;
}

static inline mp_uint_t mp_hal_ticks_cpu(void) {
    return rt_tick_get();
}

static inline void mp_hal_delay_us(mp_uint_t delay) {
    rt_thread_delay(rt_tick_from_millisecond(delay / 1000));
}

static inline void mp_hal_delay_ms(mp_uint_t delay) {
    rt_thread_delay(rt_tick_from_millisecond(delay));
}

