/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sma.h"

typedef struct SSmaStat SSmaStat;

#define SMA_MGMT_REF_NUM 10240

extern SSmaMgmt smaMgmt;

// declaration of static functions

static int32_t tdNewSmaEnv(SSma *pSma, int8_t smaType, SSmaEnv **ppEnv);
static int32_t tdInitSmaEnv(SSma *pSma, int8_t smaType, SSmaEnv **ppEnv);
static int32_t tdInitSmaStat(SSmaStat **pSmaStat, int8_t smaType, const SSma *pSma);
static int32_t tdRsmaStartExecutor(const SSma *pSma);
static int32_t tdRsmaStopExecutor(const SSma *pSma);
static int32_t tdDestroySmaState(SSmaStat *pSmaStat, int8_t smaType);
static void   *tdFreeSmaState(SSmaStat *pSmaStat, int8_t smaType);
static void    tdDestroyRSmaStat(void *pRSmaStat);

/**
 * @brief rsma init
 *
 * @return int32_t
 */
// implementation
int32_t smaInit() {
  int32_t code = 0;
  int8_t  old;
  int32_t nLoops = 0;
  while (1) {
    old = atomic_val_compare_exchange_8(&smaMgmt.inited, 0, 2);
    if (old != 2) break;
    if (++nLoops > 1000) {
      TAOS_UNUSED(sched_yield());
      nLoops = 0;
    }
  }

  if (old == 0) {
    // init tref rset
    smaMgmt.rsetId = taosOpenRef(SMA_MGMT_REF_NUM, tdDestroyRSmaStat);

    if (smaMgmt.rsetId < 0) {
      atomic_store_8(&smaMgmt.inited, 0);
      code = terrno;
      smaError("failed to init sma rset since %s", tstrerror(code));
      TAOS_RETURN(code);
    }

    int32_t type = (8 == POINTER_BYTES) ? TSDB_DATA_TYPE_UBIGINT : TSDB_DATA_TYPE_UINT;
    smaMgmt.refHash = taosHashInit(64, taosGetDefaultHashFunction(type), true, HASH_ENTRY_LOCK);
    // init fetch timer handle
    smaMgmt.tmrHandle = taosTmrInit(10000, 100, 10000, "RSMA");

    if (!smaMgmt.refHash || !smaMgmt.tmrHandle) {
      code = terrno;
      taosCloseRef(smaMgmt.rsetId);
      if (smaMgmt.refHash) {
        taosHashCleanup(smaMgmt.refHash);
        smaMgmt.refHash = NULL;
      }
      atomic_store_8(&smaMgmt.inited, 0);
      smaError("failed to init sma tmr handle since %s", tstrerror(code));
      TAOS_RETURN(code);
    }

    atomic_store_8(&smaMgmt.inited, 1);
    smaInfo("sma mgmt env is initialized, rsetId:%d, tmrHandle:%p", smaMgmt.rsetId, smaMgmt.tmrHandle);
  }

  TAOS_RETURN(code);
}

/**
 * @brief rsma cleanup
 *
 */
void smaCleanUp() {
  int8_t  old;
  int32_t nLoops = 0;
  while (1) {
    old = atomic_val_compare_exchange_8(&smaMgmt.inited, 1, 2);
    if (old != 2) break;
    if (++nLoops > 1000) {
      TAOS_UNUSED(sched_yield());
      nLoops = 0;
    }
  }

  if (old == 1) {
    taosCloseRef(smaMgmt.rsetId);
    taosHashCleanup(smaMgmt.refHash);
    smaMgmt.refHash = NULL;
    taosTmrCleanUp(smaMgmt.tmrHandle);
    smaInfo("sma mgmt env is cleaned up, rsetId:%d, tmrHandle:%p", smaMgmt.rsetId, smaMgmt.tmrHandle);
    atomic_store_8(&smaMgmt.inited, 0);
  }
}

static int32_t tdNewSmaEnv(SSma *pSma, int8_t smaType, SSmaEnv **ppEnv) {
  int32_t  code = 0;
  SSmaEnv *pEnv = NULL;

  pEnv = (SSmaEnv *)taosMemoryCalloc(1, sizeof(SSmaEnv));
  if (!pEnv) {
    return terrno;
  }
  *ppEnv = pEnv;

  SMA_ENV_TYPE(pEnv) = smaType;

  taosInitRWLatch(&(pEnv->lock));

  (smaType == TSDB_SMA_TYPE_TIME_RANGE) ? atomic_store_ptr(&SMA_TSMA_ENV(pSma), *ppEnv)
                                        : atomic_store_ptr(&SMA_RSMA_ENV(pSma), *ppEnv);

  if ((code = tdInitSmaStat(&SMA_ENV_STAT(pEnv), smaType, pSma)) != TSDB_CODE_SUCCESS) {
    TAOS_UNUSED(tdFreeSmaEnv(pEnv));
    *ppEnv = NULL;
    (smaType == TSDB_SMA_TYPE_TIME_RANGE) ? atomic_store_ptr(&SMA_TSMA_ENV(pSma), NULL)
                                          : atomic_store_ptr(&SMA_RSMA_ENV(pSma), NULL);
    TAOS_RETURN(code);
  }

  TAOS_RETURN(code);
}

static int32_t tdInitSmaEnv(SSma *pSma, int8_t smaType, SSmaEnv **ppEnv) {
  if (!(*ppEnv)) {
    TAOS_CHECK_RETURN(tdNewSmaEnv(pSma, smaType, ppEnv));
  }

  TAOS_RETURN(TSDB_CODE_SUCCESS);
}

/**
 * @brief Release resources allocated for its member fields, not including itself.
 *
 * @param pSmaEnv
 * @return int32_t
 */
void tdDestroySmaEnv(SSmaEnv *pSmaEnv) {
  if (pSmaEnv) {
    pSmaEnv->pStat = tdFreeSmaState(pSmaEnv->pStat, SMA_ENV_TYPE(pSmaEnv));
  }
}

void *tdFreeSmaEnv(SSmaEnv *pSmaEnv) {
  if (pSmaEnv) {
    tdDestroySmaEnv(pSmaEnv);
    taosMemoryFreeClear(pSmaEnv);
  }
  return NULL;
}

static void tRSmaInfoHashFreeNode(void *data) {
  SRSmaInfo     *pRSmaInfo = NULL;
  SRSmaInfoItem *pItem = NULL;

  if ((pRSmaInfo = *(SRSmaInfo **)data)) {
    if ((pItem = RSMA_INFO_ITEM((SRSmaInfo *)pRSmaInfo, 0)) && pItem->level) {
      if (TSDB_CODE_SUCCESS != taosHashRemove(smaMgmt.refHash, &pItem, POINTER_BYTES)) {
        smaError("failed to hash remove %s:%d", __FUNCTION__, __LINE__);
      }
    }
    if ((pItem = RSMA_INFO_ITEM((SRSmaInfo *)pRSmaInfo, 1)) && pItem->level) {
      if (TSDB_CODE_SUCCESS != taosHashRemove(smaMgmt.refHash, &pItem, POINTER_BYTES)) {
        smaError("failed to hash remove %s:%d", __FUNCTION__, __LINE__);
      }
    }
    TAOS_UNUSED(tdFreeRSmaInfo(pRSmaInfo->pSma, pRSmaInfo));
  }
}

static int32_t tdInitSmaStat(SSmaStat **pSmaStat, int8_t smaType, const SSma *pSma) {
  int32_t code = 0;
  int32_t lino = 0;

  if (*pSmaStat) {      // no lock
    TAOS_RETURN(code);  // success, return directly
  }

  /**
   *  1. Lazy mode utilized when init SSmaStat to update expire window(or hungry mode when tdNew).
   *  2. Currently, there is mutex lock when init SSmaEnv, thus no need add lock on SSmaStat, and please add lock if
   * tdInitSmaStat invoked in other multithread environment later.
   */
  if (!(*pSmaStat)) {
    *pSmaStat = (SSmaStat *)taosMemoryCalloc(1, sizeof(SSmaStat) + sizeof(TdThread) * tsNumOfVnodeRsmaThreads);
    if (!(*pSmaStat)) {
      code = terrno;
      TAOS_CHECK_GOTO(code, &lino, _exit);
    }

    if (smaType == TSDB_SMA_TYPE_ROLLUP) {
      SRSmaStat *pRSmaStat = (SRSmaStat *)(*pSmaStat);
      pRSmaStat->pSma = (SSma *)pSma;
      atomic_store_8(RSMA_TRIGGER_STAT(pRSmaStat), TASK_TRIGGER_STAT_INIT);
      if (tsem_init(&pRSmaStat->notEmpty, 0, 0) != 0) {
        code = terrno;
        TAOS_CHECK_GOTO(code, &lino, _exit);
      }
      if (!(pRSmaStat->blocks = taosArrayInit(1, sizeof(SSDataBlock)))) {
        code = terrno;
        TAOS_CHECK_GOTO(code, &lino, _exit);
      }
      SSDataBlock datablock = {.info.type = STREAM_CHECKPOINT};
      TSDB_CHECK_NULL(taosArrayPush(pRSmaStat->blocks, &datablock), code, lino, _exit, terrno);

      // init smaMgmt
      TAOS_CHECK_GOTO(smaInit(), &lino, _exit);

      int64_t refId = taosAddRef(smaMgmt.rsetId, pRSmaStat);
      if (refId < 0) {
        code = terrno;
        smaError("vgId:%d, taosAddRef refId:%" PRIi64 " to rsetId rsetId:%d max:%d failed since:%s", SMA_VID(pSma),
                 refId, smaMgmt.rsetId, SMA_MGMT_REF_NUM, tstrerror(code));
        TAOS_CHECK_GOTO(code, &lino, _exit);
      } else {
        smaDebug("vgId:%d, taosAddRef refId:%" PRIi64 " to rsetId rsetId:%d max:%d succeed", SMA_VID(pSma), refId,
                 smaMgmt.rsetId, SMA_MGMT_REF_NUM);
      }
      pRSmaStat->refId = refId;

      // init hash
      RSMA_INFO_HASH(pRSmaStat) = taosHashInit(
          RSMA_TASK_INFO_HASH_SLOT, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, HASH_ENTRY_LOCK);
      if (!RSMA_INFO_HASH(pRSmaStat)) {
        code = terrno;
        TAOS_CHECK_GOTO(code, &lino, _exit);
      }
      taosHashSetFreeFp(RSMA_INFO_HASH(pRSmaStat), tRSmaInfoHashFreeNode);

      TAOS_CHECK_GOTO(tdRsmaStartExecutor(pSma), &lino, _exit);

      taosInitRWLatch(RSMA_FS_LOCK(pRSmaStat));
    } else if (smaType == TSDB_SMA_TYPE_TIME_RANGE) {
      // TODO
    }
  }
_exit:
  if (code) {
    smaError("vgId:%d, %s failed at line %d since %s", SMA_VID(pSma), __func__, lino, tstrerror(code));
  } else {
    smaDebug("vgId:%d, %s succeed, type:%" PRIi8, SMA_VID(pSma), __func__, smaType);
  }
  TAOS_RETURN(code);
}

static void tdDestroyTSmaStat(STSmaStat *pStat) {
  if (pStat) {
    smaDebug("destroy tsma stat");
    tDestroyTSma(pStat->pTSma);
    taosMemoryFreeClear(pStat->pTSma);
    taosMemoryFreeClear(pStat->pTSchema);
  }
}

static void tdDestroyRSmaStat(void *pRSmaStat) {
  if (pRSmaStat) {
    SRSmaStat *pStat = (SRSmaStat *)pRSmaStat;
    SSma      *pSma = pStat->pSma;
    smaDebug("vgId:%d, destroy rsma stat %p", SMA_VID(pSma), pRSmaStat);
    // step 1: set rsma trigger stat cancelled
    atomic_store_8(RSMA_TRIGGER_STAT(pStat), TASK_TRIGGER_STAT_CANCELLED);

    // step 2: wait for all triggered fetch tasks to finish
    int32_t nLoops = 0;
    while (1) {
      if (T_REF_VAL_GET((SSmaStat *)pStat) == 0) {
        smaDebug("vgId:%d, rsma fetch tasks are all finished", SMA_VID(pSma));
        break;
      } else {
        smaDebug("vgId:%d, rsma fetch tasks are not all finished yet", SMA_VID(pSma));
      }
      TD_SMA_LOOPS_CHECK(nLoops, 1000);
    }

    // step 3:
    TAOS_UNUSED(tdRsmaStopExecutor(pSma));

    // step 4: destroy the rsma info and associated fetch tasks
    taosHashCleanup(RSMA_INFO_HASH(pStat));

    // step 5: free pStat
    if (tsem_destroy(&(pStat->notEmpty)) != 0) {
      smaError("vgId:%d, failed to destroy notEmpty semaphore for rsma stat:%p since %s", SMA_VID(pSma), pRSmaStat,
               tstrerror(terrno));
    }
    taosArrayDestroy(pStat->blocks);
    taosMemoryFreeClear(pStat);
  }
}

static void *tdFreeSmaState(SSmaStat *pSmaStat, int8_t smaType) {
  TAOS_UNUSED(tdDestroySmaState(pSmaStat, smaType));
  if (smaType == TSDB_SMA_TYPE_TIME_RANGE) {
    taosMemoryFreeClear(pSmaStat);
  }
  // tref used to free rsma stat

  return NULL;
}

/**
 * @brief Release resources allocated for its member fields, not including itself.
 *
 * @param pSmaStat
 * @return int32_t
 */

static int32_t tdDestroySmaState(SSmaStat *pSmaStat, int8_t smaType) {
  if (pSmaStat) {
    if (smaType == TSDB_SMA_TYPE_TIME_RANGE) {
      tdDestroyTSmaStat(SMA_STAT_TSMA(pSmaStat));
    } else if (smaType == TSDB_SMA_TYPE_ROLLUP) {
      SRSmaStat *pRSmaStat = &pSmaStat->rsmaStat;
      int32_t    vid = SMA_VID(pRSmaStat->pSma);
      int64_t    refId = RSMA_REF_ID(pRSmaStat);
      if (taosRemoveRef(smaMgmt.rsetId, refId) < 0) {
        smaError("vgId:%d, remove refId:%" PRIi64 " from rsmaRef:%" PRIi32 " failed since %s", vid, refId,
                 smaMgmt.rsetId, terrstr());
      } else {
        smaDebug("vgId:%d, remove refId:%" PRIi64 " from rsmaRef:%" PRIi32 " succeed", vid, refId, smaMgmt.rsetId);
      }
    } else {
      smaError("%s failed at line %d since Unknown type", __func__, __LINE__);
    }
  }
  return 0;
}

int32_t tdLockSma(SSma *pSma) {
  int errCode = taosThreadMutexLock(&pSma->mutex);
  if (errCode != 0) {
    int32_t code = TAOS_SYSTEM_ERROR(errCode);
    smaError("vgId:%d, failed to lock since %s", SMA_VID(pSma), tstrerror(code));
    TAOS_RETURN(code);
  }
  pSma->locked = true;
  TAOS_RETURN(0);
}

int32_t tdUnLockSma(SSma *pSma) {
  pSma->locked = false;
  int errCode = taosThreadMutexUnlock(&pSma->mutex);
  if (errCode != 0) {
    int32_t code = TAOS_SYSTEM_ERROR(errCode);
    smaError("vgId:%d, failed to unlock since %s", SMA_VID(pSma), tstrerror(code));
    TAOS_RETURN(code);
  }
  TAOS_RETURN(TSDB_CODE_SUCCESS);
}

int32_t tdCheckAndInitSmaEnv(SSma *pSma, int8_t smaType) {
  int32_t  code = 0;
  SSmaEnv *pEnv = NULL;

  switch (smaType) {
    case TSDB_SMA_TYPE_TIME_RANGE:
      if ((pEnv = (SSmaEnv *)atomic_load_ptr(&SMA_TSMA_ENV(pSma)))) {
        TAOS_RETURN(TSDB_CODE_SUCCESS);
      }
      break;
    case TSDB_SMA_TYPE_ROLLUP:
      if ((pEnv = (SSmaEnv *)atomic_load_ptr(&SMA_RSMA_ENV(pSma)))) {
        TAOS_RETURN(TSDB_CODE_SUCCESS);
      }
      break;
    default:
      smaError("vgId:%d, undefined smaType:%" PRIi8, SMA_VID(pSma), smaType);
      TAOS_RETURN(TSDB_CODE_INVALID_PARA);
  }

  // init sma env
  TAOS_UNUSED(tdLockSma(pSma));
  pEnv = (smaType == TSDB_SMA_TYPE_TIME_RANGE) ? atomic_load_ptr(&SMA_TSMA_ENV(pSma))
                                               : atomic_load_ptr(&SMA_RSMA_ENV(pSma));
  if (!pEnv) {
    if ((code = tdInitSmaEnv(pSma, smaType, &pEnv)) < 0) {
      TAOS_UNUSED(tdUnLockSma(pSma));
      TAOS_RETURN(code);
    }
  }
  TAOS_UNUSED(tdUnLockSma(pSma));

  TAOS_RETURN(TSDB_CODE_SUCCESS);
}

void *tdRSmaExecutorFunc(void *param) {
  setThreadName("vnode-rsma");

  if (tdRSmaProcessExecImpl((SSma *)param, RSMA_EXEC_OVERFLOW) < 0) {
    smaError("vgId:%d, failed to process rsma exec", SMA_VID((SSma *)param));
  }
  return NULL;
}

static int32_t tdRsmaStartExecutor(const SSma *pSma) {
  int32_t      code = 0;
  TdThreadAttr thAttr = {0};
  (void)taosThreadAttrInit(&thAttr);
  (void)taosThreadAttrSetDetachState(&thAttr, PTHREAD_CREATE_JOINABLE);

  SSmaEnv  *pEnv = SMA_RSMA_ENV(pSma);
  SSmaStat *pStat = SMA_ENV_STAT(pEnv);
  TdThread *pthread = (TdThread *)&pStat->data;

  for (int32_t i = 0; i < tsNumOfVnodeRsmaThreads; ++i) {
    if (taosThreadCreate(&pthread[i], &thAttr, tdRSmaExecutorFunc, (void *)pSma) != 0) {
      code = TAOS_SYSTEM_ERROR(ERRNO);
      smaError("vgId:%d, failed to create pthread for rsma since %s", SMA_VID(pSma), tstrerror(code));
      TAOS_RETURN(code);
    }
    smaDebug("vgId:%d, success to create pthread for rsma", SMA_VID(pSma));
  }

  (void)taosThreadAttrDestroy(&thAttr);
  TAOS_RETURN(code);
}

static int32_t tdRsmaStopExecutor(const SSma *pSma) {
  if (pSma && VND_IS_RSMA(pSma->pVnode)) {
    SSmaEnv   *pEnv = NULL;
    SSmaStat  *pStat = NULL;
    SRSmaStat *pRSmaStat = NULL;
    TdThread  *pthread = NULL;

    if (!(pEnv = SMA_RSMA_ENV(pSma)) || !(pStat = SMA_ENV_STAT(pEnv))) {
      TAOS_RETURN(0);
    }

    pEnv->flag |= SMA_ENV_FLG_CLOSE;
    pRSmaStat = (SRSmaStat *)pStat;
    pthread = (TdThread *)&pStat->data;

    for (int32_t i = 0; i < tsNumOfVnodeRsmaThreads; ++i) {
      if (tsem_post(&(pRSmaStat->notEmpty)) != 0) {
        smaError("vgId:%d, failed to post notEmpty semaphore for rsma since %s", SMA_VID(pSma), tstrerror(terrno));
      }
    }

    for (int32_t i = 0; i < tsNumOfVnodeRsmaThreads; ++i) {
      if (taosCheckPthreadValid(pthread[i])) {
        smaDebug("vgId:%d, start to join pthread for rsma:%" PRId64, SMA_VID(pSma), taosGetPthreadId(pthread[i]));
        (void)taosThreadJoin(pthread[i], NULL);
      }
    }

    smaInfo("vgId:%d, rsma executor stopped, number:%d", SMA_VID(pSma), tsNumOfVnodeRsmaThreads);
  }
  TAOS_RETURN(0);
}
