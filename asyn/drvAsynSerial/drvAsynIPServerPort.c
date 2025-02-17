/**********************************************************************
* Asyn device support using TCP stream or UDP datagram server port           *
**********************************************************************/
/***********************************************************************
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
* National Laboratory, and the Regents of the University of
* California, as Operator of Los Alamos National Laboratory, and
* Berliner Elektronenspeicherring-Gesellschaft m.b.H. (BESSY).
* asynDriver is distributed subject to a Software License Agreement
* found in file LICENSE that is included with this distribution.
***********************************************************************/

/*
 * $Id: drvAsynIPServerPort.c,v 1.8 2013/05/15 13:09:45 zimoch Exp $
 */

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <osiUnistd.h>
#include <osiSock.h>
#include <cantProceed.h>
#include <errlog.h>
#include <iocsh.h>
#include <epicsExit.h>
#include <epicsAssert.h>
#include <epicsStdio.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <epicsTimer.h>
#include <osiUnistd.h>

#include <epicsExport.h>
#include "asynDriver.h"
#include "asynInt32.h"
#include "asynOctet.h"
#include "asynCommonSyncIO.h"
#include "asynInterposeEos.h"
#include "drvAsynIPServerPort.h"
#include "drvAsynIPPort.h"

/* This structure holds the information for an IP port created by the listener */
typedef struct {
    char               *portName;
    int                fd;
    asynUser          *pasynUser;
} portList_t;

/*
 * This structure holds the hardware-specific information for a single IP listener port.
 */
typedef struct {
    asynUser          *pasynUser;
    unsigned int       portNumber;
    char              *portName;
    char              *serverInfo;
    char              *interfaceInfo;
    int                maxClients;
    int                socketType;
    int                priority;
    int                noAutoConnect;
    int                noProcessEos;
    int                fd;
    asynInterface      common;
    asynInterface      int32;
    asynInterface      octet;
    void               *octetCallbackPvt;
    portList_t         *portList;
    char               *IPDeviceName;
    epicsTimerId       timer;
    volatile int       timeoutFlag;
    int                flags;
    unsigned long      nRead;
    unsigned long      nWritten;
    char               *UDPbuffer;
    int                UDPbufferSize;
    int                UDPbufferPos;
} ttyController_t;

#define THEORETICAL_UDP_MAX_SIZE 65507

/* Function prototypes */
static void serialBaseInit(void);
static void closeConnection(asynUser *pasynUser, ttyController_t *tty);
static void connectionListener(void *drvPvt);
static void report(void *drvPvt, FILE *fp, int details);
static asynStatus connectIt(void *drvPvt, asynUser *pasynUser);
static asynStatus disconnect(void *drvPvt, asynUser *pasynUser);
static void ttyCleanup(void *tty);
int drvAsynIPServerPortConfigure(const char *portName,
        const char *serverInfo,
        unsigned int maxClients,
        unsigned int priority,
        int noAutoConnect,
        int noProcessEos);




/* asynCommon methods */
static asynCommon drvAsynIPServerPortCommon = {
    report,
    connectIt,
    disconnect
};
/* asynInt32 methods */
static asynInt32 drvAsynIPServerPortInt32 = {
   NULL, /* Write */
   NULL, /* Read */
   NULL, /* Get Bounds */
   NULL,
   NULL
};
/* asynOctet methods */
static asynOctet drvAsynIPServerPortOctet = {
   NULL, /* Write */
   NULL, /* Write raw */
   NULL, /* Read */
   NULL, /* Read raw */
};

typedef struct serialBase {
    epicsTimerQueueId timerQueue;
} serialBase;
static serialBase *pserialBase = 0;

static void serialBaseInit(void)
{
    if (pserialBase) return;
    pserialBase = callocMustSucceed(1, sizeof (serialBase), "serialBaseInit");
    pserialBase->timerQueue = epicsTimerQueueAllocate(
            1, epicsThreadPriorityScanLow);
}

/*
 * Unblock the I/O operation
 */
static void
timeoutHandler(void *p) {
    ttyController_t *tty = (ttyController_t *) p;

    asynPrint(tty->pasynUser, ASYN_TRACE_FLOW,
            "%s timeout handler.\n", tty->portName);
    tty->timeoutFlag = 1;
}

/*
 * Close a connection
 */
static void closeConnection(asynUser *pasynUser, ttyController_t *tty)
{
    if (tty->fd >= 0) {
        asynPrint(pasynUser, ASYN_TRACE_FLOW,
                "drvAsynIPServerPort: close %s connection on port %d.\n", tty->portName, tty->portNumber);
        epicsSocketDestroy(tty->fd);
        tty->fd = INVALID_SOCKET;
        pasynManager->exceptionDisconnect(pasynUser);
    }
}

/*
 * Read from the UDP port
 */
static asynStatus readIt(void *drvPvt, asynUser *pasynUser,
        char *data, size_t maxchars, size_t *nbytesTransfered, int *gotEom) {
    ttyController_t *tty = (ttyController_t *) drvPvt;
    int thisRead;
    int readPollmsec;
    int reason = 0;
    int x;
    asynStatus status = asynSuccess;

    assert(tty);
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
            "%s %p read.\n", tty->IPDeviceName, tty->pasynUser);

    if (maxchars == 0) {
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                "%s maxchars %d. Why <=0?\n", tty->IPDeviceName, (int) maxchars);
        return asynError;
    }
    readPollmsec = (int) (pasynUser->timeout * 1000.0);
    if (readPollmsec == 0) readPollmsec = 1;
    if (readPollmsec < 0) readPollmsec = -1;
    if (gotEom) *gotEom = 0;
    if (tty->fd < 0) return asynDisconnected;
    if ((tty->UDPbufferPos == 0) && (tty->UDPbufferSize == 0)) {

        epicsThreadSleep(.001);
        thisRead = 0;
    } else {

        for (x = 0; x < (int)maxchars - 1; x++) {
            data[x] = tty->UDPbuffer[x + tty->UDPbufferPos];
        }
        thisRead = (int)maxchars - 1;
        tty->UDPbufferPos = tty->UDPbufferPos + (int)maxchars;
        if (tty->UDPbufferSize <= tty->UDPbufferPos) {
            tty->UDPbufferPos = 0;
            tty->UDPbufferSize = 0;
            reason |= ASYN_EOM_END;
        }else{
          reason |= ASYN_EOM_CNT;
        }
    }
    if (thisRead > 0) {
        asynPrintIO(pasynUser, ASYN_TRACEIO_DRIVER, data, thisRead,
                "%s read %d\n", tty->IPDeviceName, thisRead);
        tty->nRead += thisRead;
    }
    if (thisRead < 0) {
        if ((SOCKERRNO != SOCK_EINTR)) {
            epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                    "%s read error: %s",
                    tty->IPDeviceName, strerror(SOCKERRNO));
            closeConnection(pasynUser, tty);
            status = asynError;
        } else {
            epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                    "%s timeout: %s",
                    tty->IPDeviceName, strerror(SOCKERRNO));
            status = asynTimeout;
        }
    }
    if (thisRead < 0)
        thisRead = 0;
    *nbytesTransfered = thisRead;
    /* If there is room add a null byte */
    if (thisRead < (int) maxchars)
        data[thisRead] = 0;
    else
        reason |= ASYN_EOM_CNT;
    if (gotEom) *gotEom = reason;
    return status;
}

static asynStatus
flushIt(void *drvPvt, asynUser *pasynUser) {
    ttyController_t *tty = (ttyController_t *) drvPvt;
    assert(tty);
    tty->UDPbufferPos = 0;
    tty->UDPbufferSize = 0;
    return asynSuccess;
}

static asynStatus writeIt(void *drvPvt, asynUser *pasynUser,
        const char *data, size_t numchars, size_t *nbytesTransfered) {
    return asynError;
}
/*Beginning of asynCommon methods*/

/*
 * Report link parameters
 */
static void report(void *drvPvt, FILE *fp, int details)
{
    ttyController_t *tty = (ttyController_t *)drvPvt;
    portList_t *pl;
    int connected;
    int i;

    assert(tty);
    fprintf(fp, "Port %s: %sonnected\n",
        tty->portName,
        tty->fd >= 0 ? "C" : "Disc");
    if (details >= 1) {
        fprintf(fp, "            fd: %d\n", tty->fd);
        fprintf(fp, "  Max. clients: %d\n", tty->maxClients);
        for (i=0; i<tty->maxClients; i++) {
            pl = &tty->portList[i];
            pasynManager->isConnected(pl->pasynUser, &connected);
            fprintf(fp, "    Client %d name:%s fd: %d connected:%d\n", i, pl->portName, pl->fd, connected);
        }
    }
}

/*
 * This is the thread that listens for new connection requests, and issues asynOctet callbacks with the
 * port name when they occur.
 */
static void connectionListener(void *drvPvt)
{
    ttyController_t *tty = (ttyController_t *) drvPvt;
    struct sockaddr_in clientAddr;
    int clientFd;
    osiSocklen_t clientLen = sizeof (clientAddr);
    ELLLIST *pclientList;
    interruptNode *pnode;
    asynOctetInterrupt *pinterrupt;
    asynUser *pasynUser;
    asynStatus status;
    int i;
    portList_t *pl, *p;
    int connected;

    /*
     * Sanity check
     */
    assert(tty);
    pasynUser = tty->pasynUser;

    asynPrint(pasynUser, ASYN_TRACE_FLOW,
            "drvAsynIPServerPort: %s started listening for connections on %s\n",
            tty->portName, tty->serverInfo);
    while (1) {
        if (tty->socketType == SOCK_DGRAM) {
            if ((tty->UDPbufferPos == 0) && (tty->UDPbufferSize == 0)) {
                tty->UDPbufferSize = recvfrom(tty->fd, tty->UDPbuffer, THEORETICAL_UDP_MAX_SIZE , 0, NULL, NULL);
                pasynManager->interruptStart(tty->octetCallbackPvt, &pclientList);
                pnode = (interruptNode *) ellFirst(pclientList);
                while (pnode) {
                    pinterrupt = pnode->drvPvt;
                    pinterrupt->callback(pinterrupt->userPvt, pinterrupt->pasynUser,
                            tty->UDPbuffer, tty->UDPbufferSize, ASYN_EOM_END);
                    pnode = (interruptNode *) ellNext(&pnode->node);
                }
                pasynManager->interruptEnd(tty->octetCallbackPvt);
            }else{
              epicsThreadSleep(.001);
            }

        } else {
            clientFd = epicsSocketAccept(tty->fd, (struct sockaddr *) &clientAddr, &clientLen);
            asynPrint(pasynUser, ASYN_TRACE_FLOW,
                    "drvAsynIPServerPort: new connection, socket=%d on %s\n",
                    clientFd, tty->serverInfo);
            if (clientFd < 0) {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                        "drvAsynIPServerPort: accept error on %s: fd=%d, %s\n", tty->serverInfo,
                        tty->fd, strerror(errno));
                continue;
            }
            /* Search for a port which is disconnected */
            pl = NULL;
            for (i = 0, p = &tty->portList[0]; i < tty->maxClients; i++, p++) {
                pasynManager->isConnected(p->pasynUser, &connected);
                if (!connected) {
                    pl = p;
                    break;
                }
            }
            if (pl == NULL) {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "drvAsynIPServerPort: %s: too many clients\n", tty->portName);
                epicsSocketDestroy(clientFd);
                continue;
            }
            /* Set the existing port to use the new file descriptor */
            pl->pasynUser->reason = clientFd;
            pl->fd = clientFd;
            status = pasynCommonSyncIO->connectDevice(pl->pasynUser);
            if (status != asynSuccess) {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                        "%s drvAsynIPServerPort: error calling "
                        "pasynCommonSyncIO->connectDevice %s\n",
                        pl->portName, pl->pasynUser->errorMessage);
                continue;
            }
            pl->pasynUser->reason = 0;
            /* Set the new port to initially have the same trace mask that we have */
            pasynTrace->setTraceMask(pl->pasynUser, pasynTrace->getTraceMask(pasynUser));
            pasynTrace->setTraceIOMask(pl->pasynUser, pasynTrace->getTraceIOMask(pasynUser));

            /* Issue callbacks to all clients who want notification on connection */
            pasynManager->interruptStart(tty->octetCallbackPvt, &pclientList);
            pnode = (interruptNode *) ellFirst(pclientList);
            while (pnode) {
                pinterrupt = pnode->drvPvt;

                pinterrupt->callback(pinterrupt->userPvt, pinterrupt->pasynUser,
                        pl->portName, strlen(pl->portName), 0);
                pnode = (interruptNode *) ellNext(&pnode->node);
            }
            pasynManager->interruptEnd(tty->octetCallbackPvt);
        }
    }
}

int createServerSocket(ttyController_t *tty) {
    int i;
    struct sockaddr_in serverAddr;
    int oneVal=1;
    char srvaddrtxt[256];
    assert(tty);
    /*
     * Create the socket
     */
    if (tty->fd == INVALID_SOCKET) {
        if ((tty->fd = (int)epicsSocketCreate(PF_INET, tty->socketType, 0)) < 0) {
            printf("Can't create socket: %s", strerror(SOCKERRNO));
            return -1;
        }

        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(tty->portNumber);

        /*
         * Got host or interface name, resolve it unless we see "localhost":
         * This may be used in existing start scripts and we don't want to
         * break them. If you need the loopback, use "127.0.0.1".
         */
        if (tty->interfaceInfo != NULL && *tty->interfaceInfo &&
            epicsStrCaseCmp(tty->interfaceInfo, "localhost")) {
            if (hostToIPAddr(tty->interfaceInfo, &serverAddr.sin_addr) < 0) {
                printf("using ANY interface, cannot lookup '%s': %s\n",
                       tty->interfaceInfo, strerror(SOCKERRNO));
                return -1;
          }
        }

        if (ipAddrToDottedIP(&serverAddr, &srvaddrtxt[0], sizeof(srvaddrtxt)) > 0) {
            srvaddrtxt[sizeof(srvaddrtxt) - 1] = '\0';
            printf("serverAddr: %s\n", srvaddrtxt);
        }
        printf("serverPort: %u\n", tty->portNumber);
        if (tty->socketType == SOCK_DGRAM) {
            /* For Port reuse, multiple IOCs */
            epicsSocketEnableAddressUseForDatagramFanout(tty->fd);
        }
        if (setsockopt(tty->fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&oneVal, sizeof(int))) {
            printf("Error calling setsockopt %s: %s\n", tty->serverInfo, strerror(errno));
            epicsSocketDestroy(tty->fd);
            tty->fd = INVALID_SOCKET;
            return -1;
        }
        if (bind(tty->fd, (struct sockaddr *) &serverAddr, sizeof (serverAddr)) < 0) {
            printf("Error in binding %s: %s\n", tty->serverInfo, strerror(errno));
            epicsSocketDestroy(tty->fd);
            tty->fd = INVALID_SOCKET;
            return -1;
        }

        /*
         * Enable listening on this port
         */
        if (tty->socketType != SOCK_DGRAM) {
            i = listen(tty->fd, tty->maxClients);
            if (i < 0) {
                printf("Error calling listen() on %s:  %s\n",
                        tty->serverInfo, strerror(errno));
                epicsSocketDestroy(tty->fd);
                tty->fd = INVALID_SOCKET;
                return -1;
            }
        } else {
            tty->UDPbuffer = malloc(THEORETICAL_UDP_MAX_SIZE);
        }
    }
    return 0;
}

static asynStatus connectIt(void *drvPvt, asynUser *pasynUser)
{
    ttyController_t *tty = (ttyController_t *) drvPvt;
    int status;
    assert(tty);
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
            "drvAsynIPServerPort: %s connect\n", tty->portName);
    status = createServerSocket(tty);
    if (status) {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
                "drvAsynIPServerPort: error calling createSocket on %s: %s\n",
                tty->portName, pasynUser->errorMessage);
    }

    status = pasynManager->exceptionConnect(pasynUser);
    if (status) {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
                "drvAsynIPServerPort: error calling exceptionConnect on %s: %s\n",
                tty->portName, pasynUser->errorMessage);
    }
    return asynSuccess;
}

static asynStatus disconnect(void *drvPvt, asynUser *pasynUser)
{
    ttyController_t *tty = (ttyController_t *) drvPvt;

    assert(tty);
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
            "drvAsynIPServerPort: %s disconnect\n", tty->portName);
    closeConnection(pasynUser, tty);
    return asynSuccess;
}

/*
 * Clean up a ttyController
 */
static void ttyCleanup(void *pPvt)
{
    ttyController_t *tty = (ttyController_t*) pPvt;

    if (!tty) return;
    if (tty->fd >= 0) {
        asynPrint(tty->pasynUser, ASYN_TRACE_FLOW, "drvAsynIPServerPort:ttyCleanup %s: shutdown socket %d\n", tty->portName, tty->fd);
        epicsSocketDestroy(tty->fd);
    }
    free(tty->portName);
    free(tty->serverInfo);
    free(tty->interfaceInfo);
    free(tty);
}

/*
 * Configure and register an IP port listener
 */
int drvAsynIPServerPortConfigure(const char *portName,
        const char *serverInfo,
        unsigned int maxClients,
        unsigned int priority,
        int noAutoConnect,
        int noProcessEos) {
    ttyController_t *tty;
    asynStatus status;
    int i;
    int len;
    portList_t *pl;
    char protocol[6];
    char *cp;

    if (portName == NULL) {
        printf("Port name missing.\n");
        return -1;
    }
    if (serverInfo == NULL) {
        printf("TCP server information missing.\n");
        return -1;
    }
    if (maxClients == 0) {
        printf("No clients.\n");
        return -1;
    }

    /*
     * Perform some one-time-only initializations
     */
    if (pserialBase == NULL) {
        if (osiSockAttach() == 0) {
            printf("drvAsynIPServerPortConfigure: osiSockAttach failed\n");
            return -1;
        }
        serialBaseInit();
    }

    /*
     * Create a driver
     */
    tty = (ttyController_t *) callocMustSucceed(1, sizeof (ttyController_t),
            "drvAsynIPServerPortConfigure()");
    tty->fd = INVALID_SOCKET;
    tty->maxClients = maxClients;
    tty->portName = epicsStrDup(portName);
    tty->serverInfo = epicsStrDup(serverInfo);
    tty->priority = priority;
    tty->noAutoConnect = noAutoConnect;
    tty->noProcessEos = noProcessEos;
    tty->portList = callocMustSucceed(tty->maxClients, sizeof (portList_t), "drvAsynIPServerPortConfig");
    tty->UDPbuffer = NULL;
    tty->UDPbufferSize = 0;
    tty->UDPbufferPos = 0;
    /*
     * Parse configuration parameters
     */
    protocol[0] = '\0';
    if (((cp = strchr(serverInfo, ':')) == NULL)
            || (sscanf(cp, ":%u %5s", &tty->portNumber, protocol) < 1)) {
        printf("drvAsynIPPortConfigure: \"%s\" is not of the form \"<host>:<port> [protocol]\"\n",
                tty->serverInfo);
        ttyCleanup(tty);
        return -1;
    }
    *cp = '\0';
    tty->interfaceInfo = epicsStrDup(serverInfo);

    if ((protocol[0] == '\0')
            || (epicsStrCaseCmp(protocol, "tcp") == 0)) {
        tty->socketType = SOCK_STREAM;
    } else if (epicsStrCaseCmp(protocol, "udp") == 0) {
        tty->socketType = SOCK_DGRAM;
    } else {
        printf("drvAsynIPServerPortConfigure: Unknown protocol \"%s\".\n", protocol);
        ttyCleanup(tty);
        return -1;
    }
    /*
     *  Create the Server Socket
     */

    if (createServerSocket(tty)) {
        printf("drvAsynIPServerPortConfigure: Error in createServerSocket.\n");
        return -1;
    }
    /*
     * Create timeout mechanism
     */
    tty->timer = epicsTimerQueueCreateTimer(
            pserialBase->timerQueue, timeoutHandler, tty);
    if (!tty->timer) {
        printf("drvAsynSerialPortConfigure: Can't create timer.\n");
        return -1;
    }

    /*
     *  Link with higher level routines
     */
    tty->common.interfaceType = asynCommonType;
    tty->common.pinterface = &drvAsynIPServerPortCommon;
    tty->common.drvPvt = tty;
    if (pasynManager->registerPort(tty->portName,
            ASYN_CANBLOCK,
            !noAutoConnect,
            priority,
            0) != asynSuccess) {
        printf("drvAsynIPServerPortConfigure: Can't register myself.\n");
        ttyCleanup(tty);
        return -1;
    }
    status = pasynManager->registerInterface(tty->portName, &tty->common);
    if (status != asynSuccess) {
        printf("drvAsynIPServerPortConfigure: Can't register common.\n");
        ttyCleanup(tty);
        return -1;
    }
    if (tty->socketType != SOCK_DGRAM) {
        tty->int32.interfaceType = asynInt32Type;
        tty->int32.pinterface = &drvAsynIPServerPortInt32;
        tty->int32.drvPvt = tty;
        status = pasynInt32Base->initialize(tty->portName, &tty->int32);
        if (status != asynSuccess) {
            printf("drvAsynIPServerPortConfigure: pasynInt32Base->initialize failed.\n");
            ttyCleanup(tty);
            return -1;
        }
    }
    tty->octet.interfaceType = asynOctetType;
    if (tty->socketType == SOCK_DGRAM) {
        drvAsynIPServerPortOctet.read = &readIt;
        drvAsynIPServerPortOctet.write = &writeIt;
        drvAsynIPServerPortOctet.flush = &flushIt;
    }
    tty->octet.pinterface = &drvAsynIPServerPortOctet;
    tty->octet.drvPvt = tty;
    status = pasynOctetBase->initialize(tty->portName, &tty->octet, 0, 0, 0);
    if (status != asynSuccess) {
        printf("drvAsynIPServerPortConfigure: pasynOctetBase->initialize failed.\n");
        ttyCleanup(tty);
        return -1;
    }
    status = pasynManager->registerInterruptSource(tty->portName, &tty->octet,
            &tty->octetCallbackPvt);
    if (status != asynSuccess) {
        printf("drvAsynIPServerPortConfigure registerInterruptSource failed\n");
        ttyCleanup(tty);
        return -1;
    }
    tty->pasynUser = pasynManager->createAsynUser(0, 0);

    status = pasynManager->connectDevice(tty->pasynUser, tty->portName, -1);
    if (status != asynSuccess) {
        printf("connectDevice failed %s\n", tty->pasynUser->errorMessage);
        ttyCleanup(tty);
        return -1;
    }

    /* Create drvAsynIPPort drivers for maxClients ports */
    for (i=0; i<tty->maxClients; i++) {
        /* Create a new asyn port with a unique name */
        len = (int)strlen(tty->portName) + 10; /* Room for port name + ":" + i */
        pl = &tty->portList[i];
        pl->portName = callocMustSucceed(1, len, "drvAsynIPServerPortConfigure");
        pl->fd = INVALID_SOCKET;
        epicsSnprintf(pl->portName, len, "%s:%d", tty->portName, i);
        /* Must create port with noAutoConnect, we manually connect with the file descriptor */
        status = drvAsynIPPortConfigure(pl->portName,
                tty->serverInfo,
                tty->priority,
                1, /* noAutoConnect */
                tty->noProcessEos);
        if (status) {
            asynPrint(tty->pasynUser, ASYN_TRACE_ERROR,
                    "drvAsynIPServerPort: unable to create port %s\n", pl->portName);
            continue;
        }
        status = pasynCommonSyncIO->connect(pl->portName, -1, &pl->pasynUser, NULL);
        if (status != asynSuccess) {
            asynPrint(tty->pasynUser, ASYN_TRACE_ERROR,
                    "%s drvAsynIPServerPort: error calling "
                    "pasynCommonSyncIO->connect %s\n",
                    pl->portName, pl->pasynUser->errorMessage);
            continue;
        }
    }

    /* Start a thread listening on this port */
    epicsThreadCreate(tty->portName,
            epicsThreadPriorityLow,
            epicsThreadGetStackSize(epicsThreadStackSmall),
            (EPICSTHREADFUNC) connectionListener, tty);

    /*
     * Register for socket cleanup
     */
    epicsAtExit(ttyCleanup, tty);

    return 0;
}

/*
 * IOC shell command registration
 */
static const iocshArg drvAsynIPServerPortConfigureArg0 = {"port name", iocshArgString};
static const iocshArg drvAsynIPServerPortConfigureArg1 = {"localhost:port [proto]", iocshArgString};
static const iocshArg drvAsynIPServerPortConfigureArg2 = {"max clients", iocshArgInt};
static const iocshArg drvAsynIPServerPortConfigureArg3 = {"priority", iocshArgInt};
static const iocshArg drvAsynIPServerPortConfigureArg4 = {"disable auto-connect", iocshArgInt};
static const iocshArg drvAsynIPServerPortConfigureArg5 = {"noProcessEos", iocshArgInt};

static const iocshArg *drvAsynIPServerPortConfigureArgs[] = {
    &drvAsynIPServerPortConfigureArg0, &drvAsynIPServerPortConfigureArg1,
    &drvAsynIPServerPortConfigureArg2, &drvAsynIPServerPortConfigureArg3,
    &drvAsynIPServerPortConfigureArg4, &drvAsynIPServerPortConfigureArg5};

static const iocshFuncDef drvAsynIPServerPortConfigureFuncDef = {"drvAsynIPServerPortConfigure", 6, drvAsynIPServerPortConfigureArgs};

static void drvAsynIPServerPortConfigureCallFunc(const iocshArgBuf *args) {
    drvAsynIPServerPortConfigure(args[0].sval, args[1].sval, args[2].ival,
            args[3].ival, args[4].ival, args[5].ival);
}

/*
 * This routine is called before multitasking has started, so there's
 * no race condition in the test/set of firstTime.
 */
static void
drvAsynIPServerPortRegisterCommands(void)
{
    static int firstTime = 1;
    if (firstTime) {
        iocshRegister(&drvAsynIPServerPortConfigureFuncDef, drvAsynIPServerPortConfigureCallFunc);
        firstTime = 0;
    }
}
epicsExportRegistrar(drvAsynIPServerPortRegisterCommands);
