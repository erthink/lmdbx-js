
// This file is part of lmdbx-js and contains code from node-lmdb
// Copyright (c) 2013-2017 Timur Kristóf
// Licensed to you under the terms of the MIT license
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef NODE_LMDBX_H
#define NODE_LMDBX_H

#include <vector>
#include <algorithm>
#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <nan.h>
#include "mdbx.h"
#include "lz4.h"
#if ENABLE_FAST_API && NODE_VERSION_AT_LEAST(16,6,0)
#if NODE_VERSION_AT_LEAST(17,0,0)
#include "../dependencies/v8/v8-fast-api-calls.h"
#else
#include "../dependencies/v8/v8-fast-api-calls-v16.h"
#endif
#endif

using namespace v8;
using namespace node;



#ifndef __CPTHREAD_H__
#define __CPTHREAD_H__

#ifdef _WIN32
# include <windows.h>
#else
# include <pthread.h>
#endif

#ifdef _WIN32
typedef CRITICAL_SECTION pthread_mutex_t;
typedef void pthread_mutexattr_t;
typedef void pthread_condattr_t;
typedef HANDLE pthread_t;
typedef CONDITION_VARIABLE pthread_cond_t;

#endif

#ifdef _WIN32

int pthread_mutex_init(pthread_mutex_t *mutex, pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

#endif

int cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t ns);

#endif /* __CPTHREAD_H__ */

class Logging {
  public:
    static int debugLogging;
    static int initLogging();
};

enum class NodeLmdbxKeyType {

    // Invalid key (used internally by node-lmdb)
    InvalidKey = -1,
    
    // Default key (used internally by node-lmdb)
    DefaultKey = 0,

    // UCS-2/UTF-16 with zero terminator - Appears to V8 as string
    StringKey = 1,
    
    // LMDB fixed size integer key with 32 bit keys - Appearts to V8 as an Uint32
    Uint32Key = 2,
    
    // LMDB default key format - Appears to V8 as node::Buffer
    BinaryKey = 3,

};
enum class KeyCreation {
    Reset = 0,
    Continue = 1,
    InArray = 2,
};
const int THEAD_MEMORY_THRESHOLD = 4000;

class TxnWrap;
class DbiWrap;
class EnvWrap;
class CursorWrap;
class Compression;

// Exports misc stuff to the module
void setupExportMisc(Local<Object> exports);

// Helper callback
typedef void (*argtokey_callback_t)(MDBX_val &key);

void consoleLog(Local<Value> val);
void consoleLog(const char *msg);
void consoleLogN(int n);
void setFlagFromValue(int *flags, int flag, const char *name, bool defaultValue, Local<Object> options);
void writeValueToEntry(const Local<Value> &str, MDBX_val *val);
NodeLmdbxKeyType keyTypeFromOptions(const Local<Value> &val, NodeLmdbxKeyType defaultKeyType = NodeLmdbxKeyType::DefaultKey);
bool getVersionAndUncompress(MDBX_val &data, DbiWrap* dw);
int compareFast(const MDBX_val *a, const MDBX_val *b);
NAN_METHOD(setGlobalBuffer);
NAN_METHOD(lmdbxError);
//NAN_METHOD(getBufferForAddress);
NAN_METHOD(getAddress);
NAN_METHOD(getAddressShared);

#ifndef thread_local
#ifdef __GNUC__
# define thread_local __thread
#elif __STDC_VERSION__ >= 201112L
# define thread_local _Thread_local
#elif defined(_MSC_VER)
# define thread_local __declspec(thread)
#else
# define thread_local
#endif
#endif

bool valToBinaryFast(MDBX_val &data, DbiWrap* dw);
Local<Value> valToUtf8(MDBX_val &data);
Local<Value> valToString(MDBX_val &data);
Local<Value> valToStringUnsafe(MDBX_val &data);
Local<Value> valToBinary(MDBX_val &data);
Local<Value> valToBinaryUnsafe(MDBX_val &data, DbiWrap* dw);

int putWithVersion(MDBX_txn *   txn,
        MDBX_dbi     dbi,
        MDBX_val *   key,
        MDBX_val *   data,
        unsigned int    flags, double version);

void throwLmdbxError(int rc);

class TxnWrap;
class DbiWrap;
class EnvWrap;
class CursorWrap;
struct env_path_t {
    MDBX_env* env;
    char* path;
    int count;
};

const int INTERRUPT_BATCH = 9998;
const int ALLOW_COMMIT = 9997;
const int RESTART_WORKER_TXN = 9999;
const int RESUME_BATCH = 9996;
const int USER_HAS_LOCK = 9995;
const int SEPARATE_FLUSHED = 1;

class WriteWorker : public Nan::AsyncProgressWorker {
  public:
    WriteWorker(MDBX_env* env, EnvWrap* envForTxn, uint32_t* instructions, Nan::Callback *callback);
    void Write();
    MDBX_txn* txn;
    MDBX_txn* AcquireTxn(int* flags);
    void UnlockTxn();
    void Execute(const ExecutionProgress& executionProgress);
    void HandleProgressCallback(const char* data, size_t count);
    void HandleOKCallback();
    int WaitForCallbacks(MDBX_txn** txn, bool allowCommit, uint32_t* target);
    void ReportError(const char* error);
    int interruptionStatus;
    bool finishedProgress;
    EnvWrap* envForTxn;
    ~WriteWorker();
    uint32_t* instructions;
    int progressStatus;
  private:
    ExecutionProgress* executionProgress;
    MDBX_env* env;
};

class TxnTracked {
  public:
    TxnTracked(MDBX_txn *txn, unsigned int flags);
    ~TxnTracked();
    unsigned int flags;
    int cursorCount;
    bool onlyCursor;
    MDBX_txn *txn;
    TxnTracked *parent;
};

/*
    `Env`
    Represents a database environment.
    (Wrapper for `MDBX_env`)
*/
class EnvWrap : public Nan::ObjectWrap {
private:
    // List of open read transactions
    std::vector<TxnWrap*> readTxns;
    // Constructor for TxnWrap
    static thread_local Nan::Persistent<Function>* txnCtor;
    // Constructor for DbiWrap
    static thread_local Nan::Persistent<Function>* dbiCtor;
    static pthread_mutex_t* envsLock;
    static std::vector<env_path_t> envs;
    static pthread_mutex_t* initMutex();
    // compression settings and space
    Compression *compression;

    // Cleans up stray transactions
    void cleanupStrayTxns();

    friend class TxnWrap;
    friend class DbiWrap;

public:
    EnvWrap();
    ~EnvWrap();
    // The wrapped object
    MDBX_env *env;
    // Current write transaction
    TxnWrap *currentWriteTxn;
    TxnTracked *writeTxn;
    pthread_mutex_t* writingLock;
    pthread_cond_t* writingCond;

    MDBX_txn* currentReadTxn;
    WriteWorker* writeWorker;
    bool readTxnRenewed;
    unsigned int jsFlags;
    char* keyBuffer;
    MDBX_txn* getReadTxn();

    // Sets up exports for the Env constructor
    static void setupExports(Local<Object> exports);
    void closeEnv();
    
    /*
        Constructor of the database environment. You need to `open()` it before you can use it.
        (Wrapper for `mdbx_env_create`)
    */
    static NAN_METHOD(ctor);
    
    /*
        Gets statistics about the database environment.
    */
    static NAN_METHOD(stat);

    /*
        Gets statistics about the free space database
    */
    static NAN_METHOD(freeStat);
    
    /*
        Detaches a buffer from the backing store
    */
    static NAN_METHOD(detachBuffer);

    /*
        Gets information about the database environment.
    */
    static NAN_METHOD(info);
    /*
        Check for stale readers
    */
    static NAN_METHOD(readerCheck);
    /*
        Print a list of readers
    */
    static NAN_METHOD(readerList);

    /*
        Opens the database environment with the specified options. The options will be used to configure the environment before opening it.
        (Wrapper for `mdbx_env_open`)

        Parameters:

        * Options object that contains possible configuration options.

        Possible options are:

        * maxDbs: the maximum number of named databases you can have in the environment (default is 1)
        * maxReaders: the maximum number of concurrent readers of the environment (default is 126)
        * mapSize: maximal size of the memory map (the full environment) in bytes (default is 10485760 bytes)
        * path: path to the database environment
    */
    static NAN_METHOD(open);

    /*
        Resizes the maximal size of the memory map. It may be called if no transactions are active in this process.
        (Wrapper for `mdbx_env_set_mapsize`)

        Parameters:

        * maximal size of the memory map (the full environment) in bytes (default is 10485760 bytes)
    */
    static NAN_METHOD(resize);

    /*
        Copies the database environment to a file.
        (Wrapper for `mdbx_env_copy2`)

        Parameters:

        * path - Path to the target file
        * compact (optional) - Copy using compact setting
        * callback - Callback when finished (this is performed asynchronously)
    */
    static NAN_METHOD(copy);    

    /*
        Closes the database environment.
        (Wrapper for `mdbx_env_close`)
    */
    static NAN_METHOD(close);

    /*
        Starts a new transaction in the environment.
        (Wrapper for `mdbx_txn_begin`)

        Parameters:

        * Options object that contains possible configuration options.

        Possible options are:

        * readOnly: if true, the transaction is read-only
    */
    static NAN_METHOD(beginTxn);
    static NAN_METHOD(commitTxn);
    static NAN_METHOD(abortTxn);

    /*
        Opens a database in the environment.
        (Wrapper for `mdbx_dbi_open`)

        Parameters:

        * Options object that contains possible configuration options.

        Possible options are:

        * name: the name of the database (or null to use the unnamed database)
        * create: if true, the database will be created if it doesn't exist
        * keyIsUint32: if true, keys are treated as 32-bit unsigned integers
        * dupSort: if true, the database can hold multiple items with the same key
        * reverseKey: keys are strings to be compared in reverse order
        * dupFixed: if dupSort is true, indicates that the data items are all the same size
        * integerDup: duplicate data items are also integers, and should be sorted as such
        * reverseDup: duplicate data items should be compared as strings in reverse order
    */
    static NAN_METHOD(openDbi);

    /*
        Flushes all data to the disk asynchronously.
        (Asynchronous wrapper for `mdbx_env_sync`)

        Parameters:

        * Callback to be executed after the sync is complete.
    */
    static NAN_METHOD(sync);

    /*
        Performs a set of operations asynchronously, automatically wrapping it in its own transaction

        Parameters:

        * Callback to be executed after the sync is complete.
    */
    static NAN_METHOD(startWriting);
    static NAN_METHOD(compress);
#if ENABLE_FAST_API && NODE_VERSION_AT_LEAST(16,6,0)
    static void writeFast(Local<Object> receiver_obj, uint64_t instructionAddress, FastApiCallbackOptions& options);
#endif
    static void write(const v8::FunctionCallbackInfo<v8::Value>& info);

    static NAN_METHOD(resetCurrentReadTxn);
};

const int TXN_ABORTABLE = 1;
const int TXN_SYNCHRONOUS_COMMIT = 2;
const int TXN_FROM_WORKER = 4;

/*
    `Txn`
    Represents a transaction running on a database environment.
    (Wrapper for `MDBX_txn`)
*/
class TxnWrap : public Nan::ObjectWrap {
private:

    // Reference to the MDBX_env of the wrapped MDBX_txn
    MDBX_env *env;

    // Environment wrapper of the current transaction
    EnvWrap *ew;
    // parent TW, if it is exists
    TxnWrap *parentTw;
    
    // Flags used with mdbx_txn_begin
    unsigned int flags;

    friend class CursorWrap;
    friend class DbiWrap;
    friend class EnvWrap;

public:
    TxnWrap(MDBX_env *env, MDBX_txn *txn);
    ~TxnWrap();

    // The wrapped object
    MDBX_txn *txn;

    // Remove the current TxnWrap from its EnvWrap
    void removeFromEnvWrap();

    // Constructor (not exposed)
    static NAN_METHOD(ctor);

    /*
        Commits the transaction.
        (Wrapper for `mdbx_txn_commit`)
    */
    static NAN_METHOD(commit);

    /*
        Aborts the transaction.
        (Wrapper for `mdbx_txn_abort`)
    */
    static NAN_METHOD(abort);

    /*
        Aborts a read-only transaction but makes it renewable with `renew`.
        (Wrapper for `mdbx_txn_reset`)
    */
    static NAN_METHOD(reset);

    /*
        Renews a read-only transaction after it has been reset.
        (Wrapper for `mdbx_txn_renew`)
    */
    static NAN_METHOD(renew);

};

/*
    `Dbi`
    Represents a database instance in an environment.
    (Wrapper for `MDBX_dbi`)
*/
class DbiWrap : public Nan::ObjectWrap {
public:
    // Tells how keys should be treated
    NodeLmdbxKeyType keyType;
    // Stores flags set when opened
    int flags;
    // The wrapped object
    MDBX_dbi dbi;
    // Reference to the MDBX_env of the wrapped MDBX_dbi
    MDBX_env *env;
    // The EnvWrap object of the current Dbi
    EnvWrap *ew;
    // Whether the Dbi was opened successfully
    bool isOpen;
    // compression settings and space
    Compression* compression;
    // versions stored in data
    bool hasVersions;
    // current unsafe buffer for this db
    bool getFast;

    friend class TxnWrap;
    friend class CursorWrap;
    friend class EnvWrap;

    DbiWrap(MDBX_env *env, MDBX_dbi dbi);
    ~DbiWrap();

    // Constructor (not exposed)
    static NAN_METHOD(ctor);

    /*
        Closes the database instance.
        Wrapper for `mdbx_dbi_close`)
    */
    static NAN_METHOD(close);

    /*
        Drops the database instance, either deleting it completely (default) or just freeing its pages.

        Parameters:

        * Options object that contains possible configuration options.

        Possible options are:

        * justFreePages - indicates that the database pages need to be freed but the database shouldn't be deleted

    */
    static NAN_METHOD(drop);

    static NAN_METHOD(stat);
#if ENABLE_FAST_API && NODE_VERSION_AT_LEAST(16,6,0)
    static uint32_t getByBinaryFast(Local<Object> receiver_obj, uint32_t keySize, FastApiCallbackOptions& options);
#endif
    static void getByBinary(const v8::FunctionCallbackInfo<v8::Value>& info);
    static NAN_METHOD(getStringByBinary);
};

class Compression : public Nan::ObjectWrap {
public:
    char* dictionary;
    char* decompressBlock;
    char* decompressTarget;
    unsigned int decompressSize;
    unsigned int compressionThreshold;
    // compression acceleration (defaults to 1)
    int acceleration;
    static thread_local LZ4_stream_t* stream;
    void decompress(MDBX_val& data, bool &isValid, bool canAllocate);
    argtokey_callback_t compress(MDBX_val* value, argtokey_callback_t freeValue);
    int compressInstruction(EnvWrap* env, double* compressionAddress);
    static NAN_METHOD(ctor);
    static NAN_METHOD(setBuffer);
    Compression();
    ~Compression();
    friend class EnvWrap;
    friend class DbiWrap;
    //NAN_METHOD(Compression::startCompressing);
};

/*
    `Cursor`
    Represents a cursor instance that is assigned to a transaction and a database instance
    (Wrapper for `MDBX_cursor`)
*/
class CursorWrap : public Nan::ObjectWrap {

private:

    // The wrapped object
    MDBX_cursor *cursor;
    // Stores how key is represented
    NodeLmdbxKeyType keyType;
    // Key/data pair where the cursor is at, and ending key
    MDBX_val key, data, endKey;
    // Free function for the current key
    argtokey_callback_t freeKey;
    MDBX_cursor_op iteratingOp;
    int flags;
    DbiWrap *dw;
    MDBX_txn *txn;
    
    template<size_t keyIndex, size_t optionsIndex>
    friend argtokey_callback_t cursorArgToKey(CursorWrap* cw, Nan::NAN_METHOD_ARGS_TYPE info, MDBX_val &key, bool &keyIsValid);

public:
    CursorWrap(MDBX_cursor *cursor);
    ~CursorWrap();

    // Sets up exports for the Cursor constructor
    static void setupExports(Local<Object> exports);

    /*
        Opens a new cursor for the specified transaction and database instance.
        (Wrapper for `mdbx_cursor_open`)

        Parameters:

        * Transaction object
        * Database instance object
    */
    static NAN_METHOD(ctor);

    /*
        Closes the cursor.
        (Wrapper for `mdbx_cursor_close`)

        Parameters:

        * Transaction object
        * Database instance object
    */
    static NAN_METHOD(close);
    /*
        Deletes the key/data pair to which the cursor refers.
        (Wrapper for `mdbx_cursor_del`)
    */
    static NAN_METHOD(del);

    static NAN_METHOD(getCurrentValue);
    int returnEntry(int lastRC, MDBX_val &key, MDBX_val &data);
#if ENABLE_FAST_API && NODE_VERSION_AT_LEAST(16,6,0)
    static uint32_t positionFast(Local<Object> receiver_obj, uint32_t flags, uint32_t offset, uint32_t keySize, uint64_t endKeyAddress, FastApiCallbackOptions& options);
    static uint32_t iterateFast(Local<Object> receiver_obj, FastApiCallbackOptions& options);
#endif
    static void position(const v8::FunctionCallbackInfo<v8::Value>& info);    
    uint32_t doPosition(uint32_t offset, uint32_t keySize, uint64_t endKeyAddress);
    static void iterate(const v8::FunctionCallbackInfo<v8::Value>& info);    
    static NAN_METHOD(renew);
    //static NAN_METHOD(getStringByBinary);
};

// External string resource that glues MDBX_val and v8::String
class CustomExternalStringResource : public String::ExternalStringResource {
private:
    const uint16_t *d;
    size_t l;

public:
    CustomExternalStringResource(MDBX_val *val);
    ~CustomExternalStringResource();

    void Dispose();
    const uint16_t *data() const;
    size_t length() const;

    static void writeTo(Local<String> str, MDBX_val *val);
};

class CustomExternalOneByteStringResource : public String::ExternalOneByteStringResource {
private:
    const char *d;
    size_t l;

public:
    CustomExternalOneByteStringResource(MDBX_val *val);
    ~CustomExternalOneByteStringResource();

    void Dispose();
    const char *data() const;
    size_t length() const;

};


#endif // NODE_LMDBX_H
