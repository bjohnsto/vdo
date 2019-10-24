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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoResizeLogical.c#5 $
 */

#include "vdoResizeLogical.h"

#include "logger.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "vdoInternal.h"

typedef enum {
  GROW_LOGICAL_PHASE_START = 0,
  GROW_LOGICAL_PHASE_GROW_BLOCK_MAP,
  GROW_LOGICAL_PHASE_END,
  GROW_LOGICAL_PHASE_ERROR,
} GrowLogicalPhase;

static const char *GROW_LOGICAL_PHASE_NAMES[] = {
  "GROW_LOGICAL_PHASE_START",
  "GROW_LOGICAL_PHASE_GROW_BLOCK_MAP",
  "GROW_LOGICAL_PHASE_END",
  "GROW_LOGICAL_PHASE_ERROR",
};

/**
 * Implements ThreadIDGetterForPhase.
 **/
__attribute__((warn_unused_result))
static ThreadID getThreadIDForPhase(AdminCompletion *adminCompletion)
{
  return getAdminThread(getThreadConfig(adminCompletion->completion.parent));
}

/**
 * Callback to initiate a grow logical, registered in performGrowLogical().
 *
 * @param completion  The sub-task completion
 **/
static void growLogicalCallback(VDOCompletion *completion)
{
  AdminCompletion *adminCompletion = adminCompletionFromSubTask(completion);
  assertAdminOperationType(adminCompletion, ADMIN_OPERATION_GROW_LOGICAL);
  assertAdminPhaseThread(adminCompletion, __func__, GROW_LOGICAL_PHASE_NAMES);

  VDO *vdo = adminCompletion->completion.parent;
  switch (adminCompletion->phase++) {
  case GROW_LOGICAL_PHASE_START:
    if (isReadOnly(vdo->readOnlyNotifier)) {
      logErrorWithStringError(VDO_READ_ONLY,
                              "Can't grow logical size of a read-only VDO");
      finishCompletion(resetAdminSubTask(completion), VDO_READ_ONLY);
      return;
    }

    if (startOperationWithWaiter(&vdo->adminState,
                                 ADMIN_STATE_SUSPENDED_OPERATION,
                                 &adminCompletion->completion)) {

      vdo->config.logicalBlocks = getNewEntryCount(getBlockMap(vdo));
      saveVDOComponentsAsync(vdo, resetAdminSubTask(completion));
    }

    return;

  case GROW_LOGICAL_PHASE_GROW_BLOCK_MAP:
    growBlockMap(getBlockMap(vdo), resetAdminSubTask(completion));
    return;

  case GROW_LOGICAL_PHASE_END:
    break;

  case GROW_LOGICAL_PHASE_ERROR:
    enterReadOnlyMode(vdo->readOnlyNotifier, completion->result);
    break;

  default:
    setCompletionResult(resetAdminSubTask(completion), UDS_BAD_STATE);
  }

  finishOperationWithResult(&vdo->adminState, completion->result);
}

/**
 * Handle an error during the grow physical process.
 *
 * @param completion  The sub-task completion
 **/
static void handleGrowthError(VDOCompletion *completion)
{
  AdminCompletion *adminCompletion = adminCompletionFromSubTask(completion);
  if (adminCompletion->phase == GROW_LOGICAL_PHASE_GROW_BLOCK_MAP) {
    // We've failed to write the new size in the super block, so set our
    // in memory config back to the old size.
    VDO      *vdo = adminCompletion->completion.parent;
    BlockMap *map = getBlockMap(vdo);
    vdo->config.logicalBlocks = getNumberOfBlockMapEntries(map);
    abandonBlockMapGrowth(map);
  }

  adminCompletion->phase = GROW_LOGICAL_PHASE_ERROR;
  growLogicalCallback(completion);
}

/**********************************************************************/
int performGrowLogical(VDO *vdo, BlockCount newLogicalBlocks)
{
  if (getNewEntryCount(getBlockMap(vdo)) != newLogicalBlocks) {
    return VDO_PARAMETER_MISMATCH;
  }

  return performAdminOperation(vdo, ADMIN_OPERATION_GROW_LOGICAL,
                               getThreadIDForPhase, growLogicalCallback,
                               handleGrowthError);
}

/**********************************************************************/
int prepareToGrowLogical(VDO *vdo, BlockCount newLogicalBlocks)
{
  if (newLogicalBlocks < vdo->config.logicalBlocks) {
    return logErrorWithStringError(VDO_PARAMETER_MISMATCH,
                                   "Can't shrink VDO logical size from its "
                                   "current value of %" PRIu64,
                                   vdo->config.logicalBlocks);
  }

  if (newLogicalBlocks == vdo->config.logicalBlocks) {
    return logErrorWithStringError(VDO_PARAMETER_MISMATCH,
                                   "Can't grow VDO logical size to its "
                                   "current value of %" PRIu64,
                                   vdo->config.logicalBlocks);
  }

  return prepareToGrowBlockMap(getBlockMap(vdo), newLogicalBlocks);
}
