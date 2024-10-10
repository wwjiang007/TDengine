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

#include "tobjectpool.h"

struct SObjectPool {
  void          *objects;
  uint32_t      *bitmap;
  TdThreadMutex mutex;
  size_t         capacity;
  size_t         objectSize;
};

SObjectPool *taosObjectPoolInit(size_t capacity, size_t objectSize) {
  SObjectPool *pool = taosMemoryCalloc(1, sizeof(SObjectPool));
  if (pool == NULL) {
    return NULL;
  }

  pool->objects = taosMemoryMalloc(capacity * objectSize);
  if (pool->objects == NULL) {
    taosMemoryFree(pool);
    return NULL;
  }

  pool->bitmap = taosMemoryMalloc((capacity + 31) / 32 * sizeof(uint32_t));
  if (pool->bitmap == NULL) {
    taosMemoryFree(pool->objects);
    taosMemoryFree(pool);
    return NULL;
  }

  (void)taosThreadMutexInit(&pool->mutex, NULL);

  pool->capacity = capacity;
  pool->objectSize = objectSize;

  return pool;
}

void taosObjectPoolDestroy(SObjectPool *pool) {
  if (pool == NULL) {
    return;
  }

  taosMemoryFree(pool->bitmap);
  taosMemoryFree(pool->objects);

  (void)taosThreadMutexDestroy(&pool->mutex);

  taosMemoryFree(pool);
}

void* taosObjectPoolAlloc(SObjectPool *pool) {
  if (pool == NULL) {
    return NULL;
  }

  void* pObject = NULL;

  // (void)taosThreadMutexLock(&pool->mutex);

  size_t bitMapCap = (pool->capacity + 31) / 32;
  for (size_t i = 0; i < bitMapCap; ++i) {
    if (pool->bitmap[i] == 0xffffffff) {
      continue;
    }

    for (size_t j = 0; j < 32; ++j) {
      if ((i * 32 + j) >= pool->capacity) {
        break;
      }

      if ((pool->bitmap[i] & (1 << j)) == 0) {
        pool->bitmap[i] |= (1 << j);
        pObject = (char *)pool->objects + (i * 32 + j) * pool->objectSize;
        break;
      }
    }
  }

  // (void)taosThreadMutexLock(&pool->mutex);

  return pObject;
}

void taosObjectPoolFree(SObjectPool *pool, void *object) {
  if (pool == NULL || object == NULL) {
    return;
  }

  size_t offset = (char *)object - (char *)pool->objects;
  if (offset % pool->objectSize != 0) {
    return;
  }

  size_t index = offset / pool->objectSize;
  size_t i = index / 32;
  size_t j = index % 32;

  pool->bitmap[i] &= ~(1 << j);
}
