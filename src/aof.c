/*
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
#include "rio.h"
#include "functions.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>

void freeClientArgv(client *c);
off_t getAppendOnlyFileSize(sds filename, int *status);
off_t getBaseAndIncrAppendOnlyFilesSize(aofManifest *am, int *status);
int getBaseAndIncrAppendOnlyFilesNum(aofManifest *am);
int aofFileExist(char *filename);
int rewriteAppendOnlyFile(char *filename);
aofManifest *aofLoadManifestFromFile(sds am_filepath);
void aofManifestFreeAndUpdate(aofManifest *am);

/* ----------------------------------------------------------------------------
 * AOF Manifest file implementation.
 *
 * The following code implements the read/write logic of AOF manifest file, which
 * is used to track and manage all AOF files.
 *
 * Append-only files consist of three types:
 *
 * BASE: Represents a Redis snapshot from the time of last AOF rewrite. The manifest
 * file contains at most a single BASE file, which will always be the first file in the
 * list.
 *
 * INCR: Represents all write commands executed by Redis following the last successful
 * AOF rewrite. In some cases it is possible to have several ordered INCR files. For
 * example:
 *   - During an on-going AOF rewrite
 *   - After an AOF rewrite was aborted/failed, and before the next one succeeded.
 *
 * HISTORY: After a successful rewrite, the previous BASE and INCR become HISTORY files.
 * They will be automatically removed unless garbage collection is disabled.
 *
 * The following is a possible AOF manifest file content:
 *
 * file appendonly.aof.2.base.rdb seq 2 type b
 * file appendonly.aof.1.incr.aof seq 1 type h
 * file appendonly.aof.2.incr.aof seq 2 type h
 * file appendonly.aof.3.incr.aof seq 3 type h
 * file appendonly.aof.4.incr.aof seq 4 type i
 * file appendonly.aof.5.incr.aof seq 5 type i
 *
 * AOF 清单文件实现。以下代码实现了AOF manifest文件的读写逻辑，用于跟踪和管理所有AOF文件。
 * Append-only 文件包含三种类型：
 *   BASE：表示上次 AOF 重写时的 Redis 快照。清单文件最多包含一个 BASE 文件，它始终是列表中的第一个文件。
 *   INCR：表示在最后一次成功的 AOF 重写之后 Redis 执行的所有写入命令。在某些情况下，可能有多个有序的 INCR 文件。例如：
 *     - 在进行中的 AOF 重写期间
 *     - 在 AOF 重写失败后，下一次成功之前。
 *   HISTORY：重写成功后，之前的BASE和INCR成为HISTORY文件。除非禁用垃圾收集，否则它们将被自动删除。
 * 以下是可能的 AOF 清单文件内容：
     * file appendonly.aof.2.base.rdb seq 2 type b
     * file appendonly.aof.1.incr.aof seq 1 type h
     * file appendonly.aof.2.incr.aof seq 2 type h
     * file appendonly.aof.3.incr.aof seq 3 type h
     * file appendonly.aof.4.incr.aof seq 4 type i
     * * file appendonly.aof.5.incr.aof seq 5 type i
 * ------------------------------------------------------------------------- */

/* Naming rules. */
#define BASE_FILE_SUFFIX           ".base"
#define INCR_FILE_SUFFIX           ".incr"
#define RDB_FORMAT_SUFFIX          ".rdb"
#define AOF_FORMAT_SUFFIX          ".aof"
#define MANIFEST_NAME_SUFFIX       ".manifest"
#define TEMP_FILE_NAME_PREFIX      "temp-"

/* AOF manifest key. */
#define AOF_MANIFEST_KEY_FILE_NAME   "file"
#define AOF_MANIFEST_KEY_FILE_SEQ    "seq"
#define AOF_MANIFEST_KEY_FILE_TYPE   "type"

/* Create an empty aofInfo. */
aofInfo *aofInfoCreate(void) {
    return zcalloc(sizeof(aofInfo));
}

/* Free the aofInfo structure (pointed to by ai) and its embedded file_name. */
/**释放 aofInfo 结构（由 ai 指向）及其嵌入的 file_name。*/
void aofInfoFree(aofInfo *ai) {
    serverAssert(ai != NULL);
    if (ai->file_name) sdsfree(ai->file_name);
    zfree(ai);
}

/* Deep copy an aofInfo. */
aofInfo *aofInfoDup(aofInfo *orig) {
    serverAssert(orig != NULL);
    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdsdup(orig->file_name);
    ai->file_seq = orig->file_seq;
    ai->file_type = orig->file_type;
    return ai;
}

/* Format aofInfo as a string and it will be a line in the manifest. */
/**将 aofInfo 格式化为字符串，它将成为清单中的一行。*/
sds aofInfoFormat(sds buf, aofInfo *ai) {
    sds filename_repr = NULL;

    if (sdsneedsrepr(ai->file_name))
        filename_repr = sdscatrepr(sdsempty(), ai->file_name, sdslen(ai->file_name));

    sds ret = sdscatprintf(buf, "%s %s %s %lld %s %c\n",
        AOF_MANIFEST_KEY_FILE_NAME, filename_repr ? filename_repr : ai->file_name,
        AOF_MANIFEST_KEY_FILE_SEQ, ai->file_seq,
        AOF_MANIFEST_KEY_FILE_TYPE, ai->file_type);
    sdsfree(filename_repr);

    return ret;
}

/* Method to free AOF list elements. */
/**释放 AOF 列表元素的方法。*/
void aofListFree(void *item) {
    aofInfo *ai = (aofInfo *)item;
    aofInfoFree(ai);
}

/* Method to duplicate AOF list elements. */
/**复制 OF 列表元素的方法。*/
void *aofListDup(void *item) {
    return aofInfoDup(item);
}

/* Create an empty aofManifest, which will be called in `aofLoadManifestFromDisk`. */
/**创建一个空的 aofManifest，它将在 `aofLoadManifestFromDisk` 中调用。*/
aofManifest *aofManifestCreate(void) {
    aofManifest *am = zcalloc(sizeof(aofManifest));
    am->incr_aof_list = listCreate();
    am->history_aof_list = listCreate();
    listSetFreeMethod(am->incr_aof_list, aofListFree);
    listSetDupMethod(am->incr_aof_list, aofListDup);
    listSetFreeMethod(am->history_aof_list, aofListFree);
    listSetDupMethod(am->history_aof_list, aofListDup);
    return am;
}

/* Free the aofManifest structure (pointed to by am) and its embedded members. */
/**释放 aofManifest 结构（由 am 指向）及其嵌入的成员。*/
void aofManifestFree(aofManifest *am) {
    if (am->base_aof_info) aofInfoFree(am->base_aof_info);
    if (am->incr_aof_list) listRelease(am->incr_aof_list);
    if (am->history_aof_list) listRelease(am->history_aof_list);
    zfree(am);
}

sds getAofManifestFileName() {
    return sdscatprintf(sdsempty(), "%s%s", server.aof_filename,
                MANIFEST_NAME_SUFFIX);
}

sds getTempAofManifestFileName() {
    return sdscatprintf(sdsempty(), "%s%s%s", TEMP_FILE_NAME_PREFIX,
                server.aof_filename, MANIFEST_NAME_SUFFIX);
}

/* Returns the string representation of aofManifest pointed to by am.
 *
 * The string is multiple lines separated by '\n', and each line represents
 * an AOF file.
 *
 * Each line is space delimited and contains 6 fields, as follows:
 * "file" [filename] "seq" [sequence] "type" [type]
 *
 * Where "file", "seq" and "type" are keywords that describe the next value,
 * [filename] and [sequence] describe file name and order, and [type] is one
 * of 'b' (base), 'h' (history) or 'i' (incr).
 *
 * The base file, if exists, will always be first, followed by history files,
 * and incremental files.
 */
/**返回 am 指向的 aofManifest 的字符串表示形式。该字符串是由'\n'分隔的多行，每行代表一个AOF文件。
 *
 * 每行以空格分隔，包含 6 个字段，如下所示：
 *     "file" [filename] "seq" [sequence] "type" [type]
 *
 * 其中 "file"、"seq" 和 "type" 是描述下一个值的关键字, [filename] 和 [sequence] 描述文件名和顺序，
 * [type] 是 'b' (base)、'h' (history) 或 'i' (incr) 之一。
 *
 * 基本文件（如果存在）将始终位于第一位，然后是历史文件和增量文件。*/
sds getAofManifestAsString(aofManifest *am) {
    serverAssert(am != NULL);

    sds buf = sdsempty();
    listNode *ln;
    listIter li;

    /* 1. Add BASE File information, it is always at the beginning
     * of the manifest file. */
    /**1.添加BASE File信息，总是在manifest文件的开头。*/
    if (am->base_aof_info) {
        buf = aofInfoFormat(buf, am->base_aof_info);
    }

    /* 2. Add HISTORY type AOF information. */
    /**2.添加HISTORY类型的信息。*/
    listRewind(am->history_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        buf = aofInfoFormat(buf, ai);
    }

    /* 3. Add INCR type AOF information. */
    /**3.添加INCR类型的信息。*/
    listRewind(am->incr_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        buf = aofInfoFormat(buf, ai);
    }

    return buf;
}

/* Load the manifest information from the disk to `server.aof_manifest`
 * when the Redis server start.
 * Redis 服务器启动时，将清单信息从磁盘加载到 `server.aof_manifest`。
 *
 * During loading, this function does strict error checking and will abort
 * the entire Redis server process on error (I/O error, invalid format, etc.)
 * 在加载过程中，该函数会进行严格的错误检查，并在出现错误（IO错误、格式无效等）时中止整个Redis服务器进程
 *
 * If the AOF directory or manifest file do not exist, this will be ignored
 * in order to support seamless upgrades from previous versions which did not
 * use them.
 * 如果 AOF 目录或清单文件不存在，则将被忽略以支持从未使用它们的先前版本进行无缝升级。
 */
void aofLoadManifestFromDisk(void) {
    server.aof_manifest = aofManifestCreate();
    if (!dirExists(server.aof_dirname)) {
        serverLog(LL_DEBUG, "The AOF directory %s doesn't exist", server.aof_dirname);
        return;
    }

    sds am_name = getAofManifestFileName();
    sds am_filepath = makePath(server.aof_dirname, am_name);
    if (!fileExist(am_filepath)) {
        serverLog(LL_DEBUG, "The AOF manifest file %s doesn't exist", am_name);
        sdsfree(am_name);
        sdsfree(am_filepath);
        return;
    }

    aofManifest *am = aofLoadManifestFromFile(am_filepath);
    if (am) aofManifestFreeAndUpdate(am);
    sdsfree(am_name);
    sdsfree(am_filepath);
}

/* Generic manifest loading function, used in `aofLoadManifestFromDisk` and redis-check-aof tool. */
/**通用清单加载函数，用于 `aofLoadManifestFromDisk` 和 redis-check-aof 工具。*/
#define MANIFEST_MAX_LINE 1024
aofManifest *aofLoadManifestFromFile(sds am_filepath) {
    const char *err = NULL;
    long long maxseq = 0;

    aofManifest *am = aofManifestCreate();
    FILE *fp = fopen(am_filepath, "r");
    if (fp == NULL) {
        serverLog(LL_WARNING, "Fatal error: can't open the AOF manifest "
            "file %s for reading: %s", am_filepath, strerror(errno));
        exit(1);
    }

    char buf[MANIFEST_MAX_LINE+1];
    sds *argv = NULL;
    int argc;
    aofInfo *ai = NULL;

    sds line = NULL;
    int linenum = 0;

    while (1) {
        if (fgets(buf, MANIFEST_MAX_LINE+1, fp) == NULL) {
            if (feof(fp)) {
                if (linenum == 0) {
                    err = "Found an empty AOF manifest";
                    goto loaderr;
                } else {
                    break;
                }
            } else {
                err = "Read AOF manifest failed";
                goto loaderr;
            }
        }

        linenum++;

        /* Skip comments lines */
        if (buf[0] == '#') continue;

        if (strchr(buf, '\n') == NULL) {
            err = "The AOF manifest file contains too long line";
            goto loaderr;
        }

        line = sdstrim(sdsnew(buf), " \t\r\n");
        if (!sdslen(line)) {
            err = "Invalid AOF manifest file format";
            goto loaderr;
        }

        argv = sdssplitargs(line, &argc);
        /* 'argc < 6' was done for forward compatibility. */
        /**'argc < 6' 是为了向前兼容而完成的。*/
        if (argv == NULL || argc < 6 || (argc % 2)) {
            err = "Invalid AOF manifest file format";
            goto loaderr;
        }

        ai = aofInfoCreate();
        for (int i = 0; i < argc; i += 2) {
            if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_FILE_NAME)) {
                ai->file_name = sdsnew(argv[i+1]);
                if (!pathIsBaseName(ai->file_name)) {
                    err = "File can't be a path, just a filename";
                    goto loaderr;
                }
            } else if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_FILE_SEQ)) {
                ai->file_seq = atoll(argv[i+1]);
            } else if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_FILE_TYPE)) {
                ai->file_type = (argv[i+1])[0];
            }
            /* else if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_OTHER)) {} */
            /**否则 if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_OTHER)) {}*/
        }

        /* We have to make sure we load all the information. */
        /**我们必须确保加载所有信息。*/
        if (!ai->file_name || !ai->file_seq || !ai->file_type) {
            err = "Invalid AOF manifest file format";
            goto loaderr;
        }

        sdsfreesplitres(argv, argc);
        argv = NULL;

        if (ai->file_type == AOF_FILE_TYPE_BASE) {
            if (am->base_aof_info) {
                err = "Found duplicate base file information";
                goto loaderr;
            }
            am->base_aof_info = ai;
            am->curr_base_file_seq = ai->file_seq;
        } else if (ai->file_type == AOF_FILE_TYPE_HIST) {
            listAddNodeTail(am->history_aof_list, ai);
        } else if (ai->file_type == AOF_FILE_TYPE_INCR) {
            if (ai->file_seq <= maxseq) {
                err = "Found a non-monotonic sequence number";
                goto loaderr;
            }
            listAddNodeTail(am->incr_aof_list, ai);
            am->curr_incr_file_seq = ai->file_seq;
            maxseq = ai->file_seq;
        } else {
            err = "Unknown AOF file type";
            goto loaderr;
        }

        sdsfree(line);
        line = NULL;
        ai = NULL;
    }

    fclose(fp);
    return am;

loaderr:
    /* Sanitizer suppression: may report a false positive if we goto loaderr
     * and exit(1) without freeing these allocations. */
    /**消毒剂抑制：如果我们转到加载程序并exit(1)而不释放这些分配，可能会报告误报。*/
    if (argv) sdsfreesplitres(argv, argc);
    if (ai) aofInfoFree(ai);

    serverLog(LL_WARNING, "\n*** FATAL AOF MANIFEST FILE ERROR ***\n");
    if (line) {
        serverLog(LL_WARNING, "Reading the manifest file, at line %d\n", linenum);
        serverLog(LL_WARNING, ">>> '%s'\n", line);
    }
    serverLog(LL_WARNING, "%s\n", err);
    exit(1);
}

/* Deep copy an aofManifest from orig.
 *
 * In `backgroundRewriteDoneHandler` and `openNewIncrAofForAppend`, we will
 * first deep copy a temporary AOF manifest from the `server.aof_manifest` and
 * try to modify it. Once everything is modified, we will atomically make the
 * `server.aof_manifest` point to this temporary aof_manifest.
 */
/**从 orig 深拷贝一个 aofManifest。
 *
 * 在 `backgroundRewriteDoneHandler` 和 `openNewIncrAofForAppend` 中，
 * 我们将首先从 `server.aof_manifest` 中深度复制一个临时 AOF 清单并尝试对其进行修改。
 * 修改完所有内容后，我们将自动使 `server.aof_manifest` 指向这个临时的 aof_manifest。*/
aofManifest *aofManifestDup(aofManifest *orig) {
    serverAssert(orig != NULL);
    aofManifest *am = zcalloc(sizeof(aofManifest));

    am->curr_base_file_seq = orig->curr_base_file_seq;
    am->curr_incr_file_seq = orig->curr_incr_file_seq;
    am->dirty = orig->dirty;

    if (orig->base_aof_info) {
        am->base_aof_info = aofInfoDup(orig->base_aof_info);
    }

    am->incr_aof_list = listDup(orig->incr_aof_list);
    am->history_aof_list = listDup(orig->history_aof_list);
    serverAssert(am->incr_aof_list != NULL);
    serverAssert(am->history_aof_list != NULL);
    return am;
}

/* Change the `server.aof_manifest` pointer to 'am' and free the previous
 * one if we have. */
/**将 `server.aof_manifest` 指针更改为 'am' 并释放前一个（如果有）。*/
void aofManifestFreeAndUpdate(aofManifest *am) {
    serverAssert(am != NULL);
    if (server.aof_manifest) aofManifestFree(server.aof_manifest);
    server.aof_manifest = am;
}

/* Called in `backgroundRewriteDoneHandler` to get a new BASE file
 * name, and mark the previous (if we have) BASE file as HISTORY type.
 * 在 `backgroundRewriteDoneHandler` 中调用以获取新的 BASE 文件名，
 * 并将之前的（如果有的话）BASE 文件标记为 HISTORY 类型。
 *
 * BASE file naming rules: `server.aof_filename`.seq.base.format
 *
 * for example:
 *  appendonly.aof.1.base.aof  (server.aof_use_rdb_preamble is no)
 *  appendonly.aof.1.base.rdb  (server.aof_use_rdb_preamble is yes)
 */
sds getNewBaseFileNameAndMarkPreAsHistory(aofManifest *am) {
    serverAssert(am != NULL);
    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);
        am->base_aof_info->file_type = AOF_FILE_TYPE_HIST;
        listAddNodeHead(am->history_aof_list, am->base_aof_info);
    }

    char *format_suffix = server.aof_use_rdb_preamble ?
        RDB_FORMAT_SUFFIX:AOF_FORMAT_SUFFIX;

    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdscatprintf(sdsempty(), "%s.%lld%s%s", server.aof_filename,
                        ++am->curr_base_file_seq, BASE_FILE_SUFFIX, format_suffix);
    ai->file_seq = am->curr_base_file_seq;
    ai->file_type = AOF_FILE_TYPE_BASE;
    am->base_aof_info = ai;
    am->dirty = 1;
    return am->base_aof_info->file_name;
}

/* Get a new INCR type AOF name.
 *
 * INCR AOF naming rules: `server.aof_filename`.seq.incr.aof
 *
 * for example:
 *  appendonly.aof.1.incr.aof
 */
sds getNewIncrAofName(aofManifest *am) {
    aofInfo *ai = aofInfoCreate();
    ai->file_type = AOF_FILE_TYPE_INCR;
    ai->file_name = sdscatprintf(sdsempty(), "%s.%lld%s%s", server.aof_filename,
                        ++am->curr_incr_file_seq, INCR_FILE_SUFFIX, AOF_FORMAT_SUFFIX);
    ai->file_seq = am->curr_incr_file_seq;
    listAddNodeTail(am->incr_aof_list, ai);
    am->dirty = 1;
    return ai->file_name;
}

/* Get temp INCR type AOF name.  获取临时 INCR 类型的名称。*/
sds getTempIncrAofName() {
    return sdscatprintf(sdsempty(), "%s%s%s", TEMP_FILE_NAME_PREFIX, server.aof_filename,
        INCR_FILE_SUFFIX);
}

/* Get the last INCR AOF name or create a new one. */
/**获取最后一个 INCR AOF 名称或创建一个新名称。*/
sds getLastIncrAofName(aofManifest *am) {
    serverAssert(am != NULL);

    /* If 'incr_aof_list' is empty, just create a new one. */
    /**如果 'incr_aof_list' 为空，只需创建一个新的。*/
    if (!listLength(am->incr_aof_list)) {
        return getNewIncrAofName(am);
    }

    /* Or return the last one. 或者返回最后一个。*/
    listNode *lastnode = listIndex(am->incr_aof_list, -1);
    aofInfo *ai = listNodeValue(lastnode);
    return ai->file_name;
}

/* Called in `backgroundRewriteDoneHandler`. when AOFRW success, This
 * function will change the AOF file type in 'incr_aof_list' from
 * AOF_FILE_TYPE_INCR to AOF_FILE_TYPE_HIST, and move them to the
 * 'history_aof_list'.
 */
/**在`backgroundRewriteDoneHandler`中调用。
 * 当 AOFRW 成功时，此函数会将 'incr_aof_list' 中的 AOF 文件类型从 AOF_FILE_TYPE_INCR 更改为 AOF_FILE_TYPE_HIST，
 * 并将它们移动到 'history_aof_list'。*/
void markRewrittenIncrAofAsHistory(aofManifest *am) {
    serverAssert(am != NULL);
    if (!listLength(am->incr_aof_list)) {
        return;
    }

    listNode *ln;
    listIter li;

    listRewindTail(am->incr_aof_list, &li);

    /* "server.aof_fd != -1" means AOF enabled, then we must skip the
     * last AOF, because this file is our currently writing. */
    /**“server.aof_fd != -1”表示启用了AOF，那么我们必须跳过最后一个AOF，因为这个文件是我们当前正在编写的。*/
    if (server.aof_fd != -1) {
        ln = listNext(&li);
        serverAssert(ln != NULL);
    }

    /* Move aofInfo from 'incr_aof_list' to 'history_aof_list'. */
    /**将 aofInfo 从“incr_aof_list”移动到“history_aof_list”。*/
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);

        aofInfo *hai = aofInfoDup(ai);
        hai->file_type = AOF_FILE_TYPE_HIST;
        listAddNodeHead(am->history_aof_list, hai);
        listDelNode(am->incr_aof_list, ln);
    }

    am->dirty = 1;
}

/* Write the formatted manifest string to disk. */
/**将格式化的清单字符串写入磁盘。*/
int writeAofManifestFile(sds buf) {
    int ret = C_OK;
    ssize_t nwritten;
    int len;

    sds am_name = getAofManifestFileName();
    sds am_filepath = makePath(server.aof_dirname, am_name);
    sds tmp_am_name = getTempAofManifestFileName();
    sds tmp_am_filepath = makePath(server.aof_dirname, tmp_am_name);

    int fd = open(tmp_am_filepath, O_WRONLY|O_TRUNC|O_CREAT, 0644);
    if (fd == -1) {
        serverLog(LL_WARNING, "Can't open the AOF manifest file %s: %s",
            tmp_am_name, strerror(errno));

        ret = C_ERR;
        goto cleanup;
    }

    len = sdslen(buf);
    while(len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR) continue;

            serverLog(LL_WARNING, "Error trying to write the temporary AOF manifest file %s: %s",
                tmp_am_name, strerror(errno));

            ret = C_ERR;
            goto cleanup;
        }

        len -= nwritten;
        buf += nwritten;
    }

    if (redis_fsync(fd) == -1) {
        serverLog(LL_WARNING, "Fail to fsync the temp AOF file %s: %s.",
            tmp_am_name, strerror(errno));

        ret = C_ERR;
        goto cleanup;
    }

    if (rename(tmp_am_filepath, am_filepath) != 0) {
        serverLog(LL_WARNING,
            "Error trying to rename the temporary AOF manifest file %s into %s: %s",
            tmp_am_name, am_name, strerror(errno));

        ret = C_ERR;
        goto cleanup;
    }

    /* Also sync the AOF directory as new AOF files may be added in the directory */
    if (fsyncFileDir(am_filepath) == -1) {
        serverLog(LL_WARNING, "Fail to fsync AOF directory %s: %s.",
            am_filepath, strerror(errno));

        ret = C_ERR;
        goto cleanup;
    }

cleanup:
    if (fd != -1) close(fd);
    sdsfree(am_name);
    sdsfree(am_filepath);
    sdsfree(tmp_am_name);
    sdsfree(tmp_am_filepath);
    return ret;
}

/* Persist the aofManifest information pointed to by am to disk. */
/**将am指向的aofManifest信息持久化到磁盘。*/
int persistAofManifest(aofManifest *am) {
    if (am->dirty == 0) {
        return C_OK;
    }

    sds amstr = getAofManifestAsString(am);
    int ret = writeAofManifestFile(amstr);
    sdsfree(amstr);
    if (ret == C_OK) am->dirty = 0;
    return ret;
}

/* Called in `loadAppendOnlyFiles` when we upgrade from a old version redis.
 *
 * 1) Create AOF directory use 'server.aof_dirname' as the name.
 * 2) Use 'server.aof_filename' to construct a BASE type aofInfo and add it to
 *    aofManifest, then persist the manifest file to AOF directory.
 * 3) Move the old AOF file (server.aof_filename) to AOF directory.
 *
 * If any of the above steps fails or crash occurs, this will not cause any
 * problems, and redis will retry the upgrade process when it restarts.
 */
/**当我们从旧版本的 redis 升级时，在 `loadAppendOnlyFiles` 中调用。
 *   1) 创建 AOF 目录，使用 'server.aof_dirname' 作为名称。
 *   2）使用'server.aof_filename'构造一个BASE类型的aofInfo并将其添加到aofManifest，然后将manifest文件持久化到AOF目录。
 *   3) 将旧的 AOF 文件 (server.aof_filename) 移动到 AOF 目录。
 * 如果上述任一步骤失败或发生崩溃，这不会导致任何问题，redis 将在重新启动时重试升级过程。*/
void aofUpgradePrepare(aofManifest *am) {
    serverAssert(!aofFileExist(server.aof_filename));

    /* Create AOF directory use 'server.aof_dirname' as the name. */
    /**创建 AOF 目录使用 'server.aof __dirname' 为名称。*/
    if (dirCreateIfMissing(server.aof_dirname) == -1) {
        serverLog(LL_WARNING, "Can't open or create append-only dir %s: %s",
            server.aof_dirname, strerror(errno));
        exit(1);
    }

    /* Manually construct a BASE type aofInfo and add it to aofManifest. */
    /**手动构造一个 BASE 类型的 aofInfo 并将其添加到 aofManifest。*/
    if (am->base_aof_info) aofInfoFree(am->base_aof_info);
    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdsnew(server.aof_filename);
    ai->file_seq = 1;
    ai->file_type = AOF_FILE_TYPE_BASE;
    am->base_aof_info = ai;
    am->curr_base_file_seq = 1;
    am->dirty = 1;

    /* Persist the manifest file to AOF directory. */
    /**将清单文件保存到 AOF 目录。*/
    if (persistAofManifest(am) != C_OK) {
        exit(1);
    }

    /* Move the old AOF file to AOF directory. */
    /**将旧的 AOF 文件移动到 AOF 目录。*/
    sds aof_filepath = makePath(server.aof_dirname, server.aof_filename);
    if (rename(server.aof_filename, aof_filepath) == -1) {
        serverLog(LL_WARNING,
            "Error trying to move the old AOF file %s into dir %s: %s",
            server.aof_filename,
            server.aof_dirname,
            strerror(errno));
        sdsfree(aof_filepath);
        exit(1);
    }
    sdsfree(aof_filepath);

    serverLog(LL_NOTICE, "Successfully migrated an old-style AOF file (%s) into the AOF directory (%s).",
        server.aof_filename, server.aof_dirname);
}

/* When AOFRW success, the previous BASE and INCR AOFs will
 * become HISTORY type and be moved into 'history_aof_list'.
 *
 * The function will traverse the 'history_aof_list' and submit
 * the delete task to the bio thread.
 */
/**当 AOFRW 成功时，之前的 BASE 和 INCR AOF 将变为 HISTORY 类型并被移动到 'history_aof_list' 中。
 *
 * 该函数将遍历'history_aof_list'并将删除任务提交给生物线程。*/
int aofDelHistoryFiles(void) {
    if (server.aof_manifest == NULL ||
        server.aof_disable_auto_gc == 1 ||
        !listLength(server.aof_manifest->history_aof_list))
    {
        return C_OK;
    }

    listNode *ln;
    listIter li;

    listRewind(server.aof_manifest->history_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_HIST);
        serverLog(LL_NOTICE, "Removing the history file %s in the background", ai->file_name);
        sds aof_filepath = makePath(server.aof_dirname, ai->file_name);
        bg_unlink(aof_filepath);
        sdsfree(aof_filepath);
        listDelNode(server.aof_manifest->history_aof_list, ln);
    }

    server.aof_manifest->dirty = 1;
    return persistAofManifest(server.aof_manifest);
}

/* Used to clean up temp INCR AOF when AOFRW fails. */
/**用于在 AOFRW 失败时清理 temp INCR AOF。*/
void aofDelTempIncrAofFile() {
    sds aof_filename = getTempIncrAofName();
    sds aof_filepath = makePath(server.aof_dirname, aof_filename);
    serverLog(LL_NOTICE, "Removing the temp incr aof file %s in the background", aof_filename);
    bg_unlink(aof_filepath);
    sdsfree(aof_filepath);
    sdsfree(aof_filename);
    return;
}

/* Called after `loadDataFromDisk` when redis start. If `server.aof_state` is
 * 'AOF_ON', It will do three things:
 * 1. Force create a BASE file when redis starts with an empty dataset
 * 2. Open the last opened INCR type AOF for writing, If not, create a new one
 * 3. Synchronously update the manifest file to the disk
 *
 * If any of the above steps fails, the redis process will exit.
 */
/**redis 启动时在 `loadDataFromDisk` 之后调用。如果 `server.aof_state` 为 'AOF_ON'，它会做三件事：
 *  1. redis 以空数据集启动时强制创建一个 BASE 文件
 *  2. 打开最后打开的 INCR 类型 AOF 进行写入，如果没有，则新建一个
 *  3. 同步更新manifest文件到磁盘如果以上任一步骤失败，redis进程将退出。*/
void aofOpenIfNeededOnServerStart(void) {
    if (server.aof_state != AOF_ON) {
        return;
    }

    serverAssert(server.aof_manifest != NULL);
    serverAssert(server.aof_fd == -1);

    if (dirCreateIfMissing(server.aof_dirname) == -1) {
        serverLog(LL_WARNING, "Can't open or create append-only dir %s: %s",
            server.aof_dirname, strerror(errno));
        exit(1);
    }

    /* If we start with an empty dataset, we will force create a BASE file. */
    /**如果我们从一个空数据集开始，我们将强制创建一个 BASE 文件。*/
    size_t incr_aof_len = listLength(server.aof_manifest->incr_aof_list);
    if (!server.aof_manifest->base_aof_info && !incr_aof_len) {
        sds base_name = getNewBaseFileNameAndMarkPreAsHistory(server.aof_manifest);
        sds base_filepath = makePath(server.aof_dirname, base_name);
        if (rewriteAppendOnlyFile(base_filepath) != C_OK) {
            exit(1);
        }
        sdsfree(base_filepath);
        serverLog(LL_NOTICE, "Creating AOF base file %s on server start",
            base_name);
    }

    /* Because we will 'exit(1)' if open AOF or persistent manifest fails, so
     * we don't need atomic modification here. */
    /**因为如果打开 AOF 或持久化清单失败，我们将 'exit(1)'，所以这里我们不需要原子修改。*/
    sds aof_name = getLastIncrAofName(server.aof_manifest);

    /* Here we should use 'O_APPEND' flag. */
    /**这里我们应该使用 'O_APPEND' 标志。*/
    sds aof_filepath = makePath(server.aof_dirname, aof_name);
    server.aof_fd = open(aof_filepath, O_WRONLY|O_APPEND|O_CREAT, 0644);
    sdsfree(aof_filepath);
    if (server.aof_fd == -1) {
        serverLog(LL_WARNING, "Can't open the append-only file %s: %s",
            aof_name, strerror(errno));
        exit(1);
    }

    /* Persist our changes. 坚持我们的改变。*/
    int ret = persistAofManifest(server.aof_manifest);
    if (ret != C_OK) {
        exit(1);
    }

    server.aof_last_incr_size = getAppendOnlyFileSize(aof_name, NULL);

    if (incr_aof_len) {
        serverLog(LL_NOTICE, "Opening AOF incr file %s on server start", aof_name);
    } else {
        serverLog(LL_NOTICE, "Creating AOF incr file %s on server start", aof_name);
    }
}

int aofFileExist(char *filename) {
    sds file_path = makePath(server.aof_dirname, filename);
    int ret = fileExist(file_path);
    sdsfree(file_path);
    return ret;
}

/* Called in `rewriteAppendOnlyFileBackground`. If `server.aof_state`
 * is 'AOF_ON', It will do two things:
 * 1. Open a new INCR type AOF for writing
 * 2. Synchronously update the manifest file to the disk
 * 在 `rewriteAppendOnlyFileBackground` 中调用。如果 `server.aof_state` 为 'AOF_ON'，它会做两件事：
 *   1. 打开一个新的 INCR 类型 AOF 进行写入
 *   2. 同步更新清单文件到磁盘
 *
 * The above two steps of modification are atomic, that is, if
 * any step fails, the entire operation will rollback and returns
 * C_ERR, and if all succeeds, it returns C_OK.
 * 上面两步的修改都是原子的，即如果任何一步失败，整个操作会回滚并返回C_ERR，如果都成功则返回C_OK。
 * 
 * If `server.aof_state` is 'AOF_WAIT_REWRITE', It will open a temporary INCR AOF 
 * file to accumulate data during AOF_WAIT_REWRITE, and it will eventually be 
 * renamed in the `backgroundRewriteDoneHandler` and written to the manifest file.
 * 如果 `server.aof_state` 为 'AOF_WAIT_REWRITE'，它会在 AOF_WAIT_REWRITE 期间打开一个临时的 INCR AOF 文件来积累数据，
 * 最终会在 `backgroundRewriteDoneHandler` 中重命名并写入 manifest 文件。
 * */
int openNewIncrAofForAppend(void) {
    serverAssert(server.aof_manifest != NULL);
    int newfd = -1;
    aofManifest *temp_am = NULL;
    sds new_aof_name = NULL;

    /* Only open new INCR AOF when AOF enabled. */
    /**仅在启用 AOF 时打开新的 INCR AOF。*/
    if (server.aof_state == AOF_OFF) return C_OK;

    /* Open new AOF. */
    if (server.aof_state == AOF_WAIT_REWRITE) {
        /* Use a temporary INCR AOF file to accumulate data during AOF_WAIT_REWRITE. */
        /**在 AOF_WAIT_REWRITE 期间使用临时 INCR AOF 文件来累积数据。*/
        new_aof_name = getTempIncrAofName();
    } else {
        /* Dup a temp aof_manifest to modify. */
        /**复制一个临时 aof_manifest 进行修改。*/
        temp_am = aofManifestDup(server.aof_manifest);
        new_aof_name = sdsdup(getNewIncrAofName(temp_am));
    }
    sds new_aof_filepath = makePath(server.aof_dirname, new_aof_name);
    newfd = open(new_aof_filepath, O_WRONLY|O_TRUNC|O_CREAT, 0644);
    sdsfree(new_aof_filepath);
    if (newfd == -1) {
        serverLog(LL_WARNING, "Can't open the append-only file %s: %s",
            new_aof_name, strerror(errno));
        goto cleanup;
    }

    if (temp_am) {
        /* Persist AOF Manifest. AOF 宣言仍然存在。*/
        if (persistAofManifest(temp_am) == C_ERR) {
            goto cleanup;
        }
    }

    serverLog(LL_NOTICE, "Creating AOF incr file %s on background rewrite",
            new_aof_name);
    sdsfree(new_aof_name);

    /* If reaches here, we can safely modify the `server.aof_manifest`
     * and `server.aof_fd`. */
    /**如果到达这里，我们可以安全地修改`server.aof_manifest`和`server.aof_fd`。*/

    /* Close old aof_fd if needed. */
    if (server.aof_fd != -1) bioCreateCloseJob(server.aof_fd);
    server.aof_fd = newfd;

    /* Reset the aof_last_incr_size. 重置 aof_last_incr_size。*/
    server.aof_last_incr_size = 0;
    /* Update `server.aof_manifest`. */
    if (temp_am) aofManifestFreeAndUpdate(temp_am);
    return C_OK;

cleanup:
    if (new_aof_name) sdsfree(new_aof_name);
    if (newfd != -1) close(newfd);
    if (temp_am) aofManifestFree(temp_am);
    return C_ERR;
}

/* Whether to limit the execution of Background AOF rewrite.
 * 是否限制后台 AOF 重写的执行。
 *
 * At present, if AOFRW fails, redis will automatically retry. If it continues
 * to fail, we may get a lot of very small INCR files. so we need an AOFRW
 * limiting measure.
 * 目前如果 AOFRW 失败，redis 会自动重试。如果它继续失败，我们可能会得到很多非常小的 INCR 文件。
 * 所以我们需要一个 AOFRW 限制措施。
 *
 * We can't directly use `server.aof_current_size` and `server.aof_last_incr_size`,
 * because there may be no new writes after AOFRW fails.
 * 我们不能直接使用 `server.aof_current_size` 和 `server.aof_last_incr_size`，因为 AOFRW 失败后可能没有新的写入。
 *
 * So, we use time delay to achieve our goal. When AOFRW fails, we delay the execution
 * of the next AOFRW by 1 minute. If the next AOFRW also fails, it will be delayed by 2
 * minutes. The next is 4, 8, 16, the maximum delay is 60 minutes (1 hour).
 * 因此，我们使用时间延迟来实现我们的目标。当 AOFRW 失败时，我们将下一个 AOFRW 的执行延迟 1 分钟。
 * 如果下一次 AOFRW 也失败，则会延迟 2 分钟。接下来是4、8、16，最大延迟为60分钟（1小时）。
 *
 * During the limit period, we can still use the 'bgrewriteaof' command to execute AOFRW
 * immediately.
 * 在限制期内，我们仍然可以使用 'bgrewriteaof' 命令立即执行 AOFRW。
 *
 * Return 1 means that AOFRW is limited and cannot be executed. 0 means that we can execute
 * AOFRW, which may be that we have reached the 'next_rewrite_time' or the number of INCR
 * AOFs has not reached the limit threshold.
 * 返回 1 表示 AOFRW 受限，无法执行。 0 表示我们可以执行 AOFRW，
 * 这可能是我们已经达到了 'next_rewrite_time' 或者 INCR AOF 的数量没有达到限制阈值。
 * */
#define AOF_REWRITE_LIMITE_THRESHOLD    3
#define AOF_REWRITE_LIMITE_MAX_MINUTES  60 /* 1 hour */
int aofRewriteLimited(void) {
    static int next_delay_minutes = 0;
    static time_t next_rewrite_time = 0;

    if (server.stat_aofrw_consecutive_failures < AOF_REWRITE_LIMITE_THRESHOLD) {
        /* We may be recovering from limited state, so reset all states. */
        /**我们可能正在从有限状态中恢复，所以重置所有状态。*/
        next_delay_minutes = 0;
        next_rewrite_time = 0;
        return 0;
    }

    /* if it is in the limiting state, then check if the next_rewrite_time is reached */
    /**如果处于限制状态，则检查是否达到next_rewrite_time*/
    if (next_rewrite_time != 0) {
        if (server.unixtime < next_rewrite_time) {
            return 1;
        } else {
            next_rewrite_time = 0;
            return 0;
        }
    }

    next_delay_minutes = (next_delay_minutes == 0) ? 1 : (next_delay_minutes * 2);
    if (next_delay_minutes > AOF_REWRITE_LIMITE_MAX_MINUTES) {
        next_delay_minutes = AOF_REWRITE_LIMITE_MAX_MINUTES;
    }

    next_rewrite_time = server.unixtime + next_delay_minutes * 60;
    serverLog(LL_WARNING,
        "Background AOF rewrite has repeatedly failed and triggered the limit, will retry in %d minutes", next_delay_minutes);
    return 1;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */

/* Return true if an AOf fsync is currently already in progress in a
 * BIO thread. */
/**如果 AOf fsync 当前已经在 BIO 线程中进行，则返回 true。*/
int aofFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
}

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
/**启动一个后台任务，在另一个线程中针对指定的文件描述符（AOF 文件之一）执行 fsync()。*/
void aof_background_fsync(int fd) {
    bioCreateFsyncJob(fd);
}

/* Kills an AOFRW child process if exists */
/**如果存在则杀死 AOFRW 子进程*/
void killAppendOnlyChild(void) {
    int statloc;
    /* No AOFRW child? return. 没有 AOFRW 孩子？返回。*/
    if (server.child_type != CHILD_TYPE_AOF) return;
    /* Kill AOFRW child, wait for child exit. */
    /**杀死 AOFRW 孩子，等待孩子退出。*/
    serverLog(LL_NOTICE,"Killing running AOF rewrite child: %ld",
        (long) server.child_pid);
    if (kill(server.child_pid,SIGUSR1) != -1) {
        while(waitpid(-1, &statloc, 0) != server.child_pid);
    }
    aofRemoveTempFile(server.child_pid);
    resetChildState();
    server.aof_rewrite_time_start = -1;
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
/**当用户在运行时使用 CONFIG 命令从“appendonly yes”切换到“appendonly no”时调用。*/
void stopAppendOnly(void) {
    serverAssert(server.aof_state != AOF_OFF);
    flushAppendOnlyFile(1);
    if (redis_fsync(server.aof_fd) == -1) {
        serverLog(LL_WARNING,"Fail to fsync the AOF file: %s",strerror(errno));
    } else {
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    }
    close(server.aof_fd);

    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_scheduled = 0;
    server.aof_last_incr_size = 0;
    killAppendOnlyChild();
    sdsfree(server.aof_buf);
    server.aof_buf = sdsempty();
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
/**当用户在运行时使用 CONFIG 命令从“appendonly no”切换到“appendonly yes”时调用。*/
int startAppendOnly(void) {
    serverAssert(server.aof_state == AOF_OFF);

    server.aof_state = AOF_WAIT_REWRITE;
    if (hasActiveChildProcess() && server.child_type != CHILD_TYPE_AOF) {
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING,"AOF was enabled but there is already another background operation. An AOF background was scheduled to start when possible.");
    } else if (server.in_exec){
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING,"AOF was enabled during a transaction. An AOF background was scheduled to start when possible.");
    } else {
        /* If there is a pending AOF rewrite, we need to switch it off and
         * start a new one: the old one cannot be reused because it is not
         * accumulating the AOF buffer. */
        /**如果有一个挂起的 AOF 重写，我们需要关闭它并开始一个新的：旧的不能被重用，因为它没有累积 AOF 缓冲区。*/
        if (server.child_type == CHILD_TYPE_AOF) {
            serverLog(LL_WARNING,"AOF was enabled but there is already an AOF rewriting in background. Stopping background AOF and starting a rewrite now.");
            killAppendOnlyChild();
        }

        if (rewriteAppendOnlyFileBackground() == C_ERR) {
            server.aof_state = AOF_OFF;
            serverLog(LL_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
            return C_ERR;
        }
    }
    server.aof_last_fsync = server.unixtime;
    /* If AOF fsync error in bio job, we just ignore it and log the event. */
    /**如果 bio 作业中出现 AOF fsync 错误，我们将忽略它并记录该事件。*/
    int aof_bio_fsync_status;
    atomicGet(server.aof_bio_fsync_status, aof_bio_fsync_status);
    if (aof_bio_fsync_status == C_ERR) {
        serverLog(LL_WARNING,
            "AOF reopen, just ignore the AOF fsync error in bio job");
        atomicSet(server.aof_bio_fsync_status,C_OK);
    }

    /* If AOF was in error state, we just ignore it and log the event. */
    /**如果 AOF 处于错误状态，我们就忽略它并记录事件。*/
    if (server.aof_last_write_status == C_ERR) {
        serverLog(LL_WARNING,"AOF reopen, just ignore the last error.");
        server.aof_last_write_status = C_OK;
    }
    return C_OK;
}

/* This is a wrapper to the write syscall in order to retry on short writes
 * or if the syscall gets interrupted. It could look strange that we retry
 * on short writes given that we are writing to a block device: normally if
 * the first call is short, there is a end-of-space condition, so the next
 * is likely to fail. However apparently in modern systems this is no longer
 * true, and in general it looks just more resilient to retry the write. If
 * there is an actual error condition we'll get it at the next try. */
/**这是 write 系统调用的包装器，以便在短写入或系统调用被中断时重试。
 * 鉴于我们正在写入块设备，我们重试短写入可能看起来很奇怪：通常如果第一次调用很短，则存在空间结束条件，因此下一次可能会失败。
 * 然而，显然在现代系统中，这不再是正确的，而且总的来说，重试写入看起来更有弹性。
 * 如果存在实际错误情况，我们将在下次尝试时得到它。*/
ssize_t aofWrite(int fd, const char *buf, size_t len) {
    ssize_t nwritten = 0, totwritten = 0;

    while(len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR) continue;
            return totwritten ? totwritten : -1;
        }

        len -= nwritten;
        buf += nwritten;
        totwritten += nwritten;
    }

    return totwritten;
}

/* Write the append only file buffer on disk.
 * 将仅附加的文件缓冲区写入磁盘。
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 * 由于我们需要在回复客户端之前写入 AOF，并且客户端套接字获得写入的唯一方法是在事件循环时进入，
 * 所以我们将所有 AOF 写入累积在内存缓冲区中，并使用此函数将其写入磁盘就在再次进入事件循环之前。
 *
 * About the 'force' argument:
 * 关于'force'论点：
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 * 当 fsync 策略设置为 'everysec' 时，如果后台线程中仍有 fsync() 进行，我们可能会延迟刷新，
 * 因为例如在 Linux 上 write(2) 无论如何都会被后台 fsync 阻塞。
 * 当这种情况发生时，我们记得有一些 aof 缓冲区要尽快刷新，并将尝试在 serverCron() 函数中执行此操作。
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync.
 * 但是，如果 force 设置为 1，我们将不考虑背景 fsync 进行写入。*/
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;

    if (sdslen(server.aof_buf) == 0) {
        /* Check if we need to do fsync even the aof buffer is empty,
         * because previously in AOF_FSYNC_EVERYSEC mode, fsync is
         * called only when aof buffer is not empty, so if users
         * stop write commands before fsync called in one second,
         * the data in page cache cannot be flushed in time. */
        /**检查是否需要在aof缓冲区为空的情况下进行fsync，因为之前在AOF_FSYNC_EVERYSEC模式下，
         * 只有在aof缓冲区不为空时才调用fsync，所以如果用户在一秒钟内调用fsync之前停止写命令，
         * 页面缓存中的数据将无法及时冲洗。*/
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC &&
            server.aof_fsync_offset != server.aof_current_size &&
            server.unixtime > server.aof_last_fsync &&
            !(sync_in_progress = aofFsyncInProgress())) {
            goto try_fsync;
        } else {
            return;
        }
    }

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = aofFsyncInProgress();

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        /**使用这个附加 fsync 策略，我们进行后台 fsync。如果 fsync 仍在进行中，我们可以尝试将写入延迟几秒钟。*/
        if (sync_in_progress) {
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. */
                /**没有之前的 write 推迟，记住我们推迟了 flush 和 return。*/
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                /**我们已经在等待 fsync 完成了，但是不到两秒钟就可以了。再推迟。*/
                return;
            }
            /* Otherwise fall through, and go write since we can't wait
             * over two seconds. */
            /**否则失败，然后去写，因为我们不能等待超过两秒钟。*/
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */
    /**我们想要执行一次写入。至少如果我们正在编写的文件系统是一个真正的物理文件系统，这应该保证是原子的。
     * 虽然这将使我们免于服务器被杀死，但我认为整个服务器因电源问题或类似问题而停止并没有什么可做的*/

    if (server.aof_flush_sleep && sdslen(server.aof_buf)) {
        usleep(server.aof_flush_sleep);
    }

    latencyStartMonitor(latency);
    nwritten = aofWrite(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. */
    /**我们希望为延迟写入捕获不同的事件：延迟发生在挂起的 fsync 或正在保存的子节点处于活动状态时，以及上述两个条件缺失时。
     * 我们还使用一个额外的事件名称来保存所有样本，这对于图形监控目的很有用。*/
    if (sync_in_progress) {
        latencyAddSampleIfNeeded("aof-write-pending-fsync",latency);
    } else if (hasActiveChildProcess()) {
        latencyAddSampleIfNeeded("aof-write-active-child",latency);
    } else {
        latencyAddSampleIfNeeded("aof-write-alone",latency);
    }
    latencyAddSampleIfNeeded("aof-write",latency);

    /* We performed the write so reset the postponed flush sentinel to zero. */
    /**我们执行了写入，因此将推迟的刷新标记重置为零。*/
    server.aof_flush_postponed_start = 0;

    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. */
        /**将记录速率限制为每 AOF_WRITE_LOG_ERROR_RATE 秒 1 行。*/
        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. */
        /**记录 AOF 写入错误并记录错误代码。*/
        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING,"Error writing to the AOF file: %s",
                    strerror(errno));
            }
            server.aof_last_write_errno = errno;
        } else {
            if (can_log) {
                serverLog(LL_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (long long)nwritten,
                                       (long long)sdslen(server.aof_buf));
            }

            if (ftruncate(server.aof_fd, server.aof_last_incr_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", strerror(errno));
                }
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                /**如果 ftruncate() 成功，我们可以将 nwritten 设置为 -1，因为不再有部分数据进入 AOF。*/
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. 处理 AOF 写入错误。*/
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the reply
             * for the client is already in the output buffers (both writes and
             * reads), and the changes to the db can't be rolled back. Since we
             * have a contract with the user that on acknowledged or observed
             * writes are is synced on disk, we must exit. */
            /**当 fsync 策略为 ALWAYS 时，我们无法恢复，因为客户端的回复已经在输出缓冲区（写入和读取）中，
             * 并且对 db 的更改无法回滚。由于我们与用户签订了合同，确认或观察到的写入在磁盘上同步，因此我们必须退出。*/
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            /**从失败的写入中恢复，将数据留在缓冲区中。但是，只要错误条件未清除，就设置错误以停止接受写入。*/
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            /**如果存在部分写入，则修剪 sds 缓冲区，并且无法使用 ftruncate(2) 撤消它。*/
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                server.aof_last_incr_size += nwritten;
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... 我们将在下次call时再试一次...*/
        }
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        /**成功write(2)。如果 AOF 处于错误状态，则恢复 OK 状态并记录事件。*/
        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }
    server.aof_current_size += nwritten;
    server.aof_last_incr_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). */
    /**当缓冲区足够小时重新使用它。最大值来自 4k 的竞技场大小减去一些开销（但在其他方面是任意的）。*/
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000) {
        sdsclear(server.aof_buf);
    } else {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
    }

try_fsync:
    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    /**如果 no-appendfsync-on-rewrite 设置为 yes 并且有孩子在后台进行 IO，则不要 fsync。*/
    if (server.aof_no_fsync_on_rewrite && hasActiveChildProcess())
        return;

    /* Perform the fsync if needed. 如果需要，执行 fsync。*/
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* redis_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        /**redis_fsync 被定义为 Linux 的 fdatasync() 以避免刷新元数据。*/
        latencyStartMonitor(latency);
        /* Let's try to get this data on the disk. To guarantee data safe when
         * the AOF fsync policy is 'always', we should exit if failed to fsync
         * AOF (see comment next to the exit(1) after write error above). */
        /**让我们尝试在磁盘上获取这些数据。当 AOF fsync 策略为“always”时，为了保证数据安全，
         * 如果 fsync AOF 失败，我们应该退出（参见上面的写入错误后 exit(1) 旁边的注释）。*/
        if (redis_fsync(server.aof_fd) == -1) {
            serverLog(LL_WARNING,"Can't persist AOF for fsync error when the "
              "AOF fsync policy is 'always': %s. Exiting...", strerror(errno));
            exit(1);
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-fsync-always",latency);
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        if (!sync_in_progress) {
            aof_background_fsync(server.aof_fd);
            server.aof_fsync_offset = server.aof_current_size;
        }
        server.aof_last_fsync = server.unixtime;
    }
}

sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}

/* Generate a piece of timestamp annotation for AOF if current record timestamp
 * in AOF is not equal server unix time. If we specify 'force' argument to 1,
 * we would generate one without check, currently, it is useful in AOF rewriting
 * child process which always needs to record one timestamp at the beginning of
 * rewriting AOF.
 * 如果 AOF 中的当前记录时间戳不等于服务器 unix 时间，则为 AOF 生成一条时间戳注释。
 * 如果我们将 'force' 参数指定为 1，我们将生成一个不检查，目前，它在 AOF 重写子进程中非常有用，
 * 子进程总是需要在重写 AOF 开始时记录一个时间戳。
 *
 * Timestamp annotation format is "#TS:${timestamp}\r\n". "TS" is short of
 * timestamp and this method could save extra bytes in AOF.
 * 时间戳注释格式为“TS:{timestamp}\r\n”。 “TS”缺少时间戳，这种方法可以在 AOF 中节省额外的字节。*/
sds genAofTimestampAnnotationIfNeeded(int force) {
    sds ts = NULL;

    if (force || server.aof_cur_timestamp < server.unixtime) {
        server.aof_cur_timestamp = force ? time(NULL) : server.unixtime;
        ts = sdscatfmt(sdsempty(), "#TS:%I\r\n", server.aof_cur_timestamp);
        serverAssert(sdslen(ts) <= AOF_ANNOTATION_LINE_MAX_LEN);
    }
    return ts;
}

void feedAppendOnlyFile(int dictid, robj **argv, int argc) {
    sds buf = sdsempty();

    serverAssert(dictid >= 0 && dictid < server.dbnum);

    /* Feed timestamp if needed 如果需要，提要时间戳*/
    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(0);
        if (ts != NULL) {
            buf = sdscatsds(buf, ts);
            sdsfree(ts);
        }
    }

    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. */
    /**此命令所针对的数据库与我们附加的最后一个命令不同。需要发出 SELECT 命令。*/
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        server.aof_selected_db = dictid;
    }

    /* All commands should be propagated the same way in AOF as in replication.
     * No need for AOF-specific translation. */
    /**所有命令在 AOF 中的传播方式与在复制中的传播方式相同。不需要特定于 AOF 的翻译。*/
    buf = catAppendOnlyGenericCommand(buf,argc,argv);

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    /**附加到 AOF 缓冲区。这将在重新进入事件循环之前刷新到磁盘上，因此在客户端将获得有关所执行操作的肯定回复之前。*/
    if (server.aof_state == AOF_ON ||
        (server.aof_state == AOF_WAIT_REWRITE && server.child_type == CHILD_TYPE_AOF))
    {
        server.aof_buf = sdscatlen(server.aof_buf, buf, sdslen(buf));
    }

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client.
 * 在 Redis 中，命令总是在客户端的上下文中执行，因此为了加载仅附加文件，我们需要创建一个假客户端。*/
struct client *createAOFClient(void) {
    struct client *c = createClient(NULL);

    c->id = CLIENT_ID_AOF; /* So modules can identify it's the AOF client. 因此模块可以识别它是 AOF 客户端。*/

    /*
     * The AOF client should never be blocked (unlike master
     * replication connection).
     * This is because blocking the AOF client might cause
     * deadlock (because potentially no one will unblock it).
     * Also, if the AOF client will be blocked just for
     * background processing there is a chance that the
     * command execution order will be violated.
     */
    /**AOF 客户端永远不应该被阻塞（与主复制连接不同）。
     * 这是因为阻塞 AOF 客户端可能会导致死锁（因为可能没有人会解除阻塞）。
     * 此外，如果 AOF 客户端仅因后台处理而被阻止，则可能会违反命令执行顺序。*/
    c->flags = CLIENT_DENY_BLOCKING;

    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    /**我们将假客户端设置为等待同步的从属，这样 Redis 就不会尝试向该客户端发送回复。*/
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    return c;
}

/* Replay an append log file. On success AOF_OK or AOF_TRUNCATED is returned,
 * otherwise, one of the following is returned:
 * 重播附加日志文件。成功时返回 AOF_OK 或 AOF_TRUNCATED，否则返回以下之一：
 * AOF_OPEN_ERR: Failed to open the AOF file.
 * AOF_NOT_EXIST: AOF file doesn't exist.
 * AOF_EMPTY: The AOF file is empty (nothing to load).
 * AOF_FAILED: Failed to load the AOF file. */
int loadSingleAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    long loops = 0;
    off_t valid_up_to = 0; /* Offset of latest well-formed command loaded. 加载的最新格式良好的命令的偏移量。*/
    off_t valid_before_multi = 0; /* Offset before MULTI command loaded. 加载 MULTI 命令之前的偏移量。*/
    off_t last_progress_report_size = 0;
    int ret = AOF_OK;

    sds aof_filepath = makePath(server.aof_dirname, filename);
    FILE *fp = fopen(aof_filepath, "r");
    if (fp == NULL) {
        int en = errno;
        if (redis_stat(aof_filepath, &sb) == 0 || errno != ENOENT) {
            serverLog(LL_WARNING,"Fatal error: can't open the append log file %s for reading: %s", filename, strerror(en));
            sdsfree(aof_filepath);
            return AOF_OPEN_ERR;
        } else {
            serverLog(LL_WARNING,"The append log file %s doesn't exist: %s", filename, strerror(errno));
            sdsfree(aof_filepath);
            return AOF_NOT_EXIST;
        }
    }

    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        sdsfree(aof_filepath);
        return AOF_EMPTY;
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    /**暂时禁用 AOF，以防止 EXEC 将 MULTI 提供给我们将要读取的同一文件。*/
    server.aof_state = AOF_OFF;

    client *old_client = server.current_client;
    fakeClient = server.current_client = createAOFClient();

    /* Check if the AOF file is in RDB format (it may be RDB encoded base AOF
     * or old style RDB-preamble AOF). In that case we need to load the RDB file 
     * and later continue loading the AOF tail if it is an old style RDB-preamble AOF. */
    /**检查 AOF 文件是否为 RDB 格式（可能是 RDB 编码的基本 AOF 或旧式 RDB-preamble AOF）。
     * 在这种情况下，我们需要加载 RDB 文件，然后如果它是旧式 RDB 前导 AOF，则继续加载 AOF 尾部。*/
    char sig[5]; /* "REDIS" */
    if (fread(sig,1,5,fp) != 5 || memcmp(sig,"REDIS",5) != 0) {
        /* Not in RDB format, seek back at 0 offset. */
        /**不是 RDB 格式，在 0 偏移处回溯。*/
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
    } else {
        /* RDB format. Pass loading the RDB functions. */
        /**RDB 格式。通过加载 RDB 函数。*/
        rio rdb;
        int old_style = !strcmp(filename, server.aof_filename);
        if (old_style)
            serverLog(LL_NOTICE, "Reading RDB preamble from AOF file...");
        else 
            serverLog(LL_NOTICE, "Reading RDB base file on AOF loading..."); 

        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
        rioInitWithFile(&rdb,fp);
        if (rdbLoadRio(&rdb,RDBFLAGS_AOF_PREAMBLE,NULL) != C_OK) {
            if (old_style)
                serverLog(LL_WARNING, "Error reading the RDB preamble of the AOF file %s, AOF loading aborted", filename);
            else
                serverLog(LL_WARNING, "Error reading the RDB base file %s, AOF loading aborted", filename);

            goto readerr;
        } else {
            loadingAbsProgress(ftello(fp));
            last_progress_report_size = ftello(fp);
            if (old_style) serverLog(LL_NOTICE, "Reading the remaining AOF tail...");
        }
    }

    /* Read the actual AOF file, in REPL format, command by command. */
    /**逐个命令读取 REPL 格式的实际 AOF 文件。*/
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[AOF_ANNOTATION_LINE_MAX_LEN];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time 不时为客户服务*/
        if (!(loops++ % 1024)) {
            off_t progress_delta = ftello(fp) - last_progress_report_size;
            loadingIncrProgress(progress_delta);
            last_progress_report_size += progress_delta;
            processEventsWhileBlocked();
            processModuleLoadingProgressEvent(1);
        }
        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp)) {
                break;
            } else {
                goto readerr;
            }
        }
        if (buf[0] == '#') continue; /* Skip annotations 跳过注释*/
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;
        if ((size_t)argc > SIZE_MAX / sizeof(robj*)) goto fmterr;

        /* Load the next command in the AOF as our fake client
         * argv. */
        /**将 AOF 中的下一个命令加载为我们的假客户端 argv。*/
        argv = zmalloc(sizeof(robj*)*argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        fakeClient->argv_len = argc;

        for (j = 0; j < argc; j++) {
            /* Parse the argument len. 解析参数 len。*/
            char *readres = fgets(buf,sizeof(buf),fp);
            if (readres == NULL || buf[0] != '$') {
                fakeClient->argc = j; /* Free up to j-1. */
                freeClientArgv(fakeClient);
                if (readres == NULL)
                    goto readerr;
                else
                    goto fmterr;
            }
            len = strtol(buf+1,NULL,10);

            /* Read it into a string object. 将其读入字符串对象。*/
            argsds = sdsnewlen(SDS_NOINIT,len);
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. 最多释放 j-1。*/
                freeClientArgv(fakeClient);
                goto readerr;
            }
            argv[j] = createObject(OBJ_STRING,argsds);

            /* Discard CRLF. */
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. */
                freeClientArgv(fakeClient);
                goto readerr;
            }
        }

        /* Command lookup 命令查找*/
        cmd = lookupCommand(argv,argc);
        if (!cmd) {
            serverLog(LL_WARNING,
                "Unknown command '%s' reading the append only file %s",
                (char*)argv[0]->ptr, filename);
            freeClientArgv(fakeClient);
            ret = AOF_FAILED;
            goto cleanup;
        }

        if (cmd->proc == multiCommand) valid_before_multi = valid_up_to;

        /* Run the command in the context of a fake client */
        /**在假客户端的上下文中运行命令*/
        fakeClient->cmd = fakeClient->lastcmd = cmd;
        if (fakeClient->flags & CLIENT_MULTI &&
            fakeClient->cmd->proc != execCommand)
        {
            /* Note: we don't have to attempt calling evalGetCommandFlags,
             * since this is AOF, the checks in processCommand are not made
             * anyway.*/
            /**注意：我们不必尝试调用 evalGet CommandFlags，因为这是 OF，无论如何都不会检查进程 Command。*/
            queueMultiCommand(fakeClient, cmd->flags);
        } else {
            cmd->proc(fakeClient);
        }

        /* The fake client should not have a reply 假客户不应该有回复*/
        serverAssert(fakeClient->bufpos == 0 &&
                     listLength(fakeClient->reply) == 0);

        /* The fake client should never get blocked 假客户端永远不应该被阻止*/
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables.
         * 清理。命令代码可能已更改 argv argc 我们是否使用客户端的 argv argc 而不是局部变量。*/
        freeClientArgv(fakeClient);
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
        if (server.key_load_delay)
            debugDelay(server.key_load_delay);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, handle it as it was
     * a short read, even if technically the protocol is correct: we want
     * to remove the unprocessed tail and continue. */
    /**只有在没有错误地达到 EOF 时才能达到这一点。如果客户端处于 MULTIEXEC 的中间，即使从技术上讲协议是正确的，
     * 也应将其视为短读：我们希望删除未处理的尾部并继续。*/
    if (fakeClient->flags & CLIENT_MULTI) {
        serverLog(LL_WARNING,
            "Revert incomplete MULTI/EXEC transaction in AOF file %s", filename);
        valid_up_to = valid_before_multi;
        goto uxeof;
    }

loaded_ok: /* DB loaded, cleanup and return success (AOF_OK or AOF_TRUNCATED).  DB 加载、清理并返回成功（AOF_OK 或 AOF_TRUNCATED）。*/
    loadingIncrProgress(ftello(fp) - last_progress_report_size);
    server.aof_state = old_aof_state;
    goto cleanup;

readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF.  读取错误。如果 feof(fp) 为真，则进入意外的 EOF。*/
    if (!feof(fp)) {
        serverLog(LL_WARNING,"Unrecoverable error reading the append only file %s: %s", filename, strerror(errno));
        ret = AOF_FAILED;
        goto cleanup;
    }

uxeof: /* Unexpected AOF end of file. 意外的 AOF 文件结尾。*/
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING,"!!! Warning: short read while loading the AOF file %s!!!", filename);
        serverLog(LL_WARNING,"!!! Truncating the AOF %s at offset %llu !!!",
            filename, (unsigned long long) valid_up_to);
        if (valid_up_to == -1 || truncate(aof_filepath,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING,"Last valid command offset is invalid");
            } else {
                serverLog(LL_WARNING,"Error truncating the AOF file %s: %s",
                    filename, strerror(errno));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            /**确保 AOF 文件描述符在 truncate 调用后指向文件末尾。*/
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                serverLog(LL_WARNING,"Can't seek the end of the AOF file %s: %s",
                    filename, strerror(errno));
            } else {
                serverLog(LL_WARNING,
                    "AOF %s loaded anyway because aof-load-truncated is enabled", filename);
                ret = AOF_TRUNCATED;
                goto loaded_ok;
            }
        }
    }
    serverLog(LL_WARNING, "Unexpected end of file reading the append only file %s. You can: "
        "1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename.manifest>. "
        "2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.", filename);
    ret = AOF_FAILED;
    goto cleanup;

fmterr: /* Format error. */
    serverLog(LL_WARNING, "Bad file format reading the append only file %s: "
        "make a backup of your AOF file, then use ./redis-check-aof --fix <filename.manifest>", filename);
    ret = AOF_FAILED;
    /* fall through to cleanup. */

cleanup:
    if (fakeClient) freeClient(fakeClient);
    server.current_client = old_client;
    fclose(fp);
    sdsfree(aof_filepath);
    return ret;
}

/* Load the AOF files according the aofManifest pointed by am. */
/**根据 am 指向的 aofManifest 加载 AOF 文件。*/
int loadAppendOnlyFiles(aofManifest *am) {
    serverAssert(am != NULL);
    int status, ret = AOF_OK;
    long long start;
    off_t total_size = 0, base_size = 0;
    sds aof_name;
    int total_num, aof_num = 0, last_file;

    /* If the 'server.aof_filename' file exists in dir, we may be starting
     * from an old redis version. We will use enter upgrade mode in three situations.
     *
     * 1. If the 'server.aof_dirname' directory not exist
     * 2. If the 'server.aof_dirname' directory exists but the manifest file is missing
     * 3. If the 'server.aof_dirname' directory exists and the manifest file it contains
     *    has only one base AOF record, and the file name of this base AOF is 'server.aof_filename',
     *    and the 'server.aof_filename' file not exist in 'server.aof_dirname' directory
     * */
    /**如果 'server.aof_filename' 文件存在于 dir 中，我们可能是从旧的 redis 版本开始的。
     * 我们将在三种情况下使用进入升级模式。
     *   1. 如果 'server.aof_dirname' 目录不存在
     *   2. 如果 'server.aof_dirname' 目录存在但缺少清单文件
     *   3. 如果 'server.aof_dirname' 目录存在并且它包含的清单文件只有一个基AOF 记录，
     *      此基础 AOF 的文件名为“server.aof_filename”，“server.aof_dirname”目录中不存在“server.aof_filename”文件*/
    if (fileExist(server.aof_filename)) {
        if (!dirExists(server.aof_dirname) ||
            (am->base_aof_info == NULL && listLength(am->incr_aof_list) == 0) ||
            (am->base_aof_info != NULL && listLength(am->incr_aof_list) == 0 &&
             !strcmp(am->base_aof_info->file_name, server.aof_filename) && !aofFileExist(server.aof_filename)))
        {
            aofUpgradePrepare(am);
        }
    }

    if (am->base_aof_info == NULL && listLength(am->incr_aof_list) == 0) {
        return AOF_NOT_EXIST;
    }

    total_num = getBaseAndIncrAppendOnlyFilesNum(am);
    serverAssert(total_num > 0);

    /* Here we calculate the total size of all BASE and INCR files in
     * advance, it will be set to `server.loading_total_bytes`. */
    /**这里我们预先计算所有 BASE 和 INCR 文件的总大小，它会被设置为 `server.loading_total_bytes`。*/
    total_size = getBaseAndIncrAppendOnlyFilesSize(am, &status);
    if (status != AOF_OK) {
        /* If an AOF exists in the manifest but not on the disk, we consider this to be a fatal error. */
        /**如果清单中存在 AOF 但磁盘上不存在，我们认为这是一个致命错误。*/
        if (status == AOF_NOT_EXIST) status = AOF_FAILED;

        return status;
    } else if (total_size == 0) {
        return AOF_EMPTY;
    }

    startLoading(total_size, RDBFLAGS_AOF_PREAMBLE, 0);

    /* Load BASE AOF if needed.如果需要，加载 BASE AOF。 */
    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);
        aof_name = (char*)am->base_aof_info->file_name;
        updateLoadingFileName(aof_name);
        base_size = getAppendOnlyFileSize(aof_name, NULL);
        last_file = ++aof_num == total_num;
        start = ustime();
        ret = loadSingleAppendOnlyFile(aof_name);
        if (ret == AOF_OK || (ret == AOF_TRUNCATED && last_file)) {
            serverLog(LL_NOTICE, "DB loaded from base file %s: %.3f seconds",
                aof_name, (float)(ustime()-start)/1000000);
        }

        /* If the truncated file is not the last file, we consider this to be a fatal error. */
        /**如果截断的文件不是最后一个文件，我们认为这是一个致命错误。*/
        if (ret == AOF_TRUNCATED && !last_file) {
            ret = AOF_FAILED;
        }

        if (ret == AOF_OPEN_ERR || ret == AOF_FAILED) {
            goto cleanup;
        }
    }

    /* Load INCR AOFs if needed. 如果需要，加载 INCR AOF。*/
    if (listLength(am->incr_aof_list)) {
        listNode *ln;
        listIter li;

        listRewind(am->incr_aof_list, &li);
        while ((ln = listNext(&li)) != NULL) {
            aofInfo *ai = (aofInfo*)ln->value;
            serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);
            aof_name = (char*)ai->file_name;
            updateLoadingFileName(aof_name);
            last_file = ++aof_num == total_num;
            start = ustime();
            ret = loadSingleAppendOnlyFile(aof_name);
            if (ret == AOF_OK || (ret == AOF_TRUNCATED && last_file)) {
                serverLog(LL_NOTICE, "DB loaded from incr file %s: %.3f seconds",
                    aof_name, (float)(ustime()-start)/1000000);
            }

            /* We know that (at least) one of the AOF files has data (total_size > 0),
             * so empty incr AOF file doesn't count as a AOF_EMPTY result */
            /**我们知道（至少）其中一个 AOF 文件有数据（total_size > 0），所以空的 incr AOF 文件不算作 AOF_EMPTY 结果*/
            if (ret == AOF_EMPTY) ret = AOF_OK;

            if (ret == AOF_TRUNCATED && !last_file) {
                ret = AOF_FAILED;
            }

            if (ret == AOF_OPEN_ERR || ret == AOF_FAILED) {
                goto cleanup;
            }
        }
    }

    server.aof_current_size = total_size;
    /* Ideally, the aof_rewrite_base_size variable should hold the size of the
     * AOF when the last rewrite ended, this should include the size of the
     * incremental file that was created during the rewrite since otherwise we
     * risk the next automatic rewrite to happen too soon (or immediately if
     * auto-aof-rewrite-percentage is low). However, since we do not persist
     * aof_rewrite_base_size information anywhere, we initialize it on restart
     * to the size of BASE AOF file. This might cause the first AOFRW to be
     * executed early, but that shouldn't be a problem since everything will be
     * fine after the first AOFRW. */
    /**理想情况下，aof_rewrite_base_size 变量应该保存上次重写结束时 AOF 的大小，
     * 这应该包括在重写期间创建的增量文件的大小，否则我们可能会冒着下一次自动重写
     * 发生得太快的风险（或者如果 auto -aof-rewrite-percentage 低）。
     * 但是，由于我们不会在任何地方保存 aof_rewrite_base_size 信息，因此我们会在重新启动时
     * 将其初始化为 BASE AOF 文件的大小。这可能会导致第一个 AOFRW 提前执行，但这应该不是问题，
     * 因为在第一个 AOFRW 之后一切都会好起来的。*/
    server.aof_rewrite_base_size = base_size;
    server.aof_fsync_offset = server.aof_current_size;

cleanup:
    stopLoading(ret == AOF_OK || ret == AOF_TRUNCATED);
    return ret;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. */
/**委托写入对象以写入批量字符串或批量 long long。这没有放在 rio.c 中，因为它添加了 server.h 依赖项。*/
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    /**避免使用 getDecodedObject 来帮助写入时复制（调用此函数时，我们通常处于子进程中）。*/
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    } else if (sdsEncodedObject(obj)) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
/**发出重建列表对象所需的命令。该函数在错误时返回 0，在成功时返回 1。*/
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;

        while (quicklistNext(li,&entry)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"RPUSH",5) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }

            if (entry.value) {
                if (!rioWriteBulkString(r,(char*)entry.value,entry.sz)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            } else {
                if (!rioWriteBulkLongLong(r,entry.longval)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        quicklistReleaseIterator(li);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
/**发出重建集合对象所需的命令。该函数在错误时返回 0，在成功时返回 1。*/
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);

    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"SADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    return 0;
                }
            }
            if (!rioWriteBulkLongLong(r,llval)) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"SADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkString(r,ele,sdslen(ele))) {
                dictReleaseIterator(di);
                return 0;          
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
/**发出重建排序集对象所需的命令。该函数在错误时返回 0，在成功时返回 1。*/
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = lpSeek(zl,0);
        serverAssert(eptr != NULL);
        sptr = lpNext(zl,eptr);
        serverAssert(sptr != NULL);

        while (eptr != NULL) {
            vstr = lpGetValue(eptr,&vlen,&vll);
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                    !rioWriteBulkString(r,"ZADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r,score)) return 0;
            if (vstr != NULL) {
                if (!rioWriteBulkString(r,(char*)vstr,vlen)) return 0;
            } else {
                if (!rioWriteBulkLongLong(r,vll)) return 0;
            }
            zzlNext(zl,&eptr,&sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                    !rioWriteBulkString(r,"ZADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r,*score) ||
                !rioWriteBulkString(r,ele,sdslen(ele)))
            {
                dictReleaseIterator(di);
                return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 * The 'hi' argument passes a valid Redis hash iterator.
 * The 'what' filed specifies if to write a key or a value and can be
 * either OBJ_HASH_KEY or OBJ_HASH_VALUE.
 *
 * The function returns 0 on error, non-zero on success. */
/**写出散列的当前选定项的键或值。 'hi' 参数传递一个有效的 Redis 哈希迭代器。
 * 'what' 字段指定是否写入键或值，可以是 OBJ_HASH_KEY 或 OBJ_HASH_VALUE。
 *
 * 该函数在错误时返回 0，在成功时返回非零。*/
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            return rioWriteBulkString(r, (char*)vstr, vlen);
        else
            return rioWriteBulkLongLong(r, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        return rioWriteBulkString(r, value, sdslen(value));
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
/**发出重建哈希对象所需的命令。该函数在错误时返回 0，在成功时返回 1。*/
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                AOF_REWRITE_ITEMS_PER_CMD : items;

            if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                !rioWriteBulkString(r,"HMSET",5) ||
                !rioWriteBulkObject(r,key)) 
            {
                hashTypeReleaseIterator(hi);
                return 0;
            }
        }

        if (!rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) ||
            !rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE))
        {
            hashTypeReleaseIterator(hi);
            return 0;           
        }
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Helper for rewriteStreamObject() that generates a bulk string into the
 * AOF representing the ID 'id'. */
/**rewriteStreamObject() 的助手，它在 AOF 中生成一个批量字符串，表示 ID 'id'。*/
int rioWriteBulkStreamID(rio *r,streamID *id) {
    int retval;

    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    retval = rioWriteBulkString(r,replyid,sdslen(replyid));
    sdsfree(replyid);
    return retval;
}

/* Helper for rewriteStreamObject(): emit the XCLAIM needed in order to
 * add the message described by 'nack' having the id 'rawid', into the pending
 * list of the specified consumer. All this in the context of the specified
 * key and group. */
/**rewriteStreamObject() 的助手：发出所需的 XCLAIM，以便将 'nack' 描述的具有 id 'rawid' 的消息
 * 添加到指定使用者的待处理列表中。所有这一切都在指定的键和组的上下文中。*/
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer, unsigned char *rawid, streamNACK *nack) {
     /* XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
               RETRYCOUNT <count> JUSTID FORCE. */
    streamID id;
    streamDecodeID(rawid,&id);
    if (rioWriteBulkCount(r,'*',12) == 0) return 0;
    if (rioWriteBulkString(r,"XCLAIM",6) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    if (rioWriteBulkString(r,"0",1) == 0) return 0;
    if (rioWriteBulkStreamID(r,&id) == 0) return 0;
    if (rioWriteBulkString(r,"TIME",4) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_time) == 0) return 0;
    if (rioWriteBulkString(r,"RETRYCOUNT",10) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_count) == 0) return 0;
    if (rioWriteBulkString(r,"JUSTID",6) == 0) return 0;
    if (rioWriteBulkString(r,"FORCE",5) == 0) return 0;
    return 1;
}

/* Helper for rewriteStreamObject(): emit the XGROUP CREATECONSUMER is
 * needed in order to create consumers that do not have any pending entries.
 * All this in the context of the specified key and group. */
/**rewriteStreamObject() 的助手：需要发出 XGROUP CREATECONSUMER 以创建没有任何待处理条目的消费者。
 * 所有这一切都在指定的键和组的上下文中。*/
int rioWriteStreamEmptyConsumer(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer) {
    /* XGROUP CREATECONSUMER <key> <group> <consumer> */
    if (rioWriteBulkCount(r,'*',5) == 0) return 0;
    if (rioWriteBulkString(r,"XGROUP",6) == 0) return 0;
    if (rioWriteBulkString(r,"CREATECONSUMER",14) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    return 1;
}

/* Emit the commands needed to rebuild a stream object.
 * The function returns 0 on error, 1 on success. */
/**发出重建流对象所需的命令。该函数在错误时返回 0，在成功时返回 1。*/
int rewriteStreamObject(rio *r, robj *key, robj *o) {
    stream *s = o->ptr;
    streamIterator si;
    streamIteratorStart(&si,s,NULL,NULL,0);
    streamID id;
    int64_t numfields;

    if (s->length) {
        /* Reconstruct the stream data using XADD commands. */
        /**使用 XADD 命令重建流数据。*/
        while(streamIteratorGetID(&si,&id,&numfields)) {
            /* Emit a two elements array for each item. The first is
             * the ID, the second is an array of field-value pairs.
             * 为每个项目发出一个包含两个元素的数组。第一个是 ID，第二个是字段值对数组。*/

            /* Emit the XADD <key> <id> ...fields... command. */
            if (!rioWriteBulkCount(r,'*',3+numfields*2) || 
                !rioWriteBulkString(r,"XADD",4) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkStreamID(r,&id)) 
            {
                streamIteratorStop(&si);
                return 0;
            }
            while(numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si,&field,&value,&field_len,&value_len);
                if (!rioWriteBulkString(r,(char*)field,field_len) ||
                    !rioWriteBulkString(r,(char*)value,value_len)) 
                {
                    streamIteratorStop(&si);
                    return 0;                  
                }
            }
        }
    } else {
        /* Use the XADD MAXLEN 0 trick to generate an empty stream if
         * the key we are serializing is an empty string, which is possible
         * for the Stream type. */
        /**如果我们要序列化的键是空字符串，则使用 XADD MAXLEN 0 技巧生成一个空流，这对于 Stream 类型是可能的。*/
        id.ms = 0; id.seq = 1; 
        if (!rioWriteBulkCount(r,'*',7) ||
            !rioWriteBulkString(r,"XADD",4) ||
            !rioWriteBulkObject(r,key) ||
            !rioWriteBulkString(r,"MAXLEN",6) ||
            !rioWriteBulkString(r,"0",1) ||
            !rioWriteBulkStreamID(r,&id) ||
            !rioWriteBulkString(r,"x",1) ||
            !rioWriteBulkString(r,"y",1))
        {
            streamIteratorStop(&si);
            return 0;     
        }
    }

    /* Append XSETID after XADD, make sure lastid is correct,
     * in case of XDEL lastid. */
    /**在 XADD 之后附加 XSETID，确保 lastid 正确，以防 XDEL lastid。*/
    if (!rioWriteBulkCount(r,'*',7) ||
        !rioWriteBulkString(r,"XSETID",6) ||
        !rioWriteBulkObject(r,key) ||
        !rioWriteBulkStreamID(r,&s->last_id) ||
        !rioWriteBulkString(r,"ENTRIESADDED",12) ||
        !rioWriteBulkLongLong(r,s->entries_added) ||
        !rioWriteBulkString(r,"MAXDELETEDID",12) ||
        !rioWriteBulkStreamID(r,&s->max_deleted_entry_id)) 
    {
        streamIteratorStop(&si);
        return 0; 
    }


    /* Create all the stream consumer groups. 创建所有流消费者组。*/
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri,s->cgroups);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            streamCG *group = ri.data;
            /* Emit the XGROUP CREATE in order to create the group. 发出 XGROUP CREATE 以创建组。*/
            if (!rioWriteBulkCount(r,'*',7) ||
                !rioWriteBulkString(r,"XGROUP",6) ||
                !rioWriteBulkString(r,"CREATE",6) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkString(r,(char*)ri.key,ri.key_len) ||
                !rioWriteBulkStreamID(r,&group->last_id) ||
                !rioWriteBulkString(r,"ENTRIESREAD",11) ||
                !rioWriteBulkLongLong(r,group->entries_read))
            {
                raxStop(&ri);
                streamIteratorStop(&si);
                return 0;
            }

            /* Generate XCLAIMs for each consumer that happens to
             * have pending entries. Empty consumers would be generated with
             * XGROUP CREATECONSUMER. */
            /**为每个碰巧有待处理条目的消费者生成 XCLAIM。将使用 XGROUP CREATECONSUMER 生成空消费者。*/
            raxIterator ri_cons;
            raxStart(&ri_cons,group->consumers);
            raxSeek(&ri_cons,"^",NULL,0);
            while(raxNext(&ri_cons)) {
                streamConsumer *consumer = ri_cons.data;
                /* If there are no pending entries, just emit XGROUP CREATECONSUMER */
                /**如果没有待处理的条目，只需发出 XGROUP CREATECONSUMER*/
                if (raxSize(consumer->pel) == 0) {
                    if (rioWriteStreamEmptyConsumer(r,key,(char*)ri.key,
                                                    ri.key_len,consumer) == 0)
                    {
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                    continue;
                }
                /* For the current consumer, iterate all the PEL entries
                 * to emit the XCLAIM protocol. */
                /**对于当前消费者，迭代所有 PEL 条目以发出 XCLAIM 协议。*/
                raxIterator ri_pel;
                raxStart(&ri_pel,consumer->pel);
                raxSeek(&ri_pel,"^",NULL,0);
                while(raxNext(&ri_pel)) {
                    streamNACK *nack = ri_pel.data;
                    if (rioWriteStreamPendingEntry(r,key,(char*)ri.key,
                                                   ri.key_len,consumer,
                                                   ri_pel.key,nack) == 0)
                    {
                        raxStop(&ri_pel);
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                }
                raxStop(&ri_pel);
            }
            raxStop(&ri_cons);
        }
        raxStop(&ri);
    }

    streamIteratorStop(&si);
    return 1;
}

/* Call the module type callback in order to rewrite a data type
 * that is exported by a module and is not handled by Redis itself.
 * The function returns 0 on error, 1 on success. */
/**调用模块类型回调以重写由模块导出且不由 Redis 本身处理的数据类型。该函数在错误时返回 0，在成功时返回 1。*/
int rewriteModuleObject(rio *r, robj *key, robj *o, int dbid) {
    RedisModuleIO io;
    moduleValue *mv = o->ptr;
    moduleType *mt = mv->type;
    moduleInitIOContext(io,mt,r,key,dbid);
    mt->aof_rewrite(&io,key,mv->value);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return io.error ? 0 : 1;
}

static int rewriteFunctions(rio *aof) {
    dict *functions = functionsLibGet();
    dictIterator *iter = dictGetIterator(functions);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        functionLibInfo *li = dictGetVal(entry);
        if (rioWrite(aof, "*3\r\n", 4) == 0) goto werr;
        char function_load[] = "$8\r\nFUNCTION\r\n$4\r\nLOAD\r\n";
        if (rioWrite(aof, function_load, sizeof(function_load) - 1) == 0) goto werr;
        if (rioWriteBulkString(aof, li->code, sdslen(li->code)) == 0) goto werr;
    }
    dictReleaseIterator(iter);
    return 1;

werr:
    dictReleaseIterator(iter);
    return 0;
}

int rewriteAppendOnlyFileRio(rio *aof) {
    dictIterator *di = NULL;
    dictEntry *de;
    int j;
    long key_count = 0;
    long long updated_time = 0;

    /* Record timestamp at the beginning of rewriting AOF. */
    /**在重写 AOF 开始时记录时间戳。*/
    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(1);
        if (rioWrite(aof,ts,sdslen(ts)) == 0) { sdsfree(ts); goto werr; }
        sdsfree(ts);
    }

    if (rewriteFunctions(aof) == 0) goto werr;

    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);

        /* SELECT the new DB */
        if (rioWrite(aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        /**迭代这个数据库写入每个条目*/
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;
            size_t aof_bytes_before_key = aof->processed_bytes;

            keystr = dictGetKey(de);
            o = dictGetVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* Save the key and associated value  保存键和关联的值*/

            if (o->type == OBJ_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(aof,o) == 0) goto werr;
            } else if (o->type == OBJ_LIST) {
                if (rewriteListObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_SET) {
                if (rewriteSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_ZSET) {
                if (rewriteSortedSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_HASH) {
                if (rewriteHashObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_STREAM) {
                if (rewriteStreamObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_MODULE) {
                if (rewriteModuleObject(aof,&key,o,j) == 0) goto werr;
            } else {
                serverPanic("Unknown object type");
            }

            /* In fork child process, we can try to release memory back to the
             * OS and possibly avoid or decrease COW. We give the dismiss
             * mechanism a hint about an estimated size of the object we stored. */
            /**在 fork 子进程中，我们可以尝试将内存释放回操作系统，并可能避免或减少 COW。
             * 我们给dismiss机制一个关于我们存储的对象的估计大小的提示。*/
            size_t dump_size = aof->processed_bytes - aof_bytes_before_key;
            if (server.in_fork_child) dismissObject(o, dump_size);

            /* Save the expire time 保存过期时间*/
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(aof,expiretime) == 0) goto werr;
            }

            /* Update info every 1 second (approximately).
             * in order to avoid calling mstime() on each iteration, we will
             * check the diff every 1024 keys */
            /**每 1 秒（大约）更新一次信息。为了避免在每次迭代中调用 mstime()，我们将每 1024 个键检查一次 diff*/
            if ((key_count++ & 1023) == 0) {
                long long now = mstime();
                if (now - updated_time >= 1000) {
                    sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, key_count, "AOF rewrite");
                    updated_time = now;
                }
            }

            /* Delay before next key if required (for testing) */
            /**如果需要，在下一个键之前延迟（用于测试）*/
            if (server.rdb_key_save_delay)
                debugDelay(server.rdb_key_save_delay);
        }
        dictReleaseIterator(di);
        di = NULL;
    }
    return C_OK;

werr:
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
/**将能够完全重建数据集的命令序列写入“文件名”。由 REWRITEAOF 和 BGREWRITEAOF 使用。
 *
 * 为了尽量减少重写日志中所需的命令数量，Redis 尽可能使用可变参数命令，例如 RPUSH、SADD 和 ZADD。
 * 但是，每次最多使用单个命令插入 AOF_REWRITE_ITEMS_PER_CMD 项目。*/
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp = NULL;
    char tmpfile[256];

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    /**请注意，与 rewriteAppendOnlyFileBackground() 函数使用的临时名称相比，我们必须在此处使用不同的临时名称。*/
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        serverLog(LL_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
        return C_ERR;
    }

    rioInitWithFile(&aof,fp);

    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof,REDIS_AUTOSYNC_BYTES);

    startSaving(RDBFLAGS_AOF_PREAMBLE);

    if (server.aof_use_rdb_preamble) {
        int error;
        if (rdbSaveRio(SLAVE_REQ_NONE,&aof,&error,RDBFLAGS_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }
    } else {
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    /**确保数据不会保留在操作系统的输出缓冲区中*/
    if (fflush(fp)) goto werr;
    if (fsync(fileno(fp))) goto werr;
    if (fclose(fp)) { fp = NULL; goto werr; }
    fp = NULL;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    /**使用 RENAME 确保仅当生成的 DB 文件正常时才自动更改 DB 文件。*/
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    stopSaving(1);

    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}
/* ----------------------------------------------------------------------------
 * AOF background rewrite
 * ------------------------------------------------------------------------- */

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent open a new INCR AOF file to continue writing.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, it will:
 *    4a) get a new BASE file name and mark the previous (if we have) as the HISTORY type
 *    4b) rename(2) the temp file in new BASE file name
 *    4c) mark the rewritten INCR AOFs as history type
 *    4d) persist AOF manifest file
 *    4e) Delete the history files use bio
 */
/**这是在后台重写仅附加文件的工作原理：
 *   1）用户调用 BGREWRITEAOF
 *   2）Redis 调用此函数，即 forks()：
 *      2a）子在临时文件中重写仅附加文件。
 *      2b) 父级打开一个新的INCR AOF 文件继续写入。
 *   3）当孩子完成'2a'时存在。
 *   4) 父级将捕获退出代码，如果没问题，它将：
 *      4a) 获取一个新的 BASE 文件名并将前一个（如果我们有）标记为 HISTORY 类型
 *      4b) 重命名 (2) 新 BASE 中的临时文件文件名
 *      4c) 将重写的 INCR AOF 标记为历史类型
 *      4d) 保留 AOF 清单文件
 *      4e) 使用 bio 删除历史文件*/
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;

    if (dirCreateIfMissing(server.aof_dirname) == -1) {
        serverLog(LL_WARNING, "Can't open or create append-only dir %s: %s",
            server.aof_dirname, strerror(errno));
        server.aof_lastbgrewrite_status = C_ERR;
        return C_ERR;
    }

    /* We set aof_selected_db to -1 in order to force the next call to the
     * feedAppendOnlyFile() to issue a SELECT command. */
    /**我们将 aof_selected_db 设置为 -1 以强制对 feedAppendOnlyFile() 的下一次调用发出 SELECT 命令。*/
    server.aof_selected_db = -1;
    flushAppendOnlyFile(1);
    if (openNewIncrAofForAppend() != C_OK) {
        server.aof_lastbgrewrite_status = C_ERR;
        return C_ERR;
    }
    server.stat_aof_rewrites++;
    if ((childpid = redisFork(CHILD_TYPE_AOF)) == 0) {
        char tmpfile[256];

        /* Child */
        redisSetProcTitle("redis-aof-rewrite");
        redisSetCpuAffinity(server.aof_rewrite_cpulist);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
            serverLog(LL_NOTICE,
                "Successfully created the temporary AOF base file %s", tmpfile);
            sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite");
            exitFromChild(0);
        } else {
            exitFromChild(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            server.aof_lastbgrewrite_status = C_ERR;
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %ld",(long) childpid);
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        return C_OK;
    }
    return C_OK; /* unreached 未达到的 */
}

void bgrewriteaofCommand(client *c) {
    if (server.child_type == CHILD_TYPE_AOF) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (hasActiveChildProcess() || server.in_exec) {
        server.aof_rewrite_scheduled = 1;
        /* When manually triggering AOFRW we reset the count 
         * so that it can be executed immediately. */
        /**当手动触发 AOFRW 时，我们会重置计数，以便可以立即执行。*/
        server.stat_aofrw_consecutive_failures = 0;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReplyError(c,"Can't execute an AOF background rewriting. "
                        "Please check the server logs for more information.");
    }
}

void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    bg_unlink(tmpfile);

    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) childpid);
    bg_unlink(tmpfile);
}

/* Get size of an AOF file.
 * The status argument is an optional output argument to be filled with
 * one of the AOF_ status values. */
/**获取 AOF 文件的大小。 status 参数是一个可选的输出参数，用 AOF_ status 值之一填充。*/
off_t getAppendOnlyFileSize(sds filename, int *status) {
    struct redis_stat sb;
    off_t size;
    mstime_t latency;

    sds aof_filepath = makePath(server.aof_dirname, filename);
    latencyStartMonitor(latency);
    if (redis_stat(aof_filepath, &sb) == -1) {
        if (status) *status = errno == ENOENT ? AOF_NOT_EXIST : AOF_OPEN_ERR;
        serverLog(LL_WARNING, "Unable to obtain the AOF file %s length. stat: %s",
            filename, strerror(errno));
        size = 0;
    } else {
        if (status) *status = AOF_OK;
        size = sb.st_size;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat", latency);
    sdsfree(aof_filepath);
    return size;
}

/* Get size of all AOF files referred by the manifest (excluding history).
 * The status argument is an output argument to be filled with
 * one of the AOF_ status values. */
/**获取清单引用的所有 AOF 文件的大小（不包括历史记录）。
 * status 参数是一个用 AOF_ status 值之一填充的输出参数。*/
off_t getBaseAndIncrAppendOnlyFilesSize(aofManifest *am, int *status) {
    off_t size = 0;
    listNode *ln;
    listIter li;

    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);

        size += getAppendOnlyFileSize(am->base_aof_info->file_name, status);
        if (*status != AOF_OK) return 0;
    }

    listRewind(am->incr_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);
        size += getAppendOnlyFileSize(ai->file_name, status);
        if (*status != AOF_OK) return 0;
    }

    return size;
}

int getBaseAndIncrAppendOnlyFilesNum(aofManifest *am) {
    int num = 0;
    if (am->base_aof_info) num++;
    if (am->incr_aof_list) num += listLength(am->incr_aof_list);
    return num;
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
/**后台仅追加文件重写 (BGREWRITEAOF) 终止了它的工作。处理这个（事情。*/
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        char tmpfile[256];
        long long now = ustime();
        sds new_base_filepath = NULL;
        sds new_incr_filepath = NULL;
        aofManifest *temp_am;
        mstime_t latency;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof",
            (int)server.child_pid);

        serverAssert(server.aof_manifest != NULL);

        /* Dup a temporary aof_manifest for subsequent modifications. */
        /**复制一个临时的 aof_manifest 以供后续修改。*/
        temp_am = aofManifestDup(server.aof_manifest);

        /* Get a new BASE file name and mark the previous (if we have)
         * as the HISTORY type. */
        /**获取一个新的 BASE 文件名并将之前的（如果有的话）标记为 HISTORY 类型。*/
        sds new_base_filename = getNewBaseFileNameAndMarkPreAsHistory(temp_am);
        serverAssert(new_base_filename != NULL);
        new_base_filepath = makePath(server.aof_dirname, new_base_filename);

        /* Rename the temporary aof file to 'new_base_filename'. */
        /**将临时 aof 文件重命名为“new_base_filename”。*/
        latencyStartMonitor(latency);
        if (rename(tmpfile, new_base_filepath) == -1) {
            serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF base file %s into %s: %s",
                tmpfile,
                new_base_filepath,
                strerror(errno));
            aofManifestFree(temp_am);
            sdsfree(new_base_filepath);
            server.aof_lastbgrewrite_status = C_ERR;
            server.stat_aofrw_consecutive_failures++;
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rename", latency);
        serverLog(LL_NOTICE,
            "Successfully renamed the temporary AOF base file %s into %s", tmpfile, new_base_filename);

        /* Rename the temporary incr aof file to 'new_incr_filename'. */
        /**将临时 incr aof 文件重命名为“new_incr_filename”。*/
        if (server.aof_state == AOF_WAIT_REWRITE) {
            /* Get temporary incr aof name. 获取临时增加名称。*/
            sds temp_incr_aof_name = getTempIncrAofName();
            sds temp_incr_filepath = makePath(server.aof_dirname, temp_incr_aof_name);
            /* Get next new incr aof name. 获取下一个新的 incr aof 名称。*/
            sds new_incr_filename = getNewIncrAofName(temp_am);
            new_incr_filepath = makePath(server.aof_dirname, new_incr_filename);
            latencyStartMonitor(latency);
            if (rename(temp_incr_filepath, new_incr_filepath) == -1) {
                serverLog(LL_WARNING,
                    "Error trying to rename the temporary AOF incr file %s into %s: %s",
                    temp_incr_filepath,
                    new_incr_filepath,
                    strerror(errno));
                bg_unlink(new_base_filepath);
                sdsfree(new_base_filepath);
                aofManifestFree(temp_am);
                sdsfree(temp_incr_filepath);
                sdsfree(new_incr_filepath);
                sdsfree(temp_incr_aof_name);
                server.aof_lastbgrewrite_status = C_ERR;
                server.stat_aofrw_consecutive_failures++;
                goto cleanup;
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rename", latency);
            serverLog(LL_NOTICE,
                "Successfully renamed the temporary AOF incr file %s into %s", temp_incr_aof_name, new_incr_filename);
            sdsfree(temp_incr_filepath);
            sdsfree(temp_incr_aof_name);
        }

        /* Change the AOF file type in 'incr_aof_list' from AOF_FILE_TYPE_INCR
         * to AOF_FILE_TYPE_HIST, and move them to the 'history_aof_list'. */
        /**将“incr_aof_list”中的 AOF 文件类型从 AOF_FILE_TYPE_INCR 更改为 AOF_FILE_TYPE_HIST，
         * 并将它们移动到“history_aof_list”。*/
        markRewrittenIncrAofAsHistory(temp_am);

        /* Persist our modifications. 持久化我们的修改。*/
        if (persistAofManifest(temp_am) == C_ERR) {
            bg_unlink(new_base_filepath);
            aofManifestFree(temp_am);
            sdsfree(new_base_filepath);
            if (new_incr_filepath) {
                bg_unlink(new_incr_filepath);
                sdsfree(new_incr_filepath);
            }
            server.aof_lastbgrewrite_status = C_ERR;
            server.stat_aofrw_consecutive_failures++;
            goto cleanup;
        }
        sdsfree(new_base_filepath);
        if (new_incr_filepath) sdsfree(new_incr_filepath);

        /* We can safely let `server.aof_manifest` point to 'temp_am' and free the previous one. */
        /**我们可以安全地让 `server.aof_manifest` 指向 'temp_am' 并释放前一个。*/
        aofManifestFreeAndUpdate(temp_am);

        if (server.aof_fd != -1) {
            /* AOF enabled. */
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
            server.aof_current_size = getAppendOnlyFileSize(new_base_filename, NULL) + server.aof_last_incr_size;
            server.aof_rewrite_base_size = server.aof_current_size;
            server.aof_fsync_offset = server.aof_current_size;
            server.aof_last_fsync = server.unixtime;
        }

        /* We don't care about the return value of `aofDelHistoryFiles`, because the history
         * deletion failure will not cause any problems. */
        /**我们不关心 `aofDelHistoryFiles` 的返回值，因为历史删除失败不会造成任何问题。*/
        aofDelHistoryFiles();

        server.aof_lastbgrewrite_status = C_OK;
        server.stat_aofrw_consecutive_failures = 0;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        /**如果需要，将状态从 WAIT_REWRITE 更改为 ON*/
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = C_ERR;
        server.stat_aofrw_consecutive_failures++;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        /**SIGUSR1 被列入白名单，因此我们有办法在不触发错误条件的情况下杀死孩子。*/
        if (bysignal != SIGUSR1) {
            server.aof_lastbgrewrite_status = C_ERR;
            server.stat_aofrw_consecutive_failures++;
        }

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofRemoveTempFile(server.child_pid);
    /* Clear AOF buffer and delete temp incr aof for next rewrite. */
    /**清除 AOF 缓冲区并删除 temp incr aof 以进行下一次重写。*/
    if (server.aof_state == AOF_WAIT_REWRITE) {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
        aofDelTempIncrAofFile();
    }
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    /**如果我们正在等待它打开 AOF，请安排新的重写。*/
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
