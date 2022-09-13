/* Background I/O service for Redis.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there is only a single operation, that is a background close(2)
 * system call. This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 *
 * DESIGN
 * ------
 *
 * The design is trivial, we have a structure representing a job to perform
 * and a different thread and job queue for every job type.
 * Every thread waits for new jobs in its queue, and process every job
 * sequentially.
 *
 * Jobs of the same type are guaranteed to be processed from the least
 * recently inserted to the most recently inserted (older jobs processed
 * first).
 *
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 *
 * Redis 的后台 IO 服务。该文件实现了我们需要在后台执行的操作。
 * 目前只有一个操作，即后台 close(2) 系统调用。
 * 这是必需的，因为当进程是对文件的最后一个引用时，关闭它意味着取消链接，并且文件的删除速度很慢，从而阻塞了服务器。
 * 未来我们要么继续实现我们需要的新东西，要么切换到 libeio。
 * 但是，该文件可能有长期用途，因为我们可能希望将特定于 Redis 的后台任务放在这里
 * （例如，我们需要一个非阻塞的 FLUSHDBFLUSHALL 实现并非不可能）。
 * ---------设计 ------
 * 设计很简单，我们有一个表示要执行的作业的结构，以及每种作业类型的不同线程和作业队列。
 * 每个线程都在其队列中等待新作业，并按顺序处理每个作业。
 * 保证从最近插入到最近插入的同一类型的作业（首先处理较旧的作业）处理相同类型的作业。
 * 目前无法通知作业的创建者操作完成，只有在需要时才会添加。
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include "server.h"
#include "bio.h"

static pthread_t bio_threads[BIO_NUM_OPS];
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
static pthread_cond_t bio_newjob_cond[BIO_NUM_OPS];
static pthread_cond_t bio_step_cond[BIO_NUM_OPS];
static list *bio_jobs[BIO_NUM_OPS];
/* The following array is used to hold the number of pending jobs for every
 * OP type. This allows us to export the bioPendingJobsOfType() API that is
 * useful when the main thread wants to perform some operation that may involve
 * objects shared with the background thread. The main thread will just wait
 * that there are no longer jobs of this type to be executed before performing
 * the sensible operation. This data is also useful for reporting. */
/**
 * 以下数组用于保存每种 OP 类型的待处理作业的数量。
 * 这允许我们导出 bioPendingJobsOfType() API，当主线程想要执行一些可能涉及与后台线程共享的对象的操作时，该 API 很有用。
 * 主线程将等待不再有此类作业要执行，然后再执行明智的操作。该数据对于报告也很有用。
 * */
static unsigned long long bio_pending[BIO_NUM_OPS];

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all. */
//这个结构代表一个后台作业。它仅在此文件本地使用，因为 API 根本不公开内部。
struct bio_job {
    /* Job specific arguments. 工作特定的论点。*/
    int fd; /* Fd for file based background jobs  Fd 用于基于文件的后台作业*/
    lazy_free_fn *free_fn; /* Function that will free the provided arguments  将释放提供的参数的函数*/
    void *free_args[]; /* List of arguments to be passed to the free function  要传递给自由函数的参数列表*/
};

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
//确保我们有足够的堆栈来执行我们在主线程中所做的所有事情。
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Initialize the background system, spawning the thread. */
//初始化后台系统，产生线程。
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    int j;

    /* Initialization of state vars and objects */
    //状态变量和对象的初始化
    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j],NULL);
        pthread_cond_init(&bio_newjob_cond[j],NULL);
        pthread_cond_init(&bio_step_cond[j],NULL);
        bio_jobs[j] = listCreate();
        bio_pending[j] = 0;
    }

    /* Set the stack size as by default it may be small in some system */
    //将堆栈大小设置为默认情况下它在某些系统中可能很小
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr,&stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes  世界上到处都是 Solaris 修复*/
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible of. */
    //准备好生成我们的线程。我们使用线程函数接受的单个参数来传递线程负责的作业 ID。
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void*)(unsigned long) j;
        if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize Background Jobs.");
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

void bioSubmitJob(int type, struct bio_job *job) {
    pthread_mutex_lock(&bio_mutex[type]);
    listAddNodeTail(bio_jobs[type],job);
    bio_pending[type]++;
    pthread_cond_signal(&bio_newjob_cond[type]);
    pthread_mutex_unlock(&bio_mutex[type]);
}

void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;
    /* Allocate memory for the job structure and all required
     * arguments */
    //为作业结构和所有必需的参数分配内存
    struct bio_job *job = zmalloc(sizeof(*job) + sizeof(void *) * (arg_count));
    job->free_fn = free_fn;

    va_start(valist, arg_count);
    for (int i = 0; i < arg_count; i++) {
        job->free_args[i] = va_arg(valist, void *);
    }
    va_end(valist);
    bioSubmitJob(BIO_LAZY_FREE, job);
}

void bioCreateCloseJob(int fd) {
    struct bio_job *job = zmalloc(sizeof(*job));
    job->fd = fd;

    bioSubmitJob(BIO_CLOSE_FILE, job);
}

void bioCreateFsyncJob(int fd) {
    struct bio_job *job = zmalloc(sizeof(*job));
    job->fd = fd;

    bioSubmitJob(BIO_AOF_FSYNC, job);
}

void *bioProcessBackgroundJobs(void *arg) {
    struct bio_job *job;
    unsigned long type = (unsigned long) arg;
    sigset_t sigset;

    /* Check that the type is within the right interval. */
    //检查类型是否在正确的间隔内。
    if (type >= BIO_NUM_OPS) {
        serverLog(LL_WARNING,
            "Warning: bio thread started with wrong type %lu",type);
        return NULL;
    }

    switch (type) {
    case BIO_CLOSE_FILE:
        redis_set_thread_title("bio_close_file");
        break;
    case BIO_AOF_FSYNC:
        redis_set_thread_title("bio_aof_fsync");
        break;
    case BIO_LAZY_FREE:
        redis_set_thread_title("bio_lazy_free");
        break;
    }

    redisSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();

    pthread_mutex_lock(&bio_mutex[type]);
    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    //阻止 SIGALRM 以便我们确定只有主线程会收到看门狗信号。
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING,
            "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(errno));

    while(1) {
        listNode *ln;

        /* The loop always starts with the lock hold. */
        //循环始终以锁定保持开始。
        if (listLength(bio_jobs[type]) == 0) {
            pthread_cond_wait(&bio_newjob_cond[type],&bio_mutex[type]);
            continue;
        }
        /* Pop the job from the queue. 从队列中弹出作业。*/
        ln = listFirst(bio_jobs[type]);
        job = ln->value;
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        //现在可以解锁后台系统，因为我们知道有一个独立的作业结构要处理。
        pthread_mutex_unlock(&bio_mutex[type]);

        /* Process the job accordingly to its type. */
        //根据作业类型处理作业。
        if (type == BIO_CLOSE_FILE) {
            close(job->fd);
        } else if (type == BIO_AOF_FSYNC) {
            /* The fd may be closed by main thread and reused for another
             * socket, pipe, or file. We just ignore these errno because
             * aof fsync did not really fail. */
            //fd 可能会被主线程关闭并重新用于另一个套接字、管道或文件。
            // 我们只是忽略这些 errno，因为 aof fsync 并没有真正失败。
            if (redis_fsync(job->fd) == -1 &&
                errno != EBADF && errno != EINVAL)
            {
                int last_status;
                atomicGet(server.aof_bio_fsync_status,last_status);
                atomicSet(server.aof_bio_fsync_status,C_ERR);
                atomicSet(server.aof_bio_fsync_errno,errno);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING,
                        "Fail to fsync the AOF file: %s",strerror(errno));
                }
            } else {
                atomicSet(server.aof_bio_fsync_status,C_OK);
            }
        } else if (type == BIO_LAZY_FREE) {
            job->free_fn(job->free_args);
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        //在重复循环之前再次锁定，如果不再有作业要处理，我们将在 pthread_cond_wait() 中再次阻塞。
        pthread_mutex_lock(&bio_mutex[type]);
        listDelNode(bio_jobs[type],ln);
        bio_pending[type]--;

        /* Unblock threads blocked on bioWaitStepOfType() if any. */
        //如果有的话，取消阻止 bioWaitStepOfType() 上阻塞的线程。
        pthread_cond_broadcast(&bio_step_cond[type]);
    }
}

/* Return the number of pending jobs of the specified type. */
//返回指定类型的待处理作业的数量。
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* If there are pending jobs for the specified type, the function blocks
 * and waits that the next job was processed. Otherwise the function
 * does not block and returns ASAP.
 *
 * The function returns the number of jobs still to process of the
 * requested type.
 *
 * This function is useful when from another thread, we want to wait
 * a bio.c thread to do more work in a blocking way.
 */
/**
 * 如果有指定类型的待处理作业，该功能将阻塞并等待下一个作业已处理。
 * 否则，该函数不会阻塞并尽快返回。该函数返回仍要处理的请求类型的作业数。
 * 当来自另一个线程，我们想要等待 bio.c 线程以阻塞方式完成更多工作时，此函数很有用。
 * */
unsigned long long bioWaitStepOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    if (val != 0) {
        pthread_cond_wait(&bio_step_cond[type],&bio_mutex[type]);
        val = bio_pending[type];
    }
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
/**
 * 以不干净的方式杀死正在运行的生物线程。仅当出于某种原因必须停止线程时才应使用此功能。
 * 目前 Redis 仅在崩溃时执行此操作（例如在 SIGSEGV 上），以便执行快速内存检查，而其他线程不会弄乱内存。
 * */
void bioKillThreads(void) {
    int err, j;

    for (j = 0; j < BIO_NUM_OPS; j++) {
        if (bio_threads[j] == pthread_self()) continue;
        if (bio_threads[j] && pthread_cancel(bio_threads[j]) == 0) {
            if ((err = pthread_join(bio_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d can not be joined: %s",
                        j, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d terminated",j);
            }
        }
    }
}
