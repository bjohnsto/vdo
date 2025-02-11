/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabDepot.c#21 $
 */

#include "slabDepot.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "actionManager.h"
#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "constants.h"
#include "header.h"
#include "numUtils.h"
#include "readOnlyNotifier.h"
#include "refCounts.h"
#include "slab.h"
#include "slabDepotInternals.h"
#include "slabJournal.h"
#include "slabIterator.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "types.h"

typedef struct {
  SlabConfig           slabConfig;
  PhysicalBlockNumber  firstBlock;
  PhysicalBlockNumber  lastBlock;
  ZoneCount            zoneCount;
} __attribute__((packed)) SlabDepotState2_0;

static const Header SLAB_DEPOT_HEADER_2_0 = {
  .id = SLAB_DEPOT,
  .version = {
    .majorVersion = 2,
    .minorVersion = 0,
  },
  .size = sizeof(SlabDepotState2_0),
};

/**
 * Compute the number of slabs a depot with given parameters would have.
 *
 * @param firstBlock     PBN of the first data block
 * @param lastBlock      PBN of the last data block
 * @param slabSizeShift  Exponent for the number of blocks per slab
 *
 * @return The number of slabs
 **/
__attribute__((warn_unused_result))
static SlabCount computeSlabCount(PhysicalBlockNumber firstBlock,
                                  PhysicalBlockNumber lastBlock,
                                  unsigned int        slabSizeShift)
{
  BlockCount dataBlocks = lastBlock - firstBlock;
  return (SlabCount) (dataBlocks >> slabSizeShift);
}

/**********************************************************************/
SlabCount calculateSlabCount(SlabDepot *depot)
{
  return computeSlabCount(depot->firstBlock, depot->lastBlock,
                          depot->slabSizeShift);
}

/**
 * Get an iterator over all the slabs in the depot.
 *
 * @param depot  The depot
 *
 * @return An iterator over the depot's slabs
 **/
static SlabIterator getSlabIterator(SlabDepot *depot)
{
  return iterateSlabs(depot->slabs, depot->slabCount - 1, 0, 1);
}

/**
 * Allocate a new slab pointer array. Any existing slab pointers will be
 * copied into the new array, and slabs will be allocated as needed. The
 * newly allocated slabs will not be distributed for use by the block
 * allocators.
 *
 * @param depot      The depot
 * @param slabCount  The number of slabs the depot should have in the new
 *                   array
 *
 * @return VDO_SUCCESS or an error code
 **/
static int allocateSlabs(SlabDepot *depot, SlabCount slabCount)
{
  int result = ALLOCATE(slabCount, Slab *, "slab pointer array",
                        &depot->newSlabs);
  if (result != VDO_SUCCESS) {
    return result;
  }

  bool resizing = false;
  if (depot->slabs != NULL) {
    memcpy(depot->newSlabs, depot->slabs, depot->slabCount * sizeof(Slab *));
    resizing = true;
  }

  BlockCount slabSize = getSlabConfig(depot)->slabBlocks;
  PhysicalBlockNumber slabOrigin
    = depot->firstBlock + (depot->slabCount * slabSize);

  // The translation between allocator partition PBNs and layer PBNs.
  BlockCount translation = depot->origin - depot->firstBlock;
  depot->newSlabCount = depot->slabCount;
  while (depot->newSlabCount < slabCount) {
    BlockAllocator *allocator
      = depot->allocators[depot->newSlabCount % depot->zoneCount];
    Slab **slabPtr = &depot->newSlabs[depot->newSlabCount];
    result = makeSlab(slabOrigin, allocator, translation, depot->journal,
                      depot->newSlabCount, resizing, slabPtr);
    if (result != VDO_SUCCESS) {
      return result;
    }
    // Increment here to ensure that abandonNewSlabs will clean up correctly.
    depot->newSlabCount++;

    slabOrigin += slabSize;
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
void abandonNewSlabs(SlabDepot *depot)
{
  if (depot->newSlabs == NULL) {
    return;
  }
  for (SlabCount i = depot->slabCount; i < depot->newSlabCount; i++) {
    freeSlab(&depot->newSlabs[i]);
  }
  depot->newSlabCount = 0;
  FREE(depot->newSlabs);
  depot->newSlabs = NULL;
  depot->newSize  = 0;
}

/**
 * Get the ID of the thread on which a given allocator operates.
 *
 * <p>Implements ZoneThreadGetter.
 **/
static ThreadID getAllocatorThreadID(void *context, ZoneCount zoneNumber)
{
  return getBlockAllocatorForZone(context, zoneNumber)->threadID;
}

/**
 * Prepare to commit oldest tail blocks.
 *
 * <p>Implements ActionPreamble.
 **/
static void prepareForTailBlockCommit(void *context, VDOCompletion *parent)
{
  SlabDepot *depot = context;
  depot->activeReleaseRequest = depot->newReleaseRequest;
  completeCompletion(parent);
}

/**
 * Schedule a tail block commit if necessary.
 *
 * <p>Implements ActionScheduler,
 **/
static bool scheduleTailBlockCommit(void *context)
{
  SlabDepot *depot = context;
  if (depot->newReleaseRequest == depot->activeReleaseRequest) {
    return false;
  }

  return scheduleAction(depot->actionManager, prepareForTailBlockCommit,
                        releaseTailBlockLocks, NULL, NULL);
}

/**
 * Allocate those components of the slab depot which are needed only at load
 * time, not at format time.
 *
 * @param depot             The depot
 * @param nonce             The nonce of the VDO
 * @param threadConfig      The thread config of the VDO
 * @param vioPoolSize       The size of the VIO pool
 * @param layer             The physical layer below this depot
 * @param summaryPartition  The partition which holds the slab summary
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocateComponents(SlabDepot          *depot,
                              Nonce               nonce,
                              const ThreadConfig *threadConfig,
                              BlockCount          vioPoolSize,
                              PhysicalLayer      *layer,
                              Partition          *summaryPartition)
{
  /*
   * If createVIO is NULL, the slab depot is only being used to format
   * or audit the VDO. These only require the SuperBlock component, so we can
   * just skip allocating all the memory needed for runtime components.
   */
  if (layer->createMetadataVIO == NULL) {
    return VDO_SUCCESS;
  }

  int result = initializeEnqueueableCompletion(&depot->scrubbingCompletion,
                                               SUB_TASK_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeActionManager(depot->zoneCount, getAllocatorThreadID,
                             getJournalZoneThread(threadConfig), depot,
                             scheduleTailBlockCommit, layer,
                             &depot->actionManager);
  if (result != VDO_SUCCESS) {
    return result;
  }

  depot->origin = depot->firstBlock;

  result = makeSlabSummary(layer, summaryPartition, threadConfig,
                           depot->slabSizeShift, depot->slabConfig.dataBlocks,
                           depot->readOnlyNotifier, &depot->slabSummary);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabCount slabCount = calculateSlabCount(depot);
  if (threadConfig->physicalZoneCount > slabCount) {
    return logErrorWithStringError(VDO_BAD_CONFIGURATION,
                                   "%u physical zones exceeds slab count %u",
                                   threadConfig->physicalZoneCount, slabCount);
  }

  // Allocate the block allocators.
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    ThreadID threadID = getPhysicalZoneThread(threadConfig, zone);
    result = makeBlockAllocator(depot, zone, threadID, nonce, vioPoolSize,
                                layer, depot->readOnlyNotifier,
                                &depot->allocators[zone]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  // Allocate slabs.
  result = allocateSlabs(depot, slabCount);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Use the new slabs.
  for (SlabCount i = depot->slabCount; i < depot->newSlabCount; i++) {
    Slab *slab = depot->newSlabs[i];
    registerSlabWithAllocator(slab->allocator, slab);
    depot->slabCount++;
  }

  depot->slabs        = depot->newSlabs;
  depot->newSlabs     = NULL;
  depot->newSlabCount = 0;

  return VDO_SUCCESS;
}

/**
 * Allocate a slab depot.
 *
 * @param [in]  state             The parameters for the new depot
 * @param [in]  threadConfig      The thread config of the VDO
 * @param [in]  nonce             The nonce of the VDO
 * @param [in]  vioPoolSize       The size of the VIO pool
 * @param [in]  layer             The physical layer below this depot
 * @param [in]  summaryPartition  The partition which holds the slab summary
 *                                 (if NULL, the depot is format-only)
 * @param [in]  readOnlyNotifier  The context for entering read-only mode
 * @param [in]  recoveryJournal   The recovery journal of the VDO
 * @param [out] depotPtr          A pointer to hold the depot
 *
 * @return A success or error code
 **/
__attribute__((warn_unused_result))
static int allocateDepot(const SlabDepotState2_0  *state,
                         const ThreadConfig       *threadConfig,
                         Nonce                     nonce,
                         BlockCount                vioPoolSize,
                         PhysicalLayer            *layer,
                         Partition                *summaryPartition,
                         ReadOnlyNotifier         *readOnlyNotifier,
                         RecoveryJournal          *recoveryJournal,
                         SlabDepot               **depotPtr)
{
  // Calculate the bit shift for efficiently mapping block numbers to slabs.
  // Using a shift requires that the slab size be a power of two.
  BlockCount slabSize = state->slabConfig.slabBlocks;
  if (!isPowerOfTwo(slabSize)) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "slab size must be a power of two");
  }
  unsigned int slabSizeShift = logBaseTwo(slabSize);

  SlabDepot *depot;
  int result = ALLOCATE_EXTENDED(SlabDepot, threadConfig->physicalZoneCount,
                                 BlockAllocator *, __func__, &depot);
  if (result != VDO_SUCCESS) {
    return result;
  }

  depot->oldZoneCount     = state->zoneCount;
  depot->zoneCount        = threadConfig->physicalZoneCount;
  depot->slabConfig       = state->slabConfig;
  depot->readOnlyNotifier = readOnlyNotifier;
  depot->firstBlock       = state->firstBlock;
  depot->lastBlock        = state->lastBlock;
  depot->slabSizeShift    = slabSizeShift;
  depot->journal          = recoveryJournal;

  result = allocateComponents(depot, nonce, threadConfig, vioPoolSize,
                              layer, summaryPartition);
  if (result != VDO_SUCCESS) {
    freeSlabDepot(&depot);
    return result;
  }

  *depotPtr = depot;
  return VDO_SUCCESS;
}

/**
 * Configure the SlabDepot for the specified storage capacity, finding the
 * number of data blocks that will fit and still leave room for the depot
 * metadata, then return the saved state for that configuration.
 *
 * @param [in]  blockCount  The number of blocks in the underlying storage
 * @param [in]  firstBlock  The number of the first block that may be allocated
 * @param [in]  slabConfig  The configuration of a single slab
 * @param [in]  zoneCount   The number of zones the depot will use
 * @param [out] state       The state structure to be configured
 *
 * @return VDO_SUCCESS or an error code
 **/
static int configureState(BlockCount           blockCount,
                          PhysicalBlockNumber  firstBlock,
                          SlabConfig           slabConfig,
                          ZoneCount            zoneCount,
                          SlabDepotState2_0   *state)
{
  BlockCount slabSize = slabConfig.slabBlocks;
  logDebug("slabDepot configureState(blockCount=%" PRIu64
           ", firstBlock=%" PRIu64 ", slabSize=%" PRIu64 ", zoneCount=%u)",
           blockCount, firstBlock, slabSize, zoneCount);

  // We do not allow runt slabs, so we waste up to a slab's worth.
  size_t slabCount = (blockCount / slabSize);
  if (slabCount == 0) {
    return VDO_NO_SPACE;
  }

  if (slabCount > MAX_SLABS) {
    return VDO_TOO_MANY_SLABS;
  }

  BlockCount          totalSlabBlocks = slabCount * slabConfig.slabBlocks;
  BlockCount          totalDataBlocks = slabCount * slabConfig.dataBlocks;
  PhysicalBlockNumber lastBlock       = firstBlock + totalSlabBlocks;

  *state = (SlabDepotState2_0) {
    .slabConfig = slabConfig,
    .firstBlock = firstBlock,
    .lastBlock  = lastBlock,
    .zoneCount  = zoneCount,
  };

  logDebug("slabDepot lastBlock=%" PRIu64 ", totalDataBlocks=%" PRIu64
           ", slabCount=%zu, leftOver=%" PRIu64,
           lastBlock, totalDataBlocks, slabCount,
           blockCount - (lastBlock - firstBlock));

  return VDO_SUCCESS;
}

/**********************************************************************/
int makeSlabDepot(BlockCount            blockCount,
                  PhysicalBlockNumber   firstBlock,
                  SlabConfig            slabConfig,
                  const ThreadConfig   *threadConfig,
                  Nonce                 nonce,
                  BlockCount            vioPoolSize,
                  PhysicalLayer        *layer,
                  Partition            *summaryPartition,
                  ReadOnlyNotifier     *readOnlyNotifier,
                  RecoveryJournal      *recoveryJournal,
                  SlabDepot           **depotPtr)
{
  SlabDepotState2_0 state;
  int result = configureState(blockCount, firstBlock, slabConfig, 0, &state);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabDepot *depot = NULL;
  result = allocateDepot(&state, threadConfig, nonce, vioPoolSize, layer,
                         summaryPartition, readOnlyNotifier, recoveryJournal,
                         &depot);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *depotPtr = depot;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabDepot(SlabDepot **depotPtr)
{
  SlabDepot *depot = *depotPtr;
  if (depot == NULL) {
    return;
  }

  abandonNewSlabs(depot);

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    freeBlockAllocator(&depot->allocators[zone]);
  }

  if (depot->slabs != NULL) {
    for (SlabCount i = 0; i < depot->slabCount; i++) {
      freeSlab(&depot->slabs[i]);
    }
  }

  FREE(depot->slabs);
  freeActionManager(&depot->actionManager);
  freeSlabSummary(&depot->slabSummary);
  destroyEnqueueable(&depot->scrubbingCompletion);
  FREE(depot);
  *depotPtr = NULL;
}

/**********************************************************************/
size_t getSlabDepotEncodedSize(void)
{
  return ENCODED_HEADER_SIZE + sizeof(SlabDepotState2_0);
}

/**
 * Decode a slab config from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeSlabConfig(Buffer *buffer, SlabConfig *config)
{
  BlockCount count;
  int result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->slabBlocks = count;

  result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->dataBlocks = count;

  result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->referenceCountBlocks = count;

  result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->slabJournalBlocks = count;

  result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->slabJournalFlushingThreshold = count;

  result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->slabJournalBlockingThreshold = count;

  result = getUInt64LEFromBuffer(buffer, &count);
  if (result != UDS_SUCCESS) {
    return result;
  }
  config->slabJournalScrubbingThreshold = count;

  return UDS_SUCCESS;
}

/**
 * Encode a slab config into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encodeSlabConfig(const SlabConfig *config, Buffer *buffer)
{
  int result = putUInt64LEIntoBuffer(buffer, config->slabBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->dataBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->referenceCountBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabJournalBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabJournalFlushingThreshold);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabJournalBlockingThreshold);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return putUInt64LEIntoBuffer(buffer, config->slabJournalScrubbingThreshold);
}

/**********************************************************************/
int encodeSlabDepot(const SlabDepot *depot, Buffer *buffer)
{
  int result = encodeHeader(&SLAB_DEPOT_HEADER_2_0, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = encodeSlabConfig(&depot->slabConfig, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, depot->firstBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, depot->lastBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  /*
   * If this depot is currently using 0 zones, it must have been
   * synchronously loaded by a tool and is now being saved. We
   * did not load and combine the slab summary, so we still need
   * to do that next time we load with the old zone count rather
   * than 0.
   */
  ZoneCount zonesToRecord = depot->zoneCount;
  if (depot->zoneCount == 0) {
    zonesToRecord = depot->oldZoneCount;
  }
  result = putByte(buffer, zonesToRecord);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t encodedSize = contentLength(buffer) - initialLength;
  return ASSERT(SLAB_DEPOT_HEADER_2_0.size == encodedSize,
                "encoded block map component size must match header size");
}

/**
 * Decode slab depot component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeSlabDepotState_2_0(Buffer *buffer, SlabDepotState2_0 *state)
{
  size_t initialLength = contentLength(buffer);

  int result = decodeSlabConfig(buffer, &state->slabConfig);
  if (result != UDS_SUCCESS) {
    return result;
  }

  PhysicalBlockNumber firstBlock;
  result = getUInt64LEFromBuffer(buffer, &firstBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }
  state->firstBlock = firstBlock;

  PhysicalBlockNumber lastBlock;
  result = getUInt64LEFromBuffer(buffer, &lastBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }
  state->lastBlock = lastBlock;

  result = getByte(buffer, &state->zoneCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(SLAB_DEPOT_HEADER_2_0.size == decodedSize,
                "decoded slab depot component size must match header size");
}

/**********************************************************************/
int decodeSlabDepot(Buffer              *buffer,
                    const ThreadConfig  *threadConfig,
                    Nonce                nonce,
                    PhysicalLayer       *layer,
                    Partition           *summaryPartition,
                    ReadOnlyNotifier    *readOnlyNotifier,
                    RecoveryJournal     *recoveryJournal,
                    SlabDepot          **depotPtr)
{
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(&SLAB_DEPOT_HEADER_2_0, &header, true, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabDepotState2_0 state;
  result = decodeSlabDepotState_2_0(buffer, &state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return allocateDepot(&state, threadConfig, nonce, VIO_POOL_SIZE, layer,
                       summaryPartition, readOnlyNotifier, recoveryJournal,
                       depotPtr);
}

/**********************************************************************/
int decodeSodiumSlabDepot(Buffer              *buffer,
                          const ThreadConfig  *threadConfig,
                          Nonce                nonce,
                          PhysicalLayer       *layer,
                          Partition           *summaryPartition,
                          ReadOnlyNotifier    *readOnlyNotifier,
                          RecoveryJournal     *recoveryJournal,
                          SlabDepot          **depotPtr)
{
  // Sodium uses version 2.0 of the slab depot state.
  return decodeSlabDepot(buffer, threadConfig, nonce, layer, summaryPartition,
                         readOnlyNotifier, recoveryJournal, depotPtr);
}

/**********************************************************************/
int allocateSlabRefCounts(SlabDepot *depot)
{
  SlabIterator iterator = getSlabIterator(depot);
  while (hasNextSlab(&iterator)) {
    int result = allocateRefCountsForSlab(nextSlab(&iterator));
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
BlockAllocator *getBlockAllocatorForZone(SlabDepot *depot,
                                         ZoneCount  zoneNumber)
{
  return depot->allocators[zoneNumber];
}

/**********************************************************************/
int getSlabNumber(const SlabDepot     *depot,
                  PhysicalBlockNumber  pbn,
                  SlabCount           *slabNumberPtr)
{
  if (pbn < depot->firstBlock) {
    return VDO_OUT_OF_RANGE;
  }

  SlabCount slabNumber = (pbn - depot->firstBlock) >> depot->slabSizeShift;
  if (slabNumber >= depot->slabCount) {
    return VDO_OUT_OF_RANGE;
  }

  *slabNumberPtr = slabNumber;
  return VDO_SUCCESS;
}

/**********************************************************************/
Slab *getSlab(const SlabDepot *depot, PhysicalBlockNumber pbn)
{
  if (pbn == ZERO_BLOCK) {
    return NULL;
  }

  SlabCount slabNumber;
  int result = getSlabNumber(depot, pbn, &slabNumber);
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(depot->readOnlyNotifier, result);
    return NULL;
  }

  return depot->slabs[slabNumber];

}

/**********************************************************************/
SlabJournal *getSlabJournal(const SlabDepot *depot, PhysicalBlockNumber pbn)
{
  Slab *slab = getSlab(depot, pbn);
  return ((slab != NULL) ? slab->journal : NULL);
}

/**********************************************************************/
uint8_t getIncrementLimit(SlabDepot *depot, PhysicalBlockNumber pbn)
{
  Slab *slab = getSlab(depot, pbn);
  if ((slab == NULL) || isUnrecoveredSlab(slab)) {
    return 0;
  }

  return getAvailableReferences(slab->referenceCounts, pbn);
}

/**********************************************************************/
bool isPhysicalDataBlock(const SlabDepot *depot, PhysicalBlockNumber pbn)
{
  if (pbn == ZERO_BLOCK) {
    return true;
  }

  SlabCount slabNumber;
  if (getSlabNumber(depot, pbn, &slabNumber) != VDO_SUCCESS) {
    return false;
  }

  SlabBlockNumber sbn;
  int result = slabBlockNumberFromPBN(depot->slabs[slabNumber], pbn, &sbn);
  return (result == VDO_SUCCESS);
}

/**********************************************************************/
BlockCount getDepotAllocatedBlocks(const SlabDepot *depot)
{
  BlockCount total = 0;
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    // The allocators are responsible for thread safety.
    total += getAllocatedBlocks(depot->allocators[zone]);
  }
  return total;
}

/**********************************************************************/
BlockCount getDepotDataBlocks(const SlabDepot *depot)
{
  // XXX This needs to be thread safe, but resize changes the slab count. It
  // does so on the admin thread (our usual caller), so it's usually safe.
  return (depot->slabCount * depot->slabConfig.dataBlocks);
}

/**********************************************************************/
BlockCount getDepotFreeBlocks(const SlabDepot *depot)
{
  /*
   * We can't ever shrink a volume except when resize fails, and we can't
   * allocate from the new slabs until after the resize succeeds, so by
   * getting the number of allocated blocks first, we ensure the allocated
   * count is always less than the capacity. Doing it in the other order on a
   * full volume could lose a race with a sucessful resize, resulting in a
   * nonsensical negative/underflow result.
   */
  BlockCount allocated = getDepotAllocatedBlocks(depot);
  memoryFence();
  return (getDepotDataBlocks(depot) - allocated);
}

/**********************************************************************/
SlabCount getDepotSlabCount(const SlabDepot *depot)
{
  return depot->slabCount;
}

/**********************************************************************/
SlabCount getDepotUnrecoveredSlabCount(const SlabDepot *depot)
{
  SlabCount total = 0;
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    // The allocators are responsible for thread safety.
    total += getUnrecoveredSlabCount(depot->allocators[zone]);
  }
  return total;
}

/**
 * The preamble of a load operation which loads the slab summary.
 *
 * <p>Implements ActionPreamble.
 **/
static void startDepotLoad(void *context, VDOCompletion *parent)
{
  SlabDepot *depot = context;
  loadSlabSummary(depot->slabSummary,
                  getCurrentManagerOperation(depot->actionManager),
                  depot->oldZoneCount, parent);
}

/**********************************************************************/
void loadSlabDepot(SlabDepot      *depot,
                   AdminStateCode  operation,
                   VDOCompletion  *parent,
                   void           *context)
{
  if (assertLoadOperation(operation, parent)) {
    scheduleOperationWithContext(depot->actionManager, operation,
                                 startDepotLoad, loadBlockAllocator, NULL,
                                 context, parent);
  }
}

/**********************************************************************/
void prepareToAllocate(SlabDepot         *depot,
                       SlabDepotLoadType  loadType,
                       VDOCompletion     *parent)
{
  depot->loadType = loadType;
  atomicStore32(&depot->zonesToScrub, depot->zoneCount);
  scheduleAction(depot->actionManager, NULL, prepareAllocatorToAllocate,
                 NULL, parent);
}

/**********************************************************************/
void updateSlabDepotSize(SlabDepot *depot)
{
  depot->lastBlock = depot->newLastBlock;
}

/**********************************************************************/
int prepareToGrowSlabDepot(SlabDepot *depot, BlockCount newSize)
{
  if ((newSize >> depot->slabSizeShift) <= depot->slabCount) {
    return VDO_INCREMENT_TOO_SMALL;
  }

  // Generate the depot configuration for the new block count.
  SlabDepotState2_0 newState;
  int result = configureState(newSize, depot->firstBlock, depot->slabConfig,
                              depot->zoneCount, &newState);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabCount newSlabCount = computeSlabCount(depot->firstBlock,
                                            newState.lastBlock,
                                            depot->slabSizeShift);
  if (newSlabCount <= depot->slabCount) {
    return logErrorWithStringError(VDO_INCREMENT_TOO_SMALL,
                                   "Depot can only grow");
  }
  if (newSlabCount == depot->newSlabCount) {
    // Check it out, we've already got all the new slabs allocated!
    return VDO_SUCCESS;
  }

  abandonNewSlabs(depot);
  result = allocateSlabs(depot, newSlabCount);
  if (result != VDO_SUCCESS) {
    abandonNewSlabs(depot);
    return result;
  }

  depot->newSize      = newSize;
  depot->oldLastBlock = depot->lastBlock;
  depot->newLastBlock = newState.lastBlock;

  return VDO_SUCCESS;
}

/**
 * Finish registering new slabs now that all of the allocators have received
 * their new slabs.
 *
 * <p>Implements ActionConclusion.
 **/
static int finishRegistration(void *context)
{
  SlabDepot *depot = context;
  depot->slabCount = depot->newSlabCount;
  FREE(depot->slabs);
  depot->slabs        = depot->newSlabs;
  depot->newSlabs     = NULL;
  depot->newSlabCount = 0;
  return VDO_SUCCESS;
}

/**********************************************************************/
void useNewSlabs(SlabDepot *depot, VDOCompletion *parent)
{
  ASSERT_LOG_ONLY(depot->newSlabs != NULL, "Must have new slabs to use");
  scheduleOperation(depot->actionManager, ADMIN_STATE_SUSPENDED_OPERATION,
                    NULL, registerNewSlabsForAllocator, finishRegistration,
                    parent);
}

/**********************************************************************/
void drainSlabDepot(SlabDepot      *depot,
                    AdminStateCode  operation,
                    VDOCompletion  *parent)
{
  scheduleOperation(depot->actionManager, operation, NULL, drainBlockAllocator,
                    NULL, parent);
}

/**********************************************************************/
void resumeSlabDepot(SlabDepot *depot, VDOCompletion *parent)
{
  if (isReadOnly(depot->readOnlyNotifier)) {
    finishCompletion(parent, VDO_READ_ONLY);
    return;
  }

  scheduleOperation(depot->actionManager, ADMIN_STATE_RESUMING, NULL,
                    resumeBlockAllocator, NULL, parent);
}

/**********************************************************************/
void commitOldestSlabJournalTailBlocks(SlabDepot      *depot,
                                       SequenceNumber  recoveryBlockNumber)
{
  if (depot == NULL) {
    return;
  }

  depot->newReleaseRequest = recoveryBlockNumber;
  scheduleTailBlockCommit(depot);
}

/**********************************************************************/
const SlabConfig *getSlabConfig(const SlabDepot *depot)
{
  return &depot->slabConfig;
}

/**********************************************************************/
SlabSummary *getSlabSummary(const SlabDepot *depot)
{
  return depot->slabSummary;
}

/**********************************************************************/
SlabSummaryZone *getSlabSummaryForZone(const SlabDepot *depot, ZoneCount zone)
{
  if (depot->slabSummary == NULL) {
    return NULL;
  }
  return getSummaryForZone(depot->slabSummary, zone);
}

/**********************************************************************/
void scrubAllUnrecoveredSlabs(SlabDepot     *depot,
                              void          *parent,
                              VDOAction     *callback,
                              VDOAction     *errorHandler,
                              ThreadID       threadID,
                              VDOCompletion *launchParent)
{
  prepareCompletion(&depot->scrubbingCompletion, callback, errorHandler,
                    threadID, parent);
  scheduleAction(depot->actionManager, NULL, scrubAllUnrecoveredSlabsInZone,
                 NULL, launchParent);
}

/**********************************************************************/
void notifyZoneFinishedScrubbing(VDOCompletion *completion)
{
  SlabDepot *depot = completion->parent;
  if (atomicAdd32(&depot->zonesToScrub, -1) == 0) {
    // We're the last!
    completeCompletion(&depot->scrubbingCompletion);
  }
}

/**********************************************************************/
bool hasUnrecoveredSlabs(SlabDepot *depot)
{
  return (atomicLoad32(&depot->zonesToScrub) > 0);
}

/**********************************************************************/
BlockCount getNewDepotSize(const SlabDepot *depot)
{
  return (depot->newSlabs == NULL) ? 0 : depot->newSize;
}

/**********************************************************************/
bool areEquivalentDepots(SlabDepot *depotA, SlabDepot *depotB)
{
  if ((depotA->firstBlock       != depotB->firstBlock)
      || (depotA->lastBlock     != depotB->lastBlock)
      || (depotA->slabCount     != depotB->slabCount)
      || (depotA->slabSizeShift != depotB->slabSizeShift)
      || (getDepotAllocatedBlocks(depotA)
          != getDepotAllocatedBlocks(depotB))) {
    return false;
  }

  for (size_t i = 0; i < depotA->slabCount; i++) {
    Slab *slabA = depotA->slabs[i];
    Slab *slabB = depotB->slabs[i];
    if ((slabA->start  != slabB->start)
        || (slabA->end != slabB->end)
        || !areEquivalentReferenceCounters(slabA->referenceCounts,
                                           slabB->referenceCounts)) {
      return false;
    }
  }

  return true;
}

/**********************************************************************/
void allocateFromLastSlab(SlabDepot *depot)
{
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    allocateFromAllocatorLastSlab(depot->allocators[zone]);
  }
}

/**********************************************************************/
BlockAllocatorStatistics
getDepotBlockAllocatorStatistics(const SlabDepot *depot)
{
  BlockAllocatorStatistics totals;
  memset(&totals, 0, sizeof(totals));

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    BlockAllocator *allocator = depot->allocators[zone];
    BlockAllocatorStatistics stats = getBlockAllocatorStatistics(allocator);
    totals.slabCount     += stats.slabCount;
    totals.slabsOpened   += stats.slabsOpened;
    totals.slabsReopened += stats.slabsReopened;
  }

  return totals;
}

/**********************************************************************/
RefCountsStatistics getDepotRefCountsStatistics(const SlabDepot *depot)
{
  RefCountsStatistics depotStats;
  memset(&depotStats, 0, sizeof(depotStats));

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    BlockAllocator *allocator = depot->allocators[zone];
    RefCountsStatistics stats = getRefCountsStatistics(allocator);
    depotStats.blocksWritten += stats.blocksWritten;
  }

  return depotStats;
}

/**********************************************************************/
SlabJournalStatistics getDepotSlabJournalStatistics(const SlabDepot *depot)
{
  SlabJournalStatistics depotStats;
  memset(&depotStats, 0, sizeof(depotStats));

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    BlockAllocator *allocator = depot->allocators[zone];
    SlabJournalStatistics stats = getSlabJournalStatistics(allocator);
    depotStats.diskFullCount += stats.diskFullCount;
    depotStats.flushCount    += stats.flushCount;
    depotStats.blockedCount  += stats.blockedCount;
    depotStats.blocksWritten += stats.blocksWritten;
    depotStats.tailBusyCount += stats.tailBusyCount;
  }

  return depotStats;
}

/**********************************************************************/
void dumpSlabDepot(const SlabDepot *depot)
{
  logInfo("Slab Depot");
  logInfo("  zoneCount=%u oldZoneCount=%u slabCount=%" PRIu32
          " activeReleaseRequest=%" PRIu64 " newReleaseRequest=%" PRIu64,
          (unsigned int) depot->zoneCount, (unsigned int) depot->oldZoneCount,
          depot->slabCount, depot->activeReleaseRequest,
          depot->newReleaseRequest);
}
