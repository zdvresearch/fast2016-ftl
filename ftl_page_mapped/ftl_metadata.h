#ifndef FTL_METADATA_H
#define FTL_METADATA_H
#include "jasmine.h"
#include "ftl_parameters.h"

//----------------------------------
// macro
//----------------------------------
#define SW_LOG_LBN          0
#define MISCBLK_VBN         0x1    // vblock #1 <- misc metadata
#define META_BLKS_PER_BANK  (1 + 1 + MAP_BLK_PER_BANK)    // include block #0, misc, map block

typedef struct LogCtrlBlock
{
    UINT32 logLpn;
    UINT32 lpnsListAddr;
    UINT32 logBufferAddr;
    UINT32 chunkPtr;
    UINT32 dataLpn[CHUNKS_PER_PAGE];
    UINT32 chunkIdx[CHUNKS_PER_PAGE];
    void (*increaseLpn)(const UINT32 bank, struct LogCtrlBlock * ctrlBlock);
    void (*updateChunkPtr)();
    UINT32 nextLowPageOffset;
    UINT8 allChunksInLogAreValid;
    UINT8 useRecycledPage;
    UINT8 precacheDone;
} LogCtrlBlock;


typedef struct heapData
{
    //UINT32 validChunksHeapPositions[NUM_BANKS][LOG_BLK_PER_BANK];
    UINT32 positionsPtr;
    UINT32 nElInHeap[NUM_BANKS];
    UINT32 dramStartAddr;
    UINT32 logBlksPerBank;
    UINT32 firstLbn;
} heapData;

typedef struct listData
{
    logListNode* cleanListHead[NUM_BANKS];
    logListNode* cleanListTail[NUM_BANKS];
    logListNode* cleanListUnusedNodes[NUM_BANKS];
    UINT32 size[NUM_BANKS];
} listData;

typedef struct logBufMetaT
{
    UINT32 dataLpn[CHUNKS_PER_PAGE];
    UINT32 chunkIdx[CHUNKS_PER_PAGE];
} logBufMetaT;

typedef struct cleanQueueElementT
{
    UINT32 dataLpn;
    struct cleanQueueElementT* next;
} cleanQueueElementT;
//----------------------------------
// FTL metadata (maintain in SRAM)
//----------------------------------
extern UINT32 g_bsp_isr_flag[NUM_BANKS];
extern UINT8 g_mem_to_clr[PAGES_PER_BLK / 8];
extern UINT8 g_mem_to_set[PAGES_PER_BLK / 8];
extern UINT32 g_ftl_read_buf_id;
extern UINT32 g_ftl_write_buf_id;
//extern logBufMetaT logBufMeta[NUM_BANKS];
//extern UINT32 chunkPtr[NUM_BANKS];
//
extern heapData heapDataFirstUsage;
extern heapData heapDataSecondUsage;
extern heapData heapDataCold;

extern listData cleanListDataWrite;
extern UINT32 userSecWrites;
extern UINT32 totSecWrites;
extern LogCtrlBlock coldLogCtrl[NUM_BANKS];
extern LogCtrlBlock hotLogCtrl[NUM_BANKS];
extern UINT32 free_list_head[NUM_BANKS];
extern UINT32 free_list_tail[NUM_BANKS];

extern UINT32 hotFirstAccumulated[NUM_BANKS];
extern UINT32 cleanBlksAfterGcHot;
extern UINT32 secondHotFactorNum;
extern UINT32 secondHotFactorDen;
extern UINT32 lbaHotThreshold;
extern UINT32 nSectsHotThreshold;
extern UINT32 CleanBlksBackgroundGcThreshold;
extern UINT32 nValidChunksInPageToReuseThreshold;
#if WOMCanFail
extern float successRateWOM;
#endif
extern UINT32 adaptiveWindowSize;
extern UINT32 adaptiveWindow[NUM_BANKS][5];
extern UINT32 adaptiveStepUp[NUM_BANKS];
extern UINT32 adaptiveStepDown[NUM_BANKS];
extern UINT32 nStepUps[NUM_BANKS];
extern UINT32 nStepDowns[NUM_BANKS];
extern UINT32 maxStepUps;
extern UINT32 maxStepDowns;
extern UINT32 initStepUp;
extern UINT32 initStepDown;


#define GcIdle  0
#define GcRead  1
#define GcWrite 2

extern BOOL8 gcState[NUM_BANKS];
extern UINT32 victimLbn[NUM_BANKS];

//extern UINT8 hotFlag[NUM_BANKS][LOG_BLK_PER_BANK]; // 0: cold block, 1: hot block

#define SizeSRAMMetadata    ((sizeof(UINT32) * NUM_BANKS)            + \
                             (sizeof(UINT8) * (PAGES_PER_BLK/8))    + \
                             (sizeof(UINT8) * (PAGES_PER_BLK/8))    + \
                             (sizeof(UINT32))                       + \
                             (sizeof(UINT32))                       + \
                             (sizeof(logBufMetaT) * NUM_BANKS)      + \
                             (sizeof(UINT32) * NUM_BANKS)           + \
                             (sizeof(heapData))                     + \
                             (sizeof(listData))                     + \
                             (sizeof(UINT32))                       + \
                             (sizeof(UINT32))                       + \
                             (sizeof(LogCtrlBlock) * NUM_BANKS)     + \
                             (sizeof(LogCtrlBlock) * NUM_BANKS)     + \
                             (sizeof(UINT32) * NUM_BANKS)           + \
                             (sizeof(UINT32) * NUM_BANKS)           + \
                             (sizeof(UINT8) * NUM_BANKS)            \
                            )
//----------------------------------
// General Purpose Macros
//----------------------------------
#define DataPageToDataBlk(dataLpn)      (((dataLpn) / NUM_BANKS) / PAGES_PER_BLK)
#define DataPageToOffset(dataLpn)       (((dataLpn) / NUM_BANKS) % PAGES_PER_BLK)
#define PageToBank(lpn)                 ((lpn) % NUM_BANKS)
#define VPageToOffset(vpn)              ((vpn) % PAGES_PER_BLK)
#define VPageToVBlk(vpn)                ((vpn) / PAGES_PER_BLK)
#define LogPageToOffset(logLpn)         ((logLpn) % PAGES_PER_BLK)
#define LogPageToLogBlk(logLpn)         ((logLpn) / PAGES_PER_BLK)

#endif
