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

#define _DEFAULT_SOURCE
#include "syncRaftEntry.h"
#include "syncUtil.h"
#include "tref.h"


extern char*   tMsgInfo[];
extern int32_t tMsgDict[];
extern int32_t tMsgRangeDict[];

typedef uint16_t tmsg_t;

#define TMSG_SEG_CODE(TYPE) (((TYPE)&0xff00) >> 8)
#define TMSG_SEG_SEQ(TYPE)  ((TYPE)&0xff)
#define TMSG_INDEX(TYPE)    (tMsgDict[TMSG_SEG_CODE(TYPE)] + TMSG_SEG_SEQ(TYPE))


#define MSG_MAX_SIZE 2310

typedef struct {
  int32_t msgType[MSG_MAX_SIZE];
  int64_t msgNum[MSG_MAX_SIZE];
  int64_t msgSize[MSG_MAX_SIZE];
} SSyncEntryStatis;

static SSyncEntryStatis gSyncEntryStatis = {0};

void syncEntryStatisPrint() {
  int64_t nMsgNum = 0, nMsgSize = 0;
  for (int32_t i = 0; i < MSG_MAX_SIZE; ++i) {
    int64_t msgNum = atomic_load_64(&gSyncEntryStatis.msgNum[i]);
    if (msgNum > 0) {
      int64_t msgSize = atomic_load_64(&gSyncEntryStatis.msgSize[i]);
      nMsgNum += msgNum;
      nMsgSize += msgSize;
      uint32_t msgType = atomic_load_32(&gSyncEntryStatis.msgType[i]);
      sInfo("prop:[%d] msgType:%d:%s, originMsg:%d:%s, num:%" PRId64 ", size:%" PRId64 ", avg:%" PRIi64, i, msgType,
            tMsgInfo[TMSG_INDEX(msgType)], i, tMsgInfo[TMSG_INDEX(i)], msgNum, msgSize, msgSize / msgNum);
    }
  }
  if (nMsgNum > 0) {
    sInfo("prop:total, num:%" PRId64 ", size:%" PRId64 ", avg:%" PRIi64, nMsgNum, nMsgSize, nMsgSize / nMsgNum);
  }
}

void syncEntryStatisInc(SSyncRaftEntry* pEntry) {
  if (pEntry->from) {
    if(pEntry->msgType <= 0) {
      assert(0);
    }
    atomic_store_32(&gSyncEntryStatis.msgType[pEntry->originalRpcType], pEntry->msgType);
    atomic_fetch_add_64(&gSyncEntryStatis.msgNum[pEntry->originalRpcType], 1);
    atomic_fetch_add_64(&gSyncEntryStatis.msgSize[pEntry->originalRpcType], pEntry->dataLen);
  }
}

void syncEntryStatisDec(SSyncRaftEntry* pEntry) {
  if (pEntry->from) {
    if(pEntry->msgType <= 0) {
      assert(0);
    }
    atomic_fetch_sub_64(&gSyncEntryStatis.msgNum[pEntry->originalRpcType], 1);
    atomic_fetch_sub_64(&gSyncEntryStatis.msgSize[pEntry->originalRpcType], pEntry->dataLen);
  }
}

SSyncRaftEntry* syncEntryBuild(int32_t dataLen) {
  int32_t         bytes = sizeof(SSyncRaftEntry) + dataLen;
  SSyncRaftEntry* pEntry = taosMemoryCalloc(1, bytes);
  if (pEntry == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  pEntry->bytes = bytes;
  pEntry->dataLen = dataLen;
  pEntry->rid = -1;

  return pEntry;
}

SSyncRaftEntry* syncEntryBuildFromClientRequest(const SyncClientRequest* pMsg, SyncTerm term, SyncIndex index) {
  SSyncRaftEntry* pEntry = syncEntryBuild(pMsg->dataLen);
  if (pEntry == NULL) return NULL;

  pEntry->msgType = pMsg->msgType;
  pEntry->originalRpcType = pMsg->originalRpcType;
  pEntry->seqNum = pMsg->seqNum;
  pEntry->isWeak = pMsg->isWeak;
  pEntry->term = term;
  pEntry->index = index;
  memcpy(pEntry->data, pMsg->data, pMsg->dataLen);

  return pEntry;
}

static int64_t gSyncRaftRpcTs = 0;

SSyncRaftEntry* syncEntryBuildFromRpcMsg(const SRpcMsg* pMsg, SyncTerm term, SyncIndex index) {
  SSyncRaftEntry* pEntry = syncEntryBuild(pMsg->contLen);
  if (pEntry == NULL) return NULL;

  pEntry->msgType = TDMT_SYNC_CLIENT_REQUEST;
  pEntry->originalRpcType = pMsg->msgType;
  pEntry->seqNum = 0;
  pEntry->isWeak = 0;
  pEntry->term = term;
  pEntry->index = index;
  pEntry->from = 1;
  memcpy(pEntry->data, pMsg->pCont, pMsg->contLen);

  if(pEntry->originalRpcType == 21) {
    sTrace("build entry:%p, msgType:%d", pEntry, (int32_t)pEntry->originalRpcType);
  }

  syncEntryStatisInc(pEntry);
  if (gSyncRaftRpcTs == 0) {
    gSyncRaftRpcTs = taosGetTimestampMs();
  } else {
    int64_t now = taosGetTimestampMs();
    int64_t interval = now - gSyncRaftRpcTs;
    if (interval > 5000) {
      atomic_store_64(&gSyncRaftRpcTs, now);
      syncEntryStatisPrint();
    }
  }

  return pEntry;
}

SSyncRaftEntry* syncEntryBuildFromAppendEntries(const SyncAppendEntries* pMsg) {
  SSyncRaftEntry* pEntry = taosMemoryMalloc(pMsg->dataLen);
  if (pEntry == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }
  memcpy(pEntry, pMsg->data, pMsg->dataLen);
  ASSERT(pEntry->bytes == pMsg->dataLen);
  return pEntry;
}

SSyncRaftEntry* syncEntryBuildNoop(SyncTerm term, SyncIndex index, int32_t vgId) {
  SSyncRaftEntry* pEntry = syncEntryBuild(sizeof(SMsgHead));
  if (pEntry == NULL) return NULL;

  pEntry->msgType = TDMT_SYNC_CLIENT_REQUEST;
  pEntry->originalRpcType = TDMT_SYNC_NOOP;
  pEntry->seqNum = 0;
  pEntry->isWeak = 0;
  pEntry->term = term;
  pEntry->index = index;

  SMsgHead* pHead = (SMsgHead*)pEntry->data;
  pHead->vgId = vgId;
  pHead->contLen = sizeof(SMsgHead);

  return pEntry;
}

void syncEntryDestroy(SSyncRaftEntry* pEntry) {
  if (pEntry != NULL) {
    sTrace("free entry:%p", pEntry);
    if(pEntry->from) {
      syncEntryStatisDec(pEntry);
    }
    taosMemoryFree(pEntry);
  }
}

void syncEntry2OriginalRpc(const SSyncRaftEntry* pEntry, SRpcMsg* pRpcMsg) {
  pRpcMsg->msgType = pEntry->originalRpcType;
  pRpcMsg->contLen = (int32_t)(pEntry->dataLen);
  pRpcMsg->pCont = rpcMallocCont(pRpcMsg->contLen);
  memcpy(pRpcMsg->pCont, pEntry->data, pRpcMsg->contLen);
}
