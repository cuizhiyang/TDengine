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
#include "os.h"
#include "taosmsg.h"
#include "taoserror.h"
#include "tlog.h"
#include "tqueue.h"
#include "trpc.h"
#include "dnodeWrite.h"
#include "dnodeMgmt.h"

typedef struct {
  int32_t  code;
  int32_t  count;       // number of vnodes returned result
  int32_t  numOfVnodes; // number of vnodes involved
} SRpcContext;

typedef struct _write {
  void        *pCont;
  int32_t      contLen;
  SRpcMsg      rpcMsg;
  SRpcContext *pRpcContext; // RPC message context
} SWriteMsg;

typedef struct {
  taos_qset  qset;      // queue set
  pthread_t  thread;    // thread 
  int32_t    workerId;  // worker ID
} SWriteWorker;  

typedef struct _thread_obj {
  int32_t        max;        // max number of workers
  int32_t        nextId;     // from 0 to max-1, cyclic
  SWriteWorker  *writeWorker;
} SWriteWorkerPool;

static void (*dnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MAX])(void *, SWriteMsg *);
static void  *dnodeProcessWriteQueue(void *param);
static void   dnodeHandleIdleWorker(SWriteWorker *pWorker);
static void   dnodeProcessWriteResult(void *pVnode, SWriteMsg *pWrite);
static void   dnodeProcessSubmitMsg(void *pVnode, SWriteMsg *pMsg);
static void   dnodeProcessCreateTableMsg(void *pVnode, SWriteMsg *pMsg);
static void   dnodeProcessDropTableMsg(void *pVnode, SWriteMsg *pMsg);

SWriteWorkerPool wWorkerPool;

int32_t dnodeInitWrite() {
  dnodeProcessWriteMsgFp[TSDB_MSG_TYPE_SUBMIT]          = dnodeProcessSubmitMsg;
  dnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MD_CREATE_TABLE] = dnodeProcessCreateTableMsg;
  dnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MD_DROP_TABLE]   = dnodeProcessDropTableMsg;

  wWorkerPool.max = tsNumOfCores;
  wWorkerPool.writeWorker = (SWriteWorker *)calloc(sizeof(SWriteWorker), wWorkerPool.max);
  if (wWorkerPool.writeWorker == NULL) return -1;

  for (int32_t i = 0; i < wWorkerPool.max; ++i) {
    wWorkerPool.writeWorker[i].workerId = i;
  }

  return 0;
}

void dnodeCleanupWrite() {
  free(wWorkerPool.writeWorker);
}

void dnodeWrite(void *rpcMsg) {
  SRpcMsg *pMsg = rpcMsg;

  int32_t     leftLen      = pMsg->contLen;
  char        *pCont       = (char *) pMsg->pCont;
  int32_t     contLen      = 0;
  int32_t     numOfVnodes  = 0;
  int32_t     vgId         = 0;
  SRpcContext *pRpcContext = NULL;

  // parse head, get number of vnodes;

  if ( numOfVnodes > 1) {
    pRpcContext = calloc(sizeof(SRpcContext), 1);
    pRpcContext->numOfVnodes = numOfVnodes;
  }

  while (leftLen > 0) {
    // todo: parse head, get vgId, contLen

    // get pVnode from vgId
    void *pVnode = dnodeGetVnode(vgId);
    if (pVnode == NULL) {

      continue;
    }
   
    // put message into queue
    SWriteMsg *pWriteMsg = taosAllocateQitem(sizeof(SWriteMsg));
    pWriteMsg->rpcMsg      = *pMsg;
    pWriteMsg->pCont       = pCont;
    pWriteMsg->contLen     = contLen;
    pWriteMsg->pRpcContext = pRpcContext;
 
    taos_queue queue = dnodeGetVnodeWworker(pVnode);
    taosWriteQitem(queue, 0, pWriteMsg);
 
    // next vnode 
    leftLen -= contLen;
    pCont -= contLen; 
  }
}

void *dnodeAllocateWriteWorker(void *pVnode) {
  SWriteWorker *pWorker = wWorkerPool.writeWorker + wWorkerPool.nextId;

  if (pWorker->qset == NULL) {
    pWorker->qset = taosOpenQset();
    if (pWorker->qset == NULL) return NULL;

    pthread_attr_t thAttr;
    pthread_attr_init(&thAttr);
    pthread_attr_setdetachstate(&thAttr, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&pWorker->thread, &thAttr, dnodeProcessWriteQueue, pWorker) != 0) {
      dError("failed to create thread to process read queue, reason:%s", strerror(errno));
      taosCloseQset(pWorker->qset);
    }
  }

  taos_queue *queue = taosOpenQueue();
  if (queue) {
    taosAddIntoQset(pWorker->qset, queue, pVnode);
    wWorkerPool.nextId = (wWorkerPool.nextId + 1) % wWorkerPool.max;
  }
 
  return queue;
}

void dnodeFreeWriteWorker(void *wqueue) {
  taosCloseQueue(wqueue);

  // dynamically adjust the number of threads
}

static void *dnodeProcessWriteQueue(void *param) {
  SWriteWorker *pWorker = (SWriteWorker *)param;
  taos_qall     qall;
  SWriteMsg    *pWriteMsg;
  int32_t       numOfMsgs;
  int           type;
  void         *pVnode;

  while (1) {
    numOfMsgs = taosReadAllQitemsFromQset(pWorker->qset, &qall, &pVnode);
    if (numOfMsgs <=0) { 
      dnodeHandleIdleWorker(pWorker);  // thread exit if no queues anymore
      continue;
    }

    for (int32_t i=0; i<numOfMsgs; ++i) {
      // retrieve all items, and write them into WAL
      taosGetQitem(qall, &type, &pWriteMsg);

      // walWrite(pVnode->whandle, writeMsg.rpcMsg.msgType, writeMsg.pCont, writeMsg.contLen);
    }
    
    // flush WAL file
    // walFsync(pVnode->whandle);

    // browse all items, and process them one by one
    taosResetQitems(qall);
    for (int32_t i = 0; i < numOfMsgs; ++i) {
      taosGetQitem(qall, &type, &pWriteMsg);

      terrno = 0;
      if (dnodeProcessWriteMsgFp[pWriteMsg->rpcMsg.msgType]) {
        (*dnodeProcessWriteMsgFp[pWriteMsg->rpcMsg.msgType]) (pVnode, pWriteMsg);
      } else {
        terrno = TSDB_CODE_MSG_NOT_PROCESSED;  
      }
     
      dnodeProcessWriteResult(pVnode, pWriteMsg);
    }

    // free the Qitems;
    taosFreeQitems(qall);
  }

  return NULL;
}

static void dnodeProcessWriteResult(void *pVnode, SWriteMsg *pWrite) {
  SRpcContext *pRpcContext = pWrite->pRpcContext;
  int32_t      code = 0;

  dnodeReleaseVnode(pVnode);

  if (pRpcContext) {
    if (terrno) {
      if (pRpcContext->code == 0) pRpcContext->code = terrno;  
    }

    int32_t count = atomic_add_fetch_32(&pRpcContext->count, 1);
    if (count < pRpcContext->numOfVnodes) {
      // not over yet, multiple vnodes
      return;
    }

    // over, result can be merged now
    code = pRpcContext->code;
  } else {
    code = terrno;
  }

  SRpcMsg rsp;
  rsp.handle = pWrite->rpcMsg.handle;
  rsp.code   = code;
  rsp.pCont  = NULL;
  rpcSendResponse(&rsp);
  rpcFreeCont(pWrite->rpcMsg.pCont);  // free the received message
}

static void dnodeHandleIdleWorker(SWriteWorker *pWorker) {
  int32_t num = taosGetQueueNumber(pWorker->qset);

  if (num > 0) {
     usleep(100);
     sched_yield(); 
  } else {
     taosCloseQset(pWorker->qset);
     pWorker->qset = NULL;
     pthread_exit(NULL);
  }
}

static void dnodeProcessSubmitMsg(void *param, SWriteMsg *pMsg) {

}

static void dnodeProcessCreateTableMsg(void *param, SWriteMsg *pMsg) {

}

static void dnodeProcessDropTableMsg(void *param, SWriteMsg *pMsg) {

}
