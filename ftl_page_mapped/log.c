#include "log.h"
#include "ftl_parameters.h"
#include "dram_layout.h"
#include "ftl_metadata.h"
#include "garbage_collection.h"  //TODO: this probably shouldn't be here
#include "heap.h"
#include "cleanList.h"
#include "flash.h" // Flash operations and flags
#include "write.h" // updateChunkPtr functions

#define Write_log_bmt(bank, lbn, vblock) write_dram_16 (LOG_BMT_ADDR + ((bank * LOG_BLK_PER_BANK + lbn) * sizeof (UINT16)), vblock)
#define Read_log_bmt(bank, lbn) read_dram_16 (LOG_BMT_ADDR + ((bank * LOG_BLK_PER_BANK + lbn) * sizeof (UINT16)))

void increaseLpnColdBlk (UINT32 const bank, LogCtrlBlock * ctrlBlock);
void increaseLpnColdBlkReused (UINT32 const bank, LogCtrlBlock * ctrlBlock);
void increaseLpnHotBlkFirstUsage (UINT32 const bank, LogCtrlBlock * ctrlBlock);
void increaseLpnHotBlkSecondUsage (UINT32 const bank, LogCtrlBlock * ctrlBlock);
static void findNewLpnForColdLog(const UINT32 bank, LogCtrlBlock * ctrlBlock);
BOOL8 canReuseLowPage(const UINT32 bank, const UINT32 pageOffset, LogCtrlBlock * ctrlBlock);
static BOOL8 reuseCondition(UINT32 bank);
#if AlwaysReuse
static BOOL8 reuseConditionHot(UINT32 bank);
#endif

// set log vbn to log block mapping table
void set_log_vbn (UINT32 const bank, UINT32 const log_lbn, UINT32 const vblock)
{
    uart_print("set_log_vbn(bank="); uart_print_int(bank);
    uart_print(", log_lbn="); uart_print_int(log_lbn);
    uart_print(", vblock="); uart_print_int(vblock);
    uart_print(")\r\n");
    Write_log_bmt(bank, log_lbn, vblock);
    //write_dram_16 (LOG_BMT_ADDR + ((bank * LOG_BLK_PER_BANK + log_lbn) * sizeof (UINT16)), vblock);
}

// get log vbn from log block mapping table
UINT32 get_log_vbn (UINT32 const bank, UINT32 const logLbn)
{
    uart_printf("get_log_vbn(bank=%d, log_lbn=%d)", bank, log_lbn);
    uart_printf("\treading BMT: log_lbn=%d=>vbn=%d", log_lbn, Read_log_bmt(bank, log_lbn));
#if OPTION_DEBUG_LOG
    if (logLbn < LOG_BLK_PER_BANK)
        return Read_log_bmt(bank, logLbn);
    return INVALID;
#else
    return Read_log_bmt(bank, logLbn);
#endif
}

void initLog()
{

    uart_print("Initializing Write Log Space...\r\n");
    uart_print("Initializing clean list...");
    //testCleanList();
    cleanListInit(&cleanListDataWrite, CleanList(0), LOG_BLK_PER_BANK);
    uart_print("done\r\n");

    //int off = __builtin_offsetof(LogCtrlBlock, increaseLpn);

    for(int bank=0; bank<NUM_BANKS; bank++)
    {
        adaptiveStepDown[bank] = initStepDown;
        adaptiveStepUp[bank] = initStepUp;
        nStepUps[bank] = 0;
        nStepDowns[bank] = 0;

        for(int lbn=0; lbn<LOG_BLK_PER_BANK; lbn++)
        {
            cleanListPush(&cleanListDataWrite, bank, lbn);
        }

        UINT32 lbn = cleanListPop(&cleanListDataWrite, bank);

        hotLogCtrl[bank] = (LogCtrlBlock)
        {
            .logLpn = lbn * PAGES_PER_BLK,
            .lpnsListAddr = LPNS_BUF_BASE_1(bank),
            .logBufferAddr = HOT_LOG_BUF(bank),
            .chunkPtr = 0,
            .increaseLpn=increaseLpnHotBlkFirstUsage,
            .updateChunkPtr=updateChunkPtr,
            .nextLowPageOffset=INVALID,
            .allChunksInLogAreValid = TRUE,
            .useRecycledPage=FALSE,
            .precacheDone=TRUE,
        };

        for(int chunk=0; chunk<CHUNKS_PER_PAGE; ++chunk)
        {
            hotLogCtrl[bank].dataLpn[chunk] = INVALID;
            hotLogCtrl[bank].chunkIdx[chunk] = INVALID;
        }

        lbn = cleanListPop(&cleanListDataWrite, bank);

        coldLogCtrl[bank] = (LogCtrlBlock)
        {
            .logLpn = lbn * PAGES_PER_BLK,
            .lpnsListAddr = LPNS_BUF_BASE_2(bank),
            .logBufferAddr = COLD_LOG_BUF(bank),
            .chunkPtr = 0,
            .increaseLpn=increaseLpnColdBlk,
            .updateChunkPtr=updateChunkPtr,
            .nextLowPageOffset=INVALID,
            .allChunksInLogAreValid = TRUE,
            .useRecycledPage=FALSE,
            .precacheDone=TRUE,
        };
        for(int chunk=0; chunk<CHUNKS_PER_PAGE; ++chunk)
        {
            coldLogCtrl[bank].dataLpn[chunk] = INVALID;
            coldLogCtrl[bank].chunkIdx[chunk] = INVALID;
        }

        nValidChunksFromHeap[bank] = INVALID;
    }
}

static void findNewLpnForColdLog(const UINT32 bank, LogCtrlBlock * ctrlBlock)
{
    uart_print("findNewLpnForColdLog bank "); uart_print_int(bank);

    if (cleanListSize(&cleanListDataWrite, bank) > 2)
    {
        uart_print(" use clean blk\r\n");
        uart_print("cleanList size = "); uart_print_int(cleanListSize(&cleanListDataWrite, bank)); uart_print("\r\n");

        UINT32 lbn = cleanListPop(&cleanListDataWrite, bank);
        ctrlBlock[bank].logLpn = lbn * PAGES_PER_BLK;
        ctrlBlock[bank].increaseLpn = increaseLpnColdBlk;
    }
    else
    {
        if (reuseCondition(bank))
        {
#if PrintStats
            uart_print_level_1("REUSECOLD\r\n");
#endif
            uart_print(" second usage\r\n");
            UINT32 lbn = getVictim(&heapDataFirstUsage, bank);
            UINT32 nValidChunks = getVictimValidPagesNumber(&heapDataFirstUsage, bank);
            resetValidChunksAndRemove(&heapDataFirstUsage, bank, lbn, CHUNKS_PER_LOG_BLK_FIRST_USAGE);
            resetValidChunksAndRemove(&heapDataSecondUsage, bank, lbn, CHUNKS_PER_LOG_BLK_SECOND_USAGE);
            resetValidChunksAndRemove(&heapDataCold, bank, lbn, nValidChunks);
            ctrlBlock[bank].logLpn = (lbn * PAGES_PER_BLK) + 2;
            ctrlBlock[bank].increaseLpn = increaseLpnColdBlkReused;
            nand_page_ptread(bank,
                             get_log_vbn(bank, lbn),
                             125,
                             0,
                             (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR,
                             ctrlBlock[bank].lpnsListAddr,
                             RETURN_WHEN_DONE); // Read the lpns list from the max low page (125) where it was previously written by incrementLpnHotBlkFirstUsage

        }
        else
        {
            uart_print(" get new block\r\n");
            UINT32 lbn = cleanListPop(&cleanListDataWrite, bank);
            ctrlBlock[bank].logLpn = lbn * PAGES_PER_BLK;
            ctrlBlock[bank].increaseLpn = increaseLpnColdBlk;
            while(cleanListSize(&cleanListDataWrite, bank) < 2)
            {
#if PrintStats
                uart_print_level_1("GCCOLD\r\n");
#endif
                garbageCollectLog(bank);
            }
        }
    }
}

void increaseLpnColdBlkReused (UINT32 const bank, LogCtrlBlock * ctrlBlock)
{
    uart_print("increaseLpnColdBlkReused bank "); uart_print_int(bank); uart_print("\r\n");

    UINT32 lpn = ctrlBlock[bank].logLpn;
    UINT32 pageOffset = LogPageToOffset(lpn);

    if (pageOffset == UsedPagesPerLogBlk-1)
    {
        UINT32 lbn = get_log_lbn(lpn);
        nand_page_ptprogram(bank,
                            get_log_vbn(bank, lbn),
                            PAGES_PER_BLK - 1,
                            0,
                            (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR,
                            ctrlBlock[bank].lpnsListAddr,
                            RETURN_WHEN_DONE);
        mem_set_dram(ctrlBlock[bank].lpnsListAddr, INVALID, (CHUNKS_PER_BLK * CHUNK_ADDR_BYTES));
        insertBlkInHeap(&heapDataCold, bank, lbn);

        findNewLpnForColdLog(bank, ctrlBlock);
    }
    else
    {
        ctrlBlock[bank].logLpn = lpn+2;
    }

    uart_print("increaseLpnColdBlkReused (bank="); uart_print_int(bank); uart_print(") new lpn "); uart_print_int(ctrlBlock[bank].logLpn); uart_print("\r\n");
}

void increaseLpnColdBlk (UINT32 const bank, LogCtrlBlock * ctrlBlock)
{

    uart_print("increaseLpnColdBlk\r\n");

    UINT32 lpn = ctrlBlock[bank].logLpn;

    if (LogPageToOffset(lpn) == UsedPagesPerLogBlk-1)
    { // current rw log block is full

        UINT32 lbn = get_log_lbn(lpn);
        nand_page_ptprogram(bank,
                            get_log_vbn(bank, lbn),
                            PAGES_PER_BLK - 1,
                            0,
                            (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR,
                            ctrlBlock[bank].lpnsListAddr,
                            RETURN_WHEN_DONE);
        mem_set_dram(ctrlBlock[bank].lpnsListAddr, INVALID, (CHUNKS_PER_BLK * CHUNK_ADDR_BYTES));
        insertBlkInHeap(&heapDataCold, bank, lbn);

#if CanReuseBlksForColdData == 0
        lbn = cleanListPop(&cleanListDataWrite, bank); // Now the hybrid approach can pop from the cleanList
        ctrlBlock[bank].logLpn = lbn * PAGES_PER_BLK;

        while(cleanListSize(&cleanListDataWrite, bank) < 2)
        {
#if PrintStats
            uart_print_level_1("GCCOLD\r\n");
#endif
            garbageCollectLog(bank);
        }
#else
        findNewLpnForColdLog(bank, ctrlBlock);
#endif
    }

    else
    {
        ctrlBlock[bank].logLpn = lpn+1;
    }
    uart_print("increaseLpnColdBlk new lpn "); uart_print_int(ctrlBlock[bank].logLpn); uart_print("\r\n");
}

void printValidChunksInFirstUsageBlk(const UINT32 bank, const LogCtrlBlock * ctrlBlock, const UINT32 lbn)
{
    UINT32 validChunks[9] = {0,0,0,0,0,0,0,0,0};
    for(UINT32 pageOffset=0; pageOffset < 125; )
    {
        uart_print("readPage: bank="); uart_print_int(bank); uart_print(" ");
        uart_print("pageOffset="); uart_print_int(pageOffset);

        UINT32 nValidChunksInPage=0;

        UINT32 victimLpns[CHUNKS_PER_PAGE];
        mem_copy(victimLpns, ctrlBlock[bank].lpnsListAddr+(pageOffset*CHUNKS_PER_PAGE)*CHUNK_ADDR_BYTES, CHUNKS_PER_PAGE * sizeof(UINT32));


        UINT32 logChunkAddr = (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (lbn*CHUNKS_PER_BLK) + (pageOffset*CHUNKS_PER_PAGE);

        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
        {   // This loops finds valid chunks is the page. Note that chunks in GC Buf won't be considered as they temporarily don't occupy space in Log
            UINT32 victimLpn = victimLpns[chunkOffset];
            if (victimLpn != INVALID)
            {
                UINT32 i = mem_search_equ_dram_4_bytes(ChunksMapTable(victimLpn, 0), CHUNKS_PER_PAGE, logChunkAddr);

                if(i<CHUNKS_PER_PAGE)
                {
                    nValidChunksInPage++;
                }
            }
            logChunkAddr++;
        }
        validChunks[nValidChunksInPage]++;

        if (pageOffset == 0)
        {
            pageOffset++;
        }
        else
        {
            pageOffset += 2;
        }
    }

#if PrintStats
    uart_print_level_1("REUSE ");
    for(UINT32 i=0; i<9; ++i)
    {
        uart_print_level_1_int(validChunks[i]);
        uart_print_level_1(" ");
    }
    uart_print_level_1("\r\n");
#endif

}

#if AlwaysReuse
static BOOL8 reuseConditionHot(UINT32 bank)
{
    if (getVictimValidPagesNumber(&heapDataFirstUsage, bank) == 63*CHUNKS_PER_PAGE)
    {
#if PrintStats
        uart_print_level_1("FIRSTHOTEMPTY\r\n");
#endif
        return TRUE;
    }

    //if (getVictimValidPagesNumber(&heapDataFirstUsage, bank) == 63*CHUNKS_PER_PAGE)
    if (heapDataFirstUsage.nElInHeap[bank] > 1)
    {
        UINT32 validPagesSecondUsage = getVictimValidPagesNumber(&heapDataSecondUsage, bank);
        UINT32 validPagesCold = getVictimValidPagesNumber(&heapDataCold, bank);
        UINT32 validPagesMin = (validPagesCold < validPagesSecondUsage) ? validPagesCold : validPagesSecondUsage;
        if ( (getVictimValidPagesNumber(&heapDataFirstUsage, bank) - 63*CHUNKS_PER_PAGE) < validPagesMin)
        { return TRUE; }
        else
        { return FALSE; }
    }
    else
    { return FALSE; }
}
#endif


static BOOL8 reuseCondition(UINT32 bank)
{
#if AlwaysReuse
    //if (getVictimValidPagesNumber(&heapDataFirstUsage, bank) == 63*CHUNKS_PER_PAGE)
    if (heapDataFirstUsage.nElInHeap[bank] > 1)
    {
        UINT32 validPagesSecondUsage = getVictimValidPagesNumber(&heapDataSecondUsage, bank);
        UINT32 validPagesCold = getVictimValidPagesNumber(&heapDataCold, bank);
        UINT32 validPagesMin = (validPagesCold < validPagesSecondUsage) ? validPagesCold : validPagesSecondUsage;
        if ( (getVictimValidPagesNumber(&heapDataFirstUsage, bank) - 63*CHUNKS_PER_PAGE) < validPagesMin)
        { return TRUE; }
        else
        { return FALSE; }
    }
    else
    { return FALSE; }
#endif


#if PrintStats
    uart_print_level_1("reuseCondition "); uart_print_level_1_int(bank); uart_print_level_1("\r\n");

    uart_print_level_1("Valid chunks ");
    uart_print_level_1_int(getVictimValidPagesNumber(&heapDataFirstUsage, bank));
    uart_print_level_1("\r\n");
#endif

    //if (getVictimValidPagesNumber(&heapDataFirstUsage, bank) == 62*CHUNKS_PER_PAGE)
    if (getVictimValidPagesNumber(&heapDataFirstUsage, bank) == 63*CHUNKS_PER_PAGE)
    {
#if PrintStats
        uart_print_level_1("FIRSTHOTEMPTY\r\n");
#endif
        return TRUE;
    }
    UINT32 validCold = getVictimValidPagesNumber(&heapDataCold, bank);
    UINT32 validSecond = getVictimValidPagesNumber(&heapDataSecondUsage, bank);
    UINT32 validMin=0;
    if (validCold < ((validSecond*secondHotFactorNum)/secondHotFactorDen))
    { validMin = validCold; }
    else
    { validMin = validSecond; }

    float tot = 0.0;
#if PrintStats
    uart_print_level_1("AW ");
    uart_print_level_1_int(bank);
#endif
    for (int i=0; i<adaptiveWindowSize; ++i)
    {
#if PrintStats
        uart_print_level_1(" ");
        uart_print_level_1_int(adaptiveWindow[bank][i]);
#endif
        tot += adaptiveWindow[bank][i];
    }
#if PrintStats
    uart_print_level_1("\r\n");
#endif
    tot /= adaptiveWindowSize;

#if PrintStats
    uart_print_level_1("CHANGETH ");
    uart_print_level_1_int(bank);
    uart_print_level_1(" ");
    uart_print_level_1_int(validMin);
    uart_print_level_1(" ");
    uart_print_level_1_int(tot);
    uart_print_level_1("\r\n");
#endif

    if ( (float)validMin != tot)
    {
        if ( (float)validMin < tot)
        { // Step up
            //adaptiveStepDown[bank] = initStepDown;
            nStepDowns[bank]=0;
            nStepUps[bank]++;
            if (nStepUps[bank] == maxStepUps)
            {
                nStepUps[bank]=0;
                if (hotFirstAccumulated[bank] < 112)
                {
                    hotFirstAccumulated[bank] += adaptiveStepUp[bank];
                }

                //if (adaptiveStepUp[bank]>1)
                //{ adaptiveStepUp[bank]--; }

#if PrintStats
                uart_print_level_1("ASU ");
                uart_print_level_1_int(bank);
                uart_print_level_1(" ");
                uart_print_level_1_int(adaptiveStepUp[bank]);
                uart_print_level_1("\r\n");
#endif
            }
        }

        else
        { // Step down
            //adaptiveStepUp[bank] = initStepUp;
            nStepUps[bank]=0;
            nStepDowns[bank]++;
            if (nStepDowns[bank] == maxStepDowns)
            {
                nStepDowns[bank]=0;

                if (hotFirstAccumulated[bank] > 5)
                {
                    if (hotFirstAccumulated[bank] < adaptiveStepDown[bank])
                    { hotFirstAccumulated[bank] = 0; }
                    else
                    { hotFirstAccumulated[bank] -= adaptiveStepDown[bank]; }
                }

                //if (adaptiveStepDown[bank] > 1)
                //{ adaptiveStepDown[bank]--; }
#if PrintStats
                uart_print_level_1("ASD ");
                uart_print_level_1_int(bank);
                uart_print_level_1(" ");
                uart_print_level_1_int(adaptiveStepDown[bank]);
                uart_print_level_1("\r\n");
#endif
            }
        }
    }

#if PrintStats
    uart_print_level_1("HotFirstAccumulated "); uart_print_level_1_int(bank); uart_print_level_1(" "); uart_print_level_1_int(hotFirstAccumulated[bank]); uart_print_level_1("\r\n");

    uart_print_level_1("ValidMin=");
    uart_print_level_1_int(validMin);
    uart_print_level_1(" tot=");
    uart_print_level_1_int(tot);
#endif
    if (heapDataFirstUsage.nElInHeap[bank] > hotFirstAccumulated[bank])
    //if ((heapDataFirstUsage.nElInHeap[bank] > 0) && ((float)validMin > tot) )
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

void findNewLpnForHotLog(const UINT32 bank, LogCtrlBlock * ctrlBlock)
{
    uart_print("findNewLpnForHotLog bank "); uart_print_int(bank);

    if (cleanListSize(&cleanListDataWrite, bank) > 2)
    {
        uart_print(" use clean blk\r\n");

        uart_print("cleanList size = "); uart_print_int(cleanListSize(&cleanListDataWrite, bank)); uart_print("\r\n");


        UINT32 lbn = cleanListPop(&cleanListDataWrite, bank);
        ctrlBlock[bank].logLpn = lbn * PAGES_PER_BLK;
        ctrlBlock[bank].increaseLpn = increaseLpnHotBlkFirstUsage; // we are not using a recycled block anymore
        ctrlBlock[bank].updateChunkPtr = updateChunkPtr; // we are not using a recycled block anymore
        ctrlBlock[bank].useRecycledPage = FALSE;

    }
    else
    {
        //if ((heapDataFirstUsage.nElInHeap[bank] > 0) && ((float)validMin > tot) )
        //if (heapDataFirstUsage.nElInHeap[bank] > hotFirstAccumulated[bank])
#if AlwaysReuse
        if(reuseConditionHot(bank))
#else
        if(reuseCondition(bank))
#endif
        {
#if PrintStats
            uart_print_level_1("REUSEHOT\r\n");
#endif

            uart_print(" second usage\r\n");

            UINT32 lbn = getVictim(&heapDataFirstUsage, bank);
            UINT32 nValidChunks = getVictimValidPagesNumber(&heapDataFirstUsage, bank);
            resetValidChunksAndRemove(&heapDataFirstUsage, bank, lbn, CHUNKS_PER_LOG_BLK_FIRST_USAGE);
            //resetValidChunksAndRemove(&heapDataSecondUsage, bank, lbn, CHUNKS_PER_LOG_BLK_SECOND_USAGE);
            resetValidChunksAndRemove(&heapDataSecondUsage, bank, lbn, nValidChunks);
            resetValidChunksAndRemove(&heapDataCold, bank, lbn, CHUNKS_PER_LOG_BLK_SECOND_USAGE);
            ctrlBlock[bank].logLpn = lbn * PAGES_PER_BLK;
            ctrlBlock[bank].increaseLpn = increaseLpnHotBlkSecondUsage;
            ctrlBlock[bank].updateChunkPtr = updateChunkPtrRecycledPage;
            nand_page_ptread(bank,
                             get_log_vbn(bank, lbn),
                             125,
                             0,
                             (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR,
                             ctrlBlock[bank].lpnsListAddr,
                             RETURN_WHEN_DONE); // Read the lpns list from the max low page (125) where it was previously written by incrementLpnHotBlkFirstUsage

            printValidChunksInFirstUsageBlk(bank, ctrlBlock, lbn);

            if (canReuseLowPage(bank, 0, ctrlBlock))
            { // Reuse page 0 prefetching immediately
                precacheLowPage(bank, ctrlBlock);
                ctrlBlock[bank].updateChunkPtr = updateChunkPtrRecycledPage;
                ctrlBlock[bank].useRecycledPage = TRUE;
                ctrlBlock[bank].precacheDone = TRUE;
                ctrlBlock[bank].nextLowPageOffset = 0;
                return;
            }
            ctrlBlock[bank].logLpn++;

            if (canReuseLowPage(bank, 1, ctrlBlock))
            { // Reuse page 1 prefetching immediately
                precacheLowPage(bank, ctrlBlock);
                ctrlBlock[bank].updateChunkPtr = updateChunkPtrRecycledPage;
                ctrlBlock[bank].useRecycledPage = TRUE;
                ctrlBlock[bank].precacheDone = TRUE;
                ctrlBlock[bank].nextLowPageOffset = 1;
                return;
            }
            else
            {
                ctrlBlock[bank].updateChunkPtr = updateChunkPtr;
                ctrlBlock[bank].useRecycledPage = FALSE;
                ctrlBlock[bank].precacheDone = FALSE;
                ctrlBlock[bank].nextLowPageOffset = INVALID;
                increaseLpnHotBlkSecondUsage(bank, ctrlBlock);
            }

        }
        else
        {

            uart_print(" get new block\r\n");
            uart_print("No blks left for second usage\r\n");
            UINT32 lbn = cleanListPop(&cleanListDataWrite, bank);
            ctrlBlock[bank].logLpn = lbn * PAGES_PER_BLK;
            ctrlBlock[bank].increaseLpn = increaseLpnHotBlkFirstUsage; // we are not using a recycled block anymore
            ctrlBlock[bank].updateChunkPtr = updateChunkPtr; // we are not using a recycled block anymore
            ctrlBlock[bank].useRecycledPage = FALSE;

            while(cleanListSize(&cleanListDataWrite, bank) < cleanBlksAfterGcHot)
            {
#if PrintStats
                uart_print_level_1("GCHOT\r\n");
#endif
                garbageCollectLog(bank);
            }
        }
    }
}


BOOL8 canReuseLowPage(const UINT32 bank, const UINT32 pageOffset, LogCtrlBlock * ctrlBlock)
{
    //uart_print_level_1("canReuseLowPage ");
    //uart_print_level_1_int(bank);
    //uart_print_level_1(" ");
    //uart_print_level_1_int(pageOffset);
    //uart_print_level_1("\r\n");

    UINT32 lbn = LogPageToLogBlk(ctrlBlock[bank].logLpn);
    //UINT32 vbn = get_log_vbn(bank, lbn);
    UINT32 victimLpns[CHUNKS_PER_PAGE];
    mem_copy(victimLpns, ctrlBlock[bank].lpnsListAddr + (pageOffset * CHUNKS_PER_PAGE * sizeof(UINT32)), CHUNKS_PER_PAGE * sizeof(UINT32));

    UINT32 logChunkAddr = (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (lbn*CHUNKS_PER_BLK) + (pageOffset*CHUNKS_PER_PAGE);

    UINT32 dataChunkOffsets[CHUNKS_PER_PAGE];
    UINT32 dataLpns[CHUNKS_PER_PAGE];
    UINT32 validChunks[CHUNKS_PER_PAGE];
    UINT32 nValidChunksInPage = 0;

    for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
    { validChunks[chunkOffset] = FALSE; }

    for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
    {   // This loops finds valid chunks is the page.

        uart_print("chunkOffset "); uart_print_int(chunkOffset);

        UINT32 victimLpn = victimLpns[chunkOffset];
        if (victimLpn != INVALID)
        {
            UINT32 i = mem_search_equ_dram_4_bytes(ChunksMapTable(victimLpn, 0), CHUNKS_PER_PAGE, logChunkAddr);

            if(i<CHUNKS_PER_PAGE)
            {
                dataChunkOffsets[chunkOffset]=i;
                dataLpns[chunkOffset]=victimLpn;
                validChunks[chunkOffset]=TRUE;
                nValidChunksInPage++;
                uart_print(" valid\r\n");
            }
            else
            {
                uart_print(" somewhere else\r\n");
            }
        }
        else
        {
            uart_print(" invalid\r\n");
        }
        logChunkAddr++;
    }

    if (nValidChunksInPage == 0)
    {

        // note(fabio): this will be done in precacheLowPage
        //nand_page_ptread(bank, vbn, pageOffset, 0, SECTORS_PER_PAGE, PrecacheForEncoding(bank), RETURN_ON_ISSUE);

        // note(fabio): cannot use mem_set_dram here because we want to clear only 32B while the minimum allowed is 128
        //mem_set_dram(ctrlBlock[bank].lpnsListAddr + (pageOffset * CHUNKS_PER_PAGE * sizeof(UINT32)), INVALID, (CHUNKS_PER_PAGE * sizeof(UINT32)));
        UINT32 addrToClear = (ctrlBlock[bank].lpnsListAddr + (pageOffset * CHUNKS_PER_PAGE * sizeof(UINT32)));
        for (UINT32 i=0; i<CHUNKS_PER_PAGE; ++i)
        {
            write_dram_32(addrToClear + (i * sizeof(UINT32)), INVALID);
        }
        //mem_set_dram(ctrlBlock[bank].lpnsListAddr + (pageOffset * CHUNKS_PER_PAGE * sizeof(UINT32)), INVALID, (CHUNKS_PER_PAGE * sizeof(UINT32)));
        incrementValidChunksByN(&heapDataSecondUsage, bank, lbn, CHUNKS_PER_PAGE);
        return TRUE;
    }

    if (nValidChunksInPage < nValidChunksInPageToReuseThreshold)
    {
#if PrintStats
        uart_print_level_1("k\r\n");
#endif

        // note(fabio): this will be done in precacheLowPage
        //nand_page_ptread(bank, vbn, pageOffset, 0, SECTORS_PER_PAGE, PrecacheForEncoding(bank), RETURN_WHEN_DONE);

        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
        {
            if(validChunks[chunkOffset])
            {
                writeChunkOnLogBlockDuringGC(bank,
                                             dataLpns[chunkOffset],
                                             dataChunkOffsets[chunkOffset],
                                             chunkOffset,
                                             PrecacheForEncoding(bank));
            }
        }
        //mem_set_dram(ctrlBlock[bank].lpnsListAddr + (pageOffset * CHUNKS_PER_PAGE * sizeof(UINT32)), INVALID, (CHUNKS_PER_PAGE * sizeof(UINT32)));
        UINT32 addrToClear = (ctrlBlock[bank].lpnsListAddr + (pageOffset * CHUNKS_PER_PAGE * sizeof(UINT32)));
        for (UINT32 i=0; i<CHUNKS_PER_PAGE; ++i)
        {
            write_dram_32(addrToClear + (i * sizeof(UINT32)), INVALID);
        }
        incrementValidChunksByN(&heapDataSecondUsage, bank, lbn, CHUNKS_PER_PAGE - nValidChunksInPage);
        return TRUE;
    }
    return FALSE;
}


void precacheLowPage(const UINT32 bank, LogCtrlBlock * ctrlBlock)
{

    //uart_print_level_1("PRECACHE ");
    //uart_print_level_1_int(bank);
    //uart_print_level_1(" ");
    //uart_print_level_1_int(ctrlBlock[bank].nextLowPageOffset);
    //uart_print_level_1("\r\n");


    UINT32 lbn = LogPageToLogBlk(ctrlBlock[bank].logLpn);
    UINT32 vbn = get_log_vbn(bank, lbn);
    UINT32 pageOffset = ctrlBlock[bank].nextLowPageOffset;

    uart_print("precacheLowPage: pageOffset ");
    uart_print_int(pageOffset);
    uart_print("\r\n");

    nand_page_ptread(bank, vbn, pageOffset, 0, SECTORS_PER_PAGE, PrecacheForEncoding(bank), RETURN_ON_ISSUE);
    ctrlBlock[bank].precacheDone = TRUE;
}

void increaseLpnHotBlkSecondUsage (UINT32 const bank, LogCtrlBlock * ctrlBlock)
{
    uart_print("increaseLpnHotBlkSecondUsage\r\n");

    UINT32 lpn = ctrlBlock[bank].logLpn;
    UINT32 pageOffset = LogPageToOffset(lpn);

    if (pageOffset == UsedPagesPerLogBlk-1)
    {
        uart_print("Blk full\r\n");
        UINT32 lbn = LogPageToLogBlk(lpn);
        UINT32 vbn = get_log_vbn(bank, lbn);
        nand_page_ptprogram(bank,
                            vbn,
                            PAGES_PER_BLK - 1,
                            0,
                            (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR,
                            ctrlBlock[bank].lpnsListAddr,
                            RETURN_WHEN_DONE); // write lpns list to the last high page
        mem_set_dram(ctrlBlock[bank].lpnsListAddr, INVALID, (CHUNKS_PER_BLK * CHUNK_ADDR_BYTES));
        insertBlkInHeap(&heapDataSecondUsage, bank, lbn);

        findNewLpnForHotLog(bank, ctrlBlock);
    }

    else
    {
        lpn++;
        ctrlBlock[bank].logLpn = lpn;
        pageOffset++;

        //uart_print_level_1("increaseLpnHotBlkSecondUsage ");
        //uart_print_level_1_int(bank);
        //uart_print_level_1(" ");
        //uart_print_level_1_int(pageOffset);
        //uart_print_level_1("\r\n");

        if (pageOffset % 2 == 1)
        { // Next page is low

            if (ctrlBlock[bank].nextLowPageOffset == pageOffset)
            { // The page tested positively
                // Here we don't care if the page has already been prefetched because this can be done asyncronously
                ctrlBlock[bank].updateChunkPtr = updateChunkPtrRecycledPage;
                ctrlBlock[bank].useRecycledPage = TRUE;
            }

            else
            {

                if (pageOffset == 1)
                { // Special case: pageOffset 1 comes immediately after another low page, so there was no time for precaching
                    if(canReuseLowPage(bank, pageOffset, ctrlBlock))
                    {
                        ctrlBlock[bank].updateChunkPtr = updateChunkPtrRecycledPage;
                        ctrlBlock[bank].useRecycledPage = TRUE;
                        ctrlBlock[bank].precacheDone = FALSE;
                        ctrlBlock[bank].nextLowPageOffset = pageOffset;
                        return;
                    }
                }

                // Skip this page because it tested negatively

                // Set the next page to the next high page
                ctrlBlock[bank].updateChunkPtr = updateChunkPtr;
                ctrlBlock[bank].useRecycledPage = FALSE;
                lpn++;
                pageOffset++;
                ctrlBlock[bank].logLpn = lpn;

                // Already test the next low page
                pageOffset++;
                if (pageOffset < UsedPagesPerLogBlk-1)
                {
                    if(canReuseLowPage(bank, pageOffset, ctrlBlock))
                    {
                        ctrlBlock[bank].precacheDone = FALSE;
                        ctrlBlock[bank].nextLowPageOffset = pageOffset;
                    }
                    else
                    {
                        ctrlBlock[bank].precacheDone = FALSE;
                        ctrlBlock[bank].nextLowPageOffset = INVALID;
                    }
                }

            }
        }

        else
        { // Next page is high
            ctrlBlock[bank].updateChunkPtr = updateChunkPtr;
            ctrlBlock[bank].useRecycledPage = FALSE;

            // Already test the next low page
            pageOffset++;
            if (pageOffset < UsedPagesPerLogBlk-1)
            {
                if(canReuseLowPage(bank, pageOffset, ctrlBlock))
                {
                    ctrlBlock[bank].precacheDone = FALSE;
                    ctrlBlock[bank].nextLowPageOffset = pageOffset;
                }
                else
                {
                    ctrlBlock[bank].precacheDone = FALSE;
                    ctrlBlock[bank].nextLowPageOffset = INVALID;
                }
            }
            else
            {
                ctrlBlock[bank].precacheDone = FALSE;
                ctrlBlock[bank].nextLowPageOffset = INVALID;
            }
        }
    }

    uart_print("New logLpn "); uart_print_int(ctrlBlock[bank].logLpn);
    uart_print(" offset "); uart_print_int(LogPageToOffset(ctrlBlock[bank].logLpn)); uart_print("\r\n");
}

void increaseLpnHotBlkFirstUsage (UINT32 const bank, LogCtrlBlock * ctrlBlock)
{
    uart_print("increaseLpnHotBlkFirstUsage\r\n");

    UINT32 lpn = ctrlBlock[bank].logLpn;
    UINT32 pageOffset = LogPageToOffset(lpn);

    if (pageOffset == 123)
    { // current rw log block is full. Write lpns list in the highest low page (125)
        uart_print("Blk full\r\n");
        UINT32 lbn = get_log_lbn(lpn);
        nand_page_ptprogram(bank,
                            get_log_vbn(bank, lbn),
                            125,
                            0,
                            (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR,
                            ctrlBlock[bank].lpnsListAddr,
                            RETURN_WHEN_DONE);
        mem_set_dram(ctrlBlock[bank].lpnsListAddr, INVALID, (CHUNKS_PER_BLK * CHUNK_ADDR_BYTES));
        insertBlkInHeap(&heapDataFirstUsage, bank, lbn);

        findNewLpnForHotLog(bank, ctrlBlock);
    }

    else
    {
        if(pageOffset == 0)
        {
            ctrlBlock[bank].logLpn = lpn+1;
        }
        else
        {
            ctrlBlock[bank].logLpn = lpn+2;
        }
    }
}

UINT32 getLpnForCompletePage(const UINT32 bank, LogCtrlBlock * ctrlBlock)
{
    uart_print("getLpnForCompletePage bank "); uart_print_int(bank); uart_print("\r\n");

    if (ctrlBlock[bank].useRecycledPage)
    {
        UINT32 pageOffset = LogPageToOffset(ctrlBlock[bank].logLpn);
        if (pageOffset == 0)
        {
#if PrintStats
            uart_print_level_1("-\r\n");
#endif
            decrementValidChunksByN(&heapDataSecondUsage, bank, LogPageToLogBlk(ctrlBlock[bank].logLpn), CHUNKS_PER_PAGE);
            ctrlBlock[bank].logLpn = ctrlBlock[bank].logLpn + 2;
        }
        else
        {
            if (pageOffset % 2 == 1)
            {
#if PrintStats
                uart_print_level_1("-\r\n");
#endif
                decrementValidChunksByN(&heapDataSecondUsage, bank, LogPageToLogBlk(ctrlBlock[bank].logLpn), CHUNKS_PER_PAGE);
                ctrlBlock[bank].logLpn ++;
            }
        }
    }
    return ctrlBlock[bank].logLpn;
}

chunkLocation findChunkLocation(const UINT32 chunkAddr)
{
    if (chunkAddr==INVALID)
    {
        return Invalid;
    }
    if ( (chunkAddr & ColdLogBufBitFlag) > 0)
    {
        UINT32 realChunkAddr = chunkAddr & ~(ColdLogBufBitFlag);
        UINT32 chunkOffset = ChunkToChunkOffsetInBank(realChunkAddr);
        if (chunkOffset >= DramLogBufLpn * CHUNKS_PER_PAGE)
        {
            return DRAMColdLog;
        }
        else
        {
            return FlashWLogEncoded;
        }
    }
    UINT32 chunkOffset = ChunkToChunkOffsetInBank(chunkAddr);
    if (chunkOffset >= DramLogBufLpn * CHUNKS_PER_PAGE)
    {
        return DRAMHotLog;
    }
    return FlashWLog;
}
