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

#include "catalog.h"
#include "command.h"
#include "query.h"
#include "schInt.h"
#include "tglobal.h"
#include "tmisce.h"
#include "tmsg.h"
#include "tref.h"
#include "trpc.h"

// clang-format off
int32_t schValidateRspMsgType(SSchJob *pJob, SSchTask *pTask, int32_t msgType) {
  int32_t lastMsgType = pTask->lastMsgType;
  int32_t taskStatus = SCH_GET_TASK_STATUS(pTask);
  int32_t reqMsgType = (msgType & 1U) ? msgType : (msgType - 1);
  switch (msgType) {
    case TDMT_SCH_LINK_BROKEN:
    case TDMT_SCH_EXPLAIN_RSP:
      return TSDB_CODE_SUCCESS;
    case TDMT_SCH_FETCH_RSP:
    case TDMT_SCH_MERGE_FETCH_RSP:
      if (lastMsgType != reqMsgType) {
        SCH_TASK_ELOG("rsp msg type mis-match, last sent msgType:%s, rspType:%s", TMSG_INFO(lastMsgType),
                      TMSG_INFO(msgType));
        SCH_ERR_RET(TSDB_CODE_QW_MSG_ERROR);
      }
      if (taskStatus != JOB_TASK_STATUS_FETCH) {
        SCH_TASK_ELOG("rsp msg conflicted with task status, status:%s, rspType:%s", jobTaskStatusStr(taskStatus),
                      TMSG_INFO(msgType));
        SCH_ERR_RET(TSDB_CODE_QW_MSG_ERROR);
      }

      return TSDB_CODE_SUCCESS;
    case TDMT_SCH_MERGE_QUERY_RSP:
    case TDMT_SCH_QUERY_RSP:
    case TDMT_VND_CREATE_TABLE_RSP:
    case TDMT_VND_DROP_TABLE_RSP:
    case TDMT_VND_ALTER_TABLE_RSP:
    case TDMT_VND_SUBMIT_RSP:
    case TDMT_VND_DELETE_RSP:
    case TDMT_VND_COMMIT_RSP:
      break;
    default:
      SCH_TASK_ELOG("unknown rsp msg, type:%s, status:%s", TMSG_INFO(msgType), jobTaskStatusStr(taskStatus));
      SCH_ERR_RET(TSDB_CODE_INVALID_MSG);
  }

  if (lastMsgType != reqMsgType) {
    SCH_TASK_ELOG("rsp msg type mis-match, last sent msgType:%s, rspType:%s", TMSG_INFO(lastMsgType),
                  TMSG_INFO(msgType));
    SCH_ERR_RET(TSDB_CODE_QW_MSG_ERROR);
  }

  if (taskStatus != JOB_TASK_STATUS_EXEC) {
    SCH_TASK_ELOG("rsp msg conflicted with task status, status:%s, rspType:%s", jobTaskStatusStr(taskStatus),
                  TMSG_INFO(msgType));
    SCH_ERR_RET(TSDB_CODE_QW_MSG_ERROR);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t schProcessFetchRsp(SSchJob *pJob, SSchTask *pTask, char *msg, int32_t rspCode) {
  SRetrieveTableRsp *rsp = (SRetrieveTableRsp *)msg;
  int32_t code = 0;
  
  SCH_ERR_JRET(rspCode);
  
  if (NULL == msg) {
    SCH_ERR_RET(TSDB_CODE_QRY_INVALID_INPUT);
  }
  
  if (SCH_IS_EXPLAIN_JOB(pJob)) {
    if (rsp->completed) {
      SRetrieveTableRsp *pRsp = NULL;
      SCH_ERR_JRET(qExecExplainEnd(pJob->explainCtx, &pRsp));
      if (pRsp) {
        SCH_ERR_JRET(schProcessOnExplainDone(pJob, pTask, pRsp));
      } else {
        SCH_ERR_JRET(schNotifyJobAllTasks(pJob, pTask, TASK_NOTIFY_FINISHED));
      }
  
      taosMemoryFreeClear(msg);
  
      return TSDB_CODE_SUCCESS;
    }
  
    SCH_ERR_JRET(schLaunchFetchTask(pJob));
  
    taosMemoryFreeClear(msg);
  
    return TSDB_CODE_SUCCESS;
  }
  
  if (pJob->fetchRes) {
    SCH_TASK_ELOG("got fetch rsp while res already exists, res:%p", pJob->fetchRes);
    SCH_ERR_JRET(TSDB_CODE_SCH_STATUS_ERROR);
  }
  
  atomic_store_ptr(&pJob->fetchRes, rsp);
  (void)atomic_add_fetch_64(&pJob->resNumOfRows, htobe64(rsp->numOfRows));
  
  if (rsp->completed) {
    SCH_SET_TASK_STATUS(pTask, JOB_TASK_STATUS_SUCC);
  }
  
  SCH_TASK_DLOG("got fetch rsp, rows:%" PRId64 ", complete:%d", htobe64(rsp->numOfRows), rsp->completed);

  msg = NULL;
  schProcessOnDataFetched(pJob);

_return:

  taosMemoryFreeClear(msg);

  SCH_RET(code);
}

int32_t schProcessExplainRsp(SSchJob *pJob, SSchTask *pTask, SExplainRsp *rsp) {
  SRetrieveTableRsp *pRsp = NULL;
  SCH_ERR_RET(qExplainUpdateExecInfo(pJob->explainCtx, rsp, pTask->plan->id.groupId, &pRsp));
  
  if (pRsp) {
    SCH_ERR_RET(schProcessOnExplainDone(pJob, pTask, pRsp));
  }

  return TSDB_CODE_SUCCESS;
}

int32_t schProcessResponseMsg(SSchJob *pJob, SSchTask *pTask, SDataBuf *pMsg, int32_t rspCode) {
  int32_t code = 0;
  int32_t msgSize = pMsg->len;
  int32_t msgType = pMsg->msgType;

  pTask->redirectCtx.inRedirect = false;

  switch (msgType) {
    case TDMT_VND_COMMIT_RSP: {
      SCH_ERR_JRET(rspCode);
      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));
      break;
    }
    case TDMT_VND_CREATE_TABLE_RSP: {
      SVCreateTbBatchRsp batchRsp = {0};
      if (pMsg->pData) {
        SDecoder coder = {0};
        tDecoderInit(&coder, pMsg->pData, msgSize);
        code = tDecodeSVCreateTbBatchRsp(&coder, &batchRsp);
        if (TSDB_CODE_SUCCESS == code && batchRsp.nRsps > 0) {
          SCH_LOCK(SCH_WRITE, &pJob->resLock);
          if (NULL == pJob->execRes.res) {
            pJob->execRes.res = (void*)taosArrayInit(batchRsp.nRsps, POINTER_BYTES);
            if (NULL == pJob->execRes.res) {
              code = terrno;
              SCH_UNLOCK(SCH_WRITE, &pJob->resLock);
              
              tDecoderClear(&coder);
              SCH_ERR_JRET(code);
            }
            
            pJob->execRes.msgType = TDMT_VND_CREATE_TABLE;
          }

          for (int32_t i = 0; i < batchRsp.nRsps; ++i) {
            SVCreateTbRsp *rsp = batchRsp.pRsps + i;
            if (rsp->pMeta) {
              if (NULL == taosArrayPush((SArray*)pJob->execRes.res, &rsp->pMeta)) {
                code = terrno;
                SCH_UNLOCK(SCH_WRITE, &pJob->resLock);
                
                tDecoderClear(&coder);
                SCH_ERR_JRET(code);
              }
            }
            
            if (TSDB_CODE_SUCCESS != rsp->code) {
              code = rsp->code;
            }
          }

          if (taosArrayGetSize((SArray*)pJob->execRes.res) <= 0) {        
            taosArrayDestroy((SArray*)pJob->execRes.res);
            pJob->execRes.res = NULL;
          }
          SCH_UNLOCK(SCH_WRITE, &pJob->resLock);
        }
        
        tDecoderClear(&coder);
        SCH_ERR_JRET(code);
      }

      SCH_ERR_JRET(rspCode);
      taosMemoryFreeClear(pMsg->pData);

      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));
      break;
    }
    case TDMT_VND_DROP_TABLE_RSP: {
      SVDropTbBatchRsp batchRsp = {0};
      if (pMsg->pData) {
        SDecoder coder = {0};
        tDecoderInit(&coder, pMsg->pData, msgSize);
        code = tDecodeSVDropTbBatchRsp(&coder, &batchRsp);
        if (TSDB_CODE_SUCCESS == code && batchRsp.nRsps > 0) {
          for (int32_t i = 0; i < batchRsp.nRsps; ++i) {
            SVDropTbRsp *rsp = batchRsp.pRsps + i;
            if (TSDB_CODE_SUCCESS != rsp->code) {
              code = rsp->code;
              tDecoderClear(&coder);
              SCH_ERR_JRET(code);
            }
          }
        }
        tDecoderClear(&coder);
        SCH_ERR_JRET(code);
      }

      SCH_ERR_JRET(rspCode);
      taosMemoryFreeClear(pMsg->pData);

      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));
      break;
    }
    case TDMT_VND_ALTER_TABLE_RSP: {
      SVAlterTbRsp rsp = {0};
      if (pMsg->pData) {
        SDecoder coder = {0};
        tDecoderInit(&coder, pMsg->pData, msgSize);
        code = tDecodeSVAlterTbRsp(&coder, &rsp);
        tDecoderClear(&coder);
        SCH_ERR_JRET(code);
        SCH_ERR_JRET(rsp.code);

        pJob->execRes.res = rsp.pMeta;
        pJob->execRes.msgType = TDMT_VND_ALTER_TABLE;
      }

      SCH_ERR_JRET(rspCode);

      if (NULL == pMsg->pData) {
        SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
      }

      taosMemoryFreeClear(pMsg->pData);

      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));
      break;
    }
    case TDMT_VND_SUBMIT_RSP: {
      SCH_ERR_JRET(rspCode);

      if (pMsg->pData) {
        SDecoder    coder = {0};
        SSubmitRsp2 *rsp = taosMemoryMalloc(sizeof(*rsp));
        if (NULL == rsp) {
          SCH_ERR_JRET(terrno);
        }
        tDecoderInit(&coder, pMsg->pData, msgSize);
        code = tDecodeSSubmitRsp2(&coder, rsp);
        tDecoderClear(&coder);
        if (code) {
          SCH_TASK_ELOG("tDecodeSSubmitRsp2 failed, code:%d", code);
          tDestroySSubmitRsp2(rsp, TSDB_MSG_FLG_DECODE);
          taosMemoryFree(rsp);
          SCH_ERR_JRET(code);
        }

        (void)atomic_add_fetch_64(&pJob->resNumOfRows, rsp->affectedRows);

        int32_t createTbRspNum = taosArrayGetSize(rsp->aCreateTbRsp);
        SCH_TASK_DLOG("submit succeed, affectedRows:%d, createTbRspNum:%d", rsp->affectedRows, createTbRspNum);

        if (rsp->aCreateTbRsp && taosArrayGetSize(rsp->aCreateTbRsp) > 0) {
          SCH_LOCK(SCH_WRITE, &pJob->resLock);
          if (pJob->execRes.res) {
            SSubmitRsp2 *sum = pJob->execRes.res;
            sum->affectedRows += rsp->affectedRows;
            if (sum->aCreateTbRsp) {
              if (NULL == taosArrayAddAll(sum->aCreateTbRsp, rsp->aCreateTbRsp)) {
                code = terrno;
                SCH_UNLOCK(SCH_WRITE, &pJob->resLock);
                SCH_ERR_JRET(code);
              }
              
              taosArrayDestroy(rsp->aCreateTbRsp);
            } else {
              TSWAP(sum->aCreateTbRsp, rsp->aCreateTbRsp);
            }
            taosMemoryFree(rsp);
          } else {
            pJob->execRes.res = rsp;
            pJob->execRes.msgType = TDMT_VND_SUBMIT;
          }
          pJob->execRes.numOfBytes += pTask->msgLen;
          SCH_UNLOCK(SCH_WRITE, &pJob->resLock);
        } else {
          SCH_LOCK(SCH_WRITE, &pJob->resLock);
          pJob->execRes.numOfBytes += pTask->msgLen;
          if (NULL == pJob->execRes.res) {
            TSWAP(pJob->execRes.res, rsp);
            pJob->execRes.msgType = TDMT_VND_SUBMIT;
          }
          SCH_UNLOCK(SCH_WRITE, &pJob->resLock);
          tDestroySSubmitRsp2(rsp, TSDB_MSG_FLG_DECODE);
          taosMemoryFree(rsp);
        }
      }

      taosMemoryFreeClear(pMsg->pData);

      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));

      break;
    }
    case TDMT_VND_DELETE_RSP: {
      SCH_ERR_JRET(rspCode);

      if (pMsg->pData) {
        SDecoder    coder = {0};
        SVDeleteRsp rsp = {0};
        tDecoderInit(&coder, pMsg->pData, msgSize);
        if (tDecodeSVDeleteRsp(&coder, &rsp) < 0) {
          code = terrno;
          tDecoderClear(&coder);
          SCH_ERR_JRET(code);
        }
        tDecoderClear(&coder);

        (void)atomic_add_fetch_64(&pJob->resNumOfRows, rsp.affectedRows);
        SCH_TASK_DLOG("delete succeed, affectedRows:%" PRId64, rsp.affectedRows);
      }

      taosMemoryFreeClear(pMsg->pData);

      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));

      break;
    }
    case TDMT_SCH_QUERY_RSP:
    case TDMT_SCH_MERGE_QUERY_RSP: {
      SCH_ERR_JRET(rspCode);
      if (NULL == pMsg->pData) {
        SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
      }

      if (taosArrayGetSize(pTask->parents) == 0 && SCH_IS_EXPLAIN_JOB(pJob) && SCH_IS_INSERT_JOB(pJob)) {
        SRetrieveTableRsp *pRsp = NULL;
        SCH_ERR_JRET(qExecExplainEnd(pJob->explainCtx, &pRsp));
        if (pRsp) {
          SCH_ERR_JRET(schProcessOnExplainDone(pJob, pTask, pRsp));
        }
      }

      SQueryTableRsp rsp = {0};
      if (tDeserializeSQueryTableRsp(pMsg->pData, msgSize, &rsp) < 0) {
        SCH_TASK_ELOG("tDeserializeSQueryTableRsp failed, msgSize:%d", msgSize);
        SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_MSG);
      }
      
      SCH_ERR_JRET(rsp.code);

      SCH_ERR_JRET(schSaveJobExecRes(pJob, &rsp));

      (void)atomic_add_fetch_64(&pJob->resNumOfRows, rsp.affectedRows);

      taosMemoryFreeClear(pMsg->pData);

      SCH_ERR_JRET(schProcessOnTaskSuccess(pJob, pTask));

      break;
    }
    case TDMT_SCH_EXPLAIN_RSP: {
      SCH_ERR_JRET(rspCode);
      if (NULL == pMsg->pData) {
        SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
      }

      if (!SCH_IS_EXPLAIN_JOB(pJob)) {
        SCH_TASK_ELOG("invalid msg received for none explain query, msg type:%s", TMSG_INFO(msgType));
        SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
      }

      if (pJob->fetchRes) {
        SCH_TASK_ELOG("explain result is already generated, res:%p", pJob->fetchRes);
        SCH_ERR_JRET(TSDB_CODE_SCH_STATUS_ERROR);
      }

      SExplainRsp rsp = {0};
      if (tDeserializeSExplainRsp(pMsg->pData, msgSize, &rsp)) {
        tFreeSExplainRsp(&rsp);
        SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
      }

      SCH_ERR_JRET(schProcessExplainRsp(pJob, pTask, &rsp));

      taosMemoryFreeClear(pMsg->pData);
      break;
    }
    case TDMT_SCH_FETCH_RSP:
    case TDMT_SCH_MERGE_FETCH_RSP: {
      code = schProcessFetchRsp(pJob, pTask, pMsg->pData, rspCode);
      pMsg->pData = NULL;
      SCH_ERR_JRET(code);
      break;
    }
    case TDMT_SCH_DROP_TASK_RSP: {
      // NEVER REACH HERE
      SCH_TASK_ELOG("invalid status to handle drop task rsp, refId:0x%" PRIx64, pJob->refId);
      SCH_ERR_JRET(TSDB_CODE_SCH_INTERNAL_ERROR);
      break;
    }
    case TDMT_SCH_LINK_BROKEN:
      SCH_TASK_ELOG("link broken received, error:%x - %s", rspCode, tstrerror(rspCode));
      SCH_ERR_JRET(rspCode);
      break;
    default:
      SCH_TASK_ELOG("unknown rsp msg, type:%d, status:%s", msgType, SCH_GET_TASK_STATUS_STR(pTask));
      SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
  }

  return TSDB_CODE_SUCCESS;

_return:

  taosMemoryFreeClear(pMsg->pData);

  SCH_RET(schProcessOnTaskFailure(pJob, pTask, code));
} 


// Note: no more task error processing, handled in function internal
int32_t schHandleResponseMsg(SSchJob *pJob, SSchTask *pTask, uint64_t seriousId, int32_t execId, SDataBuf *pMsg, int32_t rspCode) {
  int32_t code = 0;
  int32_t msgType = pMsg->msgType;

  bool dropExecNode = (msgType == TDMT_SCH_LINK_BROKEN || SCH_NETWORK_ERR(rspCode));
  if (SCH_IS_QUERY_JOB(pJob)) {
    SCH_ERR_JRET(schUpdateTaskHandle(pJob, pTask, dropExecNode, pMsg->handle, seriousId, execId));
  }
  
  SCH_ERR_JRET(schValidateRspMsgType(pJob, pTask, msgType));

  if (pTask->seriousId < atomic_load_64(&pJob->seriousId)) {
    SCH_TASK_DLOG("task sId %" PRId64 " is smaller than current job sId %" PRId64, pTask->seriousId, pJob->seriousId);
    SCH_ERR_JRET(TSDB_CODE_SCH_IGNORE_ERROR);
  }

  int32_t reqType = IsReq(pMsg) ? pMsg->msgType : (pMsg->msgType - 1);
#if 0
  if (SCH_JOB_NEED_RETRY(pJob, pTask, reqType, rspCode)) {
    SCH_RET(schHandleJobRetry(pJob, pTask, (SDataBuf *)pMsg, rspCode));
  } else if (SCH_TASKSET_NEED_RETRY(pJob, pTask, reqType, rspCode)) {
    SCH_RET(schHandleTaskSetRetry(pJob, pTask, (SDataBuf *)pMsg, rspCode));
  }
#else 
  if (SCH_JOB_NEED_RETRY(pJob, pTask, reqType, rspCode)) {
    SCH_RET(schHandleJobRetry(pJob, pTask, (SDataBuf *)pMsg, rspCode));
  }
#endif

  pTask->redirectCtx.inRedirect = false;

  SCH_RET(schProcessResponseMsg(pJob, pTask, pMsg, rspCode));

_return:

  taosMemoryFreeClear(pMsg->pData);

  SCH_RET(schProcessOnTaskFailure(pJob, pTask, code));
} 
int32_t schHandleCallback(void *param, SDataBuf *pMsg, int32_t rspCode) {
  int32_t                code = 0;
  SSchTaskCallbackParam *pParam = (SSchTaskCallbackParam *)param;
  SSchTask              *pTask = NULL;
  SSchJob               *pJob = NULL;

  int64_t qid = pParam->queryId;
  qDebug("QID:0x%" PRIx64 ", handle rsp msg, type:%s, handle:%p, code:%s", qid,TMSG_INFO(pMsg->msgType), pMsg->handle,
         tstrerror(rspCode));

  SCH_ERR_JRET(schProcessOnCbBegin(&pJob, &pTask, pParam->queryId, pParam->refId, pParam->taskId));
  code = schHandleResponseMsg(pJob, pTask, pParam->seriousId, pParam->execId, pMsg, rspCode);
  pMsg->pData = NULL;

  schProcessOnCbEnd(pJob, pTask, code);

_return:

  taosMemoryFreeClear(pMsg->pData);
  taosMemoryFreeClear(pMsg->pEpSet);

  qTrace("QID:0x%" PRIx64 ", end to handle rsp msg, type:%s, handle:%p, code:%s", qid, TMSG_INFO(pMsg->msgType), pMsg->handle,
         tstrerror(rspCode));

  SCH_RET(code);
}

int32_t schHandleDropCallback(void *param, SDataBuf *pMsg, int32_t code) {
  SSchTaskCallbackParam *pParam = (SSchTaskCallbackParam *)param;
  qDebug("QID:0x%" PRIx64 ", SID:0x%" PRIx64 ", CID:0x%" PRIx64 ", TID:0x%" PRIx64 " drop task rsp received, code:0x%x", 
         pParam->queryId, pParam->seriousId, pParam->clientId, pParam->taskId, code);
  // called if drop task rsp received code
  (void)rpcReleaseHandle(pMsg->handle, TAOS_CONN_CLIENT, 0); // ignore error

  if (pMsg->handle == NULL) {
    qError("sch handle is NULL, may be already released and mem lea");
  }
  if (pMsg) {
    taosMemoryFree(pMsg->pData);
    taosMemoryFree(pMsg->pEpSet);
  }
  return TSDB_CODE_SUCCESS;
}

int32_t schHandleNotifyCallback(void *param, SDataBuf *pMsg, int32_t code) {
  SSchTaskCallbackParam *pParam = (SSchTaskCallbackParam *)param;
  qDebug("QID:0x%" PRIx64 ", SID:0x%" PRIx64 ", CID:0x%" PRIx64 ", TID:0x%" PRIx64 " task notify rsp received, code:0x%x", 
         pParam->queryId, pParam->seriousId, pParam->clientId, pParam->taskId, code);
  if (pMsg) {
    taosMemoryFreeClear(pMsg->pData);
    taosMemoryFreeClear(pMsg->pEpSet);
  }
  return TSDB_CODE_SUCCESS;
}


int32_t schHandleLinkBrokenCallback(void *param, SDataBuf *pMsg, int32_t code) {
  SSchCallbackParamHeader *head = (SSchCallbackParamHeader *)param;
  (void)rpcReleaseHandle(pMsg->handle, TAOS_CONN_CLIENT, 0); // ignore error

  qDebug("handle %p is broken", pMsg->handle);

  if (head->isHbParam) {
    taosMemoryFreeClear(pMsg->pData);
    taosMemoryFreeClear(pMsg->pEpSet);

    SSchHbCallbackParam *hbParam = (SSchHbCallbackParam *)param;
    SSchTrans            trans = {.pTrans = hbParam->pTrans, .pHandle = NULL, .pHandleId = 0};
    SCH_ERR_RET(schUpdateHbConnection(&hbParam->nodeEpId, &trans));

    SCH_ERR_RET(schBuildAndSendHbMsg(&hbParam->nodeEpId, NULL));
  } else {
    SCH_ERR_RET(schHandleCallback(param, pMsg, code));
  }

  return TSDB_CODE_SUCCESS;
}

int32_t schHandleCommitCallback(void *param, SDataBuf *pMsg, int32_t code) {
  return schHandleCallback(param, pMsg, code);
}

int32_t schHandleHbCallback(void *param, SDataBuf *pMsg, int32_t code) {
  SSchedulerHbRsp        rsp = {0};
  SSchHbCallbackParam *pParam = (SSchHbCallbackParam *)param;

  if (code) {
    qError("hb rsp error:%s", tstrerror(code));
    (void)rpcReleaseHandle(pMsg->handle, TAOS_CONN_CLIENT, 0); // ignore error
    SCH_ERR_JRET(code);
  }

  if (tDeserializeSSchedulerHbRsp(pMsg->pData, pMsg->len, &rsp)) {
    qError("invalid hb rsp msg, size:%d", pMsg->len);
    SCH_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
  }

  SSchTrans trans = {0};
  trans.pTrans = pParam->pTrans;
  trans.pHandle = pMsg->handle;
  trans.pHandleId = pMsg->handleRefId;

  SCH_ERR_JRET(schUpdateHbConnection(&rsp.epId, &trans));
  SCH_ERR_JRET(schProcessOnTaskStatusRsp(&rsp.epId, rsp.taskStatus));

_return:

  tFreeSSchedulerHbRsp(&rsp);
  taosMemoryFree(pMsg->pData);
  taosMemoryFree(pMsg->pEpSet);
  SCH_RET(code);
}

int32_t schMakeCallbackParam(SSchJob *pJob, SSchTask *pTask, int32_t msgType, bool isHb, SSchTrans *trans,
                             void **pParam) {
  if (!isHb) {
    SSchTaskCallbackParam *param = taosMemoryCalloc(1, sizeof(SSchTaskCallbackParam));
    if (NULL == param) {
      SCH_TASK_ELOG("calloc %d failed", (int32_t)sizeof(SSchTaskCallbackParam));
      SCH_ERR_RET(terrno);
    }

    param->queryId = pJob->queryId;
    param->seriousId = pTask->seriousId;
    param->refId = pJob->refId;
    param->clientId = SCH_CLIENT_ID(pTask);
    param->taskId = SCH_TASK_ID(pTask);
    param->pTrans = pJob->conn.pTrans;
    param->execId = pTask->execId;
    *pParam = param;

    return TSDB_CODE_SUCCESS;
  }

  if (TDMT_SCH_LINK_BROKEN == msgType) {
    SSchHbCallbackParam *param = taosMemoryCalloc(1, sizeof(SSchHbCallbackParam));
    if (NULL == param) {
      SCH_TASK_ELOG("calloc %d failed", (int32_t)sizeof(SSchHbCallbackParam));
      SCH_ERR_RET(terrno);
    }

    param->head.isHbParam = true;

    SQueryNodeAddr *addr = taosArrayGet(pTask->candidateAddrs, pTask->candidateIdx);
    if (NULL == addr) {
      taosMemoryFree(param);
      SCH_TASK_ELOG("fail to get the %dth condidateAddr, totalNum: %d", pTask->candidateIdx, (int32_t)taosArrayGetSize(pTask->candidateAddrs));
      SCH_ERR_RET(TSDB_CODE_SCH_INTERNAL_ERROR);
    }
    param->nodeEpId.nodeId = addr->nodeId;
    SEp *pEp = SCH_GET_CUR_EP(addr);
    TAOS_STRCPY(param->nodeEpId.ep.fqdn, pEp->fqdn);
    param->nodeEpId.ep.port = pEp->port;
    param->pTrans = trans->pTrans;
    *pParam = param;

    return TSDB_CODE_SUCCESS;
  }

  // hb msg
  SSchHbCallbackParam *param = taosMemoryCalloc(1, sizeof(SSchHbCallbackParam));
  if (NULL == param) {
    qError("calloc SSchTaskCallbackParam failed");
    SCH_ERR_RET(terrno);
  }

  param->head.isHbParam = true;
  param->pTrans = trans->pTrans;
  *pParam = param;

  return TSDB_CODE_SUCCESS;
}

int32_t schGenerateCallBackInfo(SSchJob *pJob, SSchTask *pTask, void *msg, uint32_t msgSize, int32_t msgType,
                                SSchTrans *trans, bool isHb, SMsgSendInfo **pMsgSendInfo) {
  int32_t       code = 0;
  SMsgSendInfo *msgSendInfo = taosMemoryCalloc(1, sizeof(SMsgSendInfo));
  if (NULL == msgSendInfo) {
    qError("calloc SMsgSendInfo size %d failed", (int32_t)sizeof(SMsgSendInfo));
    SCH_ERR_JRET(terrno);
  }

  msgSendInfo->paramFreeFp = taosAutoMemoryFree;
  SCH_ERR_JRET(schMakeCallbackParam(pJob, pTask, msgType, isHb, trans, &msgSendInfo->param));

  SCH_ERR_JRET(schGetCallbackFp(msgType, &msgSendInfo->fp));

  if (pJob) {
    msgSendInfo->requestId = pJob->conn.requestId;
    msgSendInfo->requestObjRefId = pJob->conn.requestObjRefId;
  } else {
    SCH_ERR_JRET(taosGetSystemUUIDU64(&msgSendInfo->requestId));
  }

  qDebug("ahandle %p alloced, QID:0x%" PRIx64, msgSendInfo, msgSendInfo->requestId);

  if (TDMT_SCH_LINK_BROKEN != msgType) {
    msgSendInfo->msgInfo.pData = msg;
    msgSendInfo->msgInfo.len = msgSize;
    msgSendInfo->msgInfo.handle = trans->pHandle;
    msgSendInfo->msgType = msgType;
  }

  *pMsgSendInfo = msgSendInfo;

  return TSDB_CODE_SUCCESS;

_return:

  if (msgSendInfo) {
    destroySendMsgInfo(msgSendInfo);
  }

  taosMemoryFree(msg);

  SCH_RET(code);
}

int32_t schGetCallbackFp(int32_t msgType, __async_send_cb_fn_t *fp) {
  switch (msgType) {
    case TDMT_VND_CREATE_TABLE:
    case TDMT_VND_DROP_TABLE:
    case TDMT_VND_ALTER_TABLE:
    case TDMT_VND_SUBMIT:
    case TDMT_SCH_QUERY:
    case TDMT_SCH_MERGE_QUERY:
    case TDMT_VND_DELETE:
    case TDMT_SCH_EXPLAIN:
    case TDMT_SCH_FETCH:
    case TDMT_SCH_MERGE_FETCH:
      *fp = schHandleCallback;
      break;
    case TDMT_SCH_DROP_TASK:
      *fp = schHandleDropCallback;
      break;
    case TDMT_SCH_TASK_NOTIFY:
      *fp = schHandleNotifyCallback;
      break;
    case TDMT_SCH_QUERY_HEARTBEAT:
      *fp = schHandleHbCallback;
      break;
    case TDMT_VND_COMMIT:
      *fp = schHandleCommitCallback;
      break;
    case TDMT_SCH_LINK_BROKEN:
      *fp = schHandleLinkBrokenCallback;
      break;
    default:
      qError("unknown msg type for callback, msgType:%d", msgType);
      SCH_ERR_RET(TSDB_CODE_APP_ERROR);
  }

  return TSDB_CODE_SUCCESS;
}

/*
int32_t schMakeHbCallbackParam(SSchJob *pJob, SSchTask *pTask, void **pParam) {
  SSchHbCallbackParam *param = taosMemoryCalloc(1, sizeof(SSchHbCallbackParam));
  if (NULL == param) {
    SCH_TASK_ELOG("calloc %d failed", (int32_t)sizeof(SSchHbCallbackParam));
    SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }

  param->head.isHbParam = true;

  SQueryNodeAddr *addr = taosArrayGet(pTask->candidateAddrs, pTask->candidateIdx);

  param->nodeEpId.nodeId = addr->nodeId;
  SEp* pEp = SCH_GET_CUR_EP(addr);
  tstrncpy(param->nodeEpId.ep.fqdn, pEp->fqdn, sizeof(param->nodeEpId.ep.fqdn));
  param->nodeEpId.ep.port = pEp->port;
  param->pTrans = pJob->pTrans;

  *pParam = param;

  return TSDB_CODE_SUCCESS;
}
*/

int32_t schCloneHbRpcCtx(SRpcCtx *pSrc, SRpcCtx *pDst) {
  int32_t code = 0;
  TAOS_MEMCPY(pDst, pSrc, sizeof(SRpcCtx));
  pDst->brokenVal.val = NULL;
  pDst->args = NULL;

  SCH_ERR_RET(schCloneSMsgSendInfo(pSrc->brokenVal.val, &pDst->brokenVal.val));

  pDst->args = taosHashInit(1, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), false, HASH_ENTRY_LOCK);
  if (NULL == pDst->args) {
    qError("taosHashInit %d RpcCtx failed", 1);
    SCH_ERR_JRET(terrno);
  }

  SRpcCtxVal dst = {0};
  void      *pIter = taosHashIterate(pSrc->args, NULL);
  while (pIter) {
    SRpcCtxVal *pVal = (SRpcCtxVal *)pIter;
    int32_t    *msgType = taosHashGetKey(pIter, NULL);

    dst = *pVal;
    dst.val = NULL;

    SCH_ERR_JRET(schCloneSMsgSendInfo(pVal->val, &dst.val));

    if (taosHashPut(pDst->args, msgType, sizeof(*msgType), &dst, sizeof(dst))) {
      qError("taosHashPut msg %d to rpcCtx failed", *msgType);
      (*pSrc->freeFunc)(dst.val);
      SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
    }

    pIter = taosHashIterate(pSrc->args, pIter);
  }

  return TSDB_CODE_SUCCESS;

_return:

  schFreeRpcCtx(pDst);
  SCH_RET(code);
}

int32_t schMakeHbRpcCtx(SSchJob *pJob, SSchTask *pTask, SRpcCtx *pCtx) {
  int32_t              code = 0;
  SSchHbCallbackParam *param = NULL;
  SMsgSendInfo        *pMsgSendInfo = NULL;
  SQueryNodeAddr      *addr = taosArrayGet(pTask->candidateAddrs, pTask->candidateIdx);
  SQueryNodeEpId       epId = {0};

  epId.nodeId = addr->nodeId;
  TAOS_MEMCPY(&epId.ep, SCH_GET_CUR_EP(addr), sizeof(SEp));

  pCtx->args = taosHashInit(1, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), false, HASH_ENTRY_LOCK);
  if (NULL == pCtx->args) {
    SCH_TASK_ELOG("taosHashInit %d RpcCtx failed", 1);
    SCH_ERR_RET(terrno);
  }

  pMsgSendInfo = taosMemoryCalloc(1, sizeof(SMsgSendInfo));
  if (NULL == pMsgSendInfo) {
    SCH_TASK_ELOG("calloc %d failed", (int32_t)sizeof(SMsgSendInfo));
    SCH_ERR_JRET(terrno);
  }

  param = taosMemoryCalloc(1, sizeof(SSchHbCallbackParam));
  if (NULL == param) {
    SCH_TASK_ELOG("calloc %d failed", (int32_t)sizeof(SSchHbCallbackParam));
    SCH_ERR_JRET(terrno);
  }

  int32_t              msgType = TDMT_SCH_QUERY_HEARTBEAT_RSP;
  __async_send_cb_fn_t fp = NULL;
  SCH_ERR_JRET(schGetCallbackFp(TDMT_SCH_QUERY_HEARTBEAT, &fp));

  param->head.isHbParam = true;
  param->nodeEpId = epId;
  param->pTrans = pJob->conn.pTrans;

  pMsgSendInfo->param = param;
  pMsgSendInfo->paramFreeFp = taosAutoMemoryFree;
  pMsgSendInfo->fp = fp;

  SRpcCtxVal ctxVal = {.val = pMsgSendInfo, .clone = schCloneSMsgSendInfo};
  if (taosHashPut(pCtx->args, &msgType, sizeof(msgType), &ctxVal, sizeof(ctxVal))) {
    SCH_TASK_ELOG("taosHashPut msg %d to rpcCtx failed", msgType);
    SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
  }

  SCH_ERR_JRET(schMakeBrokenLinkVal(pJob, pTask, &pCtx->brokenVal, true));
  pCtx->freeFunc = schFreeRpcCtxVal;

  return TSDB_CODE_SUCCESS;

_return:

  taosHashCleanup(pCtx->args);
  taosMemoryFreeClear(param);
  taosMemoryFreeClear(pMsgSendInfo);

  SCH_RET(code);
}

int32_t schMakeBrokenLinkVal(SSchJob *pJob, SSchTask *pTask, SRpcBrokenlinkVal *brokenVal, bool isHb) {
  int32_t       code = 0;
  int32_t       msgType = TDMT_SCH_LINK_BROKEN;
  SSchTrans     trans = {.pTrans = pJob->conn.pTrans};
  SMsgSendInfo *pMsgSendInfo = NULL;
  SCH_ERR_JRET(schGenerateCallBackInfo(pJob, pTask, NULL, 0, msgType, &trans, isHb, &pMsgSendInfo));

  brokenVal->msgType = msgType;
  brokenVal->val = pMsgSendInfo;
  brokenVal->clone = schCloneSMsgSendInfo;

  return TSDB_CODE_SUCCESS;

_return:

  taosMemoryFreeClear(pMsgSendInfo->param);
  taosMemoryFreeClear(pMsgSendInfo);

  SCH_RET(code);
}

int32_t schMakeQueryRpcCtx(SSchJob *pJob, SSchTask *pTask, SRpcCtx *pCtx) {
  int32_t       code = 0;
  SMsgSendInfo *pExplainMsgSendInfo = NULL;

  pCtx->args = taosHashInit(1, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), false, HASH_ENTRY_LOCK);
  if (NULL == pCtx->args) {
    SCH_TASK_ELOG("taosHashInit %d RpcCtx failed", 1);
    SCH_ERR_RET(terrno);
  }

  SSchTrans trans = {.pTrans = pJob->conn.pTrans, .pHandle = SCH_GET_TASK_HANDLE(pTask)};
  SCH_ERR_JRET(schGenerateCallBackInfo(pJob, pTask, NULL, 0, TDMT_SCH_EXPLAIN, &trans, false, &pExplainMsgSendInfo));

  int32_t    msgType = TDMT_SCH_EXPLAIN_RSP;
  SRpcCtxVal ctxVal = {.val = pExplainMsgSendInfo, .clone = schCloneSMsgSendInfo};
  if (taosHashPut(pCtx->args, &msgType, sizeof(msgType), &ctxVal, sizeof(ctxVal))) {
    SCH_TASK_ELOG("taosHashPut msg %d to rpcCtx failed", msgType);
    SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
  }

  SCH_ERR_JRET(schMakeBrokenLinkVal(pJob, pTask, &pCtx->brokenVal, false));
  pCtx->freeFunc = schFreeRpcCtxVal;

  return TSDB_CODE_SUCCESS;

_return:

  taosHashCleanup(pCtx->args);

  if (pExplainMsgSendInfo) {
    taosMemoryFreeClear(pExplainMsgSendInfo->param);
    taosMemoryFreeClear(pExplainMsgSendInfo);
  }

  SCH_RET(code);
}

int32_t schCloneCallbackParam(SSchCallbackParamHeader *pSrc, SSchCallbackParamHeader **pDst) {
  if (pSrc->isHbParam) {
    SSchHbCallbackParam *dst = taosMemoryMalloc(sizeof(SSchHbCallbackParam));
    if (NULL == dst) {
      qError("malloc SSchHbCallbackParam failed");
      SCH_ERR_RET(terrno);
    }

    TAOS_MEMCPY(dst, pSrc, sizeof(*dst));
    *pDst = (SSchCallbackParamHeader *)dst;

    return TSDB_CODE_SUCCESS;
  }

  SSchTaskCallbackParam *dst = taosMemoryMalloc(sizeof(SSchTaskCallbackParam));
  if (NULL == dst) {
    qError("malloc SSchTaskCallbackParam failed");
    SCH_ERR_RET(terrno);
  }

  TAOS_MEMCPY(dst, pSrc, sizeof(*dst));
  *pDst = (SSchCallbackParamHeader *)dst;

  return TSDB_CODE_SUCCESS;
}

int32_t schCloneSMsgSendInfo(void *src, void **dst) {
  SMsgSendInfo *pSrc = src;
  int32_t       code = 0;
  SMsgSendInfo *pDst = taosMemoryCalloc(1, sizeof(*pSrc));
  if (NULL == pDst) {
    qError("malloc SMsgSendInfo for rpcCtx failed, len:%d", (int32_t)sizeof(*pSrc));
    SCH_ERR_RET(terrno);
  }

  TAOS_MEMCPY(pDst, pSrc, sizeof(*pSrc));
  pDst->param = NULL;

  SCH_ERR_JRET(schCloneCallbackParam(pSrc->param, (SSchCallbackParamHeader **)&pDst->param));
  pDst->paramFreeFp = taosAutoMemoryFree;

  *dst = pDst;

  return TSDB_CODE_SUCCESS;

_return:

  taosMemoryFreeClear(pDst);
  SCH_RET(code);
}

int32_t schUpdateSendTargetInfo(SMsgSendInfo *pMsgSendInfo, SQueryNodeAddr *addr, SSchTask *pTask) {
  if (NULL == pTask || addr->nodeId < MNODE_HANDLE) {
    return TSDB_CODE_SUCCESS;
  }

  if (addr->nodeId == MNODE_HANDLE) {
    pMsgSendInfo->target.type = TARGET_TYPE_MNODE;
  } else {
    pMsgSendInfo->target.type = TARGET_TYPE_VNODE;
    pMsgSendInfo->target.vgId = addr->nodeId;
    pMsgSendInfo->target.dbFName = taosStrdup(pTask->plan->dbFName);
    if (NULL == pMsgSendInfo->target.dbFName) {
      return terrno;
    }
  }

  return TSDB_CODE_SUCCESS;
}

int32_t schAsyncSendMsg(SSchJob *pJob, SSchTask *pTask, SSchTrans *trans, SQueryNodeAddr *addr, int32_t msgType,
                        void *msg, uint32_t msgSize, bool persistHandle, SRpcCtx *ctx) {
  int32_t code = 0;
  SEpSet *epSet = &addr->epSet;

  SMsgSendInfo *pMsgSendInfo = NULL;
  bool          isHb = (TDMT_SCH_QUERY_HEARTBEAT == msgType);
  SCH_ERR_JRET(schGenerateCallBackInfo(pJob, pTask, msg, msgSize, msgType, trans, isHb, &pMsgSendInfo));
  SCH_ERR_JRET(schUpdateSendTargetInfo(pMsgSendInfo, addr, pTask));

  if (isHb && persistHandle && trans->pHandle == 0) {
    int64_t refId = 0;
    code = rpcAllocHandle(&refId); 
    if (code != 0) {
      SCH_TASK_ELOG("rpcAllocHandle failed, code:%x", code);
      SCH_ERR_JRET(code);
    }
    trans->pHandle = (void *)refId;
    pMsgSendInfo->msgInfo.handle =trans->pHandle;
  } 

  if (pJob && pTask) {
    SCH_TASK_DLOG("start to send %s msg to node[%d,%s,%d], pTrans:%p, pHandle:%p", TMSG_INFO(msgType), addr->nodeId,
           epSet->eps[epSet->inUse].fqdn, epSet->eps[epSet->inUse].port, trans->pTrans, trans->pHandle);
  } else {
    qDebug("start to send %s msg to node[%d,%s,%d], pTrans:%p, pHandle:%p", TMSG_INFO(msgType), addr->nodeId,
           epSet->eps[epSet->inUse].fqdn, epSet->eps[epSet->inUse].port, trans->pTrans, trans->pHandle);
  }
  
  if (pTask) {
    pTask->lastMsgType = msgType;
  }

  code = asyncSendMsgToServerExt(trans->pTrans, epSet, NULL, pMsgSendInfo, persistHandle, ctx);
  pMsgSendInfo = NULL;
  if (code) {
    SCH_ERR_JRET(code);
  }

  if (pJob) {
    SCH_TASK_TLOG("req msg sent, type:%d, %s", msgType, TMSG_INFO(msgType));
  } else {
    qTrace("req msg sent, type:%d, %s", msgType, TMSG_INFO(msgType));
  }
  return TSDB_CODE_SUCCESS;

_return:

  if (pJob) {
    SCH_TASK_ELOG("fail to send msg, type:%d, %s, error:%s", msgType, TMSG_INFO(msgType), tstrerror(code));
  } else {
    qError("fail to send msg, type:%d, %s, error:%s", msgType, TMSG_INFO(msgType), tstrerror(code));
  }

  if (pMsgSendInfo) {
    destroySendMsgInfo(pMsgSendInfo);
  }

  SCH_RET(code);
}

int32_t schBuildAndSendHbMsg(SQueryNodeEpId *nodeEpId, SArray *taskAction) {
  SSchedulerHbReq req = {0};
  int32_t         code = 0;
  SRpcCtx         rpcCtx = {0};
  SSchTrans       trans = {0};
  int32_t         msgType = TDMT_SCH_QUERY_HEARTBEAT;

  req.header.vgId = nodeEpId->nodeId;
  req.clientId = schMgmt.clientId;
  TAOS_MEMCPY(&req.epId, nodeEpId, sizeof(SQueryNodeEpId));

  SCH_LOCK(SCH_READ, &schMgmt.hbLock);
  SSchHbTrans *hb = taosHashGet(schMgmt.hbConnections, nodeEpId, sizeof(SQueryNodeEpId));
  if (NULL == hb) {
    SCH_UNLOCK(SCH_READ, &schMgmt.hbLock);
    qError("hb connection no longer exist, nodeId:%d, fqdn:%s, port:%d", nodeEpId->nodeId, nodeEpId->ep.fqdn,
           nodeEpId->ep.port);
    return TSDB_CODE_SUCCESS;
  }

  SCH_LOCK(SCH_WRITE, &hb->lock);
  code = schCloneHbRpcCtx(&hb->rpcCtx, &rpcCtx);
  TAOS_MEMCPY(&trans, &hb->trans, sizeof(trans));
  if (NULL == hb->trans.pTrans) {
    qError("NULL pTrans got from hbConnections for epId:%d", nodeEpId->nodeId);
  }
  SCH_UNLOCK(SCH_WRITE, &hb->lock);
  SCH_UNLOCK(SCH_READ, &schMgmt.hbLock);

  SCH_ERR_RET(code);

  int32_t msgSize = tSerializeSSchedulerHbReq(NULL, 0, &req);
  if (msgSize < 0) {
    qError("tSerializeSSchedulerHbReq hbReq failed, size:%d", msgSize);
    SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
  }
  void *msg = taosMemoryCalloc(1, msgSize);
  if (NULL == msg) {
    qError("calloc hb req %d failed", msgSize);
    SCH_ERR_JRET(terrno);
  }

  if (tSerializeSSchedulerHbReq(msg, msgSize, &req) < 0) {
    qError("tSerializeSSchedulerHbReq hbReq failed, size:%d", msgSize);
    SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
  }

  int64_t        transporterId = 0;
  SQueryNodeAddr addr = {.nodeId = nodeEpId->nodeId};
  addr.epSet.inUse = 0;
  addr.epSet.numOfEps = 1;
  TAOS_MEMCPY(&addr.epSet.eps[0], &nodeEpId->ep, sizeof(nodeEpId->ep));

  code = schAsyncSendMsg(NULL, NULL, &trans, &addr, msgType, msg, msgSize, true, &rpcCtx);
  msg = NULL;
  SCH_ERR_JRET(code);

  return TSDB_CODE_SUCCESS;

_return:

  taosMemoryFreeClear(msg);
  schFreeRpcCtx(&rpcCtx);
  SCH_RET(code);
}

int32_t schBuildAndSendMsg(SSchJob *pJob, SSchTask *pTask, SQueryNodeAddr *addr, int32_t msgType, void* param) {
  int32_t  msgSize = 0;
  void    *msg = NULL;
  int32_t  code = 0;
  bool     isCandidateAddr = false;
  bool     persistHandle = false;
  SRpcCtx  rpcCtx = {0};

  if (NULL == addr) {
    addr = taosArrayGet(pTask->candidateAddrs, pTask->candidateIdx);
    if (NULL == addr) {
      SCH_TASK_ELOG("fail to get condidateAddr, candidateIdx %d, totalNum: %d", pTask->candidateIdx, (int32_t)taosArrayGetSize(pTask->candidateAddrs));
      SCH_ERR_JRET(terrno);
    }
    
    isCandidateAddr = true;
    SCH_TASK_TLOG("target candidateIdx %d, epInUse %d/%d", pTask->candidateIdx, addr->epSet.inUse,
                  addr->epSet.numOfEps);
  }

  switch (msgType) {
    case TDMT_VND_CREATE_TABLE:
    case TDMT_VND_DROP_TABLE:
    case TDMT_VND_ALTER_TABLE:
    case TDMT_VND_SUBMIT:
    case TDMT_VND_COMMIT: {
      msgSize = pTask->msgLen;
      msg = pTask->msg;
      pTask->msg = NULL;
      break;
    }

    case TDMT_VND_DELETE: {
      SVDeleteReq req = {0};
      req.header.vgId = addr->nodeId;
      req.sId = pTask->seriousId;
      req.queryId = pJob->queryId;
      req.clientId = pTask->clientId;
      req.taskId = pTask->taskId;
      req.phyLen = pTask->msgLen;
      req.sqlLen = strlen(pJob->sql);
      req.sql = (char *)pJob->sql;
      req.msg = pTask->msg;
      req.source = pJob->source;
      msgSize = tSerializeSVDeleteReq(NULL, 0, &req);
      if (msgSize < 0) {
        SCH_TASK_ELOG("tSerializeSVDeleteReq failed, code:%x", terrno);
        SCH_ERR_JRET(terrno);
      }
      msg = taosMemoryCalloc(1, msgSize);
      if (NULL == msg) {
        SCH_TASK_ELOG("calloc %d failed", msgSize);
        SCH_ERR_JRET(terrno);
      }

      msgSize = tSerializeSVDeleteReq(msg, msgSize, &req);
      if (msgSize < 0) {
        SCH_TASK_ELOG("tSerializeSVDeleteReq second failed, code:%x", terrno);
        SCH_ERR_JRET(terrno);
      }
      break;
    }
    case TDMT_SCH_QUERY:
    case TDMT_SCH_MERGE_QUERY: {
      SCH_ERR_RET(schMakeQueryRpcCtx(pJob, pTask, &rpcCtx));

      SSubQueryMsg qMsg;
      qMsg.header.vgId = addr->nodeId;
      qMsg.header.contLen = 0;
      qMsg.sId = pTask->seriousId;
      qMsg.queryId = pJob->queryId;
      qMsg.clientId = pTask->clientId;
      qMsg.taskId = pTask->taskId;
      qMsg.refId = pJob->refId;
      qMsg.execId = pTask->execId;
      qMsg.msgMask = (pTask->plan->showRewrite) ? QUERY_MSG_MASK_SHOW_REWRITE() : 0;
      qMsg.msgMask |= (pTask->plan->isView) ? QUERY_MSG_MASK_VIEW() : 0;
      qMsg.msgMask |= (pTask->plan->isAudit) ? QUERY_MSG_MASK_AUDIT() : 0;
      qMsg.taskType = TASK_TYPE_TEMP;
      qMsg.explain = SCH_IS_EXPLAIN_JOB(pJob);
      qMsg.needFetch = SCH_TASK_NEED_FETCH(pTask);
      qMsg.sqlLen = strlen(pJob->sql);
      qMsg.sql = pJob->sql;
      qMsg.msgLen = pTask->msgLen;
      qMsg.msg = pTask->msg;

      if (strcmp(tsLocalFqdn, GET_ACTIVE_EP(&addr->epSet)->fqdn) == 0) {
        qMsg.compress = 0;
      } else {
        qMsg.compress = 1;
      }

      msgSize = tSerializeSSubQueryMsg(NULL, 0, &qMsg);
      if (msgSize < 0) {
        SCH_TASK_ELOG("tSerializeSSubQueryMsg get size, msgSize:%d", msgSize);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      
      msg = taosMemoryCalloc(1, msgSize);
      if (NULL == msg) {
        SCH_TASK_ELOG("calloc %d failed", msgSize);
        SCH_ERR_RET(terrno);
      }

      if (tSerializeSSubQueryMsg(msg, msgSize, &qMsg) < 0) {
        SCH_TASK_ELOG("tSerializeSSubQueryMsg failed, msgSize:%d", msgSize);
        taosMemoryFree(msg);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }

      persistHandle = true;
      int64_t refId = 0;
      code = rpcAllocHandle(&refId);
      if (code != 0) {
        SCH_TASK_ELOG("rpcAllocHandle failed, code:%x", code);
        SCH_ERR_JRET(code);
      }

      SCH_SET_TASK_HANDLE(pTask, (void *)refId);
      break;
    }
    case TDMT_SCH_FETCH:
    case TDMT_SCH_MERGE_FETCH: {
      SResFetchReq req = {0};
      req.header.vgId = addr->nodeId;
      req.sId = pTask->seriousId;
      req.queryId = pJob->queryId;
      req.clientId = pTask->clientId;
      req.taskId = pTask->taskId;
      req.execId = pTask->execId;

      msgSize = tSerializeSResFetchReq(NULL, 0, &req);
      if (msgSize < 0) {
        SCH_TASK_ELOG("tSerializeSResFetchReq get size, msgSize:%d", msgSize);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      
      msg = taosMemoryCalloc(1, msgSize);
      if (NULL == msg) {
        SCH_TASK_ELOG("calloc %d failed", msgSize);
        SCH_ERR_RET(terrno);
      }

      if (tSerializeSResFetchReq(msg, msgSize, &req) < 0) {
        SCH_TASK_ELOG("tSerializeSResFetchReq %d failed", msgSize);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      break;
    }
    case TDMT_SCH_DROP_TASK: {
      STaskDropReq qMsg;
      qMsg.header.vgId = addr->nodeId;
      qMsg.header.contLen = 0;
      qMsg.sId = pTask->seriousId;
      qMsg.queryId = pJob->queryId;
      qMsg.clientId = pTask->clientId;
      qMsg.taskId = pTask->taskId;
      qMsg.refId = pJob->refId;
      qMsg.execId = *(int32_t*)param;

      msgSize = tSerializeSTaskDropReq(NULL, 0, &qMsg);
      if (msgSize < 0) {
        SCH_TASK_ELOG("tSerializeSTaskDropReq get size, msgSize:%d", msgSize);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      
      msg = taosMemoryCalloc(1, msgSize);
      if (NULL == msg) {
        SCH_TASK_ELOG("calloc %d failed", msgSize);
        SCH_ERR_RET(terrno);
      }

      if (tSerializeSTaskDropReq(msg, msgSize, &qMsg) < 0) {
        SCH_TASK_ELOG("tSerializeSTaskDropReq failed, msgSize:%d", msgSize);
        taosMemoryFree(msg);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      break;
    }
/*
    case TDMT_SCH_QUERY_HEARTBEAT: {
      SCH_ERR_RET(schMakeHbRpcCtx(pJob, pTask, &rpcCtx));

      SSchedulerHbReq req = {0};
      req.clientId = schMgmt.clientId;
      req.header.vgId = addr->nodeId;
      req.epId.nodeId = addr->nodeId;
      TAOS_MEMCPY(&req.epId.ep, SCH_GET_CUR_EP(addr), sizeof(SEp));

      msgSize = tSerializeSSchedulerHbReq(NULL, 0, &req);
      if (msgSize < 0) {
        SCH_JOB_ELOG("tSerializeSSchedulerHbReq hbReq failed, size:%d", msgSize);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      msg = taosMemoryCalloc(1, msgSize);
      if (NULL == msg) {
        SCH_JOB_ELOG("calloc %d failed", msgSize);
        SCH_ERR_RET(terrno);
      }
      if (tSerializeSSchedulerHbReq(msg, msgSize, &req) < 0) {
        SCH_JOB_ELOG("tSerializeSSchedulerHbReq hbReq failed, size:%d", msgSize);
        SCH_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
      }

      persistHandle = true;
      break;
    }
*/    
    case TDMT_SCH_TASK_NOTIFY: {
      ETaskNotifyType* pType = param;
      STaskNotifyReq qMsg;
      qMsg.header.vgId = addr->nodeId;
      qMsg.header.contLen = 0;
      qMsg.sId = pTask->seriousId;
      qMsg.queryId = pJob->queryId;
      qMsg.clientId = pTask->clientId;
      qMsg.taskId = pTask->taskId;
      qMsg.refId = pJob->refId;
      qMsg.execId = pTask->execId;
      qMsg.type = *pType;

      msgSize = tSerializeSTaskNotifyReq(NULL, 0, &qMsg);
      if (msgSize < 0) {
        SCH_TASK_ELOG("tSerializeSTaskNotifyReq get size, msgSize:%d", msgSize);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      
      msg = taosMemoryCalloc(1, msgSize);
      if (NULL == msg) {
        SCH_TASK_ELOG("calloc %d failed", msgSize);
        SCH_ERR_RET(terrno);
      }

      if (tSerializeSTaskNotifyReq(msg, msgSize, &qMsg) < 0) {
        SCH_TASK_ELOG("tSerializeSTaskNotifyReq failed, msgSize:%d", msgSize);
        taosMemoryFree(msg);
        SCH_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
      }
      break;      
    }
    default:
      SCH_TASK_ELOG("unknown msg type to send, msgType:%d", msgType);
      SCH_ERR_RET(TSDB_CODE_SCH_INTERNAL_ERROR);
  }

  if ((tsBypassFlag & TSDB_BYPASS_RB_RPC_SEND_SUBMIT) && (TDMT_VND_SUBMIT == msgType)) {
    taosMemoryFree(msg);
    SCH_ERR_RET(schProcessOnTaskSuccess(pJob, pTask));
  } else {
    if (msgType == TDMT_SCH_QUERY || msgType == TDMT_SCH_MERGE_QUERY) {
      SCH_ERR_JRET(schAppendTaskExecNode(pJob, pTask, addr, pTask->execId));
    }

    SSchTrans trans = {.pTrans = pJob->conn.pTrans, .pHandle = SCH_GET_TASK_HANDLE(pTask)};
    code = schAsyncSendMsg(pJob, pTask, &trans, addr, msgType, msg, (uint32_t)msgSize, persistHandle, (rpcCtx.args ? &rpcCtx : NULL));
    msg = NULL;
    SCH_ERR_JRET(code);
  }

  return TSDB_CODE_SUCCESS;

_return:

  pTask->lastMsgType = -1;
  schFreeRpcCtx(&rpcCtx);

  taosMemoryFreeClear(msg);
  SCH_RET(code);
}
// clang-format on
