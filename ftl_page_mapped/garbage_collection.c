#include "garbage_collection.h"
#include "dram_layout.h"
#include "ftl_parameters.h"
#include "ftl_metadata.h"
#include "log.h"
#include "heap.h"
#include "cleanList.h"
#include "write.h"

#include <stdio.h>

void initGC(UINT32 bank);
void initNewNestLevel(UINT32 bank);
void readPage(UINT32 bank);
void readPageSingleStep(UINT32 bank);
void writePage(UINT32 bank);

UINT32 dataChunkOffsets[NUM_BANKS][CHUNKS_PER_PAGE];
UINT32 dataLpns[NUM_BANKS][CHUNKS_PER_PAGE];
UINT32 validChunks[NUM_BANKS][CHUNKS_PER_PAGE];
UINT32 nValidChunksInPage[NUM_BANKS];
UINT32 nValidChunksInBlk[NUM_BANKS];

UINT32 nValidChunksFromHeap[NUM_BANKS];
UINT32 victimVbn[NUM_BANKS];

UINT8 pageOffset[NUM_BANKS];
UINT8 gcOnRecycledPage[NUM_BANKS];

void finishGC()
{
    while(1)
    {
        int completedBanks = 0;
        for (UINT32 bank=0; bank < NUM_BANKS; ++bank)
        {
            uart_print("bank "); uart_print_int(bank); uart_print(" ");
            switch (gcState[bank])
            {

                case GcIdle:
                {
                    if(cleanListSize(&cleanListDataWrite, bank) < 2)
                    { initGC(bank); }
                    else
                    { completedBanks++;}
                    break;
                }

                case GcRead:
                {
                    uart_print("read\r\n");
                    readPage(bank);
                    break;
                }

                case GcWrite:
                {
                    uart_print("write\r\n");
                    writePage(bank);
                    break;
                }

                default:
                {
                    uart_print_level_1("ERROR in garbageCollectLog: on bank "); uart_print_level_1_int(bank);
                    uart_print_level_1(", undefined GC state: "); uart_print_level_1_int(gcState[bank]); uart_print_level_1("\r\n");
                    while(1);
                }
            }
        }
        if (completedBanks == NUM_BANKS)
        {
            uart_print_level_1("GC Completed!\r\n");
            break;
        }
    }
#if PrintStats
    UINT32 totSparePages = 0;
    for (UINT32 bank=0; bank < NUM_BANKS; ++bank)
    {
        uart_print_level_1("FINALCNT ");
        uart_print_level_1_int(bank);
        uart_print_level_1(" ");
        uart_print_level_1_int(cleanListSize(&cleanListDataWrite, bank));
        uart_print_level_1(" ");
        uart_print_level_1_int(heapDataFirstUsage.nElInHeap[bank]);
        uart_print_level_1(" ");
        uart_print_level_1_int(heapDataSecondUsage.nElInHeap[bank]);
        uart_print_level_1(" ");
        uart_print_level_1_int(heapDataCold.nElInHeap[bank]);

        uart_print_level_1(" H ");

        UINT32 lpn = LogPageToOffset(hotLogCtrl[bank].logLpn);
        if (hotLogCtrl[bank].useRecycledPage)
        { // second usage, add the low pages
            lpn = lpn + 62;
        }
        else
        { // first usage, only low pages have been used
            lpn = (lpn/2)+1;
        }
        uart_print_level_1_int(lpn);
        totSparePages += LogPageToOffset(lpn);

        uart_print_level_1(" C ");
        lpn = coldLogCtrl[bank].logLpn;
        uart_print_level_1_int(LogPageToOffset(lpn));
        totSparePages += LogPageToOffset(lpn);
        uart_print_level_1("\r\n");
    }
    uart_print_level_1("TOTSPAREPAGES ");
    uart_print_level_1_int(totSparePages);
    uart_print_level_1("\r\n");

    for (UINT32 bank=0; bank < NUM_BANKS; ++bank)
    {
        uart_print_level_1("PARTIALLYUSEDTH ");
        uart_print_level_1_int(bank);
        uart_print_level_1(" ");
        uart_print_level_1_int(hotFirstAccumulated[bank]);
        uart_print_level_1("\r\n");
    }
#endif
}

void garbageCollectLog(const UINT32 bank_)
{

    //UINT32 firstBank = (bank_ / NUM_CHANNELS) * NUM_CHANNELS;
    //UINT32 lastBank = firstBank + NUM_CHANNELS;
    //UINT32 firstBank = bank_;
    //UINT32 lastBank = firstBank + 1;

    UINT32 banks[NUM_CHANNELS];

    for (UINT32 i=0; i<NUM_CHANNELS; ++i)
    {
        banks[i] = INVALID;
    }

#if PrintStats
    uart_print_level_1("Choose banks: ");
#endif
    for (UINT32 i=0; i<NUM_CHANNELS; ++i)
    {
#if PrintStats
        uart_print_level_1("i="); uart_print_level_1_int(i);
#endif
        if ((bank_ % NUM_CHANNELS) == i)
        {
#if PrintStats
            uart_print_level_1(" bank_ = "); uart_print_level_1_int(bank_); uart_print_level_1("; ");
#endif
            banks[i] = bank_;
        }
        else
        {
            UINT32 bank = (bank_/NUM_CHANNELS)*NUM_CHANNELS + i;
            for (UINT32 column=bank_/NUM_CHANNELS; column < NUM_BANKS/NUM_CHANNELS; ++column)
            {
#if PrintStats
                uart_print_level_1(" try bank = "); uart_print_level_1_int(bank);
#endif
                if (cleanListSize(&cleanListDataWrite, bank)<CleanBlksBackgroundGcThreshold)
                {
                    banks[i]=bank;
                    break;
                }
                else
                {
#if PrintStats
                    uart_print_level_1(" cl = "); uart_print_level_1_int(cleanListSize(&cleanListDataWrite, bank));
#endif
                    bank = (bank + NUM_CHANNELS) % NUM_BANKS;
                }
            }
#if PrintStats
            uart_print_level_1("; ");
#endif
        }
    }
#if PrintStats
            uart_print_level_1("\r\n");
#endif

#if PrintStats
    // note(fabio): if uncomment the if statement it will print Clean banks only when a new GC is started,
    //              otherwise print it also when GC is resumed after allocating new cold blk
    //if (gcState[bank_] == GcIdle)
    //{
        uart_print_level_1("Clean banks ");
        for (UINT32 i=0; i<NUM_CHANNELS; ++i)
        {
            if(banks[i] != INVALID)
            {
                uart_print_level_1_int(banks[i]);
                uart_print_level_1(" ");
            }
        }
        uart_print_level_1("\r\n");
    //}
#endif

    uart_print("garbageCollectLog bank_="); uart_print_int(bank_); uart_print("\r\n");

    for (UINT32 bankIdx=0; bankIdx < NUM_CHANNELS; ++bankIdx)
    {
        if (banks[bankIdx] != INVALID)
        {
            if (gcState[banks[bankIdx]] == GcIdle)
            {
                //start_interval_measurement(TIMER_CH2, TIMER_PRESCALE_0);
                initGC(banks[bankIdx]);
                //UINT32 timerValue=GET_TIMER_VALUE(TIMER_CH2);
                //UINT32 nTicks = 0xFFFFFFFF - timerValue;
                //uart_print_level_1("i "); uart_print_level_1_int(nTicks); uart_print_level_1("\r\n");
            }
        }
    }

    for (UINT32 bankIdx=0; bankIdx < NUM_CHANNELS; ++bankIdx)
    {
        if (banks[bankIdx] != INVALID)
        {
            if (gcState[banks[bankIdx]] == GcRead)
            {
                waitBusyBank(banks[bankIdx]);
            }
        }
    }

    while(1)
    {
        for (UINT32 bankIdx=0; bankIdx < NUM_CHANNELS; ++bankIdx)
        {
            uart_print("bankIdx "); uart_print_int(bankIdx); uart_print(" ");
            uart_print("bank "); uart_print_int(banks[bankIdx]); uart_print(" ");
            if (banks[bankIdx] != INVALID)
            {
                switch (gcState[banks[bankIdx]])
                {

                    case GcIdle:
                    {

                        uart_print("idle\r\n");

                        if (banks[bankIdx] == bank_)
                        {
                            return;
                        }
                        break;
                    }

                    case GcRead:
                    {
                        uart_print("read\r\n");
                        //start_interval_measurement(TIMER_CH2, TIMER_PRESCALE_0);
                        readPage(banks[bankIdx]);
                        //UINT32 timerValue=GET_TIMER_VALUE(TIMER_CH2);
                        //UINT32 nTicks = 0xFFFFFFFF - timerValue;
                        //uart_print_level_1("r "); uart_print_level_1_int(nTicks); uart_print_level_1("\r\n");
                        break;
                    }

                    case GcWrite:
                    {
                        uart_print("write\r\n");
                        //start_interval_measurement(TIMER_CH2, TIMER_PRESCALE_0);
                        writePage(banks[bankIdx]);
                        //UINT32 timerValue=GET_TIMER_VALUE(TIMER_CH2);
                        //UINT32 nTicks = 0xFFFFFFFF - timerValue;
                        //uart_print_level_1("w "); uart_print_level_1_int(nTicks); uart_print_level_1("\r\n");
                        break;
                    }

                    default:
                    {
                        uart_print_level_1("ERROR in garbageCollectLog: on bankIdx "); uart_print_level_1_int(bankIdx);
                        uart_print_level_1(" bank "); uart_print_level_1_int(banks[bankIdx]);
                        uart_print_level_1(", undefined GC state: "); uart_print_level_1_int(gcState[banks[bankIdx]]); uart_print_level_1("\r\n");
                        while(1);
                    }
                }
            }
        }
    }
}

//UINT32 bankForBGCleaning=0;

UINT32 bgCleaningColumn=0;

void backgroundCleaning(const UINT32 bank_)
{
}

/*
    uart_print("backgroundCleaning bank "); uart_print_int(bank_); uart_print("\r\n");

    //UINT32 bgCleaningColumn= ( (bank_/4) + 1) % 8;
    UINT32 firstBank = bgCleaningColumn * 4;
    UINT32 lastBank = firstBank + 4;

    for (UINT32 bankForBGCleaning=firstBank; bankForBGCleaning < lastBank; ++bankForBGCleaning)
    //while(1)
    {
        if( (cleanListSize(&cleanListDataWrite, bankForBGCleaning) <= 5) && (isBankBusy(bankForBGCleaning) == FALSE) )
        {
            switch (gcState[bankForBGCleaning])
            {

                case GcIdle:
                {
                    //start_interval_measurement(TIMER_CH2, TIMER_PRESCALE_0);
                    initGC(bankForBGCleaning);
                    //waitBusyBank(bankForBGCleaning);
                    //UINT32 timerValue=GET_TIMER_VALUE(TIMER_CH2);
                    //UINT32 nTicks = 0xFFFFFFFF - timerValue;
                    //uart_print_level_1("i "); uart_print_level_1_int(nTicks); uart_print_level_1("\r\n");
                    break;
                }

                case GcRead:
                {
                    //start_interval_measurement(TIMER_CH2, TIMER_PRESCALE_0);
                    readPageSingleStep(bankForBGCleaning);
                    //UINT32 timerValue=GET_TIMER_VALUE(TIMER_CH2);
                    //UINT32 nTicks = 0xFFFFFFFF - timerValue;
                    //uart_print_level_1("rs "); uart_print_level_1_int(nTicks); uart_print_level_1("\r\n");
                    break;
                }

                case GcWrite:
                {
                    //start_interval_measurement(TIMER_CH2, TIMER_PRESCALE_0);
                    writePage(bankForBGCleaning);
                    //UINT32 timerValue=GET_TIMER_VALUE(TIMER_CH2);
                    //UINT32 nTicks = 0xFFFFFFFF - timerValue;
                    //uart_print_level_1("w "); uart_print_level_1_int(nTicks); uart_print_level_1("\r\n");
                    break;
                }

                default:
                {
                    uart_print_level_1("ERROR in garbageCollectLog: on bank "); uart_print_level_1_int(bankForBGCleaning);
                    uart_print_level_1(", undefined GC state: "); uart_print_level_1_int(gcState[bankForBGCleaning]); uart_print_level_1("\r\n");
                    while(1);
                }
            }

            //return;

        }
        //else
        //{
            //bankForBGCleaning = (bankForBGCleaning+1) % NUM_BANKS;
            //return;
        //}
    }
    bgCleaningColumn = (bgCleaningColumn+1) % 8;
    //bankForBGCleaning = (bankForBGCleaning+1) % NUM_BANKS;
}
*/

void initGC(UINT32 bank)
{

#if PrintStats
    uart_print_level_1("CNT ");
    uart_print_level_1_int(bank);
    uart_print_level_1(" ");
    uart_print_level_1_int(cleanListSize(&cleanListDataWrite, bank));
    uart_print_level_1(" ");
    uart_print_level_1_int(heapDataFirstUsage.nElInHeap[bank]);
    uart_print_level_1(" ");
    uart_print_level_1_int(heapDataSecondUsage.nElInHeap[bank]);
    uart_print_level_1(" ");
    uart_print_level_1_int(heapDataCold.nElInHeap[bank]);
    uart_print_level_1("\r\n");
#endif

    nValidChunksInBlk[bank] = 0;

    // note(fabio): this version of the GC cleans only completely used blocks (from heapDataSecondUsage).

    UINT32 validCold = getVictimValidPagesNumber(&heapDataCold, bank);
    UINT32 validSecond = getVictimValidPagesNumber(&heapDataSecondUsage, bank);

    uart_print("Valid cold ");
    uart_print_int(validCold);
    uart_print(" valid second ");
    uart_print_int(validSecond);
    uart_print("\r\n");

    if (validCold < ((validSecond*secondHotFactorNum)/secondHotFactorDen))
    {
        uart_print("GC on cold block\r\n");
        nValidChunksFromHeap[bank] = validCold;
        victimLbn[bank] = getVictim(&heapDataCold, bank);

#if PrintStats
#if MeasureGc
        uart_print_level_1("COLD "); uart_print_level_1_int(bank); uart_print_level_1(" ");
        uart_print_level_1_int(validCold); uart_print_level_1("\r\n");
#endif
#endif
    }
    else
    {
        uart_print("GC on second hot block\r\n");
        nValidChunksFromHeap[bank] = validSecond;
        victimLbn[bank] = getVictim(&heapDataSecondUsage, bank);

#if PrintStats
#if MeasureGc
        uart_print_level_1("SECOND "); uart_print_level_1_int(bank); uart_print_level_1(" ");
        uart_print_level_1_int(validSecond); uart_print_level_1("\r\n");
#endif
#endif
    }

    victimVbn[bank] = get_log_vbn(bank, victimLbn[bank]);

    uart_print("initGC, bank "); uart_print_int(bank);
    uart_print(" victimLbn "); uart_print_int(victimLbn[bank]);
    uart_print(" valid chunks "); uart_print_int(nValidChunksFromHeap[bank]); uart_print("\r\n");

#if PrintStats
    { // print the Hot First Accumulated parameters
        uart_print_level_1("HFMAX ");
        for (int i=0; i<NUM_BANKS; ++i)
        {
            uart_print_level_1_int(hotFirstAccumulated[i]);
            uart_print_level_1(" ");
        }
        uart_print_level_1("\r\n");
    }
#endif

    { // Insert new value at position 0 in adaptive window and shift all others
        for (int i=adaptiveWindowSize-1; i>0; --i)
        {
            adaptiveWindow[bank][i] = adaptiveWindow[bank][i-1];
        }
        adaptiveWindow[bank][0] = nValidChunksFromHeap[bank];
    }

    if (nValidChunksFromHeap[bank] > 0)
    {
        nand_page_ptread(bank, victimVbn[bank], PAGES_PER_BLK - 1, 0, (CHUNK_ADDR_BYTES * CHUNKS_PER_LOG_BLK + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR, VICTIM_LPN_LIST(bank), RETURN_WHEN_DONE); // read twice the lpns list size because there might be the recycled lpns list appended
        gcOnRecycledPage[bank]=FALSE;
        pageOffset[bank]=0;
        gcState[bank]=GcRead;
    }

    else
    {
        resetValidChunksAndRemove(&heapDataFirstUsage, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_FIRST_USAGE);
        resetValidChunksAndRemove(&heapDataSecondUsage, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_SECOND_USAGE);
        resetValidChunksAndRemove(&heapDataCold, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_SECOND_USAGE);
        nand_block_erase(bank, victimVbn[bank]);
        cleanListPush(&cleanListDataWrite, bank, victimLbn[bank]);

#if MeasureGc
        uart_print_level_2("GCW "); uart_print_level_2_int(bank);
        uart_print_level_2(" "); uart_print_level_2_int(0);
        uart_print_level_2(" "); uart_print_level_2_int(nValidChunksFromHeap[bank]);
        uart_print_level_2("\r\n");
#endif

        gcState[bank]=GcIdle;
    }

}

void checkNoChunksAreValid(UINT32 bank, UINT32 lbn)
{
    for(UINT32 page=0; page<PAGES_PER_BLK; ++page)
    {
        for (UINT32 chunk=0; chunk<CHUNKS_PER_PAGE; ++chunk)
        {
            UINT32 logChunkAddr = (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (lbn*CHUNKS_PER_BLK) + (page*CHUNKS_PER_PAGE) + chunk;
            UINT32 i = mem_search_equ_dram_4_bytes(CHUNKS_MAP_TABLE_ADDR, CHUNKS_MAP_TABLE_BYTES/4, logChunkAddr);
            if (i<CHUNKS_MAP_TABLE_BYTES/4)
            {
                uart_print_level_1("ERROR: after GC not all chunks are invalid. bank=");
                uart_print_level_1_int(bank);
                uart_print_level_1(" lbn=");
                uart_print_level_1_int(lbn);
                uart_print_level_1(" page=");
                uart_print_level_1_int(page);
                uart_print_level_1(" chunk=");
                uart_print_level_1_int(chunk);
                uart_print_level_1(". dataLpn=");
                uart_print_level_1_int(i/CHUNKS_PER_PAGE);
                uart_print_level_1("\r\n");
                if (read_dram_32(ChunksMapTable(i/CHUNKS_PER_PAGE, i%CHUNKS_PER_PAGE)) != logChunkAddr )
                {
                    uart_print_level_1("Nevermind\r\n");
                }
            }
        }
    }
}

void readPage(UINT32 bank)
{

    for(; pageOffset[bank] < UsedPagesPerLogBlk; pageOffset[bank]++)
    {
        uart_print("readPage: bank="); uart_print_int(bank); uart_print(" ");
        uart_print("pageOffset[bank]="); uart_print_int(pageOffset[bank]);

        nValidChunksInPage[bank]=0;
        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++) validChunks[bank][chunkOffset]=FALSE;

        UINT32 victimLpns[CHUNKS_PER_PAGE];
        mem_copy(victimLpns, VICTIM_LPN_LIST(bank)+(pageOffset[bank]*CHUNKS_PER_PAGE)*CHUNK_ADDR_BYTES, CHUNKS_PER_PAGE * sizeof(UINT32));

        gcOnRecycledPage[bank] = FALSE;

        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_RECYCLED_PAGE; ++chunkOffset)
        {
            if (victimLpns[chunkOffset] != INVALID && victimLpns[chunkOffset] & ColdLogBufBitFlag)
            {
                gcOnRecycledPage[bank] = TRUE;
            }
            else
            {
                if (gcOnRecycledPage[bank])
                {
                    if (victimLpns[chunkOffset] != INVALID && !(victimLpns[chunkOffset] & ColdLogBufBitFlag))
                    {
                        uart_print_level_1("ERROR in readPage: inconsistent lpns in recycled page\r\n");
                        while(1);
                    }
                }
            }
        }

        if (gcOnRecycledPage[bank])
        {
            uart_print(" recycled page ");

            UINT32 logChunkAddr = ( (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (victimLbn[bank]*CHUNKS_PER_BLK) + (pageOffset[bank]*CHUNKS_PER_PAGE) )| ColdLogBufBitFlag;

            for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_RECYCLED_PAGE; chunkOffset++)
            {   // This loops finds valid chunks is the page. Note that chunks in GC Buf won't be considered as they temporarily don't occupy space in Log
                UINT32 victimLpn = victimLpns[chunkOffset];
                if (victimLpn != INVALID)
                {
                    UINT32 i = mem_search_equ_dram_4_bytes(ChunksMapTable(victimLpn, 0), CHUNKS_PER_PAGE, logChunkAddr);

                    if(i<CHUNKS_PER_PAGE)
                    {
                        dataChunkOffsets[bank][chunkOffset]=i;
                        dataLpns[bank][chunkOffset]=victimLpn & ~(ColdLogBufBitFlag);
                        validChunks[bank][chunkOffset]=TRUE;
                        nValidChunksInPage[bank]++;
                        nValidChunksInBlk[bank]++;
                    }
                    else
                    {
                        uart_print(" nf ");
                    }
                }
                else
                {
                    uart_print(" i ");
                }
                logChunkAddr++;
            }

        }

        else
        {
            uart_print(" normal page ");

            UINT32 logChunkAddr = (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (victimLbn[bank]*CHUNKS_PER_BLK) + (pageOffset[bank]*CHUNKS_PER_PAGE);

            for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
            {   // This loops finds valid chunks is the page. Note that chunks in GC Buf won't be considered as they temporarily don't occupy space in Log
                UINT32 victimLpn = victimLpns[chunkOffset];
                if (victimLpn != INVALID)
                {
                    UINT32 i = mem_search_equ_dram_4_bytes(ChunksMapTable(victimLpn, 0), CHUNKS_PER_PAGE, logChunkAddr);

                    if(i<CHUNKS_PER_PAGE)
                    {
                        dataChunkOffsets[bank][chunkOffset]=i;
                        dataLpns[bank][chunkOffset]=victimLpn;
                        validChunks[bank][chunkOffset]=TRUE;
                        nValidChunksInPage[bank]++;
                        nValidChunksInBlk[bank]++;
                    }
                    else
                    {
                        uart_print(" nf ");
                    }
                }
                else
                {
                    uart_print(" i ");
                }
                logChunkAddr++;
            }
        }

        if(nValidChunksInPage[bank] > 0)
        {
            uart_print(" found "); uart_print_int(nValidChunksInPage[bank]); uart_print(" valid chunks. Proceed to writePage\r\n");

            nand_page_ptread(bank, victimVbn[bank], pageOffset[bank], 0, SECTORS_PER_PAGE, GC_BUF(bank), RETURN_ON_ISSUE);

            gcState[bank] = GcWrite;
            return;
        }
        else
        {
            uart_print(" found "); uart_print_int(nValidChunksInPage[bank]); uart_print(" valid chunks.\r\n");
        }
    }

    if (nValidChunksInBlk[bank] != nValidChunksFromHeap[bank])
    {
        uart_print_level_1("ERROR: found different number of valid chunks than expected at the end of readPage for normal block. GC on bank "); uart_print_level_1_int(bank);
        uart_print_level_1(" victimLbn "); uart_print_level_1_int(victimLbn[bank]); uart_print_level_1("\r\n");
        uart_print_level_1("Found "); uart_print_level_1_int(nValidChunksInBlk[bank]);
        uart_print_level_1(" instead of expected "); uart_print_level_1_int(nValidChunksFromHeap[bank]); uart_print_level_1("\r\n");
        uart_print_level_1("pageOffset: "); uart_print_level_1_int(pageOffset[bank]);
        while(1);
    }
    else
    {
        uart_print("readPage: successful GC on normal block in bank "); uart_print_int(bank); uart_print("\r\n");
        //checkNoChunksAreValid(bank, victimLbn[bank]);
    }

#if MeasureGc
    uart_print_level_2("GCW "); uart_print_level_2_int(bank);
    uart_print_level_2(" "); uart_print_level_2_int(0);
    uart_print_level_2(" "); uart_print_level_2_int(nValidChunksFromHeap[bank]);
    uart_print_level_2("\r\n");
#endif

    resetValidChunksAndRemove(&heapDataFirstUsage, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_FIRST_USAGE);
    resetValidChunksAndRemove(&heapDataSecondUsage, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_SECOND_USAGE);
    resetValidChunksAndRemove(&heapDataCold, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_SECOND_USAGE);
    nand_block_erase(bank, victimVbn[bank]);
    cleanListPush(&cleanListDataWrite, bank, victimLbn[bank]);

    uart_print("After GC: victim lbn was "); uart_print_int(victimLbn[bank]); uart_print("\r\n");

    gcState[bank]=GcIdle;

}

void readPageSingleStep(UINT32 bank)
{
    uart_print("readPageSingleStep: bank="); uart_print_int(bank); uart_print(" ");
    uart_print("pageOffset[bank]="); uart_print_int(pageOffset[bank]); uart_print("\r\n");

    if (pageOffset[bank] == UsedPagesPerLogBlk)
    {
        if (nValidChunksInBlk[bank] != nValidChunksFromHeap[bank])
        {
            uart_print_level_1("ERROR: found different number of valid chunks than expected at the end of readPageSingleStep on normal block. GC on bank "); uart_print_level_1_int(bank);
            uart_print_level_1(" victimLbn "); uart_print_level_1_int(victimLbn[bank]); uart_print_level_1("\r\n");
            uart_print_level_1("Found "); uart_print_level_1_int(nValidChunksInBlk[bank]);
            uart_print_level_1(" instead of expected "); uart_print_level_1_int(nValidChunksFromHeap[bank]); uart_print_level_1("\r\n");
            uart_print_level_1("pageOffset: "); uart_print_level_1_int(pageOffset[bank]);
            while(1);
        }
        else
        {
            uart_print("readPageSingleStep: successful GC on normal block in bank "); uart_print_int(bank); uart_print("\r\n");
            //checkNoChunksAreValid(bank, victimLbn[bank]);
        }

        resetValidChunksAndRemove(&heapDataFirstUsage, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_FIRST_USAGE);
        resetValidChunksAndRemove(&heapDataSecondUsage, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_SECOND_USAGE);
        resetValidChunksAndRemove(&heapDataCold, bank, victimLbn[bank], CHUNKS_PER_LOG_BLK_SECOND_USAGE);
        nand_block_erase(bank, victimVbn[bank]);
        cleanListPush(&cleanListDataWrite, bank, victimLbn[bank]);
#if MeasureGc
        uart_print_level_2("GCW "); uart_print_level_2_int(bank);
        uart_print_level_2(" "); uart_print_level_2_int(0);
        uart_print_level_2(" "); uart_print_level_2_int(nValidChunksFromHeap[bank]);
        uart_print_level_2("\r\n");
#endif
        gcState[bank]=GcIdle;
        return;
    }

    uart_print("\r\npageOffset[bank]="); uart_print_int(pageOffset[bank]); uart_print("\r\n");

    nValidChunksInPage[bank]=0;
    for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++) validChunks[bank][chunkOffset]=FALSE;

    UINT32 victimLpns[CHUNKS_PER_PAGE];
    mem_copy(victimLpns, VICTIM_LPN_LIST(bank)+(pageOffset[bank]*CHUNKS_PER_PAGE)*CHUNK_ADDR_BYTES, CHUNKS_PER_PAGE * sizeof(UINT32));

    gcOnRecycledPage[bank] = FALSE;

    for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_RECYCLED_PAGE; ++chunkOffset)
    {
        if (victimLpns[chunkOffset] != INVALID && victimLpns[chunkOffset] & ColdLogBufBitFlag)
        {
            gcOnRecycledPage[bank] = TRUE;
        }
        else
        {
            if (gcOnRecycledPage[bank])
            {
                uart_print_level_1("ERROR in readSinglePage: inconsistent lpns in recycled page\r\n");
                while(1);
            }
        }
    }

    if (gcOnRecycledPage[bank])
    {

        UINT32 logChunkAddr = ( (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (victimLbn[bank]*CHUNKS_PER_BLK) + (pageOffset[bank]*CHUNKS_PER_PAGE) ) | ColdLogBufBitFlag;

        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_RECYCLED_PAGE; ++chunkOffset)
        {   // This loops finds valid chunks is the page. Note that chunks in GC Buf won't be considered as they temporarily don't occupy space in Log
            UINT32 victimLpn = victimLpns[chunkOffset];
            if (victimLpn != INVALID)
            {
                UINT32 i = mem_search_equ_dram_4_bytes(ChunksMapTable(victimLpn, 0), CHUNKS_PER_PAGE, logChunkAddr);

                if(i<CHUNKS_PER_PAGE)
                {
                    dataChunkOffsets[bank][chunkOffset]=i;
                    dataLpns[bank][chunkOffset]=victimLpn & ~(ColdLogBufBitFlag);
                    validChunks[bank][chunkOffset]=TRUE;
                    nValidChunksInPage[bank]++;
                    nValidChunksInBlk[bank]++;
                }
            }
            logChunkAddr++;
        }
    }
    else
    {
        UINT32 logChunkAddr = (bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (victimLbn[bank]*CHUNKS_PER_BLK) + (pageOffset[bank]*CHUNKS_PER_PAGE);

        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
        {   // This loops finds valid chunks is the page. Note that chunks in GC Buf won't be considered as they temporarily don't occupy space in Log
            UINT32 victimLpn = victimLpns[chunkOffset];
            if (victimLpn != INVALID)
            {
                UINT32 i = mem_search_equ_dram_4_bytes(ChunksMapTable(victimLpn, 0), CHUNKS_PER_PAGE, logChunkAddr);

                if(i<CHUNKS_PER_PAGE)
                {
                    dataChunkOffsets[bank][chunkOffset]=i;
                    dataLpns[bank][chunkOffset]=victimLpn;
                    validChunks[bank][chunkOffset]=TRUE;
                    nValidChunksInPage[bank]++;
                    nValidChunksInBlk[bank]++;
                }
            }
            logChunkAddr++;
        }

    }

    if(nValidChunksInPage[bank] > 0)
    {
        uart_print("Current bank is full, copy page to another one\r\n");

        nand_page_ptread(bank, victimVbn[bank], pageOffset[bank], 0, SECTORS_PER_PAGE, GC_BUF(bank), RETURN_ON_ISSUE);

        gcState[bank] = GcWrite;
    }
    else
    {
        pageOffset[bank]++;
    }

}

void writePage(UINT32 bank)
{
    uart_print("writePage: bank="); uart_print_int(bank);
    uart_print(" victimLbn "); uart_print_int(victimLbn[bank]);
    uart_print(" pageOffset "); uart_print_int(pageOffset[bank]); uart_print(" ");

    if(nValidChunksInPage[bank] == 8)
    {

        UINT32 logChunkBase = ((bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (victimLbn[bank]*CHUNKS_PER_BLK) + (pageOffset[bank]*CHUNKS_PER_PAGE));
        if (gcOnRecycledPage[bank])
        {
            logChunkBase = logChunkBase | ColdLogBufBitFlag;
        }

        for(UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; chunkOffset++)
        {
            UINT32 chunkAddr = read_dram_32(ChunksMapTable(dataLpns[bank][chunkOffset], dataChunkOffsets[bank][chunkOffset]));

            // note (fabio): here we check against the normal chunkAddr (not recycled) because if there are 8 valid chunks the blk cannot be a recycled one
            if(chunkAddr != logChunkBase + chunkOffset)
            {
                // note(fabio): here invalidate only the first chunk that was moved by another write. If other chunks were also moved they'll be found by the code after the goto
                validChunks[bank][chunkOffset]=FALSE;
                nValidChunksInPage[bank]--;
                nValidChunksInBlk[bank]--;
                goto WritePartialPage;
            }

        }

        UINT32 dstLpn = getRWLpn(bank, coldLogCtrl);
        UINT32 dstVbn = get_log_vbn(bank, LogPageToLogBlk(dstLpn));
        UINT32 dstPageOffset = LogPageToOffset(dstLpn);

        uart_print(" dstLpn="); uart_print_int(dstLpn);
        uart_print(" dstVbn="); uart_print_int(dstVbn); uart_print(" dstPageOffset="); uart_print_int(dstPageOffset); uart_print("\r\n");

#if PrintStats
        uart_print_level_1("^\r\n");
#endif

        nand_page_program(bank, dstVbn, dstPageOffset, GC_BUF(bank), RETURN_ON_ISSUE);

        mem_copy(chunkInLpnsList(coldLogCtrl[bank].lpnsListAddr, dstPageOffset, 0), dataLpns[bank], CHUNKS_PER_PAGE * sizeof(UINT32));

        for (UINT32 chunkOffset=0; chunkOffset<CHUNKS_PER_PAGE; ++chunkOffset)
        {
            write_dram_32(ChunksMapTable(dataLpns[bank][chunkOffset], dataChunkOffsets[bank][chunkOffset]), (bank * LOG_BLK_PER_BANK * CHUNKS_PER_BLK) + (dstLpn * CHUNKS_PER_PAGE) + chunkOffset);
        }

        nValidChunksInPage[bank] = 0;
        gcOnRecycledPage[bank]=FALSE;

        gcState[bank] = GcRead;

        pageOffset[bank]++;

        coldLogCtrl[bank].increaseLpn(bank, coldLogCtrl);

    }
    else
WritePartialPage:
    {
        uart_print("write partial ");
        UINT32 chunkOffset=0;
        UINT32 logChunkBase=((bank*LOG_BLK_PER_BANK*CHUNKS_PER_BLK) + (victimLbn[bank]*CHUNKS_PER_BLK) + (pageOffset[bank]*CHUNKS_PER_PAGE));
        if (gcOnRecycledPage[bank])
        {
            logChunkBase = logChunkBase | ColdLogBufBitFlag;
            // note(fabio): Here we should decode
        }
        while(nValidChunksInPage[bank] > 0)
        {

            if(validChunks[bank][chunkOffset])
            {
                validChunks[bank][chunkOffset] = FALSE;
                nValidChunksInPage[bank]--;

                UINT32 chunkAddr = read_dram_32(ChunksMapTable(dataLpns[bank][chunkOffset], dataChunkOffsets[bank][chunkOffset]));

                if(chunkAddr == logChunkBase+chunkOffset)
                {

                    writeChunkOnLogBlockDuringGC(bank,
                                                 dataLpns[bank][chunkOffset],
                                                 dataChunkOffsets[bank][chunkOffset],
                                                 chunkOffset,
                                                 GC_BUF(bank));
                }
                else
                {
                    uart_print(" one chunk was moved during GC ");
                    nValidChunksInBlk[bank]--;
                }
            }
            chunkOffset++;
        }
        uart_print(" current nValidChunksInBlk="); uart_print_int(nValidChunksInBlk[bank]); uart_print("\r\n");

        if (gcState[bank] == GcWrite)
        {
            gcState[bank] = GcRead;
            gcOnRecycledPage[bank]=FALSE;
            pageOffset[bank]++;
        }
    }
}
