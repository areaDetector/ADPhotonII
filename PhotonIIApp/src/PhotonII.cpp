/* PhotonII.cpp
 *
 * This is a driver for Bruker Instrument Service (PhotonII) detectors.
 *
 * Author:  Mark Rivers
 *          University of Chicago
 *
 * Created:  June 11, 2017
 *
 */
 
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsTimer.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <cantProceed.h>
#include <iocsh.h>

#include <asynOctetSyncIO.h>

#include <ADDriver.h>

#include <epicsExport.h>
#include <PhotonII.h>

/** Frame type choices */
typedef enum {
    PhotonIIFrameNormal,
    PhotonIIFrameDark,
    PhotonIIFrameRaw,
    PhotonIIFrameDoubleCorrelation
} PhotonIIFrameType_t;

/** Messages to/from p2util */
#define PII_SIZEX 768
#define PII_SIZEY 1024
/** Time to poll when reading from p2util */
#define ASYN_POLL_TIME .01 
#define PII_POLL_DELAY .01 
#define PII_DEFAULT_TIMEOUT 1.0 
#define MAX_FILENAME_LEN 256
/** Time between checking to see if .SFRM file is complete */
#define FILE_READ_DELAY .01

static const char *driverName = "PhotonII";

/** This function is called when the exposure time timer expires */
extern "C" {static void timerCallbackC(void *drvPvt)
{
    PhotonII *pPvt = (PhotonII *)drvPvt;
    
    epicsEventSignal(pPvt->stopEventId_);
}}

static void statusTaskC(void *drvPvt)
{
    PhotonII *pPvt = (PhotonII *)drvPvt;
    
    pPvt->statusTask();
}

static void PhotonIITaskC(void *drvPvt)
{
    PhotonII *pPvt = (PhotonII *)drvPvt;
    
    pPvt->PhotonIITask();
}

/** Constructor for PhotonII driver; most parameters are simply passed to ADDriver::ADDriver.
  * After calling the base class constructor this method creates a thread to collect the detector data, 
  * and sets reasonable default values for the parameters defined in this class, asynNDArrayDriver, and ADDriver.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] commandPort The name of the asyn port previously created with drvAsynIPPortConfigure to
  *            send commands to PhotonII.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  */
PhotonII::PhotonII(const char *portName, const char *commandPort,
                             int maxBuffers, size_t maxMemory,
                             int priority, int stackSize)

    : ADDriver(portName, 1, 0, maxBuffers, maxMemory,
               0, 0,             /* No interfaces beyond those set in ADDriver.cpp */
               ASYN_CANBLOCK, 1, /* ASYN_CANBLOCK=1, ASYN_MULTIDEVICE=0, autoConnect=1 */
               priority, stackSize),
      detSizeX_(PII_SIZEX), detSizeY_(PII_SIZEY)
{
    int status = asynSuccess;
    epicsTimerQueueId timerQ;
    const char *functionName = "PhotonII";

    createParam(PhotonIIFileTimeoutString,  asynParamFloat64, &PhotonIIFileTimeout);
    createParam(PhotonIINumDarksString,     asynParamInt32,   &PhotonIINumDarks);
    createParam(PhotonIIStatusString,       asynParamOctet,   &PhotonIIStatus);

    /* Create the epicsEvents for signaling to the PhotonII task when acquisition starts and stops */
    startEventId_ = epicsEventMustCreate(epicsEventEmpty);
    stopEventId_ = epicsEventMustCreate(epicsEventEmpty);
    readoutEventId_ = epicsEventMustCreate(epicsEventEmpty);
    
    /* Create the epicsTimerQueue for exposure time handling */
    timerQ = epicsTimerQueueAllocate(1, epicsThreadPriorityScanHigh);
    timerId_ = epicsTimerQueueCreateTimer(timerQ, timerCallbackC, this);

    /* Connect to PhotonII */
    status = pasynOctetSyncIO->connect(commandPort, 0, &pasynUserCommand_, NULL);

    /* Set some default values for parameters */
    status =  setStringParam (ADManufacturer, "Bruker");
    status |= setStringParam (ADModel, "PhotonII");
    status |= setIntegerParam(ADMaxSizeX, detSizeX_);
    status |= setIntegerParam(ADMaxSizeY, detSizeY_);
    status |= setIntegerParam(ADSizeX, detSizeX_);
    status |= setIntegerParam(ADSizeY, detSizeY_);
    status |= setIntegerParam(NDArraySizeX, detSizeX_);
    status |= setIntegerParam(NDArraySizeY, detSizeY_);
    status |= setIntegerParam(NDArraySize, detSizeX_*detSizeY_*sizeof(epicsInt32));
    status |= setIntegerParam(NDDataType,  NDUInt32);
    status |= setIntegerParam(ADImageMode, ADImageContinuous);
       
    if (status) {
        printf("%s: unable to set camera parameters\n", functionName);
        return;
    }
    
    /* Create the thread that updates the images */
    status = (epicsThreadCreate("PhotonIIDetTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)PhotonIITaskC,
                                this) == NULL);
    if (status) {
        printf("%s:%s epicsThreadCreate failure for image task\n", 
            driverName, functionName);
        return;
    }

    /* Create the thread that reads the status socket */
/*    status = (epicsThreadCreate("PhotonIIStatusTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)statusTaskC,
                                this) == NULL);
    if (status) {
        printf("%s:%s epicsThreadCreate failure for status task\n", 
            driverName, functionName);
        return;
    }
*/
}

/** This function reads RAW files that PhotonII creates.  
 * It checks to make sure that the creation time of the file is after a start time passed to it, to force it to
 * wait for a new file to be created.
 */
 
asynStatus PhotonII::readRaw(const char *fileName, epicsTimeStamp *pStartTime, double timeout, NDArray *pImage)
{
    FILE *file=NULL;
    int fileExists=0;
    struct stat statBuff;
    epicsTimeStamp tStart, tCheck;
    time_t acqStartTime;
    double deltaTime;
    int status=-1;
    size_t nBytes;
    epicsUInt32 *pData = (epicsUInt32 *)pImage->pData;
    static const char *functionName = "readRaw";
        
    deltaTime = 0.;
    if (pStartTime) epicsTimeToTime_t(&acqStartTime, pStartTime);
    epicsTimeGetCurrent(&tStart);
    
    while (deltaTime <= timeout) {
        file = fopen(fileName, "rb");
        if (file && (timeout != 0.)) {
            fileExists = 1;
            /* The file exists.  Make sure it is a new file, not an old one. */
            status = stat(fileName, &statBuff);
            if (status){
                asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s error calling fstat, errno=%d %s\n",
                    driverName, functionName, errno, fileName);
                fclose(file);
                return(asynError);
            }
            /* We allow up to 10 second clock skew between time on machine running this IOC
             * and the machine with the file system returning modification time */
            if (difftime(statBuff.st_mtime, acqStartTime) > -10) break;
            fclose(file);
            file = NULL;
        }
        /* Sleep, but check for stop event, which can be used to abort a long acquisition */
        unlock();
        status = epicsEventWaitWithTimeout(stopEventId_, FILE_READ_DELAY);
        lock();
        if (status == epicsEventWaitOK) {
            return(asynError);
        }
        epicsTimeGetCurrent(&tCheck);
        deltaTime = epicsTimeDiffInSeconds(&tCheck, &tStart);
    }
    if (file == NULL) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s timeout waiting for file to be created %s\n",
            driverName, functionName, fileName);
        if (fileExists) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "  file exists but is more than 10 seconds old, possible clock synchronization problem\n");
        } 
        return(asynError);
    }
    nBytes = fread(pData, sizeof(epicsInt32), detSizeX_*detSizeY_, file);
    if (nBytes != detSizeX_*detSizeY_*sizeof(epicsInt32)) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s wrong number of bytes read from file %s = %d\n",
            driverName, functionName, fileName, (int)nBytes);
    }
    fclose(file);
    return asynSuccess;
}   

asynStatus PhotonII::writePhotonII(double timeout)
{
    size_t nwrite, nread;
    int eomReason;
    asynStatus status;
    const char *functionName="writePhotonII";

    status = pasynOctetSyncIO->writeRead(pasynUserCommand_, 
                                         toPhotonII_, strlen(toPhotonII_), 
                                         fromPhotonII_, sizeof(fromPhotonII_), 
                                         timeout, &nwrite, &nread, &eomReason);
                                                                                 
    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s, status=%d, sent\n%s\n",
                    driverName, functionName, status, toPhotonII_);

    /* Set output string so it can get back to EPICS */
    setStringParam(ADStringToServer, toPhotonII_);
    setStringParam(ADStringFromServer, fromPhotonII_);
    
    return(status);
}

asynStatus PhotonII::readPhotonII(double timeout)
{
    size_t nread;
    int eomReason;
    asynStatus status;
    const char *functionName="readPhotonII";

    status = pasynOctetSyncIO->read(pasynUserCommand_, 
                                    fromPhotonII_, sizeof(fromPhotonII_), 
                                    timeout, &nread, &eomReason);
                                                                                 
    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s, status=%d, sent\n%s\n",
                    driverName, functionName, status, toPhotonII_);

    /* Set output string so it can get back to EPICS */
    setStringParam(ADStringFromServer, fromPhotonII_);
    
    return(status);
}


/** This thread reads status strings from the status socket, makes the string available to EPICS, and
  * sends an event when it detects acquisition complete, etc. */
void PhotonII::statusTask()
{
    int status = asynSuccess;
    char response[MAX_MESSAGE_SIZE];
    char *p;
    double timeout=-1.0;
    size_t nread;
    int eomReason;
    int acquire;
    int framesize;
    int open;
    double temperature;

    while(1) {
        status = pasynOctetSyncIO->read(pasynUserCommand_, response,
                                        sizeof(response), timeout,
                                        &nread, &eomReason);
        if (status == asynSuccess) {
            this->lock();
            setStringParam(PhotonIIStatus, response);
            getIntegerParam(ADAcquire, &acquire);
            if (strstr(response, "[INSTRUMENTQUEUE /PROCESSING=0]")) {
                if (acquire) {
                    epicsEventSignal(readoutEventId_);
                }
            } 
            else if (strstr(response, "[CCDTEMPERATURE")) {
                p = strstr(response, "DEGREESC=");
                sscanf(p, "DEGREESC=%lf", &temperature);
                setDoubleParam(ADTemperature, temperature);
            }
            else if (strstr(response, "[DETECTORSTATUS")) {
                p = strstr(response, "FRAMESIZE=");
                sscanf(p, "FRAMESIZE=%d", &framesize);
                p = strstr(response, "CCDTEMP=");
                sscanf(p, "CCDTEMP=%lf", &temperature);
                setIntegerParam(ADSizeX, framesize);
                setIntegerParam(ADSizeY, framesize);
                setDoubleParam(ADTemperature, temperature);
            }
            else if (strstr(response, "[SHUTTERSTATUS")) {
                p = strstr(response, "STATUS=");
                sscanf(p, "STATUS=%d", &open);
                setIntegerParam(ADShutterStatus, open);
            }
            callParamCallbacks();
            /* Sleep a short while to allow EPICS record to process 
             * before sending the next string */
            epicsThreadSleep(0.01);
            this->unlock();
        }
    }
}

/** This thread controls acquisition, reads SFRM files to get the image data, and
  * does the callbacks to send it to higher layers */
void PhotonII::PhotonIITask()
{
    int status = asynSuccess;
    int imageCounter;
        int numImages, numImagesCounter;
    int imageMode;
    int acquire;
    NDArray *pImage;
    double acquireTime, timeRemaining;
    ADShutterMode_t shutterMode;
    int frameType;
    int numDarks;
    double readFileTimeout;
    epicsTimeStamp startTime, currentTime;
    const char *functionName = "PhotonIITask";
    std::string fileName;
    std::string filePath;
    int fileNumber;
    char fullFileName[MAX_FILENAME_LEN];
    char statusMessage[MAX_MESSAGE_SIZE];
    char *pBuff;
    size_t dims[2];
    int itemp;
    int i;
    int arrayCallbacks;
    
    this->lock();

    /* Loop forever */
    while (1) {
        setStringParam(ADStatusMessage, "Waiting for acquire command");
        setIntegerParam(ADStatus, ADStatusIdle);
        callParamCallbacks();
        /* Release the lock while we wait for an event that says acquire has started, then lock again */
        this->unlock();
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: waiting for acquire to start\n", driverName, functionName);
        status = epicsEventWait(startEventId_);
        this->lock();
        setIntegerParam(ADNumImagesCounter, 0);
        
        /* Get current values of some parameters */
        getIntegerParam(ADFrameType, &frameType);
        /* Get the exposure parameters */
        getDoubleParam(ADAcquireTime, &acquireTime);
        getIntegerParam(ADShutterMode, &itemp);  shutterMode = (ADShutterMode_t)itemp;
        getIntegerParam(ADImageMode, &imageMode);
        getIntegerParam(ADNumImages, &numImages);
        if (imageMode == ADImageSingle) numImages = 1;
        getIntegerParam(NDFileNumber, &fileNumber);
        getStringParam(NDFilePath, filePath);
        getStringParam(NDFileName, fileName);
        getDoubleParam(PhotonIIFileTimeout, &readFileTimeout);
        
        setIntegerParam(ADStatus, ADStatusAcquire);

        setStringParam(ADStatusMessage, "Starting exposure");
        /* Call the callbacks to update any changes */
        callParamCallbacks();
        switch (frameType) {
            case PhotonIIFrameNormal:
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "grab --dstdir %s --basename %s --count %d", filePath.c_str(), fileName.c_str(), numImages);
                break;
            case PhotonIIFrameDark:
                getIntegerParam(PhotonIINumDarks, &numDarks);
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "grab --darkframe --dstdir %s --basename %s --count %d", filePath.c_str(), fileName.c_str(), numDarks);
                break;
        }
        /* Send the acquire command to PhotonII */
        writePhotonII(PII_DEFAULT_TIMEOUT);

        setStringParam(ADStatusMessage, "Waiting for Acquisition");
        callParamCallbacks();
        /* Set the the start time for the TimeRemaining counter */
        epicsTimeGetCurrent(&startTime);
        timeRemaining = acquireTime * numImages;

        /* If we are using the EPICS shutter then tell it to open */
        if (shutterMode == ADShutterModeEPICS) ADDriver::setShutter(1);

        /* Wait for the frames to be written  */
        for (i=0; i<numImages; i++) {

            this->unlock();
            status = readPhotonII(acquireTime + readFileTimeout);
            this->lock();
            /* If there was an error jump to bottom of loop */
            if (status != asynSuccess) {
                asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s: error waiting for file written message\n",
                    driverName, functionName);
                goto done;
            }
            getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
            getIntegerParam(NDArrayCounter, &imageCounter);
            imageCounter++;
            setIntegerParam(NDArrayCounter, imageCounter);
            getIntegerParam(ADNumImagesCounter, &numImagesCounter);
            numImagesCounter++;
            setIntegerParam(ADNumImagesCounter, numImagesCounter);
            callParamCallbacks();
    
            if (arrayCallbacks && frameType != PhotonIIFrameDark) {
                /* Get an image buffer from the pool */
                getIntegerParam(ADSizeX, &itemp); dims[0] = itemp;
                getIntegerParam(ADSizeY, &itemp); dims[1] = itemp;
                pImage = this->pNDArrayPool->alloc(2, dims, NDInt32, 0, NULL);
printf("Preparing to scan file name, fromPhotonII_ = %s\n", fromPhotonII_);
                pBuff = strstr(fromPhotonII_, "bytes to ");
                if (pBuff == 0) {
                    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                        "%s::%s, unexpected response when looking for file name\n",
                        driverName, functionName);
                    goto done;
                }
                strcpy(fullFileName, pBuff + strlen("bytes to "));
                epicsSnprintf(statusMessage, sizeof(statusMessage), "Reading from File %s", fullFileName);
                setStringParam(ADStatusMessage, statusMessage);
                setStringParam(NDFullFileName, fullFileName);
                callParamCallbacks();
                status = readRaw(fullFileName, &startTime, acquireTime + readFileTimeout, pImage); 
                /* If there was an error jump to bottom of loop */
                if (status) {
                    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                        "%s::%s, error calling readRaw(), status=%d\n",
                        driverName, functionName, status);
                    setIntegerParam(ADAcquire, 0);
                    pImage->release();
                    goto done;
                } 
    
                /* Put the frame number and time stamp into the buffer */
                pImage->uniqueId = imageCounter;
                pImage->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;
                updateTimeStamp(&pImage->epicsTS);
    
                /* Get any attributes that have been defined for this driver */        
                this->getAttributes(pImage->pAttributeList);
    
                /* Call the NDArray callback */
                /* Must release the lock here, or we can get into a deadlock, because we can
                 * block on the plugin lock, and the plugin can be calling us */
                this->unlock();
                asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
                     "%s:%s: calling NDArray callback\n", driverName, functionName);
                doCallbacksGenericPointer(pImage, NDArrayData, 0);
                this->lock();
                /* Free the image buffer */
                pImage->release();
            }
        }
        setIntegerParam(ADAcquire, 0);
        if (shutterMode == ADShutterModeEPICS) ADDriver::setShutter(0);
        done:
        callParamCallbacks();
    }
}

/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including ADAcquire, ADTriggerMode, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus PhotonII::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int adstatus;
    asynStatus status = asynSuccess;
    const char *functionName = "writeInt32";

    status = setIntegerParam(function, value);

    if (function == ADAcquire) {
        getIntegerParam(ADStatus, &adstatus);
        if (value && (adstatus == ADStatusIdle)) {
            /* Send an event to wake up the PhotonII task.  */
            epicsEventSignal(startEventId_);
        } 
        if (!value && (adstatus != ADStatusIdle)) {
            /* This was a command to stop acquisition */
            epicsEventSignal(stopEventId_);
        }
    }
    else if (function == NDFileNumber) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --runnumber %d", value);
        writePhotonII(PII_DEFAULT_TIMEOUT);
    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PII_PARAM) status = ADDriver::writeInt32(pasynUser, value);
    }
            
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s: error, status=%d function=%d, value=%d\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%d\n", 
              driverName, functionName, function, value);
    return status;
}

/** Called when asyn clients call pasynFloat64->write().
  * This function performs actions for some parameters, including ADAcquireTime, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus PhotonII::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "writeFloat64";

    status = setDoubleParam(function, value);

    if (function == ADAcquireTime) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --exposure-time %f", value);
        writePhotonII(PII_DEFAULT_TIMEOUT);
    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PII_PARAM) status = ADDriver::writeFloat64(pasynUser, value);
    }
            
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s: error, status=%d function=%d, value=%f\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%f\n", 
              driverName, functionName, function, value);
    return status;
}


/** Report status of the driver.
  * Prints details about the driver if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details If >0 then driver details are printed.
  */
void PhotonII::report(FILE *fp, int details)
{

    fprintf(fp, "PhotonII detector %s\n", this->portName);
    if (details > 0) {
        int nx, ny, dataType;
        getIntegerParam(ADSizeX, &nx);
        getIntegerParam(ADSizeY, &ny);
        getIntegerParam(NDDataType, &dataType);
        fprintf(fp, "  NX, NY:            %d  %d\n", nx, ny);
        fprintf(fp, "  Data type:         %d\n", dataType);
    }
    /* Invoke the base class method */
    ADDriver::report(fp, details);
}

extern "C" int PhotonIIConfig(const char *portName, const char *commandPort,
                                   int maxBuffers, size_t maxMemory,
                                   int priority, int stackSize)
{
    new PhotonII(portName, commandPort, maxBuffers, maxMemory, priority, stackSize);
    return(asynSuccess);
}


/* Code for iocsh registration */
static const iocshArg PhotonIIConfigArg0 = {"Port name", iocshArgString};
static const iocshArg PhotonIIConfigArg1 = {"PhotonII port name", iocshArgString};
static const iocshArg PhotonIIConfigArg2 = {"maxBuffers", iocshArgInt};
static const iocshArg PhotonIIConfigArg3 = {"maxMemory", iocshArgInt};
static const iocshArg PhotonIIConfigArg4 = {"priority", iocshArgInt};
static const iocshArg PhotonIIConfigArg5 = {"stackSize", iocshArgInt};
static const iocshArg * const PhotonIIConfigArgs[] =    {&PhotonIIConfigArg0,
                                                         &PhotonIIConfigArg1,
                                                         &PhotonIIConfigArg2,
                                                         &PhotonIIConfigArg3,
                                                         &PhotonIIConfigArg4,
                                                         &PhotonIIConfigArg5};
static const iocshFuncDef configPhotonII = {"PhotonIIConfig", 6, PhotonIIConfigArgs};
static void configPhotonIICallFunc(const iocshArgBuf *args)
{
    PhotonIIConfig(args[0].sval, args[1].sval, args[2].ival,  args[3].ival,  
                   args[4].ival, args[5].ival);
}

static void PhotonIIRegister(void)
{

    iocshRegister(&configPhotonII, configPhotonIICallFunc);
}

extern "C" {
epicsExportRegistrar(PhotonIIRegister);
}

