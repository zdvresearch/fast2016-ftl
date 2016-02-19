#ifndef GARBAGE_COLLECTOR_H
#define GARBAGE_COLLECTOR_H
#include "jasmine.h"
#include "ftl_metadata.h"


extern UINT32 nValidChunksFromHeap[NUM_BANKS];

void finishGC();

void progressiveMerge(const UINT32 bank, const UINT32 maxFlashOps);
void garbageCollectLog(const UINT32 bank);
void backgroundCleaning(const UINT32 bank);
//#define backgroundCleaning(X)
#endif
