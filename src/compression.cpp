#include "lz4.h"
#include "lmdbx-js.h"
#include <atomic>

using namespace v8;
using namespace node;

thread_local LZ4_stream_t* Compression::stream = nullptr;
Compression::Compression() {
}
Compression::~Compression() {
    delete dictionary;
}
NAN_METHOD(Compression::ctor) {
    unsigned int compressionThreshold = 1000;
    char* dictionary = nullptr;
    unsigned int dictSize = 0;
    if (info[0]->IsObject()) {
        Local<Value> dictionaryOption = Nan::To<v8::Object>(info[0]).ToLocalChecked()->Get(Nan::GetCurrentContext(), Nan::New<String>("dictionary").ToLocalChecked()).ToLocalChecked();
        if (!dictionaryOption->IsUndefined()) {
            if (!node::Buffer::HasInstance(dictionaryOption)) {
                return Nan::ThrowError("Dictionary must be a buffer");
            }
            dictSize = node::Buffer::Length(dictionaryOption);
            dictSize = (dictSize >> 3) << 3; // make sure it is word-aligned
            dictionary = node::Buffer::Data(dictionaryOption);

        }
        Local<Value> thresholdOption = Nan::To<v8::Object>(info[0]).ToLocalChecked()->Get(Nan::GetCurrentContext(), Nan::New<String>("threshold").ToLocalChecked()).ToLocalChecked();
        if (thresholdOption->IsNumber()) {
            compressionThreshold = thresholdOption->IntegerValue(Nan::GetCurrentContext()).FromJust();
        }
    }
    Compression* compression = new Compression();
    compression->dictionary = dictionary;
    compression->decompressTarget = dictionary + dictSize;
    compression->decompressSize = 0;
    compression->acceleration = 1;
    compression->compressionThreshold = compressionThreshold;
    compression->Wrap(info.This());
    compression->Ref();
    info.This()->Set(Nan::GetCurrentContext(), Nan::New<String>("address").ToLocalChecked(), Nan::New<Number>((double) (size_t) compression));

    return info.GetReturnValue().Set(info.This());
}

NAN_METHOD(Compression::setBuffer) {
    Compression *compression = Nan::ObjectWrap::Unwrap<Compression>(info.This());
    unsigned int dictSize = Local<Number>::Cast(info[1])->IntegerValue(Nan::GetCurrentContext()).FromJust();
    compression->dictionary = node::Buffer::Data(info[0]);
    compression->decompressTarget = compression->dictionary + dictSize;
    compression->decompressSize = node::Buffer::Length(info[0]) - dictSize;
}

void Compression::decompress(MDBX_val& data, bool &isValid, bool canAllocate) {
    uint32_t uncompressedLength;
    int compressionHeaderSize;
    uint32_t compressedLength = data.iov_len;
    unsigned char* charData = (unsigned char*) data.iov_base;

    if (charData[0] == 254) {
        uncompressedLength = ((uint32_t)charData[1] << 16) | ((uint32_t)charData[2] << 8) | (uint32_t)charData[3];
        compressionHeaderSize = 4;
    }
    else if (charData[0] == 255) {
        uncompressedLength = ((uint32_t)charData[4] << 24) | ((uint32_t)charData[5] << 16) | ((uint32_t)charData[6] << 8) | (uint32_t)charData[7];
        compressionHeaderSize = 8;
    }
    else {
        fprintf(stderr, "Unknown status byte %u\n", charData[0]);
        if (canAllocate)
            Nan::ThrowError("Unknown status byte");
        isValid = false;
        return;
    }
    data.iov_base = decompressTarget;
    data.iov_len = uncompressedLength;
    //TODO: For larger blocks with known encoding, it might make sense to allocate space for it and use an ExternalString
    //fprintf(stdout, "compressed size %u uncompressedLength %u, first byte %u\n", data.iov_len, uncompressedLength, charData[compressionHeaderSize]);
    if (uncompressedLength > decompressSize) {
        isValid = false;
        return;
    }
    int written = LZ4_decompress_safe_usingDict(
        (char*)charData + compressionHeaderSize, decompressTarget,
        compressedLength - compressionHeaderSize, uncompressedLength,
        dictionary, decompressTarget - dictionary);
    //fprintf(stdout, "first uncompressed byte %X %X %X %X %X %X\n", uncompressedData[0], uncompressedData[1], uncompressedData[2], uncompressedData[3], uncompressedData[4], uncompressedData[5]);
    if (written < 0) {
        //fprintf(stderr, "Failed to decompress data %u %u %u %u\n", charData[0], data.iov_len, compressionHeaderSize, uncompressedLength);
        if (canAllocate)
            Nan::ThrowError("Failed to decompress data");
        isValid = false;
        return;
    }
    isValid = true;
}

int Compression::compressInstruction(EnvWrap* env, double* compressionAddress) {
    MDBX_val value;
    value.iov_base = (void*)((size_t) * (compressionAddress - 1));
    value.iov_len = *(((uint32_t*)compressionAddress) - 3);
    argtokey_callback_t compressedData = compress(&value, nullptr);
    if (compressedData) {
        *(((uint32_t*)compressionAddress) - 3) = value.iov_len;
        *((size_t*)(compressionAddress - 1)) = (size_t)value.iov_base;
        int64_t status = std::atomic_exchange((std::atomic<int64_t>*) compressionAddress, (int64_t) 0);
        if (status == 1 && env) {
            pthread_mutex_lock(env->writingLock);
            pthread_cond_signal(env->writingCond);
            pthread_mutex_unlock(env->writingLock);
            //fprintf(stderr, "sent compression completion signal\n");
        }
        //fprintf(stdout, "compressed to %p %u %u %p\n", value.iov_base, value.iov_len, status, env);
        return 0;
    } else {
        fprintf(stdout, "failed to compress\n");
        return 1;
    }
}

argtokey_callback_t Compression::compress(MDBX_val* value, argtokey_callback_t freeValue) {
    size_t dataLength = value->iov_len;
    char* data = (char*)value->iov_base;
    if (value->iov_len < compressionThreshold && !(value->iov_len > 0 && ((uint8_t*)data)[0] >= 250))
        return freeValue; // don't compress if less than threshold (but we must compress if the first byte is the compression indicator)
    bool longSize = dataLength >= 0x1000000;
    int prefixSize = (longSize ? 8 : 4);
    int maxCompressedSize = LZ4_COMPRESSBOUND(dataLength);
    char* compressed = new char[maxCompressedSize + prefixSize];
    //fprintf(stdout, "compressing %u\n", dataLength);
    if (!stream)
        stream = LZ4_createStream();
    LZ4_loadDict(stream, dictionary, decompressTarget - dictionary);
    int compressedSize = LZ4_compress_fast_continue(stream, data, compressed + prefixSize, dataLength, maxCompressedSize, acceleration);
    if (compressedSize > 0) {
        if (freeValue)
            freeValue(*value);
        uint8_t* compressedData = (uint8_t*)compressed;
        if (longSize) {
            compressedData[0] = 255;
            compressedData[2] = (uint8_t)(dataLength >> 40u);
            compressedData[3] = (uint8_t)(dataLength >> 32u);
            compressedData[4] = (uint8_t)(dataLength >> 24u);
            compressedData[5] = (uint8_t)(dataLength >> 16u);
            compressedData[6] = (uint8_t)(dataLength >> 8u);
            compressedData[7] = (uint8_t)dataLength;
        }
        else {
            compressedData[0] = 254;
            compressedData[1] = (uint8_t)(dataLength >> 16u);
            compressedData[2] = (uint8_t)(dataLength >> 8u);
            compressedData[3] = (uint8_t)dataLength;
        }
        value->iov_base = compressed;
        value->iov_len = compressedSize + prefixSize;
        return ([](MDBX_val &value) -> void {
            delete[] (char*)value.iov_base;
        });
    }
    else {
        delete[] compressed;
        return nullptr;
    }
}

class CompressionWorker : public Nan::AsyncWorker {
  public:
    CompressionWorker(EnvWrap* env, double* compressionAddress, Nan::Callback *callback)
      : Nan::AsyncWorker(callback), env(env), compressionAddress(compressionAddress) {}


    void Execute() {
        uint64_t compressionPointer;
        compressionPointer = std::atomic_exchange((std::atomic<int64_t>*) compressionAddress, (int64_t) 2);
        if (compressionPointer > 1) {
            Compression* compression = (Compression*)(size_t) * ((double*)&compressionPointer);
            compression->compressInstruction(env, compressionAddress);
        }
    }
    void HandleOKCallback() {
        // don't actually call the callback, no need
    }

  private:
    EnvWrap* env;
    double* compressionAddress;
};

NAN_METHOD(EnvWrap::compress) {
    EnvWrap *env = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    size_t compressionAddress = Local<Number>::Cast(info[0])->Value();
    Nan::Callback* callback = new Nan::Callback(Local<v8::Function>::Cast(info[1]));
    CompressionWorker* worker = new CompressionWorker(env, (double*) compressionAddress, callback);
    Nan::AsyncQueueWorker(worker);
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

