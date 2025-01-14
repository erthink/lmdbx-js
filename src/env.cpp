#include "lmdbx-js.h"
using namespace v8;
using namespace node;

#define IGNORE_NOTFOUND    (1)
thread_local Nan::Persistent<Function>* EnvWrap::txnCtor;
thread_local Nan::Persistent<Function>* EnvWrap::dbiCtor;

pthread_mutex_t* EnvWrap::envsLock = EnvWrap::initMutex();
std::vector<env_path_t> EnvWrap::envs;

pthread_mutex_t* EnvWrap::initMutex() {
    pthread_mutex_t* mutex = new pthread_mutex_t;
    pthread_mutex_init(mutex, nullptr);
    return mutex;
}

EnvWrap::EnvWrap() {
    this->env = nullptr;
    this->currentWriteTxn = nullptr;
	this->currentReadTxn = nullptr;
    this->writeTxn = nullptr;
    this->writeWorker = nullptr;
	this->readTxnRenewed = false;
    this->writingLock = new pthread_mutex_t;
    this->writingCond = new pthread_cond_t;
    pthread_mutex_init(this->writingLock, nullptr);
    pthread_cond_init(this->writingCond, nullptr);
}

EnvWrap::~EnvWrap() {
    // Close if not closed already
    if (this->env) {
        this->cleanupStrayTxns();
        mdbx_env_close(env);
    }
    if (this->compression)
        this->compression->Unref();
    pthread_mutex_destroy(this->writingLock);
    pthread_cond_destroy(this->writingCond);
    
}

void EnvWrap::cleanupStrayTxns() {
    if (this->currentWriteTxn) {
        mdbx_txn_abort(this->currentWriteTxn->txn);
        this->currentWriteTxn->removeFromEnvWrap();
    }
    while (this->readTxns.size()) {
        TxnWrap *tw = *this->readTxns.begin();
        mdbx_txn_abort(tw->txn);
        tw->removeFromEnvWrap();
    }
}

NAN_METHOD(EnvWrap::ctor) {
    Nan::HandleScope scope;

    int rc;

    EnvWrap* ew = new EnvWrap();
    rc = mdbx_env_create(&(ew->env));

    if (rc != 0) {
        mdbx_env_close(ew->env);
        return throwLmdbxError(rc);
    }

    ew->Wrap(info.This());
    ew->Ref();

    return info.GetReturnValue().Set(info.This());
}

template<class T>
int applyUint32Setting(int (*f)(MDBX_env *, T), MDBX_env* e, Local<Object> options, T dflt, const char* keyName) {
    int rc;
    const Local<Value> value = options->Get(Nan::GetCurrentContext(), Nan::New<String>(keyName).ToLocalChecked()).ToLocalChecked();
    if (value->IsUint32()) {
        rc = f(e, value->Uint32Value(Nan::GetCurrentContext()).FromJust());
    }
    else {
        rc = f(e, dflt);
    }

    return rc;
}

class SyncWorker : public Nan::AsyncWorker {
  public:
    SyncWorker(MDBX_env* env, Nan::Callback *callback)
      : Nan::AsyncWorker(callback), env(env) {}

    void Execute() {
/*       int rc = mdbx_env_sync(env, 1);
        if (rc != 0) {
            SetErrorMessage(mdbx_strerror(rc));
        }*/
    }

    void HandleOKCallback() {
        Nan::HandleScope scope;
        Local<v8::Value> argv[] = {
            Nan::Null()
        };

        callback->Call(1, argv, async_resource);
    }

  private:
    MDBX_env* env;
};

class CopyWorker : public Nan::AsyncWorker {
  public:
    CopyWorker(MDBX_env* env, char* inPath, int flags, Nan::Callback *callback)
      : Nan::AsyncWorker(callback), env(env), path(strdup(inPath)), flags(flags) {
      }
    ~CopyWorker() {
        free(path);
    }

    void Execute() {
/*        int rc = mdbx_env_copy2(env, path, flags);
        if (rc != 0) {
            fprintf(stderr, "Error on copy code: %u\n", rc);
            SetErrorMessage("Error on copy");
        }*/
    }

    void HandleOKCallback() {
        Nan::HandleScope scope;
        Local<v8::Value> argv[] = {
            Nan::Null()
        };

        callback->Call(1, argv, async_resource);
    }

  private:
    MDBX_env* env;
    char* path;
    int flags;
};
MDBX_txn* EnvWrap::getReadTxn() {
    MDBX_txn* txn = writeTxn ? writeTxn->txn : nullptr;
    if (txn)
        return txn;
    txn = currentReadTxn;
    if (readTxnRenewed)
        return txn;
    if (txn)
        mdbx_txn_renew(txn);
    else {
        mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
        currentReadTxn = txn;
    }
    readTxnRenewed = true;
    return txn;
}

void cleanup(void* data) {
    ((EnvWrap*) data)->closeEnv();
}

NAN_METHOD(EnvWrap::open) {
    Nan::HandleScope scope;

    int rc;
    MDBX_env_flags_t flags = MDBX_ENV_DEFAULTS;
    int jsFlags = 0;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());

    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }
    Local<Object> options = Local<Object>::Cast(info[0]);
    ew->compression = nullptr;
    Local<Value> compressionOption = options->Get(Nan::GetCurrentContext(), Nan::New<String>("compression").ToLocalChecked()).ToLocalChecked();
    if (compressionOption->IsObject()) {
        ew->compression = Nan::ObjectWrap::Unwrap<Compression>(Nan::To<Object>(compressionOption).ToLocalChecked());
        ew->compression->Ref();
    }
    Local<Value> keyBytesValue = options->Get(Nan::GetCurrentContext(), Nan::New<String>("keyBytes").ToLocalChecked()).ToLocalChecked();
    if (keyBytesValue->IsArrayBufferView())
        ew->keyBuffer = node::Buffer::Data(keyBytesValue);
    setFlagFromValue(&jsFlags, SEPARATE_FLUSHED, "separateFlushed", false, options);
    ew->jsFlags = jsFlags;
    Local<String> path = Local<String>::Cast(options->Get(Nan::GetCurrentContext(), Nan::New<String>("path").ToLocalChecked()).ToLocalChecked());
    Nan::Utf8String charPath(path);
    pthread_mutex_lock(envsLock);
    for (env_path_t envPath : envs) {
        char* existingPath = envPath.path;
        if (!strcmp(existingPath, *charPath)) {
            envPath.count++;
            mdbx_env_close(ew->env);
            ew->env = envPath.env;
            pthread_mutex_unlock(envsLock);
            int maxKeySize = mdbx_env_get_maxkeysize_ex(ew->env, MDBX_DB_DEFAULTS);
            info.GetReturnValue().Set(Nan::New<Number>(maxKeySize));
            return;
        }
    }

    // Parse the maxDbs option
    rc = applyUint32Setting<unsigned>(&mdbx_env_set_maxdbs, ew->env, options, 1, "maxDbs");
    if (rc != 0) {
        pthread_mutex_unlock(envsLock);
        return throwLmdbxError(rc);
    }

    // Parse the mapSize option
    Local<Value> mapSizeOption = options->Get(Nan::GetCurrentContext(), Nan::New<String>("mapSize").ToLocalChecked()).ToLocalChecked();
    if (mapSizeOption->IsNumber()) {
        intptr_t mapSizeSizeT = mapSizeOption->NumberValue(Nan::GetCurrentContext()).FromJust();
        rc = mdbx_env_set_geometry(ew->env, -1, -1, mapSizeSizeT, -1, -1, -1);
        if (rc != 0) {
            pthread_mutex_unlock(envsLock);
            return throwLmdbxError(rc);
        }
    }

    // Parse the maxReaders option
    // NOTE: mdbx.c defines DEFAULT_READERS as 126
    rc = applyUint32Setting<unsigned>(&mdbx_env_set_maxreaders, ew->env, options, 126, "maxReaders");
    if (rc != 0) {
        return throwLmdbxError(rc);
    }

    // NOTE: MDBX_FIXEDMAP is not exposed here since it is "highly experimental" + it is irrelevant for this use case
    // NOTE: MDBX_NOTLS is not exposed here because it is irrelevant for this use case, as node will run all this on a single thread anyway
    setFlagFromValue((int*) &flags, (int)MDBX_NOSUBDIR, "noSubdir", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_RDONLY, "readOnly", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_WRITEMAP, "useWritemap", false, options);
    //setFlagFromValue(&flags, MDBX_PREVSNAPSHOT, "usePreviousSnapshot", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_NOMEMINIT , "noMemInit", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_NORDAHEAD , "noReadAhead", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_NOMETASYNC, "noMetaSync", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_SAFE_NOSYNC, "safeNoSync", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_UTTERLY_NOSYNC, "noSync", false, options);
    setFlagFromValue((int*) &flags, (int)MDBX_MAPASYNC, "mapAsync", false, options);
    //setFlagFromValue(&flags, MDBX_NOLOCK, "unsafeNoLock", false, options);*/

    /*if ((int) flags & (int) MDBX_NOLOCK) {
        fprintf(stderr, "You chose to use MDBX_NOLOCK which is not officially supported by node-lmdbx. You have been warned!\n");
    }*/

    // Set MDBX_NOTLS to enable multiple read-only transactions on the same thread (in this case, the nodejs main thread)
    flags |= MDBX_NOTLS;

    // TODO: make file attributes configurable
    #if NODE_VERSION_AT_LEAST(12,0,0)
    rc = mdbx_env_open(ew->env, *String::Utf8Value(Isolate::GetCurrent(), path), flags, 0664);
    #else
    rc = mdbx_env_open(ew->env, *String::Utf8Value(path), flags, 0664);
    #endif

    if (rc != 0) {
        mdbx_env_close(ew->env);
        uv_mutex_unlock(envsLock);
        ew->env = nullptr;
        return throwLmdbxError(rc);
    }
    node::AddEnvironmentCleanupHook(Isolate::GetCurrent(), cleanup, ew);
    env_path_t envPath;
    envPath.path = strdup(*charPath);
    envPath.env = ew->env;
    envPath.count = 1;
    envs.push_back(envPath);
    pthread_mutex_unlock(envsLock);
    int maxKeySize = mdbx_env_get_maxkeysize_ex(ew->env, MDBX_DB_DEFAULTS);
    info.GetReturnValue().Set(Nan::New<Number>(maxKeySize));
}

NAN_METHOD(EnvWrap::resize) {
    Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());

    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    // Check that the correct number/type of arguments was given.
    if (info.Length() != 1 || !info[0]->IsNumber()) {
        return Nan::ThrowError("Call env.resize() with exactly one argument which is a number.");
    }

    // Since this function may only be called if no transactions are active in this process, check this condition.
    if (ew->currentWriteTxn/* || ew->readTxns.size()*/) {
        return Nan::ThrowError("Only call env.resize() when there are no active transactions. Please close all transactions before calling env.resize().");
    }

    size_t mapSizeSizeT = info[0]->IntegerValue(Nan::GetCurrentContext()).FromJust();
    int rc = mdbx_env_set_mapsize(ew->env, mapSizeSizeT);
    if (rc == EINVAL) {
        //fprintf(stderr, "Resize failed, will try to get transaction and try again");
        MDBX_txn *txn;
        rc = mdbx_txn_begin(ew->env, nullptr, MDBX_TXN_READWRITE, &txn);
        if (rc != 0)
            return throwLmdbxError(rc);
        rc = mdbx_txn_commit(txn);
        if (rc != 0)
            return throwLmdbxError(rc);
        rc = mdbx_env_set_mapsize(ew->env, mapSizeSizeT);
    }
    if (rc != 0) {
        return throwLmdbxError(rc);
    }
}

void EnvWrap::closeEnv() {
    cleanupStrayTxns();

    pthread_mutex_lock(envsLock);
    for (auto envPath = envs.begin(); envPath != envs.end(); ) {
        if (envPath->env == env) {
            envPath->count--;
            if (envPath->count <= 0) {
                // last thread using it, we can really close it now
                envs.erase(envPath);
                mdbx_env_close(env);
            }
            break;
        }
        ++envPath;
    }
    pthread_mutex_unlock(envsLock);

    env = nullptr;

}
NAN_METHOD(EnvWrap::close) {
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    ew->Unref();

    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }
    ew->closeEnv();
}

NAN_METHOD(EnvWrap::stat) {
    Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    int rc;
    MDBX_stat stat;

    rc = mdbx_env_stat(ew->env, &stat, sizeof(MDBX_stat));
    if (rc != 0) {
        return throwLmdbxError(rc);
    }

    Local<Context> context = Nan::GetCurrentContext();
    Local<Object> obj = Nan::New<Object>();
    (void)obj->Set(context, Nan::New<String>("pageSize").ToLocalChecked(), Nan::New<Number>(stat.ms_psize));
    (void)obj->Set(context, Nan::New<String>("treeDepth").ToLocalChecked(), Nan::New<Number>(stat.ms_depth));
    (void)obj->Set(context, Nan::New<String>("treeBranchPageCount").ToLocalChecked(), Nan::New<Number>(stat.ms_branch_pages));
    (void)obj->Set(context, Nan::New<String>("treeLeafPageCount").ToLocalChecked(), Nan::New<Number>(stat.ms_leaf_pages));
    (void)obj->Set(context, Nan::New<String>("entryCount").ToLocalChecked(), Nan::New<Number>(stat.ms_entries));
    (void)obj->Set(context, Nan::New<String>("overflowPages").ToLocalChecked(), Nan::New<Number>(stat.ms_overflow_pages));

    info.GetReturnValue().Set(obj);
}

NAN_METHOD(EnvWrap::freeStat) {
    Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    if (info.Length() != 1) {
        return Nan::ThrowError("env.freeStat should be called with a single argument which is a txn.");
    }

    TxnWrap *txn = Nan::ObjectWrap::Unwrap<TxnWrap>(Local<Object>::Cast(info[0]));

    int rc;
    MDBX_stat stat;

    rc = mdbx_dbi_stat(txn->txn, 0, &stat, sizeof(MDBX_stat));
    if (rc != 0) {
        return throwLmdbxError(rc);
    }

    Local<Context> context = Nan::GetCurrentContext();
    Local<Object> obj = Nan::New<Object>();
    (void)obj->Set(context, Nan::New<String>("pageSize").ToLocalChecked(), Nan::New<Number>(stat.ms_psize));
    (void)obj->Set(context, Nan::New<String>("treeDepth").ToLocalChecked(), Nan::New<Number>(stat.ms_depth));
    (void)obj->Set(context, Nan::New<String>("treeBranchPageCount").ToLocalChecked(), Nan::New<Number>(stat.ms_branch_pages));
    (void)obj->Set(context, Nan::New<String>("treeLeafPageCount").ToLocalChecked(), Nan::New<Number>(stat.ms_leaf_pages));
    (void)obj->Set(context, Nan::New<String>("entryCount").ToLocalChecked(), Nan::New<Number>(stat.ms_entries));
    (void)obj->Set(context, Nan::New<String>("overflowPages").ToLocalChecked(), Nan::New<Number>(stat.ms_overflow_pages));

    info.GetReturnValue().Set(obj);
}

NAN_METHOD(EnvWrap::info) {
    Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    int rc;
    MDBX_envinfo envinfo;

    rc = mdbx_env_info(ew->env, &envinfo, sizeof(MDBX_envinfo));
    if (rc != 0) {
        return throwLmdbxError(rc);
    }

    Local<Context> context = Nan::GetCurrentContext();
    Local<Object> obj = Nan::New<Object>();
//    (void)obj->Set(context, Nan::New<String>("mapAddress").ToLocalChecked(), Nan::New<Number>((uint64_t) envinfo.mi_mapaddr));
    (void)obj->Set(context, Nan::New<String>("mapSize").ToLocalChecked(), Nan::New<Number>(envinfo.mi_mapsize));
    (void)obj->Set(context, Nan::New<String>("lastPageNumber").ToLocalChecked(), Nan::New<Number>(envinfo.mi_last_pgno));
    //(void)obj->Set(context, Nan::New<String>("lastTxnId").ToLocalChecked(), Nan::New<Number>(envinfo.mi_last_txnid));
    (void)obj->Set(context, Nan::New<String>("maxReaders").ToLocalChecked(), Nan::New<Number>(envinfo.mi_maxreaders));
    (void)obj->Set(context, Nan::New<String>("numReaders").ToLocalChecked(), Nan::New<Number>(envinfo.mi_numreaders));

    info.GetReturnValue().Set(obj);
}

NAN_METHOD(EnvWrap::readerCheck) {
    Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    int rc, dead;
    rc = mdbx_reader_check(ew->env, &dead);
    if (rc != 0) {
        return throwLmdbxError(rc);
    }

    info.GetReturnValue().Set(Nan::New<Number>(dead));
}

/*Local<Array> readerStrings;
MDBX_msg_func* printReaders = ([](const char* message, void* ctx) -> int {
    readerStrings->Set(Nan::GetCurrentContext(), readerStrings->Length(), Nan::New<String>(message).ToLocalChecked()).ToChecked();
    return 0;
});*/

NAN_METHOD(EnvWrap::readerList) {
    /*Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap* ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    readerStrings = Nan::New<Array>(0);
    int rc;
    rc = mdbx_reader_list(ew->env, printReaders, nullptr);
    if (rc != 0) {
        return throwLmdbxError(rc);
    }
    info.GetReturnValue().Set(readerStrings);*/
}


NAN_METHOD(EnvWrap::copy) {
    /*Nan::HandleScope scope;

    // Get the wrapper
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());

    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    // Check that the correct number/type of arguments was given.
    if (!info[0]->IsString()) {
        return Nan::ThrowError("Call env.copy(path, compact?, callback) with a file path.");
    }
    if (!info[info.Length() - 1]->IsFunction()) {
        return Nan::ThrowError("Call env.copy(path, compact?, callback) with a file path.");
    }
    Nan::Utf8String path(info[0].As<String>());

    int flags = 0;
    if (info.Length() > 1 && info[1]->IsTrue()) {
        flags = MDBX_CP_COMPACT;
    }

    Nan::Callback* callback = new Nan::Callback(
      Local<v8::Function>::Cast(info[info.Length()  > 2 ? 2 : 1])
    );

    CopyWorker* worker = new CopyWorker(
      ew->env, *path, flags, callback
    );

    Nan::AsyncQueueWorker(worker);*/
}

NAN_METHOD(EnvWrap::detachBuffer) {
    Nan::HandleScope scope;
    #if NODE_VERSION_AT_LEAST(12,0,0)
    Local<v8::ArrayBuffer>::Cast(info[0])->Detach();
    #endif
}

NAN_METHOD(EnvWrap::beginTxn) {
    Nan::HandleScope scope;

    Nan::MaybeLocal<Object> maybeInstance;

    int flags = info[0]->IntegerValue(Nan::GetCurrentContext()).FromJust();
    if (!(flags & (int) MDBX_TXN_RDONLY)) {
        //fprintf(stderr, "begin sync txn %i\n", flags);
        EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
        MDBX_env *env = ew->env;
        unsigned int envFlags;
        mdbx_env_get_flags(env, &envFlags);
        MDBX_txn *txn;

        if (ew->writeTxn)
            txn = ew->writeTxn->txn;
        else if (ew->writeWorker) {
            // try to acquire the txn from the current batch
            txn = ew->writeWorker->AcquireTxn(&flags);
            //fprintf(stderr, "acquired %p %p %p\n", ew->writeWorker, txn, flags);
        } else {
            pthread_mutex_lock(ew->writingLock);
            txn = nullptr;
        }

        if (txn) {
            if (flags & TXN_ABORTABLE) {
                if (envFlags & MDBX_WRITEMAP)
                    flags &= ~TXN_ABORTABLE;
                else {
                    // child txn
                    mdbx_txn_begin(env, txn, (MDBX_txn_flags_t)(flags & 0xf0000), &txn);
                    TxnTracked* childTxn = new TxnTracked(txn, flags);
                    childTxn->parent = ew->writeTxn;
                    ew->writeTxn = childTxn;
                    return;
                }
            }
        } else {
            mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)(flags & 0xf0000), &txn);
            flags |= TXN_ABORTABLE;
        }
        ew->writeTxn = new TxnTracked(txn, flags);
        return;
    }

    if (info.Length() > 1) {
        const int argc = 3;

        Local<Value> argv[argc] = { info.This(), info[0], info[1] };
        maybeInstance = Nan::NewInstance(Nan::New(*txnCtor), argc, argv);

    } else {
        const int argc = 2;

        Local<Value> argv[argc] = { info.This(), info[0] };
        //fprintf(stdout, "beginTxn %u", info[0]->IsTrue());
        maybeInstance = Nan::NewInstance(Nan::New(*txnCtor), argc, argv);
    }

    // Check if txn could be created
    if ((maybeInstance.IsEmpty())) {
        // The maybeInstance is empty because the txnCtor called Nan::ThrowError.
        // No need to call that here again, the user will get the error thrown there.
        return;
    }

    Local<Object> instance = maybeInstance.ToLocalChecked();
    info.GetReturnValue().Set(instance);
}
NAN_METHOD(EnvWrap::commitTxn) {
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    TxnTracked *currentTxn = ew->writeTxn;
    //fprintf(stderr, "commitTxn %p\n", currentTxn);
    int rc = 0;
    if (currentTxn->flags & TXN_ABORTABLE) {
        //fprintf(stderr, "txn_commit\n");
        rc = mdbx_txn_commit(currentTxn->txn);
    }
    ew->writeTxn = currentTxn->parent;
    if (!ew->writeTxn) {
        //fprintf(stderr, "unlock txn\n");
        if (ew->writeWorker)
            ew->writeWorker->UnlockTxn();
        else
            pthread_mutex_unlock(ew->writingLock);
    }
    delete currentTxn;
    if (rc)
        throwLmdbxError(rc);
}
NAN_METHOD(EnvWrap::abortTxn) {
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    TxnTracked *currentTxn = ew->writeTxn;
    //fprintf(stderr, "abortTxn\n");
    if (currentTxn->flags & TXN_ABORTABLE) {
        //fprintf(stderr, "txn_abort\n");
        mdbx_txn_abort(currentTxn->txn);
    } else {
        Nan::ThrowError("Can not abort this transaction");
    }
    ew->writeTxn = currentTxn->parent;
    if (!ew->writeTxn) {
        //fprintf(stderr, "unlock txn\n");
        if (ew->writeWorker)
            ew->writeWorker->UnlockTxn();
        else
            pthread_mutex_unlock(ew->writingLock);
    }
    delete currentTxn;
}

NAN_METHOD(EnvWrap::openDbi) {
    Nan::HandleScope scope;

    const unsigned argc = 2;
    Local<Value> argv[argc] = { info.This(), info[0] };
    Nan::MaybeLocal<Object> maybeInstance = Nan::NewInstance(Nan::New(*dbiCtor), argc, argv);

    // Check if database could be opened
    if ((maybeInstance.IsEmpty())) {
        // The maybeInstance is empty because the dbiCtor called Nan::ThrowError.
        // No need to call that here again, the user will get the error thrown there.
        return;
    }

    Local<Object> instance = maybeInstance.ToLocalChecked();
    info.GetReturnValue().Set(instance);
}

NAN_METHOD(EnvWrap::sync) {
    Nan::HandleScope scope;

    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());

    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }

    Nan::Callback* callback = new Nan::Callback(
      Local<v8::Function>::Cast(info[0])
    );

    SyncWorker* worker = new SyncWorker(
      ew->env, callback
    );

    Nan::AsyncQueueWorker(worker);
    return;
}

NAN_METHOD(EnvWrap::resetCurrentReadTxn) {
    EnvWrap* ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    mdbx_txn_reset(ew->currentReadTxn);
    ew->readTxnRenewed = false;
}

void EnvWrap::setupExports(Local<Object> exports) {
    // EnvWrap: Prepare constructor template
    Local<FunctionTemplate> envTpl = Nan::New<FunctionTemplate>(EnvWrap::ctor);
    envTpl->SetClassName(Nan::New<String>("Env").ToLocalChecked());
    envTpl->InstanceTemplate()->SetInternalFieldCount(1);
    // EnvWrap: Add functions to the prototype
    Isolate *isolate = Isolate::GetCurrent();
    envTpl->PrototypeTemplate()->Set(isolate, "open", Nan::New<FunctionTemplate>(EnvWrap::open));
    envTpl->PrototypeTemplate()->Set(isolate, "close", Nan::New<FunctionTemplate>(EnvWrap::close));
    envTpl->PrototypeTemplate()->Set(isolate, "beginTxn", Nan::New<FunctionTemplate>(EnvWrap::beginTxn));
    envTpl->PrototypeTemplate()->Set(isolate, "commitTxn", Nan::New<FunctionTemplate>(EnvWrap::commitTxn));
    envTpl->PrototypeTemplate()->Set(isolate, "abortTxn", Nan::New<FunctionTemplate>(EnvWrap::abortTxn));
    envTpl->PrototypeTemplate()->Set(isolate, "openDbi", Nan::New<FunctionTemplate>(EnvWrap::openDbi));
    envTpl->PrototypeTemplate()->Set(isolate, "sync", Nan::New<FunctionTemplate>(EnvWrap::sync));
    envTpl->PrototypeTemplate()->Set(isolate, "startWriting", Nan::New<FunctionTemplate>(EnvWrap::startWriting));
    envTpl->PrototypeTemplate()->Set(isolate, "compress", Nan::New<FunctionTemplate>(EnvWrap::compress));
    envTpl->PrototypeTemplate()->Set(isolate, "stat", Nan::New<FunctionTemplate>(EnvWrap::stat));
    envTpl->PrototypeTemplate()->Set(isolate, "freeStat", Nan::New<FunctionTemplate>(EnvWrap::freeStat));
    envTpl->PrototypeTemplate()->Set(isolate, "info", Nan::New<FunctionTemplate>(EnvWrap::info));
    envTpl->PrototypeTemplate()->Set(isolate, "readerCheck", Nan::New<FunctionTemplate>(EnvWrap::readerCheck));
    envTpl->PrototypeTemplate()->Set(isolate, "readerList", Nan::New<FunctionTemplate>(EnvWrap::readerList));
    envTpl->PrototypeTemplate()->Set(isolate, "resize", Nan::New<FunctionTemplate>(EnvWrap::resize));
    envTpl->PrototypeTemplate()->Set(isolate, "copy", Nan::New<FunctionTemplate>(EnvWrap::copy));
    envTpl->PrototypeTemplate()->Set(isolate, "detachBuffer", Nan::New<FunctionTemplate>(EnvWrap::detachBuffer));
    envTpl->PrototypeTemplate()->Set(isolate, "resetCurrentReadTxn", Nan::New<FunctionTemplate>(EnvWrap::resetCurrentReadTxn));

    // TxnWrap: Prepare constructor template
    Local<FunctionTemplate> txnTpl = Nan::New<FunctionTemplate>(TxnWrap::ctor);
    txnTpl->SetClassName(Nan::New<String>("Txn").ToLocalChecked());
    txnTpl->InstanceTemplate()->SetInternalFieldCount(1);
    // TxnWrap: Add functions to the prototype
    txnTpl->PrototypeTemplate()->Set(isolate, "commit", Nan::New<FunctionTemplate>(TxnWrap::commit));
    txnTpl->PrototypeTemplate()->Set(isolate, "abort", Nan::New<FunctionTemplate>(TxnWrap::abort));
    txnTpl->PrototypeTemplate()->Set(isolate, "reset", Nan::New<FunctionTemplate>(TxnWrap::reset));
    txnTpl->PrototypeTemplate()->Set(isolate, "renew", Nan::New<FunctionTemplate>(TxnWrap::renew));
    // TODO: wrap mdbx_cmp too
    // TODO: wrap mdbx_dcmp too
    // TxnWrap: Get constructor
    EnvWrap::txnCtor = new Nan::Persistent<Function>();
    EnvWrap::txnCtor->Reset( txnTpl->GetFunction(Nan::GetCurrentContext()).ToLocalChecked());

    // DbiWrap: Prepare constructor template
    Local<FunctionTemplate> dbiTpl = Nan::New<FunctionTemplate>(DbiWrap::ctor);
    dbiTpl->SetClassName(Nan::New<String>("Dbi").ToLocalChecked());
    dbiTpl->InstanceTemplate()->SetInternalFieldCount(1);
    // DbiWrap: Add functions to the prototype
    dbiTpl->PrototypeTemplate()->Set(isolate, "close", Nan::New<FunctionTemplate>(DbiWrap::close));
    dbiTpl->PrototypeTemplate()->Set(isolate, "drop", Nan::New<FunctionTemplate>(DbiWrap::drop));
    dbiTpl->PrototypeTemplate()->Set(isolate, "stat", Nan::New<FunctionTemplate>(DbiWrap::stat));
    #if ENABLE_FAST_API && NODE_VERSION_AT_LEAST(16,6,0)
    auto getFast = CFunction::Make(DbiWrap::getByBinaryFast);
    dbiTpl->PrototypeTemplate()->Set(isolate, "getByBinary", v8::FunctionTemplate::New(
          isolate, DbiWrap::getByBinary, v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasNoSideEffect, &getFast));
    auto writeFast = CFunction::Make(EnvWrap::writeFast);
    envTpl->PrototypeTemplate()->Set(isolate, "write", v8::FunctionTemplate::New(
        isolate, EnvWrap::write, v8::Local<v8::Value>(),
        v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
        v8::SideEffectType::kHasNoSideEffect, &writeFast));

    #else
    dbiTpl->PrototypeTemplate()->Set(isolate, "getByBinary", v8::FunctionTemplate::New(
          isolate, DbiWrap::getByBinary, v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasNoSideEffect));
    envTpl->PrototypeTemplate()->Set(isolate, "write", v8::FunctionTemplate::New(
        isolate, EnvWrap::write, v8::Local<v8::Value>(),
        v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
        v8::SideEffectType::kHasNoSideEffect));
    #endif
    dbiTpl->PrototypeTemplate()->Set(isolate, "getStringByBinary", Nan::New<FunctionTemplate>(DbiWrap::getStringByBinary));
    dbiTpl->PrototypeTemplate()->Set(isolate, "stat", Nan::New<FunctionTemplate>(DbiWrap::stat));


    // TODO: wrap mdbx_stat too
    // DbiWrap: Get constructor
    EnvWrap::dbiCtor = new Nan::Persistent<Function>();
    EnvWrap::dbiCtor->Reset( dbiTpl->GetFunction(Nan::GetCurrentContext()).ToLocalChecked());

    Local<FunctionTemplate> compressionTpl = Nan::New<FunctionTemplate>(Compression::ctor);
    compressionTpl->SetClassName(Nan::New<String>("Compression").ToLocalChecked());
    compressionTpl->InstanceTemplate()->SetInternalFieldCount(1);
    compressionTpl->PrototypeTemplate()->Set(isolate, "setBuffer", Nan::New<FunctionTemplate>(Compression::setBuffer));
    (void)exports->Set(Nan::GetCurrentContext(), Nan::New<String>("Compression").ToLocalChecked(), compressionTpl->GetFunction(Nan::GetCurrentContext()).ToLocalChecked());

    // Set exports
    (void)exports->Set(Nan::GetCurrentContext(), Nan::New<String>("Env").ToLocalChecked(), envTpl->GetFunction(Nan::GetCurrentContext()).ToLocalChecked());
}

#ifdef _WIN32
#define EXTERN __declspec(dllexport)
# else
#define EXTERN __attribute__((visibility("default")))
#endif
extern "C" EXTERN size_t envOpen(uint32_t flags, const uint8_t * path, size_t length);
size_t envOpen(uint32_t flags, const uint8_t * path, size_t length) {
//	fprintf(stderr, "start!! %p %u\n", path, length);
    EnvWrap* ew = new EnvWrap();
    return (size_t) ew;
}

// This file contains code from the node-lmdb project
// Copyright (c) 2013-2017 Timur Kristóf
// Copyright (c) 2021 Kristopher Tate
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

