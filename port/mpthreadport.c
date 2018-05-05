/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Armink (armink.ztl@gmail.com)
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

#include "stdio.h"

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "py/mpthread.h"
#include "mpthreadport.h"

#include <rthw.h>

#if MICROPY_PY_THREAD

#define MP_THREAD_MIN_STACK_SIZE                        (5 * 1024)
#define MP_THREAD_DEFAULT_STACK_SIZE                    (MP_THREAD_MIN_STACK_SIZE + 1024)
#define MP_THREAD_PRIORITY                              (RT_THREAD_PRIORITY_MAX / 2)

typedef struct {
    rt_thread_t thread;
    /* whether the thread is ready and running */
    rt_bool_t ready;
    /* thread Python args, a GC root pointer */
    void *arg;
    /* pointer to the stack */
    void *stack;
    void *tcb;
    /* number of words in the stack */
    size_t stack_len;
    rt_list_t list;
} mp_thread, *mp_thread_t;

typedef struct {
    rt_mutex_t mutex;
    rt_list_t list;
} mp_mutex, *mp_mutex_t;

// the mutex controls access to the linked list
STATIC mp_thread_mutex_t thread_mutex;
STATIC rt_list_t thread_list, mutex_list;
STATIC mp_thread thread_entry0;
/* root pointer, handled by mp_thread_gc_others */
STATIC mp_thread *main_thread;

/**
 * thread port initialization
 *
 * @param stack MicroPython main thread stack
 * @param stack_len MicroPython main thread stack, unit: word
 */
void mp_thread_init(void *stack, uint32_t stack_len) {
    mp_thread_set_state(&mp_state_ctx.thread);

    main_thread = &thread_entry0;
    main_thread->thread = rt_thread_self();
    main_thread->ready = RT_TRUE;
    main_thread->arg = NULL;
    main_thread->stack = stack;
    main_thread->stack_len = stack_len;

    rt_list_init(&thread_list);
    rt_list_init(&mutex_list);

    rt_list_insert_before(&thread_list, &(main_thread->list));

    mp_thread_mutex_init(&thread_mutex);
}

void mp_thread_gc_others(void) {
    struct rt_list_node *list = &thread_list, *node = NULL;
    mp_thread_t cur_thread_node = NULL;

    mp_thread_mutex_lock(&thread_mutex, 1);

    for (node = list->next; node != list; node = node->next) {
        cur_thread_node = rt_list_entry(node, mp_thread, list);
        gc_collect_root((void **)&cur_thread_node->thread, 1);
        /* probably not needed */
        gc_collect_root(&cur_thread_node->arg, 1);
        if (cur_thread_node->thread == rt_thread_self()) {
            continue;
        }
        if (!cur_thread_node->ready) {
            continue;
        }
        /* probably not needed */
        gc_collect_root(cur_thread_node->stack, cur_thread_node->stack_len);
    }

    mp_thread_mutex_unlock(&thread_mutex);
}

mp_state_thread_t *mp_thread_get_state(void) {
    return (mp_state_thread_t *)(rt_thread_self()->user_data);
}

void mp_thread_set_state(void *state) {
    rt_thread_self()->user_data = (rt_uint32_t)state;
}

void mp_thread_start(void) {
    struct rt_list_node *list = &thread_list, *node = NULL;
    mp_thread_t cur_thread_node = NULL;

    mp_thread_mutex_lock(&thread_mutex, 1);

    for (node = list->next; node != list; node = node->next) {
        cur_thread_node = rt_list_entry(node, mp_thread, list);
        if (cur_thread_node->thread == rt_thread_self()) {
            cur_thread_node->ready = RT_TRUE;
            break;
        }
    }

    mp_thread_mutex_unlock(&thread_mutex);
}

STATIC void *(*ext_thread_entry)(void*) = NULL;

STATIC void rtthread_entry(void *arg) {
    if (ext_thread_entry) {
        ext_thread_entry(arg);
    }

    rt_thread_detach(rt_thread_self());
}

void mp_thread_create_ex(void *(*entry)(void*), void *arg, size_t *stack_size, int priority, char *name) {
    // store thread entry function into a global variable so we can access it
    ext_thread_entry = entry;//(void (*)(void *parameter))

    if (*stack_size == 0) {
        *stack_size = MP_THREAD_DEFAULT_STACK_SIZE; // default stack size
    } else if (*stack_size < MP_THREAD_MIN_STACK_SIZE) {
        *stack_size = MP_THREAD_MIN_STACK_SIZE; // minimum stack size
    }

    // allocate TCB, stack and linked-list node (must be outside thread_mutex lock)
    rt_thread_t th = m_new_obj(struct rt_thread);
    if (th == NULL) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "can't create thread TCB"));
    }
    uint8_t *stack = m_new(uint8_t, *stack_size);
    if (stack == NULL) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "can't create thread stack"));
    }
    mp_thread *node = m_new_obj(mp_thread);
    if (node == NULL) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "can't create thread list node"));
    }
    // adjust the stack_size to provide room to recover from hitting the limit
    *stack_size -= 1024;

    node->ready = RT_FALSE;
    node->arg = arg;
    node->stack = stack;
    node->stack_len = *stack_size / 4;

    mp_thread_mutex_lock(&thread_mutex, 1);

    rt_thread_init(th, name, rtthread_entry, arg, stack, *stack_size, priority, 0);

    // add thread to linked list of all threads
    {
        rt_base_t level;

        level = rt_hw_interrupt_disable();

        node->thread = th;
        rt_list_insert_before(&thread_list, &(node->list));

        rt_hw_interrupt_enable(level);
    }

    rt_thread_startup(th);

    mp_thread_mutex_unlock(&thread_mutex);
}

void mp_thread_create(void *(*entry)(void*), void *arg, size_t *stack_size) {
    static uint8_t count = 0;
    char name[RT_NAME_MAX];

    /* build name */
    rt_snprintf(name, sizeof(name), "mp%02d", count ++);

    mp_thread_create_ex(entry, arg, stack_size, MP_THREAD_PRIORITY, name);
}

void mp_thread_finish(void) {
    struct rt_list_node *list = &thread_list, *node = NULL;
    mp_thread_t cur_thread_node = NULL;

    mp_thread_mutex_lock(&thread_mutex, 1);

    for (node = list->next; node != list; node = node->next) {
        cur_thread_node = rt_list_entry(node, mp_thread, list);
        if (cur_thread_node->thread == rt_thread_self()) {
            cur_thread_node->ready = RT_FALSE;
            // explicitly release all its memory
            m_del(rt_thread_t, cur_thread_node->thread, 1);
            m_del(uint8_t, cur_thread_node->stack, cur_thread_node->stack_len);
//            m_del(mp_thread, cur_thread_node, 1);
            rt_list_remove(node);
            break;
        }
    }

    mp_thread_mutex_unlock(&thread_mutex);

    rt_thread_detach(rt_thread_self());
}

void mp_thread_mutex_init(mp_thread_mutex_t *mutex) {
    static uint8_t count = 0;
    char name[RT_NAME_MAX];
    rt_base_t level;

    level = rt_hw_interrupt_disable();

    mp_mutex *node = rt_malloc(sizeof(mp_mutex));
    if (node == NULL) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "can't create mutex list node"));
    }

    /* build name */
    rt_snprintf(name, sizeof(name), "mp%02d", count ++);

    rt_mutex_init((rt_mutex_t) mutex, name, RT_IPC_FLAG_FIFO);

    // add mutex to linked list of all mutexs
    node->mutex = (rt_mutex_t)mutex;
    rt_list_insert_before(&mutex_list, &(node->list));

    rt_hw_interrupt_enable(level);
}

int mp_thread_mutex_lock(mp_thread_mutex_t *mutex, int wait) {
    return (RT_EOK == rt_mutex_take((rt_mutex_t) mutex, wait ? RT_WAITING_FOREVER : 0));
}

void mp_thread_mutex_unlock(mp_thread_mutex_t *mutex) {
    rt_mutex_release((rt_mutex_t) mutex);
}

void mp_thread_deinit(void) {
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    /* remove all thread node on list */
    {
        struct rt_list_node *list = &thread_list, *node = NULL;
        mp_thread_t cur_thread_node = NULL;

        for (node = list->next; node != list; node = node->next) {
            cur_thread_node = rt_list_entry(node, mp_thread, list);
            if (cur_thread_node->thread != main_thread->thread) {
                rt_thread_detach(cur_thread_node->thread);
            }
        }
    }
    /* remove all mutex node on list */
    {
        struct rt_list_node *list = &mutex_list, *node = NULL;
        mp_mutex_t cur_mutex_node = NULL;

        for (node = list->next; node != list; node = node->next) {
            cur_mutex_node = rt_list_entry(node, mp_mutex, list);
            rt_mutex_detach(cur_mutex_node->mutex);
            rt_free(cur_mutex_node);
        }
    }

    rt_hw_interrupt_enable(level);
    // allow RT-Thread to clean-up the threads
    rt_thread_delay(200);
}

#endif /* MICROPY_PY_THREAD */
