#ifndef __FSOPENNODE_H__
#define __FSOPENNODE_H__

#include "platform.h"
#include "kthread.h"

class FsOpenNode {
public:
    FsOpenNode(BoxedPtr<FsNode> node, U32 flags);

    U32 read(U32 address, U32 len); // will call into readNative
    U32 write(U32 address, U32 len); // will call into writeNative

    U32 getDirectoryEntryCount();
    BoxedPtr<FsNode> getDirectoryEntry(U32 index, BString& name);

    virtual ~FsOpenNode();

    virtual S64  length()=0;
    virtual bool setLength(S64 length)=0;
    virtual S64  getFilePointer()=0;
    virtual S64  seek(S64 pos)=0;	    
    virtual U32  map(U32 address, U32 len, S32 prot, S32 flags, U64 off)=0;
    virtual bool canMap()=0;
    virtual U32  ioctl(U32 request)=0;	
    virtual void setAsync(bool isAsync)=0;
    virtual bool isAsync()=0;
    virtual void waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events)=0;
    virtual bool isWriteReady()=0;
    virtual bool isReadReady()=0;    
    virtual U32 readNative(U8* buffer, U32 len)=0;
    virtual U32 writeNative(U8* buffer, U32 len)=0;
    virtual void close()=0;
    virtual void reopen()=0;
    virtual bool isOpen()=0;

    BoxedPtr<FsNode> const node;
    const U32 flags;     
    BString openedPath; // when call fchdir, we should set the current directory to what was passed in, not what was linked to

private:
    std::vector<BoxedPtr<FsNode> > dirEntries;
    void loadDirEntries();

    friend FsNode;
    KListNode<FsOpenNode*> listNode;
};

#endif
