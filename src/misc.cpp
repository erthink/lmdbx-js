#include "lmdbx-js.h"
#include <string.h>
#include <stdio.h>

static thread_local char* globalUnsafePtr;
static thread_local size_t globalUnsafeSize;

void setupExportMisc(Local<Object> exports) {
    Local<Object> versionObj = Nan::New<Object>();

    /*int major, minor, patch;
    char *str = mdbx_version(&major, &minor, &patch);
    Local<Context> context = Nan::GetCurrentContext();
    (void)versionObj->Set(context, Nan::New<String>("versionString").ToLocalChecked(), Nan::New<String>(str).ToLocalChecked());
    (void)versionObj->Set(context, Nan::New<String>("major").ToLocalChecked(), Nan::New<Integer>(major));
    (void)versionObj->Set(context, Nan::New<String>("minor").ToLocalChecked(), Nan::New<Integer>(minor));
    (void)versionObj->Set(context, Nan::New<String>("patch").ToLocalChecked(), Nan::New<Integer>(patch));
*/
    Local<Context> context = Nan::GetCurrentContext();
    (void)exports->Set(context, Nan::New<String>("version").ToLocalChecked(), versionObj);
    Nan::SetMethod(exports, "setGlobalBuffer", setGlobalBuffer);
    Nan::SetMethod(exports, "lmdbxError", lmdbxError);
    //Nan::SetMethod(exports, "getBufferForAddress", getBufferForAddress);
    Nan::SetMethod(exports, "getAddress", getAddress);
    Nan::SetMethod(exports, "getAddressShared", getAddressShared);
    // this is set solely for the purpose of giving a good name to the set of native functions for the profiler since V8
    // just uses the name of the last exported native function:
    Nan::SetMethod(exports, "lmdbxNativeFunctions", getAddress);
}


void setFlagFromValue(int *flags, int flag, const char *name, bool defaultValue, Local<Object> options) {
    Local<Context> context = Nan::GetCurrentContext();
    Local<Value> opt = options->Get(context, Nan::New<String>(name).ToLocalChecked()).ToLocalChecked();
    #if NODE_VERSION_AT_LEAST(12,0,0)
    if (opt->IsBoolean() ? opt->BooleanValue(Isolate::GetCurrent()) : defaultValue) {
    #else
    if (opt->IsBoolean() ? opt->BooleanValue(context).FromJust() : defaultValue) {
    #endif
        *flags |= flag;
    }
}

NodeLmdbxKeyType keyTypeFromOptions(const Local<Value> &val, NodeLmdbxKeyType defaultKeyType) {
    if (!val->IsObject()) {
        return defaultKeyType;
    }
    auto obj = Local<Object>::Cast(val);

    NodeLmdbxKeyType keyType = defaultKeyType;
    int keyIsUint32 = 0;
    int keyIsBuffer = 0;
    int keyIsString = 0;
    
    setFlagFromValue(&keyIsUint32, 1, "keyIsUint32", false, obj);
    setFlagFromValue(&keyIsString, 1, "keyIsString", false, obj);
    setFlagFromValue(&keyIsBuffer, 1, "keyIsBuffer", false, obj);
    
    const char *keySpecificationErrorText = "You can't specify multiple key types at once. Either set keyIsUint32, or keyIsBuffer or keyIsString (default).";
    
    if (keyIsUint32) {
        keyType = NodeLmdbxKeyType::Uint32Key;
        if (keyIsBuffer || keyIsString) {
            Nan::ThrowError(keySpecificationErrorText);
            return NodeLmdbxKeyType::InvalidKey;
        }
    }
    else if (keyIsBuffer) {
        keyType = NodeLmdbxKeyType::BinaryKey;
        
        if (keyIsUint32 || keyIsString) {
            Nan::ThrowError(keySpecificationErrorText);
            return NodeLmdbxKeyType::InvalidKey;
        }
    }
    else if (keyIsString) {
        keyType = NodeLmdbxKeyType::StringKey;
    }
    
    return keyType;
}
Local<Value> valToStringUnsafe(MDBX_val &data) {
    auto resource = new CustomExternalOneByteStringResource(&data);
    auto str = Nan::New<v8::String>(resource);

    return str.ToLocalChecked();
}

Local<Value> valToUtf8(MDBX_val &data) {
    //const uint8_t *buffer = (const uint8_t*)(data.iov_base);
    //Isolate *isolate = Isolate::GetCurrent();
    //auto str = v8::String::NewFromOneByte(isolate, buffer, v8::NewStringType::kNormal, data.iov_len);
    const char *buffer = (const char*)(data.iov_base);
    auto str = Nan::New<v8::String>(buffer, data.iov_len);

    return str.ToLocalChecked();
}

Local<Value> valToString(MDBX_val &data) {
    // UTF-16 buffer
    const uint16_t *buffer = reinterpret_cast<const uint16_t*>(data.iov_base);
    // Number of UTF-16 code points
    size_t n = data.iov_len / sizeof(uint16_t);
    
    // Check zero termination
    if (n < 1 || buffer[n - 1] != 0) {
        Nan::ThrowError("Invalid zero-terminated UTF-16 string");
        return Nan::Undefined();
    }
    
    size_t length = n - 1;
    auto str = Nan::New<v8::String>(buffer, length);

    return str.ToLocalChecked();
}

bool valToBinaryFast(MDBX_val &data, DbiWrap* dw) {
    Compression* compression = dw->compression;
    if (compression) {
        if (data.iov_base == compression->decompressTarget) {
            // already decompressed to the target, nothing more to do
        } else {
            if (data.iov_len > compression->decompressSize) {
                return false;
            }
            // copy into the buffer target
            memcpy(compression->decompressTarget, data.iov_base, data.iov_len);
        }
    } else {
        if (data.iov_len > globalUnsafeSize) {
            // TODO: Provide a direct reference if for really large blocks, but we do that we need to detach that in the next turn
            /* if(data.iov_len > 64000) {
                dw->SetUnsafeBuffer(data.iov_base, data.iov_len);
                return Nan::New<Number>(data.iov_len);
            }*/
            return false;
        }
        memcpy(globalUnsafePtr, data.iov_base, data.iov_len);
    }
    return true;
}
Local<Value> valToBinaryUnsafe(MDBX_val &data, DbiWrap* dw) {
    valToBinaryFast(data, dw);
    return Nan::New<Number>(data.iov_len);
}


bool getVersionAndUncompress(MDBX_val &data, DbiWrap* dw) {
    //fprintf(stdout, "uncompressing %u\n", compressionThreshold);
    unsigned char* charData = (unsigned char*) data.iov_base;
    if (dw->hasVersions) {
        *((double*) (dw->ew->keyBuffer + 16)) = *((double*) charData);
//        fprintf(stderr, "getVersion %u\n", lastVersion);
        charData = charData + 8;
        data.iov_base = charData;
        data.iov_len -= 8;
    }
    if (data.iov_len == 0) {
        return true;// successFunc(data);
    }
    unsigned char statusByte = dw->compression ? charData[0] : 0;
        //fprintf(stdout, "uncompressing status %X\n", statusByte);
    if (statusByte >= 250) {
        bool isValid;
        dw->compression->decompress(data, isValid, !dw->getFast);
        if (!isValid)
            return false;
            //return Nan::Null();
    }
    return true;
}

NAN_METHOD(lmdbxError) {
    throwLmdbxError(Nan::To<v8::Number>(info[0]).ToLocalChecked()->Value());
}

NAN_METHOD(setGlobalBuffer) {
    globalUnsafePtr = node::Buffer::Data(info[0]);
    globalUnsafeSize = node::Buffer::Length(info[0]);
}

/*NAN_METHOD(getBufferForAddress) {
    char* address = (char*) (size_t) Nan::To<v8::Number>(info[0]).ToLocalChecked()->Value();
    std::unique_ptr<v8::BackingStore> backing = v8::ArrayBuffer::NewBackingStore(
    address, 0x100000000, [](void*, size_t, void*){}, nullptr);
    auto array_buffer = v8::ArrayBuffer::New(Isolate::GetCurrent(), std::move(backing));
    info.GetReturnValue().Set(array_buffer);
}*/
NAN_METHOD(getAddress) {
    void* address;
    Local<ArrayBuffer> buffer = Local<ArrayBuffer>::Cast(info[0]);
    #if _MSC_VER && NODE_RUNTIME_ELECTRON && NODE_MODULE_VERSION >= 89
    // this is a terrible thing we have to do because of https://github.com/electron/electron/issues/29893
    v8::Local<v8::Object> bufferView;
    bufferView = node::Buffer::New(Isolate::GetCurrent(), buffer, 0, buffer->ByteLength()).ToLocalChecked();
    address = node::Buffer::Data(bufferView);
    #elif V8_MAJOR_VERSION >= 8
    address = buffer->GetBackingStore()->Data();
    #else
    address = buffer->GetContents().Data();
    #endif
    info.GetReturnValue().Set(Nan::New<Number>((size_t) address));
}
NAN_METHOD(getAddressShared) {
    void* address;
    #if _MSC_VER && NODE_RUNTIME_ELECTRON && NODE_MODULE_VERSION >= 89
    // this is a terrible thing we have to do because of https://github.com/electron/electron/issues/29893
    v8::Local<v8::Object> bufferView;
    Local<ArrayBuffer> buffer = Local<ArrayBuffer>::Cast(info[0]);
    bufferView = node::Buffer::New(Isolate::GetCurrent(), buffer, 0, buffer->ByteLength()).ToLocalChecked();
    address = node::Buffer::Data(bufferView);
    #elif V8_MAJOR_VERSION >= 8
    address = Local<SharedArrayBuffer>::Cast(info[0])->GetBackingStore()->Data();
    #else
    address = Local<SharedArrayBuffer>::Cast(info[0])->GetContents().Data();
    #endif
    info.GetReturnValue().Set(Nan::New<Number>((size_t) address));
}


void throwLmdbxError(int rc) {
    auto err = Nan::Error(mdbx_strerror(rc));
    (void)err.As<Object>()->Set(Nan::GetCurrentContext(), Nan::New("code").ToLocalChecked(), Nan::New(rc));
    return Nan::ThrowError(err);
}

void consoleLog(const char *msg) {
    Local<String> str = Nan::New("console.log('").ToLocalChecked();
    //str = String::Concat(str, Nan::New<String>(msg).ToLocalChecked());
    //str = String::Concat(str, Nan::New("');").ToLocalChecked());

    Local<Script> script = Nan::CompileScript(str).ToLocalChecked();
    Nan::RunScript(script);
}

void consoleLog(Local<Value> val) {
    Local<String> str = Nan::New<String>("console.log('").ToLocalChecked();
    //str = String::Concat(str, Local<String>::Cast(val));
    //str = String::Concat(str, Nan::New<String>("');").ToLocalChecked());

    Local<Script> script = Nan::CompileScript(str).ToLocalChecked();
    Nan::RunScript(script);
}

void consoleLogN(int n) {
    char c[20];
    memset(c, 0, 20 * sizeof(char));
    sprintf(c, "%d", n);
    consoleLog(c);
}

void writeValueToEntry(const Local<Value> &value, MDBX_val *val) {
    if (value->IsString()) {
        Local<String> str = Local<String>::Cast(value);
        int strLength = str->Length();
        // an optimized guess at buffer length that works >99% of time and has good byte alignment
        int byteLength = str->IsOneByte() ? strLength :
            (((strLength >> 3) + ((strLength + 116) >> 6)) << 3);
        char *data = new char[byteLength];
        int utfWritten = 0;
#if NODE_VERSION_AT_LEAST(11,0,0)
        int bytes = str->WriteUtf8(Isolate::GetCurrent(), data, byteLength, &utfWritten, v8::String::WriteOptions::NO_NULL_TERMINATION);
#else
        int bytes = str->WriteUtf8(data, byteLength, &utfWritten, v8::String::WriteOptions::NO_NULL_TERMINATION);
#endif        
        if (utfWritten < strLength) {
            // we didn't allocate enough memory, need to expand
            delete[] data;
            byteLength = strLength * 3;
            data = new char[byteLength];
#if NODE_VERSION_AT_LEAST(11,0,0)
            bytes = str->WriteUtf8(Isolate::GetCurrent(), data, byteLength, &utfWritten, v8::String::WriteOptions::NO_NULL_TERMINATION);
#else
            bytes = str->WriteUtf8(data, byteLength, &utfWritten, v8::String::WriteOptions::NO_NULL_TERMINATION);
#endif        
        }
        val->iov_base = data;
        val->iov_len = bytes;
        //fprintf(stdout, "size of data with string %u header size %u\n", val->iov_len, headerSize);
    } else {
        Nan::ThrowError("Unknown value type");
    }
}

int putWithVersion(MDBX_txn *   txn,
        MDBX_dbi     dbi,
        MDBX_val *   key,
        MDBX_val *   data,
        unsigned int    flags, double version) {
    // leave 8 header bytes available for version and copy in with reserved memory
    char* sourceData = (char*) data->iov_base;
    int size = data->iov_len;
    data->iov_len = size + 8;
    int rc = mdbx_put(txn, dbi, key, data, MDBX_RESERVE);
    if (rc == 0) {
        // if put is successful, data->iov_base will point into the database where we copy the data to
        memcpy((char*) data->iov_base + 8, sourceData, size);
        *((double*) data->iov_base) = version;
    }
    data->iov_base = sourceData; // restore this so that if it points to data that needs to be freed, it points to the right place
    return rc;
}

void CustomExternalStringResource::writeTo(Local<String> str, MDBX_val *val) {
    unsigned int l = str->Length() + 1;
    uint16_t *d = new uint16_t[l];
    #if NODE_VERSION_AT_LEAST(12,0,0)
    str->Write(Isolate::GetCurrent(), d);
    #else
    str->Write(d);
    #endif
    d[l - 1] = 0;

    val->iov_base = d;
    val->iov_len = l * sizeof(uint16_t);
}

CustomExternalStringResource::CustomExternalStringResource(MDBX_val *val) {
    // The UTF-16 data
    this->d = (uint16_t*)(val->iov_base);
    // Number of UTF-16 characters in the string
    size_t n = val->iov_len / sizeof(uint16_t);
    // Silently generate a 0 length if length invalid
    this->l = n ? (n - 1) : 0;
}

CustomExternalStringResource::~CustomExternalStringResource() { }

void CustomExternalStringResource::Dispose() {
    // No need to do anything, the data is owned by LMDBX, not us
    
    // But actually need to delete the string resource itself:
    // the docs say that "The default implementation will use the delete operator."
    // while initially I thought this means using delete on the string,
    // apparently they meant just calling the destructor of this class.
    delete this;
}

const uint16_t *CustomExternalStringResource::data() const {
    return this->d;
}

size_t CustomExternalStringResource::length() const {
    return this->l;
}

CustomExternalOneByteStringResource::CustomExternalOneByteStringResource(MDBX_val *val) {
    // The Latin data
    this->d = (char*)(val->iov_base);
    // Number of Latin characters in the string
    this->l = val->iov_len;
}

CustomExternalOneByteStringResource::~CustomExternalOneByteStringResource() { }

void CustomExternalOneByteStringResource::Dispose() {
    // No need to do anything, the data is owned by LMDBX, not us
    
    // But actually need to delete the string resource itself:
    // the docs say that "The default implementation will use the delete operator."
    // while initially I thought this means using delete on the string,
    // apparently they meant just calling the destructor of this class.
    delete this;
}

const char *CustomExternalOneByteStringResource::data() const {
    return this->d;
}

size_t CustomExternalOneByteStringResource::length() const {
    return this->l;
}


#ifdef _WIN32

int pthread_mutex_init(pthread_mutex_t *mutex, pthread_mutexattr_t *attr)
{
    (void)attr;

    if (mutex == NULL)
        return 1;

    InitializeCriticalSection(mutex);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (mutex == NULL)
        return 1;
    DeleteCriticalSection(mutex);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (mutex == NULL)
        return 1;
    EnterCriticalSection(mutex);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (mutex == NULL)
        return 1;
    LeaveCriticalSection(mutex);
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, pthread_condattr_t *attr)
{
    (void)attr;
    if (cond == NULL)
        return 1;
    InitializeConditionVariable(cond);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    /* Windows does not have a destroy for conditionals */
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (cond == NULL || mutex == NULL)
        return 1;
    if (!SleepConditionVariableCS(cond, mutex, INFINITE))
        return 1;
    return 0;
}

int cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t ms)
{
    if (cond == NULL || mutex == NULL)
        return 1;
    if (!SleepConditionVariableCS(cond, mutex, ms))
        return 1;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    if (cond == NULL)
        return 1;
    WakeConditionVariable(cond);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (cond == NULL)
        return 1;
    WakeAllConditionVariable(cond);
    return 0;
}

#else
int cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t cms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = ts.tv_nsec + cms * 10000;
    ts.tv_sec += ns / 1000000000;
    ts.tv_nsec += ns % 1000000000;
    return pthread_cond_timedwait(cond, mutex, &ts);
}

#endif

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

