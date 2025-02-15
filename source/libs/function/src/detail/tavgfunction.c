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

#include "builtinsimpl.h"
#include "function.h"
#include "tdatablock.h"
#include "tfunctionInt.h"
#include "tglobal.h"

#define SET_VAL(_info, numOfElem, res) \
  do {                                 \
    if ((numOfElem) <= 0) {            \
      break;                           \
    }                                  \
    (_info)->numOfRes = (res);         \
  } while (0)

#define LIST_AVG_N(sumT, T)                                               \
  do {                                                                    \
    T* plist = (T*)pCol->pData;                                           \
    for (int32_t i = start; i < numOfRows + pInput->startRowIndex; ++i) { \
      if (colDataIsNull_f(pCol->nullbitmap, i)) {                         \
        continue;                                                         \
      }                                                                   \
                                                                          \
      numOfElem += 1;                                                     \
      pAvgRes->count -= 1;                                                \
      sumT -= plist[i];                                                   \
    }                                                                     \
  } while (0)

// define signed number sum with check overflow
#define CHECK_OVERFLOW_SUM_SIGNED(out, val)                                      \
  if (out->sum.overflow) {                                                       \
    out->sum.dsum += val;                                                        \
  } else if (out->sum.isum > 0 && val > 0 && INT64_MAX - out->sum.isum <= val || \
             out->sum.isum < 0 && val < 0 && INT64_MIN - out->sum.isum >= val) { \
    double dsum = (double)out->sum.isum;                                         \
    out->sum.overflow = true;                                                    \
    out->sum.dsum = dsum + val;                                                  \
  } else {                                                                       \
    out->sum.isum += val;                                                        \
  }

// val is big than INT64_MAX, val come from merge
#define CHECK_OVERFLOW_SUM_SIGNED_BIG(out, val, big)                             \
  if (out->sum.overflow) {                                                       \
    out->sum.dsum += val;                                                        \
  } else if (out->sum.isum > 0 && val > 0 && INT64_MAX - out->sum.isum <= val || \
             out->sum.isum < 0 && val < 0 && INT64_MIN - out->sum.isum >= val || \
             big) {                                                              \
    double dsum = (double)out->sum.isum;                                         \
    out->sum.overflow = true;                                                    \
    out->sum.dsum = dsum + val;                                                  \
  } else {                                                                       \
    out->sum.isum += val;                                                        \
  }

// define unsigned number sum with check overflow
#define CHECK_OVERFLOW_SUM_UNSIGNED(out, val)                 \
  if (out->sum.overflow) {                                    \
    out->sum.dsum += val;                                     \
  } else if (UINT64_MAX - out->sum.usum <= val) {             \
    double dsum = (double)out->sum.usum;                      \
    out->sum.overflow = true;                                 \
    out->sum.dsum = dsum + val;                               \
  } else {                                                    \
    out->sum.usum += val;                                     \
  }

// val is big than UINT64_MAX, val come from merge
#define CHECK_OVERFLOW_SUM_UNSIGNED_BIG(out, val, big)        \
  if (out->sum.overflow) {                                    \
    out->sum.dsum += val;                                     \
  } else if (UINT64_MAX - out->sum.usum <= val || big) {      \
    double dsum = (double)out->sum.usum;                      \
    out->sum.overflow = true;                                 \
    out->sum.dsum = dsum + val;                               \
  } else {                                                    \
    out->sum.usum += val;                                     \
  }

int32_t getAvgInfoSize() { return (int32_t)sizeof(SAvgRes); }

bool getAvgFuncEnv(SFunctionNode* UNUSED_PARAM(pFunc), SFuncExecEnv* pEnv) {
  pEnv->calcMemSize = sizeof(SAvgRes);
  return true;
}

int32_t avgFunctionSetup(SqlFunctionCtx* pCtx, SResultRowEntryInfo* pResultInfo) {
  if (pResultInfo->initialized) {
    return TSDB_CODE_SUCCESS;
  }
  if (TSDB_CODE_SUCCESS != functionSetup(pCtx, pResultInfo)) {
    return TSDB_CODE_FUNC_SETUP_ERROR;
  }

  SAvgRes* pRes = GET_ROWCELL_INTERBUF(pResultInfo);
  (void)memset(pRes, 0, sizeof(SAvgRes));
  return TSDB_CODE_SUCCESS;
}

static int32_t calculateAvgBySMAInfo(SAvgRes* pRes, int32_t numOfRows, int32_t type, const SColumnDataAgg* pAgg) {
  int32_t numOfElem = numOfRows - pAgg->numOfNull;

  pRes->count += numOfElem;
  if (IS_SIGNED_NUMERIC_TYPE(type)) {
    CHECK_OVERFLOW_SUM_SIGNED(pRes, pAgg->sum);
  } else if (IS_UNSIGNED_NUMERIC_TYPE(type)) {
    CHECK_OVERFLOW_SUM_UNSIGNED(pRes, pAgg->sum);
  } else if (IS_FLOAT_TYPE(type)) {
    pRes->sum.dsum += GET_DOUBLE_VAL((const char*)&(pAgg->sum));
  }

  return numOfElem;
}

static int32_t doAddNumericVector(SColumnInfoData* pCol, int32_t type, SInputColumnInfoData *pInput, SAvgRes* pRes) {
  int32_t start = pInput->startRowIndex;
  int32_t numOfRows = pInput->numOfRows;
  int32_t numOfElems = 0;

  switch (type) {
    case TSDB_DATA_TYPE_TINYINT: {
      int8_t* plist = (int8_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_SIGNED(pRes, plist[i])
      }

      break;
    }

    case TSDB_DATA_TYPE_SMALLINT: {
      int16_t* plist = (int16_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_SIGNED(pRes, plist[i])
      }
      break;
    }

    case TSDB_DATA_TYPE_INT: {
      int32_t* plist = (int32_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_SIGNED(pRes, plist[i])
      }

      break;
    }

    case TSDB_DATA_TYPE_BIGINT: {
      int64_t* plist = (int64_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_SIGNED(pRes, plist[i])
      }
      break;
    }

    case TSDB_DATA_TYPE_UTINYINT: {
      uint8_t* plist = (uint8_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_UNSIGNED(pRes, plist[i])
      }

      break;
    }

    case TSDB_DATA_TYPE_USMALLINT: {
      uint16_t* plist = (uint16_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_UNSIGNED(pRes, plist[i])
      }
      break;
    }

    case TSDB_DATA_TYPE_UINT: {
      uint32_t* plist = (uint32_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_UNSIGNED(pRes, plist[i])
      }

      break;
    }

    case TSDB_DATA_TYPE_UBIGINT: {
      uint64_t* plist = (uint64_t*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        CHECK_OVERFLOW_SUM_UNSIGNED(pRes, plist[i])
        
      }
      break;
    }

    case TSDB_DATA_TYPE_FLOAT: {
      float* plist = (float*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        pRes->sum.dsum += plist[i];
      }
      break;
    }

    case TSDB_DATA_TYPE_DOUBLE: {
      double* plist = (double*)pCol->pData;
      for (int32_t i = start; i < numOfRows + start; ++i) {
        if (colDataIsNull_f(pCol->nullbitmap, i)) {
          continue;
        }

        numOfElems += 1;
        pRes->count += 1;
        pRes->sum.dsum += plist[i];
      }
      break;
    }

    default:
      break;
  }

  return numOfElems;
}

int32_t avgFunction(SqlFunctionCtx* pCtx) {
  int32_t       numOfElem = 0;
  const int32_t THRESHOLD_SIZE = 8;

  SInputColumnInfoData* pInput = &pCtx->input;
  SColumnDataAgg*       pAgg = pInput->pColumnDataAgg[0];
  int32_t               type = pInput->pData[0]->info.type;

  SAvgRes* pAvgRes = GET_ROWCELL_INTERBUF(GET_RES_INFO(pCtx));

  // computing based on the true data block
  SColumnInfoData* pCol = pInput->pData[0];

  int32_t start = pInput->startRowIndex;
  int32_t numOfRows = pInput->numOfRows;

  if (IS_NULL_TYPE(type)) {
    goto _over;
  }

  pAvgRes->type = type;

  if (pInput->colDataSMAIsSet) {  // try to use SMA if available
    numOfElem = calculateAvgBySMAInfo(pAvgRes, numOfRows, type, pAgg);
  } else if (!pCol->hasNull) {  // try to employ the simd instructions to speed up the loop
    numOfElem = pInput->numOfRows;
    pAvgRes->count += pInput->numOfRows;

    switch(type) {
      case TSDB_DATA_TYPE_UTINYINT:
      case TSDB_DATA_TYPE_TINYINT: {
        const int8_t* plist = (const int8_t*) pCol->pData;

        for (int32_t i = pInput->startRowIndex; i < pInput->numOfRows + pInput->startRowIndex; ++i) {
          if (type == TSDB_DATA_TYPE_TINYINT) {
            CHECK_OVERFLOW_SUM_SIGNED(pAvgRes, plist[i])
          } else {
            CHECK_OVERFLOW_SUM_UNSIGNED(pAvgRes, (uint8_t)plist[i])
          }
        }
        break;
      }

      case TSDB_DATA_TYPE_USMALLINT:
      case TSDB_DATA_TYPE_SMALLINT: {
        const int16_t* plist = (const int16_t*)pCol->pData;

        for (int32_t i = pInput->startRowIndex; i < pInput->numOfRows + pInput->startRowIndex; ++i) {
          if (type == TSDB_DATA_TYPE_SMALLINT) {
            CHECK_OVERFLOW_SUM_SIGNED(pAvgRes, plist[i])
          } else {
            CHECK_OVERFLOW_SUM_UNSIGNED(pAvgRes, (uint16_t)plist[i])
          }
        }
        break;
      }

      case TSDB_DATA_TYPE_UINT:
      case TSDB_DATA_TYPE_INT: {
        const int32_t* plist = (const int32_t*) pCol->pData;

        for (int32_t i = pInput->startRowIndex; i < pInput->numOfRows + pInput->startRowIndex; ++i) {
          if (type == TSDB_DATA_TYPE_INT) {
            CHECK_OVERFLOW_SUM_SIGNED(pAvgRes, plist[i])
          } else {
            CHECK_OVERFLOW_SUM_UNSIGNED(pAvgRes, (uint32_t)plist[i])
          }
        }
        break;
      }

      case TSDB_DATA_TYPE_UBIGINT:
      case TSDB_DATA_TYPE_BIGINT: {
        const int64_t* plist = (const int64_t*) pCol->pData;

        for (int32_t i = pInput->startRowIndex; i < pInput->numOfRows + pInput->startRowIndex; ++i) {
          if (type == TSDB_DATA_TYPE_BIGINT) {
            CHECK_OVERFLOW_SUM_SIGNED(pAvgRes, plist[i])
          } else {
            CHECK_OVERFLOW_SUM_UNSIGNED(pAvgRes, (uint64_t)plist[i])
          }
        }
        break;
      }

      case TSDB_DATA_TYPE_FLOAT: {
        const float* plist = (const float*) pCol->pData;

        for (int32_t i = pInput->startRowIndex; i < pInput->numOfRows + pInput->startRowIndex; ++i) {
          pAvgRes->sum.dsum += plist[i];
        }
        break;
      }
      case TSDB_DATA_TYPE_DOUBLE: {
        const double* plist = (const double*)pCol->pData;

        for (int32_t i = pInput->startRowIndex; i < pInput->numOfRows + pInput->startRowIndex; ++i) {
          pAvgRes->sum.dsum += plist[i];
        }
        break;
      }
      default:
        return TSDB_CODE_FUNC_FUNTION_PARA_TYPE;
    }
  } else {
    numOfElem = doAddNumericVector(pCol, type, pInput, pAvgRes);
  }

_over:
  // data in the check operation are all null, not output
  SET_VAL(GET_RES_INFO(pCtx), numOfElem, 1);
  return TSDB_CODE_SUCCESS;
}

static void avgTransferInfo(SAvgRes* pInput, SAvgRes* pOutput) {
  if (IS_NULL_TYPE(pInput->type)) {
    return;
  }

  pOutput->type = pInput->type;
  if (IS_SIGNED_NUMERIC_TYPE(pOutput->type)) {
    bool overflow = pInput->sum.overflow;
    CHECK_OVERFLOW_SUM_SIGNED_BIG(pOutput, (overflow ? pInput->sum.dsum : pInput->sum.isum), overflow);
  } else if (IS_UNSIGNED_NUMERIC_TYPE(pOutput->type)) {
    bool overflow = pInput->sum.overflow;
    CHECK_OVERFLOW_SUM_UNSIGNED_BIG(pOutput, (overflow ? pInput->sum.dsum : pInput->sum.usum), overflow);
  } else {
    pOutput->sum.dsum += pInput->sum.dsum;
  }

  pOutput->count += pInput->count;
}

int32_t avgFunctionMerge(SqlFunctionCtx* pCtx) {
  SInputColumnInfoData* pInput = &pCtx->input;
  SColumnInfoData*      pCol = pInput->pData[0];

  if (IS_NULL_TYPE(pCol->info.type)) {
    SET_VAL(GET_RES_INFO(pCtx), 0, 1);
    return TSDB_CODE_SUCCESS;
  }

  if (pCol->info.type != TSDB_DATA_TYPE_BINARY) {
    return TSDB_CODE_FUNC_FUNTION_PARA_TYPE;
  }

  SAvgRes* pInfo = GET_ROWCELL_INTERBUF(GET_RES_INFO(pCtx));

  int32_t start = pInput->startRowIndex;

  for (int32_t i = start; i < start + pInput->numOfRows; ++i) {
    if(colDataIsNull_s(pCol, i)) continue;
    char*    data = colDataGetData(pCol, i);
    SAvgRes* pInputInfo = (SAvgRes*)varDataVal(data);
    avgTransferInfo(pInputInfo, pInfo);
  }

  SET_VAL(GET_RES_INFO(pCtx), 1, 1);

  return TSDB_CODE_SUCCESS;
}

#ifdef BUILD_NO_CALL
int32_t avgInvertFunction(SqlFunctionCtx* pCtx) {
  int32_t numOfElem = 0;

  // Only the pre-computing information loaded and actual data does not loaded
  SInputColumnInfoData* pInput = &pCtx->input;
  SAvgRes* pAvgRes = GET_ROWCELL_INTERBUF(GET_RES_INFO(pCtx));

  // computing based on the true data block
  SColumnInfoData* pCol = pInput->pData[0];

  int32_t start = pInput->startRowIndex;
  int32_t numOfRows = pInput->numOfRows;

  switch (pCol->info.type) {
    case TSDB_DATA_TYPE_TINYINT: {
      LIST_AVG_N(pAvgRes->sum.isum, int8_t);
      break;
    }
    case TSDB_DATA_TYPE_SMALLINT: {
      LIST_AVG_N(pAvgRes->sum.isum, int16_t);
      break;
    }
    case TSDB_DATA_TYPE_INT: {
      LIST_AVG_N(pAvgRes->sum.isum, int32_t);
      break;
    }
    case TSDB_DATA_TYPE_BIGINT: {
      LIST_AVG_N(pAvgRes->sum.isum, int64_t);
      break;
    }
    case TSDB_DATA_TYPE_UTINYINT: {
      LIST_AVG_N(pAvgRes->sum.usum, uint8_t);
      break;
    }
    case TSDB_DATA_TYPE_USMALLINT: {
      LIST_AVG_N(pAvgRes->sum.usum, uint16_t);
      break;
    }
    case TSDB_DATA_TYPE_UINT: {
      LIST_AVG_N(pAvgRes->sum.usum, uint32_t);
      break;
    }
    case TSDB_DATA_TYPE_UBIGINT: {
      LIST_AVG_N(pAvgRes->sum.usum, uint64_t);
      break;
    }
    case TSDB_DATA_TYPE_FLOAT: {
      LIST_AVG_N(pAvgRes->sum.dsum, float);
      break;
    }
    case TSDB_DATA_TYPE_DOUBLE: {
      LIST_AVG_N(pAvgRes->sum.dsum, double);
      break;
    }
    default:
      break;
  }

  // data in the check operation are all null, not output
  SET_VAL(GET_RES_INFO(pCtx), numOfElem, 1);
  return TSDB_CODE_SUCCESS;
}
#endif

int32_t avgCombine(SqlFunctionCtx* pDestCtx, SqlFunctionCtx* pSourceCtx) {
  SResultRowEntryInfo* pDResInfo = GET_RES_INFO(pDestCtx);
  SAvgRes*             pDBuf = GET_ROWCELL_INTERBUF(pDResInfo);

  SResultRowEntryInfo* pSResInfo = GET_RES_INFO(pSourceCtx);
  SAvgRes*             pSBuf = GET_ROWCELL_INTERBUF(pSResInfo);
  int16_t              type = pDBuf->type == TSDB_DATA_TYPE_NULL ? pSBuf->type : pDBuf->type;

  if (IS_SIGNED_NUMERIC_TYPE(type)) {
    CHECK_OVERFLOW_SUM_SIGNED(pDBuf, pSBuf->sum.isum)
  } else if (IS_UNSIGNED_NUMERIC_TYPE(type)) {
    CHECK_OVERFLOW_SUM_UNSIGNED(pDBuf, pSBuf->sum.usum)
  } else {
    pDBuf->sum.dsum += pSBuf->sum.dsum;
  }
  pDBuf->count += pSBuf->count;

  return TSDB_CODE_SUCCESS;
}

int32_t avgFinalize(SqlFunctionCtx* pCtx, SSDataBlock* pBlock) {
  SResultRowEntryInfo* pEntryInfo = GET_RES_INFO(pCtx);

  SAvgRes* pRes = GET_ROWCELL_INTERBUF(pEntryInfo);
  int32_t  type = pRes->type;

  if (pRes->count > 0) {
    if(pRes->sum.overflow) {
      // overflow flag set , use dsum
      pRes->result = pRes->sum.dsum / ((double)pRes->count);
    }else if (IS_SIGNED_NUMERIC_TYPE(type)) {
      pRes->result = pRes->sum.isum / ((double)pRes->count);
    } else if (IS_UNSIGNED_NUMERIC_TYPE(type)) {
      pRes->result = pRes->sum.usum / ((double)pRes->count);
    } else {
      pRes->result = pRes->sum.dsum / ((double)pRes->count);
    }
  }

  if (pRes->count == 0 || isinf(pRes->result) || isnan(pRes->result)) {
    pEntryInfo->numOfRes = 0;
  } else {
    pEntryInfo->numOfRes = 1;
  }

  return functionFinalize(pCtx, pBlock);
}

int32_t avgPartialFinalize(SqlFunctionCtx* pCtx, SSDataBlock* pBlock) {
  SResultRowEntryInfo* pResInfo = GET_RES_INFO(pCtx);
  SAvgRes*             pInfo = GET_ROWCELL_INTERBUF(GET_RES_INFO(pCtx));
  int32_t              resultBytes = getAvgInfoSize();
  char*                res = taosMemoryCalloc(resultBytes + VARSTR_HEADER_SIZE, sizeof(char));
  int32_t              code = TSDB_CODE_SUCCESS;
  if (NULL == res) {
    return terrno;
  }
  (void)memcpy(varDataVal(res), pInfo, resultBytes);
  varDataSetLen(res, resultBytes);

  int32_t          slotId = pCtx->pExpr->base.resSchema.slotId;
  SColumnInfoData* pCol = taosArrayGet(pBlock->pDataBlock, slotId);
  if(NULL == pCol) {
    code = TSDB_CODE_OUT_OF_RANGE;
    goto _exit;
  }

  code = colDataSetVal(pCol, pBlock->info.rows, res, false);

_exit:
  taosMemoryFree(res);
  return code;
}
