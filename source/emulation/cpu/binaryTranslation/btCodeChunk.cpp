#include "boxedwine.h"
#include "btCodeChunk.h"
#include "btCpu.h"

#ifdef BOXEDWINE_BINARY_TRANSLATOR

BtCodeChunk::BtCodeChunk(U32 instructionCount, U32* eipInstructionAddress, U32* hostInstructionIndex, U8* hostInstructionBuffer, U32 hostInstructionBufferLen, U32 eip, U32 eipLen, bool dynamic) {
    CPU* cpu = KThread::currentThread()->cpu;
    this->instructionCount = instructionCount;
    this->emulatedAddress = eip + cpu->seg[CS].address;
    this->emulatedLen = eipLen;
    this->hostAddress = cpu->thread->memory->allocateExcutableMemory(hostInstructionBufferLen + 4, &this->hostAddressSize); // +4 for a guard
    this->hostLen = hostInstructionBufferLen;
    this->emulatedInstructionLen = new U8[instructionCount];
    this->hostInstructionLen = new U32[instructionCount];
    this->dynamic = dynamic;

    Platform::writeCodeToMemory(this->hostAddress, this->hostAddressSize, [this]() {
        memset(this->hostAddress, 0xce, this->hostAddressSize);
        });
    if (instructionCount) {
        for (U32 i = 0; i < instructionCount; i++) {
            if (i == instructionCount - 1) {
                this->emulatedInstructionLen[i] = eipLen - (eipInstructionAddress[i] - this->emulatedAddress);
                this->hostInstructionLen[i] = hostInstructionBufferLen - hostInstructionIndex[i];
            } else {
                this->emulatedInstructionLen[i] = eipInstructionAddress[i + 1] - eipInstructionAddress[i];
                this->hostInstructionLen[i] = hostInstructionIndex[i + 1] - hostInstructionIndex[i];
            }
            if (this->emulatedInstructionLen[i] > K_MAX_X86_OP_LEN) {
                kpanic("BtCodeChunk::allocChunk emulatedInstructionLen sanity check failed");
            }
        }
    }
    Platform::writeCodeToMemory(this->hostAddress, this->hostAddressSize, [=]() {
        memcpy(this->hostAddress, hostInstructionBuffer, hostInstructionBufferLen);
        });
}

void BtCodeChunk::makeLive() {
    CPU* cpu = KThread::currentThread()->cpu;
    U32 eip = this->emulatedAddress;
    U8* host = (U8*)this->hostAddress;

    for (U32 i = 0; i < instructionCount; i++) {
        if (KSystem::useLargeAddressSpace) {
            cpu->thread->memory->setEipForHostMapping(eip, host);
        } else {
            U32 page = eip >> K_PAGE_SHIFT;
            U32 offset = eip & K_PAGE_MASK;
            void** table = cpu->thread->memory->eipToHostInstructionPages[page];
            if (!table) {
                table = new void* [K_PAGE_SIZE];
                memset(table, 0, sizeof(void*) * K_PAGE_SIZE);
                cpu->thread->memory->eipToHostInstructionPages[page] = table;
            }
            if (table[offset]) {
                kpanic("BtCodeChunk::allocChunk eip already mapped");
            }
            table[offset] = host;
        }
        eip += this->emulatedInstructionLen[i];
        host += this->hostInstructionLen[i];
    }
    cpu->thread->memory->addCodeChunk(shared_from_this());
    this->clearInstructionCache((U8*)this->hostAddress, this->hostLen);
}

void BtCodeChunk::detachFromHost(Memory* memory) {
    U32 eip = this->emulatedAddress;
    KThread* thread = KThread::currentThread();
    std::shared_ptr<KProcess> process;

    if (thread) {
        process = thread->process;
    }

    for (U32 i = 0; i < this->instructionCount; i++) {
        if (KSystem::useLargeAddressSpace) {
            memory->setEipForHostMapping(eip, process ? process->reTranslateChunkAddressFromReg : NULL);
        } else {
            if (memory->eipToHostInstructionPages[eip >> K_PAGE_SHIFT]) { // might span multiple pages and the other pages are already deleted
                memory->eipToHostInstructionPages[eip >> K_PAGE_SHIFT][eip & K_PAGE_MASK] = NULL;
            }
        }
        eip += this->emulatedInstructionLen[i];
    }
    memory->removeCodeChunk(shared_from_this());
}

void BtCodeChunk::release(Memory* memory) {
    this->detachFromHost(memory);
    this->internalDealloc();
}

void BtCodeChunk::internalDealloc() {
    KThread::currentThread()->memory->freeExcutableMemory(this->hostAddress, this->hostAddressSize);
    this->hostAddress = NULL;
    delete[] this->emulatedInstructionLen;
    this->emulatedInstructionLen = NULL;
    delete[] this->hostInstructionLen;
    this->hostInstructionLen = NULL;
}

U32 BtCodeChunk::getEipThatContainsHostAddress(void* address, void** startOfHostInstruction, U32* index) {
    if (this->containsHostAddress(address)) {
        U8* p = (U8*)this->hostAddress;
        U32 result = this->emulatedAddress;

        for (unsigned int i = 0; i < this->instructionCount; i++) {
            U32 len = this->hostInstructionLen[i];
            if (address >= p && address < p + len) {
                if (startOfHostInstruction) {
                    *startOfHostInstruction = p;
                }
                if (index) {
                    *index = i;
                }
                return result;
            }
            p += len;
            result += this->emulatedInstructionLen[i];
        }
    }
    return 0;
}

U32 BtCodeChunk::getStartOfInstructionByEip(U32 eip, U8** host, U32* index) {
    if (this->containsEip(eip)) {
        U32 result = this->emulatedAddress;
        U8* hostResult = (U8*)this->hostAddress;

        for (unsigned int i = 0; i < this->instructionCount; i++) {
            U32 len = this->emulatedInstructionLen[i];
            if (eip >= result && eip < result + len) {
                if (index) {
                    *index = i;
                }
                if (host) {
                    *host = hostResult;
                }
                return result;
            }
            result += len;
            hostResult += this->hostInstructionLen[i];
        }
    }
    return 0;
}

std::shared_ptr<BtCodeChunkLink> BtCodeChunk::addLinkFrom(std::shared_ptr<BtCodeChunk>& from, U32 toEip, void* toHostInstruction, void* fromHostOffset, bool direct) {
    if (from == shared_from_this()) {
        kpanic("BtCodeChunk::addLinkFrom can not link to itself");
    }
    std::shared_ptr<BtCodeChunkLink> link = std::make_shared<BtCodeChunkLink>(fromHostOffset, toEip, toHostInstruction, direct);
    from->linksTo.push_back(link);
    this->linksFrom.push_back(link);
    return link;
}

void BtCodeChunk::releaseAndRetranslate() {
    // remove this chunk and its mappings from being used (since it is about to be replaced)
    BtCPU* cpu = (BtCPU*)KThread::currentThread()->cpu;
    detachFromHost(cpu->thread->memory);

    std::shared_ptr<BtCodeChunk> chunk = cpu->translateChunk(this->emulatedAddress - cpu->seg[CS].address);
    cpu->makePendingCodePagesReadOnly();
    for (auto& link : this->linksFrom) {
        U64 destHost = (U64)chunk->getHostFromEip(link->toEip);

        if (destHost) {
            chunk->linksFrom.push_back(link);
            if (link->direct) {
                U32 fromInstructionIndex;
                std::shared_ptr<BtCodeChunk> fromChunk = cpu->thread->memory->getCodeChunkContainingHostAddress(link->fromHostOffset);
                void* srcHostInstruction = NULL;
                fromChunk->getEipThatContainsHostAddress(link->fromHostOffset, &srcHostInstruction, &fromInstructionIndex);
                U64 srcHost = (U64)srcHostInstruction;
                U64 endOfJump = (U64)link->fromHostOffset - srcHost + 4;
                *((U32*)link->fromHostOffset) = (U32)(destHost - srcHost - endOfJump);
            } else {
                ATOMIC_WRITE64((U64*)&link->toHostInstruction, destHost);
            }
        }
    };
    chunk->makeLive();

    this->internalDealloc(); // don't call dealloc() because the new chunk occupies the memory cache and we don't want to mess with it
}

void BtCodeChunk::clearInstructionCache(U8* hostAddress, U32 len) {
    // x86 doesn't need to do anything
}

void BtCodeChunk::invalidateStartingAt(U32 eipAddress) {
    U32 eipIndex = 0;
    U8* host = NULL;
    BtCPU* cpu = (BtCPU*)KThread::currentThread()->cpu;
    U32 currentEip = (cpu->isBig() ? cpu->eip.u32 : cpu->eip.u16) + KThread::currentThread()->cpu->seg[CS].address;
    U32 eip = this->getStartOfInstructionByEip(eipAddress, &host, &eipIndex);
    // make sure we won't invalidate the current instruction, *2 just to be sure 
    // getStartOfInstructionByEip doesn't roll back to the current instruction
    if (currentEip >= eip && currentEip < this->emulatedAddress + this->emulatedLen) {
        eip = this->getStartOfInstructionByEip(currentEip, &host, &eipIndex);
        if (eipIndex == this->instructionCount - 1) {
            // :TODO: maybe clear the instructions before?
            return; // the current instruction is the last
        }
        eip = this->getStartOfInstructionByEip(eip + this->emulatedInstructionLen[eipIndex], &host, &eipIndex);
    }
    U32 remainingLen = this->hostLen - (U32)(host - (U8*)this->hostAddress);
    Platform::writeCodeToMemory(host, remainingLen, [host, remainingLen] {
        memset(host, 0xce, remainingLen);
        });
    this->clearInstructionCache(host, remainingLen);
}

bool BtCodeChunk::containsEip(U32 eip, U32 len) {
    // do we begin in this chunk?
    if (this->containsEip(eip)) {
        return true;
    }
    // do we end in this chunk?
    if (this->containsEip(eip + len - 1)) {
        return true;
    }
    // we we span this chunk
    if (eip < this->emulatedAddress && eip + len>this->emulatedAddress + this->emulatedLen) {
        return true;
    }
    return false;
}

#endif
