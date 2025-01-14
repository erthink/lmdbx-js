/* write instructions

0-3 flags
4-7 dbi
8-11 key-size
12 ... key followed by at least 2 32-bit zeros
4 value-size
8 bytes: value pointer (or value itself)
8 compressor pointer?
8 bytes (optional): conditional version
8 bytes (optional): version
inline value?
*/
#include "lmdbx-js.h"
#include <atomic>
// flags:
const uint32_t NO_INSTRUCTION_YET = 0;
const int PUT = 15;
const int DEL = 13;
const int DEL_VALUE = 14;
const int START_CONDITION_BLOCK = 4;
const int START_CONDITION_VALUE_BLOCK = 6;
const int START_BLOCK = 1;
const int BLOCK_END = 2;
const int POINTER_NEXT = 3;
const int USER_CALLBACK = 8;
const int USER_CALLBACK_STRICT_ORDER = 0x100000;
const int DROP_DB = 12;
const int HAS_KEY = 4;
const int HAS_VALUE = 2;
const int CONDITIONAL = 8;
const int CONDITIONAL_VERSION = 0x100;
const int SET_VERSION = 0x200;
const int HAS_INLINE_VALUE = 0x400;
const int COMPRESSIBLE = 0x100000;
const int DELETE_DATABASE = 0x400;
const int TXN_HAD_ERROR = 0x40000000;
const int TXN_DELIMITER = 0x8000000;
const int TXN_COMMITTED = 0x10000000;
const int TXN_FLUSHED = 0x20000000;
const int WAITING_OPERATION = 0x2000000;
const int IF_NO_EXISTS = MDBX_NOOVERWRITE; //0x10;
// result codes:
const int FAILED_CONDITION = 0x4000000;
const int FINISHED_OPERATION = 0x1000000;


WriteWorker::~WriteWorker() {
	// TODO: Make sure this runs on the JS main thread, or we need to move it
	if (envForTxn->writeWorker == this)
		envForTxn->writeWorker = nullptr;
}

WriteWorker::WriteWorker(MDBX_env* env, EnvWrap* envForTxn, uint32_t* instructions, Nan::Callback *callback)
		: Nan::AsyncProgressWorker(callback, "lmdb:write"),
		envForTxn(envForTxn),
		instructions(instructions),
		env(env) {
	//fprintf(stdout, "nextCompressibleArg %p\n", nextCompressibleArg);
		interruptionStatus = 0;
		txn = nullptr;
	}

void WriteWorker::Execute(const ExecutionProgress& executionProgress) {
	this->executionProgress = (ExecutionProgress*) &executionProgress;
	Write();
}
MDBX_txn* WriteWorker::AcquireTxn(int* flags) {
	bool commitSynchronously = *flags & TXN_SYNCHRONOUS_COMMIT;
	
	// TODO: if the conditionDepth is 0, we could allow the current worker's txn to be continued, committed and restarted
	pthread_mutex_lock(envForTxn->writingLock);
	if (commitSynchronously && interruptionStatus == ALLOW_COMMIT) {
		//fprintf(stderr, "acquire interupting lock %p %u\n", this, commitSynchronously);
		interruptionStatus = INTERRUPT_BATCH;
		pthread_cond_signal(envForTxn->writingCond);
		pthread_cond_wait(envForTxn->writingCond, envForTxn->writingLock);
        *flags |= TXN_FROM_WORKER;
		return nullptr;
	} else {
		//if (interruptionStatus == RESTART_WORKER_TXN)
		//	pthread_cond_wait(envForTxn->writingCond, envForTxn->writingLock);
		interruptionStatus = USER_HAS_LOCK;
		*flags |= TXN_FROM_WORKER;
		//fprintf(stderr, "acquire lock %p %u\n", txn, commitSynchronously);
		return txn;
	}
}

void WriteWorker::UnlockTxn() {
	//fprintf(stderr, "release txn %u\n", interruptionStatus);
	interruptionStatus = 0;
	pthread_cond_signal(envForTxn->writingCond);
	pthread_mutex_unlock(envForTxn->writingLock);
}
void WriteWorker::ReportError(const char* error) {
	SetErrorMessage(error);
}
int WriteWorker::WaitForCallbacks(MDBX_txn** txn, bool allowCommit, uint32_t* target) {
	int rc;
	//fprintf(stderr, "wait for callback %p\n", this);
	if (!finishedProgress)
		executionProgress->Send(nullptr, 0);
	pthread_cond_signal(envForTxn->writingCond);
	interruptionStatus = allowCommit ? ALLOW_COMMIT : 0;
	if (target) {
		uint64_t delay = 1;
		do {
			cond_timedwait(envForTxn->writingCond, envForTxn->writingLock, delay);
			delay = delay << 1ll;
			//if (delay > 5000)
			//	fprintf(stderr, "waited, %llu %p\n", delay, *target);
		} while(!(
			(*target & 0xf) ||
			(allowCommit && (interruptionStatus == INTERRUPT_BATCH || finishedProgress))));
	} else
		pthread_cond_wait(envForTxn->writingCond, envForTxn->writingLock);
	if (interruptionStatus == INTERRUPT_BATCH) { // interrupted by JS code that wants to run a synchronous transaction
	//	fprintf(stderr, "Performing batch interruption %u\n", allowCommit);
		interruptionStatus = RESTART_WORKER_TXN;
		rc = mdbx_txn_commit(*txn);
		if (rc == 0) {
			// wait again until the sync transaction is completed
			//fprintf(stderr, "Waiting after interruption\n");
			this->txn = *txn = nullptr;
			pthread_cond_signal(envForTxn->writingCond);
			pthread_cond_wait(envForTxn->writingCond, envForTxn->writingLock);
			// now restart our transaction
			rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, txn);
			this->txn = *txn;
			//fprintf(stderr, "Restarted txn after interruption\n");
			interruptionStatus = 0;
		}
		if (rc != 0) {
			fprintf(stdout, "wfc unlock due to error %u\n", rc);
			return rc;
		}
	} else
		interruptionStatus = 0;
	return 0;
}
int DoWrites(MDBX_txn* txn, EnvWrap* envForTxn, uint32_t* instruction, WriteWorker* worker) {
	MDBX_val key, value;
	int rc = 0;
	int conditionDepth = 0;
	int validatedDepth = 0;
	double conditionalVersion, setVersion = 0;
	bool overlappedWord = !!worker;
	uint32_t* start;
		do {
next_inst:	start = instruction++;
		uint32_t flags = *start;
		MDBX_dbi dbi = 0;
		bool validated = conditionDepth == validatedDepth;
		if (flags & 0xf0c0) {
			fprintf(stderr, "Unknown flag bits %u %p\n", flags, start);
			fprintf(stderr, "flags after message %u\n", *start);
			worker->ReportError("Unknown flags\n");
			return 0;
		}
		if (flags & HAS_KEY) {
			// a key based instruction, get the key
			dbi = (MDBX_dbi) *instruction++;
			key.iov_len = *instruction++;
			key.iov_base = instruction;
			instruction = (uint32_t*) (((size_t) instruction + key.iov_len + 16) & (~7));
			if (flags & HAS_VALUE) {
				if (flags & COMPRESSIBLE) {
					int64_t status = -1;
					if (*(instruction + 1) > 0x40000000) { // not compressed yet
						status = std::atomic_exchange((std::atomic<int64_t>*)(instruction + 2), (int64_t)1);
						if (status == 2) {
							//fprintf(stderr, "wait on compression %p\n", instruction);
							do {
								pthread_cond_wait(envForTxn->writingCond, envForTxn->writingLock);
							} while (*(instruction + 1) > 0x40000000);
						} else if (status > 2) {
							//fprintf(stderr, "doing the compression ourselves\n");
							((Compression*) (size_t) *((double*)&status))->compressInstruction(nullptr, (double*) (instruction + 2));
						} // else status is 0 and compression is done
					}
					// compressed
					value.iov_base = (void*)(size_t) * ((size_t*)instruction);
					if ((size_t)value.iov_base > 0x1000000000000)
						fprintf(stderr, "compression not completed %p %i\n", value.iov_base, (int) status);
					value.iov_len = *(instruction - 1);
					instruction += 4; // skip compression pointers
				} else {
					value.iov_base = (void*)(size_t) * ((double*)instruction);
					value.iov_len = *(instruction - 1);
					instruction += 2;
				}
			}
			if (flags & CONDITIONAL_VERSION) {
				conditionalVersion = *((double*) instruction);
				instruction += 2;
				rc = mdbx_get(txn, dbi, &key, &value);
				if (rc)
					validated = false;
				else
					validated = validated && conditionalVersion == *((double*)value.iov_base);
			}
			if (flags & SET_VERSION) {
				setVersion = *((double*) instruction);
				instruction += 2;
			}
			if ((flags & IF_NO_EXISTS) && (flags & START_CONDITION_BLOCK)) {
				rc = mdbx_get(txn, dbi, &key, &value);
				validated = validated && rc == MDBX_NOTFOUND;
			}
		} else
			instruction++;
		//fprintf(stderr, "instr flags %p %p %u\n", start, flags, conditionDepth);
		if (validated || !(flags & CONDITIONAL)) {
			switch (flags & 0xf) {
			case NO_INSTRUCTION_YET:
				instruction -= 2; // reset back to the previous flag as the current instruction
				rc = 0;
				//fprintf(stderr, "no instruction yet %p %u\n", start, conditionDepth);
				// in windows InterlockedCompareExchange might be faster
				if (!worker->finishedProgress || conditionDepth) {
					//fprintf(stderr, "write thread waiting %p\n", lastStart);
					if (std::atomic_compare_exchange_strong((std::atomic<uint32_t>*) start,
							(uint32_t*) &flags,
							(uint32_t)WAITING_OPERATION))
						worker->WaitForCallbacks(&txn, conditionDepth == 0, start);
					goto next_inst;
				} else {
					if (std::atomic_compare_exchange_strong((std::atomic<uint32_t>*) start,
							(uint32_t*) &flags,
							(uint32_t)TXN_DELIMITER)) {
						//fprintf(stderr, "t");//set txn_delimiter %p\n", start);
						worker->instructions = start;
						return 0;
					} else
						goto next_inst;						
				}
			case BLOCK_END:
				conditionDepth--;
				if (validatedDepth > conditionDepth)
					validatedDepth--;
				if (conditionDepth < 0) {
					fprintf(stderr, "Negative condition depth");
				}
				goto next_inst;
			case PUT:
				if (flags & SET_VERSION)
					rc = putWithVersion(txn, dbi, &key, &value, flags & (MDBX_NOOVERWRITE | MDBX_NODUPDATA | MDBX_APPEND | MDBX_APPENDDUP), setVersion);
				else
					rc = mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)(flags & (MDBX_NOOVERWRITE | MDBX_NODUPDATA | MDBX_APPEND | MDBX_APPENDDUP)));
				if (flags & COMPRESSIBLE)
					free(value.iov_base);
				//fprintf(stdout, "put %u \n", key.iov_len);
				break;
			case DEL:
				rc = mdbx_del(txn, dbi, &key, nullptr);
				break;
			case DEL_VALUE:
				rc = mdbx_del(txn, dbi, &key, &value);
				if (flags & COMPRESSIBLE)
					free(value.iov_base);
				break;
			case START_BLOCK: case START_CONDITION_BLOCK:
				rc = validated ? 0 : MDBX_NOTFOUND;
				if (validated)
					validatedDepth++;
				conditionDepth++;
				break;
			case USER_CALLBACK:
				worker->finishedProgress = false;
				worker->progressStatus = 2;
				rc = 0;
				if (flags & USER_CALLBACK_STRICT_ORDER) {
					//fprintf(stderr, "strict order\n");
					std::atomic_fetch_or((std::atomic<uint32_t>*) start, (uint32_t) FINISHED_OPERATION); // mark it as finished so it is processed
					while (!worker->finishedProgress) {
						worker->WaitForCallbacks(&txn, conditionDepth == 0, nullptr);
					}
				}
				break;
			case DROP_DB:
				rc = mdbx_drop(txn, dbi, (flags & DELETE_DATABASE) ? 1 : 0);
				break;
			case POINTER_NEXT:
				instruction = (uint32_t*)(size_t) * ((double*)instruction);
				goto next_inst;
			default:
				fprintf(stderr, "Unknown flags %u %p\n", flags, start);
				fprintf(stderr, "flags after message %u\n", *start);
				worker->ReportError("Unknown flags\n");
				return 22;
			}
			if (rc) {
				if (!(rc == MDBX_KEYEXIST || rc == MDBX_NOTFOUND)) {
					if (worker) {
						worker->ReportError(mdbx_strerror(rc));
					} else {
						return rc;
					}
					fprintf(stderr, "flags after return code %u\n", *start);
				}
				flags = FINISHED_OPERATION | FAILED_CONDITION;
			}
			else
				flags = FINISHED_OPERATION;
		} else
			flags = FINISHED_OPERATION | FAILED_CONDITION;
		//fprintf(stderr, "finished flag %p\n", flags);
		if (overlappedWord) {
			std::atomic_fetch_or((std::atomic<uint32_t>*) start, flags);
			overlappedWord = false;
		} else
			*start |= flags;
	} while(worker); // keep iterating in async/multiple-instruction mode, just one instruction in sync mode
	return rc;
}

void WriteWorker::Write() {
	int rc;
	finishedProgress = true;
	unsigned int envFlags;
	mdbx_env_get_flags(env, &envFlags);
	pthread_mutex_lock(envForTxn->writingLock);
	rc = mdbx_txn_begin(env, nullptr, /*(envFlags & MDBX_OVERLAPPINGSYNC) ? MDBX_NOSYNC : */MDBX_TXN_READWRITE, &txn);
	if (rc != 0) {
		return SetErrorMessage(mdbx_strerror(rc));
	}

	rc = DoWrites(txn, envForTxn, instructions, this);

	if (callback) {
		if (rc)
			mdbx_txn_abort(txn);
		else
			rc = mdbx_txn_commit(txn);
		txn = nullptr;
		pthread_mutex_unlock(envForTxn->writingLock);
		if (rc) {
			std::atomic_fetch_or((std::atomic<uint32_t>*) instructions, (uint32_t) TXN_HAD_ERROR);
			return SetErrorMessage(mdbx_strerror(rc));
		}/* else if (envFlags & MDBX_OVERLAPPINGSYNC) {
			// note that once we set the instructions byte to committed, we can *not* touch it again
			// because JS can then GC and deallocate the buffer it references and it can segfault if we access again
			if (envForTxn->jsFlags & SEPARATE_FLUSHED)
				std::atomic_fetch_or((std::atomic<uint32_t>*) instructions, (uint32_t) TXN_COMMITTED);
			progressStatus = 1;
			executionProgress->Send(nullptr, 0);
			rc = mdbx_env_sync(env, true);
			if (!(envForTxn->jsFlags & SEPARATE_FLUSHED))
				std::atomic_fetch_or((std::atomic<uint32_t>*) instructions, (uint32_t) TXN_COMMITTED);
		} */else {
			std::atomic_fetch_or((std::atomic<uint32_t>*) instructions, (uint32_t) TXN_COMMITTED);
		}
	} else
		interruptionStatus = rc;
}

void WriteWorker::HandleProgressCallback(const char* data, size_t count) {
	Nan::HandleScope scope;
	v8::Local<v8::Value> argv[] = {
		Nan::New<Number>(progressStatus)
	};
	if (progressStatus == 1) {
		callback->Call(1, argv, async_resource).ToLocalChecked()->IsTrue();
		return;
	}
	if (finishedProgress)
		return;
	pthread_mutex_lock(envForTxn->writingLock);
	while(!txn) // possible to jump in after an interrupted txn here
		pthread_cond_wait(envForTxn->writingCond, envForTxn->writingLock);
	envForTxn->writeTxn = new TxnTracked(txn, 0);
	finishedProgress = true;
	callback->Call(1, argv, async_resource).ToLocalChecked()->IsTrue();
	delete envForTxn->writeTxn;
	envForTxn->writeTxn = nullptr;
	pthread_cond_signal(envForTxn->writingCond);
	pthread_mutex_unlock(envForTxn->writingLock);
}

void WriteWorker::HandleOKCallback() {
	Nan::HandleScope scope;
	Local<v8::Value> argv[] = {
		Nan::New<Number>(0)
	};
	finishedProgress = true;
	callback->Call(1, argv, async_resource);
}

NAN_METHOD(EnvWrap::startWriting) {
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    if (!ew->env) {
        return Nan::ThrowError("The environment is already closed.");
    }
    size_t instructionAddress = Local<Number>::Cast(info[0])->Value();
    Nan::Callback* callback = new Nan::Callback(Local<v8::Function>::Cast(info[1]));

    WriteWorker* worker = new WriteWorker(ew->env, ew, (uint32_t*) instructionAddress, callback);
	ew->writeWorker = worker;
    Nan::AsyncQueueWorker(worker);
}


#ifdef ENABLE_FAST_API
void EnvWrap::writeFast(Local<Object> receiver_obj, uint64_t instructionAddress, FastApiCallbackOptions& options) {
	EnvWrap* ew = static_cast<EnvWrap*>(
		receiver_obj->GetAlignedPointerFromInternalField(0));
	int rc;
	if (instructionAddress)
		rc = DoWrites(ew->writeTxn->txn, ew, (uint32_t*)instructionAddress, nullptr);
	else {
		pthread_cond_signal(ew->writingCond);
		rc = 0;
	}
	if (rc && !(rc == MDBX_KEYEXIST || rc == MDBX_NOTFOUND))
		options.fallback = true;
}
#endif
void EnvWrap::write(
	const v8::FunctionCallbackInfo<v8::Value>& info) {
	v8::Local<v8::Object> instance =
		v8::Local<v8::Object>::Cast(info.Holder());
	//fprintf(stderr,"Doing sync write\n");
	EnvWrap* ew = Nan::ObjectWrap::Unwrap<EnvWrap>(instance);
	if (!ew->env) {
		return Nan::ThrowError("The environment is already closed.");
	}
	size_t instructionAddress = Local<Number>::Cast(info[0])->Value();
	int rc = 0;
	if (instructionAddress)
		rc = DoWrites(ew->writeTxn->txn, ew, (uint32_t*)instructionAddress, nullptr);
	else if (ew->writeWorker) {
		pthread_cond_signal(ew->writingCond);
	}
	if (rc && !(rc == MDBX_KEYEXIST || rc == MDBX_NOTFOUND))
		return Nan::ThrowError(mdbx_strerror(rc));
}
