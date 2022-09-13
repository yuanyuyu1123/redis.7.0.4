/*
 * Copyright (c) 2021, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __FUNCTIONS_H_
#define __FUNCTIONS_H_

/*
 * functions.c unit provides the Redis Functions API:
 * * FUNCTION CREATE
 * * FUNCTION CALL
 * * FUNCTION DELETE
 * * FUNCTION KILL
 * * FUNCTION INFO
 *
 * Also contains implementation for:
 * * Save/Load function from rdb
 * * Register engines
 * 还包含以下实现：来自 rdb Register 引擎的 SaveLoad 函数
 */

#include "server.h"
#include "script.h"
#include "redismodule.h"

typedef struct functionLibInfo functionLibInfo;

typedef struct engine {
    /* engine specific context 引擎特定上下文*/
    void *engine_ctx;

    /* Create function callback, get the engine_ctx, and function code.
     * returns NULL on error and set sds to be the error message */
    //创建函数回调，获取engine_ctx，以及函数代码。出错时返回 NULL 并将 sds 设置为错误消息
    int (*create)(void *engine_ctx, functionLibInfo *li, sds code, sds *err);

    /* Invoking a function, r_ctx is an opaque object (from engine POV).
     * The r_ctx should be used by the engine to interaction with Redis,
     * such interaction could be running commands, set resp, or set
     * replication mode
     */
    /**
     * 调用一个函数，r_ctx 是一个不透明的对象（来自引擎 POV）。
     * r_ctx 应该被引擎用来与 Redis 交互，这样的交互可以是运行命令、设置 resp 或设置复制模式
     * */
    void (*call)(scriptRunCtx *r_ctx, void *engine_ctx, void *compiled_function,
            robj **keys, size_t nkeys, robj **args, size_t nargs);

    /* get current used memory by the engine 获取引擎当前使用的内存*/
    size_t (*get_used_memory)(void *engine_ctx);

    /* Return memory overhead for a given function,
     * such memory is not counted as engine memory but as general
     * structs memory that hold different information */
    //返回给定函数的内存开销，此类内存不计为引擎内存，而是作为保存不同信息的通用结构内存
    size_t (*get_function_memory_overhead)(void *compiled_function);

    /* Return memory overhead for engine (struct size holding the engine)*/
    //返回引擎的内存开销（保存引擎的结构大小）
    size_t (*get_engine_memory_overhead)(void *engine_ctx);

    /* free the given function 释放给定的函数*/
    void (*free_function)(void *engine_ctx, void *compiled_function);
} engine;

/* Hold information about an engine.
 * Used on rdb.c so it must be declared here. */
//保存有关引擎的信息。在 rdb.c 上使用，因此必须在此处声明。
typedef struct engineInfo {
    sds name;       /* Name of the engine */
    engine *engine; /* engine callbacks that allows to interact with the engine 允许与引擎交互的引擎回调*/
    client *c;      /* Client that is used to run commands 用于运行命令的客户端*/
} engineInfo;

/* Hold information about the specific function.
 * Used on rdb.c so it must be declared here. */
//保存有关特定功能的信息。在 rdb.c 上使用，因此必须在此处声明。
typedef struct functionInfo {
    sds name;            /* Function name */
    void *function;      /* Opaque object that set by the function's engine and allow it
                            to run the function, usually it's the function compiled code.
                            由函数引擎设置并允许其运行函数的不透明对象，通常是函数编译后的代码。*/
    functionLibInfo* li; /* Pointer to the library created the function 指向创建函数的库的指针*/
    sds desc;            /* Function description */
    uint64_t f_flags;    /* Function flags */
} functionInfo;

/* Hold information about the specific library.
 * Used on rdb.c so it must be declared here. */
//保存有关特定库的信息。在 rdb.c 上使用，因此必须在此处声明。
struct functionLibInfo {
    sds name;        /* Library name */
    dict *functions; /* Functions dictionary 函数字典*/
    engineInfo *ei;  /* Pointer to the function engine */
    sds code;        /* Library code */
};

int functionsRegisterEngine(const char *engine_name, engine *engine_ctx);
sds functionsCreateWithLibraryCtx(sds code, int replace, sds* err, functionsLibCtx *lib_ctx);
unsigned long functionsMemory();
unsigned long functionsMemoryOverhead();
unsigned long functionsNum();
unsigned long functionsLibNum();
dict* functionsLibGet();
size_t functionsLibCtxfunctionsLen(functionsLibCtx *functions_ctx);
functionsLibCtx* functionsLibCtxGetCurrent();
functionsLibCtx* functionsLibCtxCreate();
void functionsLibCtxClearCurrent(int async);
void functionsLibCtxFree(functionsLibCtx *lib_ctx);
void functionsLibCtxClear(functionsLibCtx *lib_ctx);
void functionsLibCtxSwapWithCurrent(functionsLibCtx *lib_ctx);

int functionLibCreateFunction(sds name, void *function, functionLibInfo *li, sds desc, uint64_t f_flags, sds *err);

int luaEngineInitEngine();
int functionsInit();

#endif /* __FUNCTIONS_H_ */
