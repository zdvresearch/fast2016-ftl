#include "jasmine.h"
#include "ftl_metadata.h"
#include "ftl_parameters.h"
#include "log.h"
#include "garbage_collection.h"
#include "heap.h" // decrementValidChunks
#include "flash.h" // RETURN_ON_ISSUE RETURN_WHEN_DONE
#include "read.h" // rebuildPageToFtlBuf
#include "write.h"
#include "cleanList.h" // cleanListSize

#if WOMCanFail
#include "stdlib.h"
#endif

static void flushLogBuffer();
static void flushLogBufferRecycledPage();

static void initWrite(LogCtrlBlock * ctrlBlock, const UINT32 dataLpn, const UINT32 sectOffset, const UINT32 nSects);
//static void manageOldCompletePage();
//static void manageOldChunkForCompletePageWrite(int chunkIdx);
static void manageOldChunkForCompletePageWrite(const UINT32 oldChunkAddr);
static void updateDramBufMetadata();

static void flushLogBufferDuringGC(const UINT32 bank);
static void updateChunkPtrDuringGC(const UINT32 bank);
static void updateDramBufMetadataDuringGc(const UINT32 bank, const UINT32 lpn, const UINT32 sectOffset);

static void syncWithWriteLimit();

static void writeCompletePage();
static void writeChunkNew(UINT32 nSectsToWrite);
static void writePartialPageOld();
static void writeChunkOld();
#if OPTION_NO_DRAM_ABSORB == 0
static void writePartialChunkWhenOldIsInDRAMBuf(UINT32 nSectsToWrite, UINT32 oldSectOffset, UINT32 DRAMBufStart);
#endif
static void writePartialChunkWhenOldChunkIsInFlashLog(UINT32 nSectsToWrite, UINT32 oldChunkAddr);
static void writePartialChunkWhenOldChunkIsInFlashLogEncoded(UINT32 nSectsToWrite, UINT32 oldChunkAddr);

#if WOMCanFail
static void copyReuseBufToColdBuf();
#endif

// Data members
static UINT32 bank_;
static LogCtrlBlock * ctrlBlock_;
static UINT32 lpn_;
static UINT32 sectOffset_;
static UINT32 nSects_;
static UINT32 remainingSects_;

static void flushLogBuffer()
{

#if PrintStats
    uart_print_level_1("+\r\n");
#endif

    UINT32 newLogLpn = ctrlBlock_[bank_].logLpn;

    uart_print(" flushLogBuffer in bank "); uart_print_int(bank_);
    uart_print(": logLpn="); uart_print_int(newLogLpn); uart_print("\r\n");

    UINT32 vBlk = get_log_vbn(bank_, LogPageToLogBlk(newLogLpn));
    UINT32 pageOffset = LogPageToOffset(newLogLpn);
    UINT32 chunksToFlush=CHUNKS_PER_PAGE;
    UINT32 lChunkAddr = (bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (newLogLpn * CHUNKS_PER_PAGE);

    nand_page_program(bank_, vBlk, pageOffset, ctrlBlock_[bank_].logBufferAddr, RETURN_ON_ISSUE);

    if( __builtin_expect(ctrlBlock_[bank_].allChunksInLogAreValid, TRUE))
    {
        mem_copy(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), 0), ctrlBlock_[bank_].dataLpn, chunksToFlush * sizeof(UINT32));

        for(int i=0; i<chunksToFlush; i++)
        {
            write_dram_32(ChunksMapTable(ctrlBlock_[bank_].dataLpn[i], ctrlBlock_[bank_].chunkIdx[i]), lChunkAddr);
            lChunkAddr++;
        }
    }

    else
    {
        for(int i=0; i<chunksToFlush; i++)
        {
            write_dram_32(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), i), ctrlBlock_[bank_].dataLpn[i]);

            if (ctrlBlock_[bank_].dataLpn[i] != INVALID)
            {
                write_dram_32(ChunksMapTable(ctrlBlock_[bank_].dataLpn[i], ctrlBlock_[bank_].chunkIdx[i]), lChunkAddr);
            }
            else
            {
                decrementValidChunks(&heapDataFirstUsage, bank_, LogPageToLogBlk(newLogLpn)); // decrement blk with previous copy
                decrementValidChunks(&heapDataSecondUsage, bank_, LogPageToLogBlk(newLogLpn)); // decrement blk with previous copy
                decrementValidChunks(&heapDataCold, bank_, LogPageToLogBlk(newLogLpn)); // decrement blk with previous copy
            }
            lChunkAddr++;
        }
        ctrlBlock_[bank_].allChunksInLogAreValid = TRUE;
    }
    ctrlBlock_[bank_].increaseLpn(bank_, ctrlBlock_);
}

static void flushLogBufferRecycledPage()
{
#if OPTION_DEBUG_WRITE
    if (ctrlBlock_ != hotLogCtrl)
    {
        uart_print_level_1("ERROR in flushLogBufferRecycledPage: ctrlBlock_ is not hotLogCtrl!\r\n");
        while(1);
    }
#endif

#if WOMCanFail
    if (successRateWOM < 100.0)
    {
        float r = (float)rand() / (float) (RAND_MAX/100.0);
        if (r >= successRateWOM)
        {
            copyReuseBufToColdBuf();
            return;
        }
    }
#endif

#if PrintStats
    uart_print_level_1("*\r\n");
#endif

    UINT32 newLogLpn = ctrlBlock_[bank_].logLpn;

    uart_print(" flushLogBufferRecycledPage in bank "); uart_print_int(bank_);
    uart_print(": logLpn="); uart_print_int(newLogLpn); uart_print("\r\n");

    UINT32 vBlk = get_log_vbn(bank_, LogPageToLogBlk(newLogLpn));
    UINT32 pageOffset = LogPageToOffset(newLogLpn);

    UINT32 chunksToFlush=CHUNKS_PER_RECYCLED_PAGE;
    UINT32 lChunkAddr = ((bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (newLogLpn * CHUNKS_PER_PAGE)) | ColdLogBufBitFlag;

    if (ctrlBlock_[bank_].nextLowPageOffset != pageOffset)
    {
        uart_print_level_1("ERROR in flushLogBufferRecycledPage: flushing different pageOffset from the precached one\r\n");
        while(1);
    }

    if (ctrlBlock_[bank_].precacheDone == FALSE)
    { // Couldn't find time to do precaching, so we really need to do it now!
        precacheLowPage(bank_, ctrlBlock_);
    }

    //TODO (fabio): must encode here. Something like: encode(ctrlblock_[bank_].logBufferAddr, PrecacheForEncoding(bank));

    //uart_print_level_1("REPROGRAM ");
    //uart_print_level_1_int(bank_);
    //uart_print_level_1(" ");
    //uart_print_level_1_int(pageOffset);
    //uart_print_level_1("\r\n");

    nand_page_program(bank_, vBlk, pageOffset, ctrlBlock_[bank_].logBufferAddr, RETURN_ON_ISSUE);

    if( __builtin_expect(ctrlBlock_[bank_].allChunksInLogAreValid, TRUE))
    {

        for(int i=0; i<chunksToFlush; i++)
        {
            write_dram_32(ChunksMapTable(ctrlBlock_[bank_].dataLpn[i], ctrlBlock_[bank_].chunkIdx[i]), lChunkAddr);
            ctrlBlock_[bank_].dataLpn[i] |= ColdLogBufBitFlag; // Set 31st bit in the inverse map so that during GC we know that these chunks were encoded
            lChunkAddr++;
        }
        mem_copy(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, pageOffset, 0), ctrlBlock_[bank_].dataLpn, chunksToFlush * sizeof(UINT32));
        decrementValidChunksByN(&heapDataSecondUsage, bank_, LogPageToLogBlk(newLogLpn), CHUNKS_PER_PAGE - CHUNKS_PER_RECYCLED_PAGE);
    }

    else
    {
        UINT8 validChunks = 0;
        for(int i=0; i<chunksToFlush; i++)
        {

            if (ctrlBlock_[bank_].dataLpn[i] != INVALID)
            {
                write_dram_32(ChunksMapTable(ctrlBlock_[bank_].dataLpn[i], ctrlBlock_[bank_].chunkIdx[i]), lChunkAddr);
                validChunks++;
            }

            ctrlBlock_[bank_].dataLpn[i] |= ColdLogBufBitFlag; // Set 31st bit in the inverse map so that during GC we know that these chunks were encoded
            write_dram_32(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), i), ctrlBlock_[bank_].dataLpn[i]);
            lChunkAddr++;
        }

        ctrlBlock_[bank_].allChunksInLogAreValid = TRUE;
        decrementValidChunksByN(&heapDataSecondUsage, bank_, LogPageToLogBlk(newLogLpn), CHUNKS_PER_PAGE - validChunks);
    }
    ctrlBlock_[bank_].increaseLpn(bank_, ctrlBlock_);
}

#if WOMCanFail
static void copyReuseBufToColdBuf()
{
    ctrlBlock_ = coldLogCtrl; // IMPORTANT: we're calling updateChunkPtr and this updated ctrlBlock_, so this must be
                              // coldlogctrl because this is where we're copying the chunks now.

    /* TODO(fabio): The following code should take care of the case in which the entire reuse buffer is full and we copy it to the
     * cold buffer in one mem_copy.
     * For some reason it doesn't work.
     * The error we observe is that, when the reused blk will be GCed, the GC finds 1 valid chunks while the heap was expecting 3.
    if((hotLogCtrl[bank_].allChunksInLogAreValid == TRUE) &&
       (CHUNKS_PER_PAGE - coldLogCtrl[bank_].chunkPtr < CHUNKS_PER_RECYCLED_PAGE))
    { // The chunks fit the cold buffer. Possibly triggering GC at the end.
        UINT32 src = hotLogCtrl[bank_].logBufferAddr;
        UINT32 dst = coldLogCtrl[bank_].logBufferAddr+(coldLogCtrl[bank_].chunkPtr*BYTES_PER_CHUNK);
        mem_copy(dst, src, CHUNKS_PER_RECYCLED_PAGE * BYTES_PER_CHUNK);

        for (UINT32 chunk=0; chunk<CHUNKS_PER_RECYCLED_PAGE; ++chunk)
        {
            if (hotLogCtrl[bank_].dataLpn[chunk] == INVALID)
            {
                uart_print_level_1("ERROR1!!!\r\n");
                while(1);
            }
            UINT32 lpn = hotLogCtrl[bank_].dataLpn[chunk];
            UINT32 chunkIdx = hotLogCtrl[bank_].chunkIdx[chunk];
            write_dram_32(ChunksMapTable(lpn, chunkIdx),
                          (((bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) +
                            (DramLogBufLpn * CHUNKS_PER_PAGE) +
                            coldLogCtrl[bank_].chunkPtr) | StartOwLogLpn));
            coldLogCtrl[bank_].dataLpn[coldLogCtrl[bank_].chunkPtr]= lpn;
            coldLogCtrl[bank_].chunkIdx[coldLogCtrl[bank_].chunkPtr]= chunkIdx;
            updateChunkPtr();
        }
    }

    else
    */
    { // We've got to copy one chunk at a time either because we'll trigger GC or because not all chunks are valid
        for (UINT32 chunk=0; chunk<CHUNKS_PER_RECYCLED_PAGE; ++chunk)
        {
            if (hotLogCtrl[bank_].dataLpn[chunk] != INVALID)
            {
                UINT32 src = hotLogCtrl[bank_].logBufferAddr+(chunk * BYTES_PER_CHUNK);
                UINT32 dst = coldLogCtrl[bank_].logBufferAddr+(coldLogCtrl[bank_].chunkPtr*BYTES_PER_CHUNK);
                mem_copy(dst, src, BYTES_PER_CHUNK);

                UINT32 lpn = hotLogCtrl[bank_].dataLpn[chunk];
                UINT32 chunkIdx = hotLogCtrl[bank_].chunkIdx[chunk];
                coldLogCtrl[bank_].dataLpn[coldLogCtrl[bank_].chunkPtr] = lpn;
                coldLogCtrl[bank_].chunkIdx[coldLogCtrl[bank_].chunkPtr] = chunkIdx;
                write_dram_32(ChunksMapTable(lpn, chunkIdx),
                              (((bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) +
                                (DramLogBufLpn * CHUNKS_PER_PAGE) +
                                coldLogCtrl[bank_].chunkPtr) | StartOwLogLpn));
                updateChunkPtr();
            }
        }
    }

    hotLogCtrl[bank_].chunkPtr = 0;
    hotLogCtrl[bank_].allChunksInLogAreValid = TRUE;
    ctrlBlock_ = hotLogCtrl; // Restore ctrlBlock_ in case the write was not finished.
}
#endif

static void flushLogBufferDuringGC(const UINT32 bank)
{
#if PrintStats
    uart_print_level_1("^\r\n");
#endif

    uart_print("flushLogBufferDuringGC bank="); uart_print_int(bank); uart_print("\r\n");
    UINT32 newLogLpn = getRWLpn(bank, coldLogCtrl);
    uart_print("FlushLog to lpn="); uart_print_int(newLogLpn); uart_print("\r\n");
    UINT32 vBlk = get_log_vbn(bank, LogPageToLogBlk(newLogLpn));
    UINT32 pageOffset = LogPageToOffset(newLogLpn);
    nand_page_program(bank, vBlk, pageOffset, coldLogCtrl[bank].logBufferAddr, RETURN_ON_ISSUE);

    if (__builtin_expect(coldLogCtrl[bank].allChunksInLogAreValid, TRUE))
    {
        mem_copy(chunkInLpnsList(coldLogCtrl[bank].lpnsListAddr, LogPageToOffset(newLogLpn), 0), coldLogCtrl[bank].dataLpn, CHUNKS_PER_PAGE * sizeof(UINT32));
        UINT32 lChunkAddr = (newLogLpn * CHUNKS_PER_PAGE);
        for(int i=0; i<CHUNKS_PER_PAGE; i++)
        {
            write_dram_32(ChunksMapTable(coldLogCtrl[bank].dataLpn[i], coldLogCtrl[bank].chunkIdx[i]),
                          (bank * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + lChunkAddr);
            lChunkAddr++;
        }
    }

    else
    {
        mem_copy(chunkInLpnsList(coldLogCtrl[bank].lpnsListAddr, LogPageToOffset(newLogLpn), 0), coldLogCtrl[bank].dataLpn, CHUNKS_PER_PAGE * sizeof(UINT32));
        UINT32 lChunkAddr = (newLogLpn * CHUNKS_PER_PAGE);
        for(int i=0; i<CHUNKS_PER_PAGE; i++)
        {
            if (coldLogCtrl[bank].dataLpn[i] != INVALID)
            {
                write_dram_32(ChunksMapTable(coldLogCtrl[bank].dataLpn[i], coldLogCtrl[bank].chunkIdx[i]),
                              (bank * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + lChunkAddr);
            }
            else
            {
                decrementValidChunks(&heapDataFirstUsage, bank, LogPageToLogBlk(newLogLpn));
                decrementValidChunks(&heapDataSecondUsage, bank, LogPageToLogBlk(newLogLpn));
                decrementValidChunks(&heapDataCold, bank, LogPageToLogBlk(newLogLpn));
            }
            lChunkAddr++;
        }
        coldLogCtrl[bank].allChunksInLogAreValid = TRUE;
    }
    coldLogCtrl[bank].increaseLpn(bank, coldLogCtrl);
}

void writeChunkOnLogBlockDuringGC( const UINT32 bank,
                                   const UINT32 dataLpn,
                                   const UINT32 dataChunkOffset,
                                   const UINT32 chunkOffsetInBuf,
                                   const UINT32 bufAddr)
{
    /* Need this function because during GC can't use the global variables, because those might be related to an outstanding write (which triggered the GC) */
    uart_print("writeChunkOnLogBlockDuringGC, bank="); uart_print_int(bank); uart_print(" dataLpn="); uart_print_int(dataLpn);
    uart_print(", dataChunkOffset="); uart_print_int(dataChunkOffset); uart_print("\r\n");
    int sectOffset = dataChunkOffset * SECTORS_PER_CHUNK;
    UINT32 src = bufAddr + (chunkOffsetInBuf * BYTES_PER_CHUNK);
    UINT32 dst = coldLogCtrl[bank].logBufferAddr + (coldLogCtrl[bank].chunkPtr * BYTES_PER_CHUNK); // base address of the destination chunk
    waitBusyBank(bank);
    mem_copy(dst, src, BYTES_PER_CHUNK);
    updateDramBufMetadataDuringGc(bank, dataLpn, sectOffset);
    updateChunkPtrDuringGC(bank);
}

static void updateDramBufMetadataDuringGc(const UINT32 bank, const UINT32 lpn, const UINT32 sectOffset)
{
    uart_print("updateDramBufMetadataDuringGc\r\n");
    UINT32 chunkIdx = sectOffset / SECTORS_PER_CHUNK;
    coldLogCtrl[bank].dataLpn[coldLogCtrl[bank].chunkPtr]=lpn;
    coldLogCtrl[bank].chunkIdx[coldLogCtrl[bank].chunkPtr]=chunkIdx;
    write_dram_32(ChunksMapTable(lpn, chunkIdx),
                  (((bank * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (DramLogBufLpn * CHUNKS_PER_PAGE) + coldLogCtrl[bank].chunkPtr) | StartOwLogLpn));
}

/*
UINT32 chooseNewBank()
{
    uart_print("chooseNewBank w log: ");
    UINT32 maxFreePages = 0;
    UINT32 candidateBank = 0;
    for (UINT32 i = 0; i < NUM_BANKS; ++i)
    {
        UINT32 cleanPages = cleanListSize(&cleanListDataWrite, i) * UsedPagesPerLogBlk;
        //UINT32 SWLpn = SWCtrl[i].logLpn != INVALID ? LogPageToOffset(SWCtrl[i].logLpn) : UsedPagesPerLogBlk;
        UINT32 RWLpn = RWCtrl[i].logLpn != INVALID ? LogPageToOffset(RWCtrl[i].logLpn) : UsedPagesPerLogBlk;
#if Overwrite
        UINT32 OWLpn = OWCtrl[i].logLpn != INVALID ? LogPageToOffset(OWCtrl[i].logLpn)/2 : UsedPagesPerLogBlk;
#else
        UINT32 OWLpn = UsedPagesPerLogBlk;
#endif
        //uart_print(" cleanPages: "); uart_print_int(cleanPages);
        //uart_print(" SWLpn: "); uart_print_int(UsedPagesPerLogBlk - SWLpn);
        //uart_print(" RWLpn: "); uart_print_int(UsedPagesPerLogBlk - RWLpn); uart_print("\r\n");
        //UINT32 validChunksInWriteHeap = getVictimValidPagesNumber(&heapDataWrite, i) == INVALID ? CHUNKS_PER_LOG_BLK : getVictimValidPagesNumber(&heapDataWrite, i);
        //UINT32 validChunksInOverwriteHeap = getVictimValidPagesNumber(&heapDataOverwrite, i) == INVALID ? CHUNKS_PER_LOG_BLK : getVictimValidPagesNumber(&heapDataOverwrite, i);
        //UINT32 freePages = cleanPages + (UsedPagesPerLogBlk - OWLpn) + (UsedPagesPerLogBlk - SWLpn) + (UsedPagesPerLogBlk - RWLpn) + ((CHUNKS_PER_LOG_BLK - validChunksInWriteHeap)/CHUNKS_PER_PAGE) + ((CHUNKS_PER_LOG_BLK - validChunksInOverwriteHeap)/CHUNKS_PER_PAGE);
        //UINT32 freePages = cleanPages + (UsedPagesPerLogBlk - OWLpn) + (UsedPagesPerLogBlk - SWLpn) + (UsedPagesPerLogBlk - RWLpn);
        UINT32 freePages = cleanPages + (UsedPagesPerLogBlk - OWLpn) + (UsedPagesPerLogBlk - RWLpn);
        if (freePages > maxFreePages)
        {
            {
                maxFreePages = freePages;
                candidateBank = i;
            }
        }
    }
    uart_print("chosen bank="); uart_print_int(candidateBank); uart_print("\r\n");
    return(candidateBank);
}
*/
UINT32 chooseNewBank(UINT32 chunkNumber)
{
    return (chunkNumber % NUM_BANKS);
}


static void initWrite(LogCtrlBlock * ctrlBlock, const UINT32 dataLpn, const UINT32 sectOffset, const UINT32 nSects)
{
    uart_print("initWrite\r\n");
    ctrlBlock_ = ctrlBlock;
    lpn_ = dataLpn;
    sectOffset_ = sectOffset;
    nSects_ = nSects;
    remainingSects_ = nSects;
}

/*
static void manageOldCompletePage()
{
    uart_print("manageOldCompletePage\r\n");
    for (int chunkIdx=0; chunkIdx<CHUNKS_PER_PAGE; chunkIdx++)
    {
        uart_print("Chunk "); uart_print_int(chunkIdx); uart_print("\r\n");
        manageOldChunkForCompletePageWrite(chunkIdx);
    }
}
*/

/* Completely invalid every metadata related to the chunk, because no GC can happen before the new page written since it is a complete page write */
//static void manageOldChunkForCompletePageWrite(int chunkIdx)
static void manageOldChunkForCompletePageWrite(const UINT32 oldChunkAddr)
{
#if OPTION_DEBUG_WRITE
    if (ChunksMapTable(lpn_, chunkIdx) > DRAM_BASE + DRAM_SIZE)
    {
        uart_print_level_1("ERROR in manageOldChunkForCompletePageWrite 1: reading above DRAM address space\r\n");
    }
#endif
    //UINT32 oldChunkAddr = read_dram_32(ChunksMapTable(lpn_, chunkIdx));
    switch (findChunkLocation(oldChunkAddr))
    {
        case Invalid:
        {
            return;
        }
        case FlashWLog:
        {
            UINT32 oldChunkBank = ChunkToBank(oldChunkAddr);
            UINT32 oldChunkLbn = ChunkToLbn(oldChunkAddr);
            decrementValidChunks(&heapDataFirstUsage, oldChunkBank, oldChunkLbn);
            decrementValidChunks(&heapDataSecondUsage, oldChunkBank, oldChunkLbn);
            decrementValidChunks(&heapDataCold, oldChunkBank, oldChunkLbn);
// note(fabio): insert this just to have GC tests working. Probably should disable this checks if they become too heavy
            if (oldChunkLbn == victimLbn[oldChunkBank])
            {
                if (gcState[oldChunkBank] != GcIdle)
                {
                    uart_print("manageOldChunkForCompletePageWrite decreases nValidChunksFromHeap on bank "); uart_print_int(oldChunkBank); uart_print("\r\n");
                    nValidChunksFromHeap[oldChunkBank]--;
                }
            }
            return;
        }
        case FlashWLogEncoded:
        {
            UINT32 realOldChunkAddr = oldChunkAddr & ~(ColdLogBufBitFlag);
            UINT32 oldChunkBank = ChunkToBank(realOldChunkAddr);
            UINT32 oldChunkLbn = ChunkToLbn(realOldChunkAddr);
            decrementValidChunks(&heapDataFirstUsage, oldChunkBank, oldChunkLbn);
            decrementValidChunks(&heapDataSecondUsage, oldChunkBank, oldChunkLbn);
            decrementValidChunks(&heapDataCold, oldChunkBank, oldChunkLbn);
// note(fabio): insert this just to have GC tests working. Probably should disable this checks if they become too heavy
            if (oldChunkLbn == victimLbn[oldChunkBank])
            {
                if (gcState[oldChunkBank] != GcIdle)
                {
                    nValidChunksFromHeap[oldChunkBank]--;
                }
            }
            return;
        }
        case DRAMHotLog:
        {
            UINT32 oldChunkBank = ChunkToBank(oldChunkAddr);
            hotLogCtrl[oldChunkBank].dataLpn[oldChunkAddr % CHUNKS_PER_PAGE]=INVALID;
            hotLogCtrl[oldChunkBank].allChunksInLogAreValid = FALSE;
            return;
        }
        case DRAMColdLog:
        {
            UINT32 realOldChunkAddr = oldChunkAddr & ~(ColdLogBufBitFlag);
            uart_print("masked oldChunkAddr is "); uart_print_int(realOldChunkAddr); uart_print("\r\n");
            UINT32 oldChunkBank = ChunkToBank(realOldChunkAddr);
            coldLogCtrl[oldChunkBank].dataLpn[realOldChunkAddr % CHUNKS_PER_PAGE]=INVALID;
            coldLogCtrl[oldChunkBank].allChunksInLogAreValid = FALSE;
            return;
        }
    }
}

static void updateDramBufMetadata()
{
    uart_print("updateDramBufMetadata\r\n");
    UINT32 chunkIdx = sectOffset_ / SECTORS_PER_CHUNK;
    ctrlBlock_[bank_].dataLpn[ctrlBlock_[bank_].chunkPtr]=lpn_;
    ctrlBlock_[bank_].chunkIdx[ctrlBlock_[bank_].chunkPtr]=chunkIdx;
    if (ctrlBlock_ == hotLogCtrl)
    { // hot data
        write_dram_32(ChunksMapTable(lpn_, chunkIdx),
                      (bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (DramLogBufLpn * CHUNKS_PER_PAGE) + ctrlBlock_[bank_].chunkPtr);
    }
    else
    { // cold data
        write_dram_32(ChunksMapTable(lpn_, chunkIdx),
                      (((bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (DramLogBufLpn * CHUNKS_PER_PAGE) + ctrlBlock_[bank_].chunkPtr) | StartOwLogLpn));
    }
}

void updateChunkPtr()
{
    uart_print("updateChunkPtr\r\n");

    ctrlBlock_[bank_].chunkPtr = (ctrlBlock_[bank_].chunkPtr + 1) % CHUNKS_PER_PAGE;

    uart_print("new chunkPtr for bank "); uart_print_int(bank_); uart_print(" is "); uart_print_int(ctrlBlock_[bank_].chunkPtr); uart_print("\r\n");

    if (ctrlBlock_[bank_].chunkPtr == 0)
    {
        flushLogBuffer();
    }
    else
    {
        if (ctrlBlock_[bank_].nextLowPageOffset != INVALID)
        {
            if (ctrlBlock_[bank_].precacheDone == FALSE)
            {
                if (isBankBusy(bank_) == FALSE)
                {
                    precacheLowPage(bank_, ctrlBlock_);
                }
            }
        }
    }
}

void updateChunkPtrRecycledPage()
{
    uart_print("updateChunkPtrRecycledPage\r\n");

    ctrlBlock_[bank_].chunkPtr = (ctrlBlock_[bank_].chunkPtr + 1) % CHUNKS_PER_RECYCLED_PAGE;

    uart_print("new chunkPtr for bank "); uart_print_int(bank_); uart_print(" is "); uart_print_int(ctrlBlock_[bank_].chunkPtr); uart_print("\r\n");

    if (ctrlBlock_[bank_].chunkPtr == 0)
    {
        flushLogBufferRecycledPage();
    }
    else
    {
        if (ctrlBlock_[bank_].nextLowPageOffset != INVALID)
        {
            if (ctrlBlock_[bank_].precacheDone == FALSE)
            {
                if (isBankBusy(bank_) == FALSE)
                {
                    precacheLowPage(bank_, ctrlBlock_);
                }
            }
        }
    }
}

static void updateChunkPtrDuringGC(const UINT32 bank)
{
    uart_print("updateChunkPtrDuringGC\r\n");
    coldLogCtrl[bank].chunkPtr = (coldLogCtrl[bank].chunkPtr + 1) % CHUNKS_PER_PAGE;
    uart_print("new chunkPtr for bank "); uart_print_int(bank); uart_print(" is "); uart_print_int(coldLogCtrl[bank].chunkPtr); uart_print("\r\n");
    if (coldLogCtrl[bank].chunkPtr == 0)
        flushLogBufferDuringGC(bank);
}

static void syncWithWriteLimit() {
#if OPTION_DEBUG_WRITE
    int count=0;
    while(g_ftl_write_buf_id != GETREG(BM_WRITE_LIMIT))
    {
        backgroundCleaning(bank_);
        count++;
        if (count == 100000) {
            count=0;
            uart_print_level_1("*\r\n");
            uart_print_level_1("FTL Buf Id: "); uart_print_level_1_int(g_ftl_write_buf_id); uart_print_level_1("\r\n");
            uart_print_level_1("SATA Buf Id: "); uart_print_level_1_int(GETREG(BM_WRITE_LIMIT)); uart_print_level_1("\r\n");
        }
    }
#else
    while(g_ftl_write_buf_id != GETREG(BM_WRITE_LIMIT))
    {
        backgroundCleaning(bank_);
    }
#endif
}

//void writeToLogBlk (const UINT32 bank, LogCtrlBlock * ctrlBlock, UINT32 const dataLpn, UINT32 const sectOffset, UINT32 const nSects)
void writeToLogBlk (LogCtrlBlock * ctrlBlock, UINT32 const dataLpn, UINT32 const sectOffset, UINT32 const nSects)
{

    uart_print("writeToLogBlk dataLpn="); uart_print_int(dataLpn);
    uart_print(", sect_offset="); uart_print_int(sectOffset);
    uart_print(", num_sectors="); uart_print_int(nSects); uart_print("\r\n");
    bank_ = chooseNewBank( (dataLpn * CHUNKS_PER_PAGE) + (sectOffset / SECTORS_PER_CHUNK) );
    initWrite(ctrlBlock, dataLpn, sectOffset, nSects);
    if (nSects_ != SECTORS_PER_PAGE)
    {
        syncWithWriteLimit();
        writePartialPageOld();
    }
    else
    { // writing entire page, so write directly to
        //manageOldCompletePage();
        writeCompletePage();
    }
}

static void writeCompletePage()
{

#if PrintStats
    uart_print_level_1("+\r\n");
#endif

    uart_print("writeCompletePage\r\n");

    UINT32 newLogLpn = getLpnForCompletePage(bank_, ctrlBlock_);

    UINT32 vBlk = get_log_vbn(bank_, LogPageToLogBlk(newLogLpn));
    UINT32 pageOffset = LogPageToOffset(newLogLpn);
    nand_page_ptprogram_from_host (bank_, vBlk, pageOffset, 0, SECTORS_PER_PAGE);


    UINT32 dataLpns[CHUNKS_PER_PAGE];
    UINT32 logicalAddresses[CHUNKS_PER_PAGE];
    UINT32 oldChunkAddresses[CHUNKS_PER_PAGE];

    mem_copy(oldChunkAddresses, ChunksMapTable(lpn_, 0), CHUNKS_PER_PAGE * sizeof(UINT32));


    UINT32 logicalAddress = (bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (newLogLpn * CHUNKS_PER_PAGE);

    for(UINT32 i=0; i<CHUNKS_PER_PAGE; i++)
    {
#if OPTION_DEBUG_WRITE
        if((chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), i)) >=(DRAM_BASE + DRAM_SIZE))
        {
            uart_print_level_1("ERROR in write::writeCompletePage 1: writing to ");
            uart_print_level_1_int(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), i));
            uart_print_level_1("\r\n");
        }
#endif
        manageOldChunkForCompletePageWrite(oldChunkAddresses[i]);


        //write_dram_32(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), i), lpn_);
        dataLpns[i] = lpn_;

        //write_dram_32(ChunksMapTable(lpn_, i), (bank_ * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (newLogLpn * CHUNKS_PER_PAGE) + i);
        logicalAddresses[i] = logicalAddress;
        logicalAddress++;

    }

    mem_copy(chunkInLpnsList(ctrlBlock_[bank_].lpnsListAddr, LogPageToOffset(newLogLpn), 0), dataLpns, CHUNKS_PER_PAGE*sizeof(UINT32));
    mem_copy(ChunksMapTable(lpn_, 0), logicalAddresses, CHUNKS_PER_PAGE*sizeof(UINT32));

    ctrlBlock_[bank_].increaseLpn(bank_, ctrlBlock_);
    if (ctrlBlock_[bank_].chunkPtr >= CHUNKS_PER_RECYCLED_PAGE)
    {
        newLogLpn = getLpnForCompletePage(bank_, ctrlBlock_);
    }
}

static void writeChunkNew(UINT32 nSectsToWrite)
{
    uart_print("writeChunkNew\r\n");
    UINT32 src = WR_BUF_PTR(g_ftl_write_buf_id)+(sectOffset_*BYTES_PER_SECTOR);
    UINT32 dst = ctrlBlock_[bank_].logBufferAddr+(ctrlBlock_[bank_].chunkPtr*BYTES_PER_CHUNK); // base address of the destination chunk
    waitBusyBank(bank_);
    if (nSectsToWrite != SECTORS_PER_CHUNK)
    {
        mem_set_dram (dst, 0xFFFFFFFF, BYTES_PER_CHUNK); // Initialize chunk in dram log buffer with 0xFF
    }
    mem_copy(dst+((sectOffset_ % SECTORS_PER_CHUNK) * BYTES_PER_SECTOR), src, nSectsToWrite*BYTES_PER_SECTOR);
}

static void writePartialPageOld()
{
    uart_print("writePartialPageOld\r\n");
#if OPTION_DEBUG_WRITE
    int count=0;
#endif
    while (remainingSects_ != 0)
    {
#if OPTION_DEBUG_WRITE
        count++;
        if (count > 100000)
        {
            count=0;
            uart_print_level_1("Warning in writePartialPageOld\r\n");
        }
#endif
        writeChunkOld();
    }
    // SATA buffer management
    //g_ftl_write_buf_id = (g_ftl_write_buf_id + 1) % NUM_WR_BUFFERS;
    //SETREG (BM_STACK_WRSET, g_ftl_write_buf_id);
    //SETREG (BM_STACK_RESET, 0x01);
}

static void writeChunkOld()
{
    uart_print("writeChunkOld\r\n");
    UINT32 nSectsToWrite = (((sectOffset_ % SECTORS_PER_CHUNK) + remainingSects_) < SECTORS_PER_CHUNK) ? remainingSects_ : (SECTORS_PER_CHUNK - (sectOffset_ % SECTORS_PER_CHUNK));
    UINT32 chunkIdx = sectOffset_ / SECTORS_PER_CHUNK;
#if OPTION_DEBUG_WRITE
    if (ChunksMapTable(lpn_, chunkIdx) > DRAM_BASE + DRAM_SIZE)
    {
        uart_print_level_1("ERROR in writeChunkOld 1: reading above DRAM address space\r\n");
    }
#endif
    UINT32 oldChunkAddr = read_dram_32(ChunksMapTable(lpn_, chunkIdx));
    uart_print("Old chunk is ");
    switch (findChunkLocation(oldChunkAddr))
    {

        case Invalid:
        {
            uart_print(" invalid\r\n");
            writeChunkNew(nSectsToWrite);
            updateDramBufMetadata();
            ctrlBlock_[bank_].updateChunkPtr();
            sectOffset_ += nSectsToWrite;
            remainingSects_ -= nSectsToWrite;
            return;
        }

        case FlashWLog:
        {
            uart_print(" in w log\r\n");
            if (nSectsToWrite == SECTORS_PER_CHUNK)
            {
                writeChunkNew(nSectsToWrite);
            }
            else
            {
                writePartialChunkWhenOldChunkIsInFlashLog(nSectsToWrite, oldChunkAddr);
            }
            UINT32 oldChunkBank = ChunkToBank(oldChunkAddr);

            uart_print("Decrementing bank "); uart_print_int(oldChunkBank);
            uart_print(" lpn "); uart_print_int( ChunkToLpn(oldChunkAddr) ); uart_print("\r\n");

            decrementValidChunks(&heapDataFirstUsage, oldChunkBank, ChunkToLbn(oldChunkAddr));
            decrementValidChunks(&heapDataSecondUsage, oldChunkBank, ChunkToLbn(oldChunkAddr));
            decrementValidChunks(&heapDataCold, oldChunkBank, ChunkToLbn(oldChunkAddr));

// note(fabio): insert this just to have GC tests working. Probably should disable this checks if they become too heavy
            if (gcState[oldChunkBank] != GcIdle && ChunkToLbn(oldChunkAddr) == victimLbn[oldChunkBank])
            {
                uart_print("writeOldChunk decreases nValidChunksFromHeap on bank "); uart_print_int(oldChunkBank); uart_print("\r\n");
                nValidChunksFromHeap[oldChunkBank]--;
            }

            updateDramBufMetadata();
            ctrlBlock_[bank_].updateChunkPtr();
            sectOffset_ += nSectsToWrite;
            remainingSects_ -= nSectsToWrite;
            return;
        }

        case FlashWLogEncoded:
        {
            uart_print(" in w log\r\n");
            if (nSectsToWrite == SECTORS_PER_CHUNK)
            {
                writeChunkNew(nSectsToWrite);
            }
            else
            {
                writeChunkNew(SECTORS_PER_CHUNK);
                //writePartialChunkWhenOldChunkIsInFlashLogEncoded(nSectsToWrite, oldChunkAddr);
            }
            UINT32 realOldChunkAddr = oldChunkAddr & ~(ColdLogBufBitFlag);
            UINT32 oldChunkBank = ChunkToBank(realOldChunkAddr);
            decrementValidChunks(&heapDataFirstUsage, oldChunkBank, ChunkToLbn(realOldChunkAddr));
            decrementValidChunks(&heapDataSecondUsage, oldChunkBank, ChunkToLbn(realOldChunkAddr));
            decrementValidChunks(&heapDataCold, oldChunkBank, ChunkToLbn(realOldChunkAddr));

// note(fabio): insert this just to have GC tests working. Probably should disable this checks if they become too heavy
            if (gcState[oldChunkBank] != GcIdle && ChunkToLbn(realOldChunkAddr) == victimLbn[oldChunkBank])
            {
                nValidChunksFromHeap[oldChunkBank]--;
            }

            updateDramBufMetadata();
            ctrlBlock_[bank_].updateChunkPtr();
            sectOffset_ += nSectsToWrite;
            remainingSects_ -= nSectsToWrite;
            return;
        }

        case DRAMHotLog:
        {
            uart_print(" in hot DRAM buf\r\n");
            UINT32 oldBank = ChunkToBank(oldChunkAddr);
            UINT32 oldSectOffset = ChunkToSectOffset(oldChunkAddr);
            waitBusyBank(oldBank);

#if OPTION_NO_DRAM_ABSORB
            { //note(fabio): Silly strategy that don't overwrites in DRAM and writes to another location
                UINT32 oldChunkOffset = oldSectOffset / SECTORS_PER_CHUNK;

                hotLogCtrl[oldBank].dataLpn[oldChunkOffset]=INVALID;
                hotLogCtrl[oldBank].chunkIdx[oldChunkOffset]=INVALID;
                hotLogCtrl[oldBank].allChunksInLogAreValid = FALSE;
                if (nSectsToWrite == SECTORS_PER_CHUNK)
                {
                    writeChunkNew(nSectsToWrite);
                }
                else
                {
                    uart_print_level_1("Warning in silly DRAM buffer implementation for DRAMHotLog: request not chunk aligned. Writing ");
                    uart_print_level_1_int(nSectsToWrite);
                    uart_print_level_1(" sectors, lpn ");
                    uart_print_level_1_int(lpn_);
                    uart_print_level_1(", sector_offset ");
                    uart_print_level_1_int(sectOffset_);
                    uart_print_level_1(", nSectsToWrite ");
                    uart_print_level_1_int(nSectsToWrite);
                    uart_print_level_1("\r\nThis feature is not fully implemented (i.e. old chunk won't be copied)\r\n");
                    writeChunkNew(nSectsToWrite);
                }
                updateDramBufMetadata();
                ctrlBlock_[bank_].updateChunkPtr();
            }
#else
            writePartialChunkWhenOldIsInDRAMBuf(nSectsToWrite, oldSectOffset, HOT_LOG_BUF(oldBank));
#endif
            sectOffset_ += nSectsToWrite;
            remainingSects_ -= nSectsToWrite;
            break;
        }

        case DRAMColdLog:
        {
            uart_print(" in cold DRAM buf\r\n");
            oldChunkAddr = oldChunkAddr & ~(ColdLogBufBitFlag);
            uart_print("masked oldChunkAddr is "); uart_print_int(oldChunkAddr); uart_print("\r\n");
            UINT32 oldBank = ChunkToBank(oldChunkAddr);
            UINT32 oldSectOffset = ChunkToSectOffset(oldChunkAddr);
            waitBusyBank(oldBank);
#if OPTION_NO_DRAM_ABSORB
            { //note(fabio): Silly strategy that don't overwrites in DRAM and writes to another location
                UINT32 oldChunkOffset = oldSectOffset / SECTORS_PER_CHUNK;

                coldLogCtrl[oldBank].dataLpn[oldChunkOffset]=INVALID;
                coldLogCtrl[oldBank].chunkIdx[oldChunkOffset]=INVALID;
                coldLogCtrl[oldBank].allChunksInLogAreValid = FALSE;
                if (nSectsToWrite == SECTORS_PER_CHUNK)
                {
                    writeChunkNew(nSectsToWrite);
                }
                else
                {
                    uart_print_level_1("Warning in silly DRAM buffer implementation for DRAMColdLog: request not chunk aligned. Writing ");
                    uart_print_level_1_int(nSectsToWrite);
                    uart_print_level_1(" sectors, lpn ");
                    uart_print_level_1_int(lpn_);
                    uart_print_level_1(", sector_offset ");
                    uart_print_level_1_int(sectOffset_);
                    uart_print_level_1(", nSectsToWrite ");
                    uart_print_level_1_int(nSectsToWrite);
                    uart_print_level_1("\r\nThis feature is not fully implemented (i.e. old chunk won't be copied)\r\n");
                    writeChunkNew(nSectsToWrite);
                    //while(1);
                }
                updateDramBufMetadata();
                ctrlBlock_[bank_].updateChunkPtr();
            }
#else
            writePartialChunkWhenOldIsInDRAMBuf(nSectsToWrite, oldSectOffset, COLD_LOG_BUF(oldBank));
#endif
            sectOffset_ += nSectsToWrite;
            remainingSects_ -= nSectsToWrite;
            break;
        }
    }
}

#if OPTION_NO_DRAM_ABSORB == 0
static void writePartialChunkWhenOldIsInDRAMBuf(UINT32 nSectsToWrite, UINT32 oldSectOffset, UINT32 DRAMBufStart)
{
    uart_print("writePartialChunkWhenOldIsInDRAMBuf\r\n");
    // Buffers
    UINT32 dstWBufStart = DRAMBufStart+(oldSectOffset*BYTES_PER_SECTOR); // location of old chunk
    UINT32 srcSataBufStart = WR_BUF_PTR(g_ftl_write_buf_id)+((sectOffset_ / SECTORS_PER_CHUNK)*BYTES_PER_CHUNK);
    // Sizes
    UINT32 startOffsetWrite = (sectOffset_ % SECTORS_PER_CHUNK) * BYTES_PER_SECTOR;

    mem_copy(dstWBufStart+startOffsetWrite, srcSataBufStart+startOffsetWrite, nSectsToWrite*BYTES_PER_SECTOR);
    #if MeasureDramAbsorb
    uart_print_level_1("WRDRAM "); uart_print_level_1_int(nSectsToWrite); uart_print_level_1("\r\n");
    #endif
}
#endif

static void writePartialChunkWhenOldChunkIsInFlashLog(UINT32 nSectsToWrite, UINT32 oldChunkAddr)
{
    uart_print("writePartialChunkWhenOldChunkIsInFlashLog\r\n");
    UINT32 src = WR_BUF_PTR(g_ftl_write_buf_id)+((sectOffset_ / SECTORS_PER_CHUNK)*BYTES_PER_CHUNK);
    UINT32 dstWBufChunkStart = ctrlBlock_[bank_].logBufferAddr + (ctrlBlock_[bank_].chunkPtr * BYTES_PER_CHUNK); // base address of the destination chunk
    UINT32 startOffsetWrite = (sectOffset_ % SECTORS_PER_CHUNK) * BYTES_PER_SECTOR;
    // Old Chunk Location
    UINT32 oldBank = ChunkToBank(oldChunkAddr);
    UINT32 oldVbn = get_log_vbn(oldBank, ChunkToLbn(oldChunkAddr));
    UINT32 oldPageOffset = ChunkToPageOffset(oldChunkAddr);
    UINT32 oldSectOffset = ChunkToSectOffset(oldChunkAddr);
    // Offsets
    UINT32 dstByteOffset = ctrlBlock_[bank_].chunkPtr * BYTES_PER_CHUNK;
    UINT32 srcByteOffset = ChunkToChunkOffset(oldChunkAddr) * BYTES_PER_CHUNK;
    UINT32 alignedWBufAddr = ctrlBlock_[bank_].logBufferAddr + dstByteOffset - srcByteOffset;
    waitBusyBank(bank_);
    nand_page_ptread(oldBank, oldVbn, oldPageOffset, oldSectOffset, SECTORS_PER_CHUNK, alignedWBufAddr, RETURN_WHEN_DONE);
    mem_copy(dstWBufChunkStart + startOffsetWrite, src + startOffsetWrite, nSectsToWrite*BYTES_PER_SECTOR);
}

static void writePartialChunkWhenOldChunkIsInFlashLogEncoded(UINT32 nSectsToWrite, UINT32 oldChunkAddr)
{
    uart_print_level_1("writePartialChunkWhenOldChunkIsInFlashLogEncoded is not implemented yet!\r\n");
    while(1);
}
