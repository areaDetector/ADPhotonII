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
    PII_FrameNormal,
    PII_FrameDark,
    PII_FrameADC0
} PII_FrameType_t;

typedef enum {
    PII_TriggerRising,
    PII_TriggerFalling
} PII_TriggerEdge_t;

typedef enum {
    PII_TriggerStep,
    PII_TriggerContinuous
} PII_TriggerMode_t;

#define PII_SIZEX 768
#define PII_SIZEY 1024
#define PII_COMMAND_TIMEOUT 1.0
#define PII_MAX_FILENAME_LEN 256
/** Time between checking to see if raw file is complete */
#define PII_FILE_READ_DELAY .01
#define PII_FILE_READ_TIMEOUT 3.0 

static const char *driverName = "PhotonII";

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
    char versionString[20];
    const char *functionName = "PhotonII";

    createParam(PII_DRSumEnableString,     asynParamInt32,   &PII_DRSumEnable);
    createParam(PII_NumDarksString,        asynParamInt32,   &PII_NumDarks);
    createParam(PII_TriggerTypeString,     asynParamInt32,   &PII_TriggerType);
    createParam(PII_TriggerEdgeString,     asynParamInt32,   &PII_TriggerEdge);
    createParam(PII_NumSubFramesString,    asynParamInt32,   &PII_NumSubFrames);

    epicsSnprintf(versionString, sizeof(versionString), "%d.%d.%d", 
                  DRIVER_VERSION, DRIVER_REVISION, DRIVER_MODIFICATION);
    setStringParam(NDDriverVersion, versionString);

    /* Create the epicsEvents for signaling to the PhotonII task when acquisition starts and stops */
    startEventId_ = epicsEventMustCreate(epicsEventEmpty);
    stopEventId_ = epicsEventMustCreate(epicsEventEmpty);
     
    /* Connect to p2util running in procServ */
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
        printf("%s::%s epicsThreadCreate failure for image task\n", 
            driverName, functionName);
        return;
    }

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
    size_t expectedSize = detSizeX_ * detSizeY_ * sizeof(epicsInt32);
    epicsUInt32 *pData = (epicsUInt32 *)pImage->pData;
    static const char *functionName = "readRaw";
        
    deltaTime = 0.;
    epicsTimeToTime_t(&acqStartTime, pStartTime);
    epicsTimeGetCurrent(&tStart);
    
    while (deltaTime <= timeout) {
        file = fopen(fileName, "rb");
        if (file) {
            fileExists = 1;
            /* The file exists.  Make sure it is a new file, not an old one and that it is the correct size */
            status = stat(fileName, &statBuff);
            if (status){
                asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s error calling fstat, errno=%d %s\n",
                    driverName, functionName, errno, fileName);
                fclose(file);
                return(asynError);
            }
            if (((size_t)statBuff.st_size == expectedSize) &&
                (difftime(statBuff.st_mtime, acqStartTime) >= 0)) {
                break;
            }
            fclose(file);
            file = NULL;
        }
        /* Sleep, but check for stop event, which can be used to abort a long acquisition */
        unlock();
        status = epicsEventWaitWithTimeout(stopEventId_, PII_FILE_READ_DELAY);
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
                "  file exists but is either old or the wrong size\n");
        } 
        return(asynError);
    }
    nBytes = fread(pData, 1, expectedSize, file);
    if (nBytes != expectedSize) {
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
                    "%s::%s, status=%d, sent\n%s\n",
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
                                                                                 
    if (status && status != asynTimeout) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s, status=%d\n",
            driverName, functionName, status);
    }

    /* Set output string so it can get back to EPICS */
    setStringParam(ADStringFromServer, fromPhotonII_);
    
    return(status);
}


/** This thread controls acquisition, reads SFRM files to get the image data, and
  * does the callbacks to send it to higher layers */
void PhotonII::PhotonIITask()
{
    int status = asynSuccess;
    int imageCounter;
        int numImages, numImagesCounter;
    int imageMode;
    NDArray *pImage;
    double acquireTime;
    double expectedTime;
    double timeRemaining;
    ADShutterMode_t shutterMode;
    int frameType;
    int numDarks;
    epicsTimeStamp startTime;
    const char *functionName = "PhotonIITask";
    std::string fileName;
    std::string filePath;
    int fileNumber;
    char fullFileName[PII_MAX_FILENAME_LEN];
    char statusMessage[PII_MAX_MESSAGE_SIZE];
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
            "%s::%s: waiting for acquire to start\n", driverName, functionName);
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
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --runnumber %d", fileNumber);
        writePhotonII(PII_COMMAND_TIMEOUT);
        getStringParam(NDFilePath, filePath);
        getStringParam(NDFileName, fileName);
        createFileName(sizeof(fullFileName), fullFileName);
        
        setIntegerParam(ADStatus, ADStatusAcquire);

        setStringParam(ADStatusMessage, "Starting exposure");
        /* Call the callbacks to update any changes */
        callParamCallbacks();
        switch (frameType) {
            case PII_FrameNormal:
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "grab --dstdir %s --basename %s --count %d", filePath.c_str(), fileName.c_str(), numImages);
                break;
            case PII_FrameDark:
                getIntegerParam(PII_NumDarks, &numDarks);
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "grab --darkframe --dstdir %s --basename %s --count %d", filePath.c_str(), fileName.c_str(), numDarks);
                break;
        }
        /* Send the acquire command to PhotonII */
        writePhotonII(PII_COMMAND_TIMEOUT);

        setStringParam(ADStatusMessage, "Waiting for Acquisition");
        callParamCallbacks();
        epicsTimeGetCurrent(&startTime);
        expectedTime = numImages * acquireTime;

        /* If we are using the EPICS shutter then tell it to open */
        if (shutterMode == ADShutterModeEPICS) ADDriver::setShutter(1);

        /* Wait for the frames to be written  */
        for (i=0; i<numImages; i++) {
            epicsTimeStamp pollStart, pollCheck;
            double deltaTime;
            // Read from p2util until we get a line containing the string "bytes to"
            // Poll so we can abort
            epicsTimeGetCurrent(&pollStart);
            while(1) {
                // Update the remaining time
                epicsTimeGetCurrent(&pollCheck);
                timeRemaining = expectedTime - epicsTimeDiffInSeconds(&pollCheck, &startTime);
                setDoubleParam(ADTimeRemaining, timeRemaining);
                callParamCallbacks();
                this->unlock();
                status = readPhotonII(PII_FILE_READ_DELAY);
                this->lock();
                if (status == asynSuccess) {
                    pBuff = strstr(fromPhotonII_, "bytes to ");
                    if (pBuff) break;
                } else if (status == asynTimeout) {
                    continue;
                } else {
                    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                        "%s::%s: error reading file written message status=%d\n",
                        driverName, functionName, status);
                    goto done;
                }
                /* Sleep, but check for stop event, which can be used to abort a long acquisition */
                unlock();
                status = epicsEventWaitWithTimeout(stopEventId_, PII_FILE_READ_DELAY);
                lock();
                if (status == epicsEventWaitOK) {
                    goto done;
                }
                epicsTimeGetCurrent(&pollCheck);
                deltaTime = epicsTimeDiffInSeconds(&pollCheck, &pollStart);
                if (deltaTime > acquireTime + PII_FILE_READ_TIMEOUT) {
                    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                        "%s::%s: timeout waiting for file written message\n",
                        driverName, functionName);
                    goto done;
                    
                }
            }
            strcpy(fullFileName, pBuff + strlen("bytes to "));
            getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
            getIntegerParam(NDArrayCounter, &imageCounter);
            imageCounter++;
            setIntegerParam(NDArrayCounter, imageCounter);
            getIntegerParam(ADNumImagesCounter, &numImagesCounter);
            numImagesCounter++;
            setIntegerParam(ADNumImagesCounter, numImagesCounter);
            callParamCallbacks();
    
            if (arrayCallbacks && frameType != PII_FrameDark) {
                /* Get an image buffer from the pool */
                getIntegerParam(ADSizeX, &itemp); dims[0] = itemp;
                getIntegerParam(ADSizeY, &itemp); dims[1] = itemp;
                pImage = this->pNDArrayPool->alloc(2, dims, NDInt32, 0, NULL);
                epicsSnprintf(statusMessage, sizeof(statusMessage), "Reading from File %s", fullFileName);
                setStringParam(ADStatusMessage, statusMessage);
                setStringParam(NDFullFileName, fullFileName);
                callParamCallbacks();
                status = readRaw(fullFileName, &startTime, acquireTime + PII_FILE_READ_TIMEOUT, pImage); 
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
                asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
                     "%s::%s: calling NDArray callback\n", driverName, functionName);
                doCallbacksGenericPointer(pImage, NDArrayData, 0);
                /* Free the image buffer */
                pImage->release();
            }
        }
        done:
        setIntegerParam(ADAcquire, 0);
        setDoubleParam(ADTimeRemaining, 0);
        if (shutterMode == ADShutterModeEPICS) ADDriver::setShutter(0);
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
    int acquiring;
    asynStatus status = asynSuccess;
    const char *functionName = "writeInt32";

    // Get the current acquiring status before setting value in parameter library
    getIntegerParam(ADAcquire, &acquiring);
    status = setIntegerParam(function, value);

    if (function == ADAcquire) {
        if (value && (!acquiring)) {
            /* Send an event to wake up the PhotonII task.  */
            epicsEventSignal(startEventId_);
        } 
        if (!value && acquiring) {
            /* This was a command to stop acquisition */
            epicsEventSignal(stopEventId_);
            epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                "abort");
            writePhotonII(PII_COMMAND_TIMEOUT);
        }
    
    } else if (function == PII_DRSumEnable) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --dr-summation %d", value);
        writePhotonII(PII_COMMAND_TIMEOUT);
      
    } else if (function == ADTriggerMode) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --frame-trigger-source %s", value==0 ? "internal" : "external");
        writePhotonII(PII_COMMAND_TIMEOUT);
      
    } else if (function == PII_TriggerType) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --frame-trigger-mode %s", value==0 ? "step" : "continuous");
        writePhotonII(PII_COMMAND_TIMEOUT);
      
    } else if (function == PII_TriggerEdge) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --frame-trigger-edge %s", value==0 ? "rising" : "falling");
        writePhotonII(PII_COMMAND_TIMEOUT);
      
    } else if (function == PII_NumSubFrames) {
        epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
            "set --subframes-per-frame %d", value);
        writePhotonII(PII_COMMAND_TIMEOUT);
      
    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PII_PARAM) status = ADDriver::writeInt32(pasynUser, value);
    }
            
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s::%s: error, status=%d function=%d, value=%d\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s::%s: function=%d, value=%d\n", 
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
        writePhotonII(PII_COMMAND_TIMEOUT);

    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PII_PARAM) status = ADDriver::writeFloat64(pasynUser, value);
    }
            
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s::%s: error, status=%d function=%d, value=%f\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s::%s: function=%d, value=%f\n", 
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

asynStatus PhotonII::p2util(const char *command)
{
    strncpy(toPhotonII_, command, sizeof(toPhotonII_));
    return writePhotonII(PII_COMMAND_TIMEOUT);
}

extern "C" int PhotonIIConfig(const char *portName, const char *commandPort,
                                   int maxBuffers, size_t maxMemory,
                                   int priority, int stackSize)
{
    new PhotonII(portName, commandPort, maxBuffers, maxMemory, priority, stackSize);
    return asynSuccess;
}

extern "C" int p2util(const char *portName, const char *command)
{
    PhotonII *pPII = (PhotonII *)findAsynPortDriver(portName);
    return pPII->p2util(command);
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

static const iocshArg PhotonIIUtilArg0 = {"Port name", iocshArgString};
static const iocshArg PhotonIIUtilArg1 = {"command", iocshArgString};
static const iocshArg * const PhotonIIUtilArgs[] =    {&PhotonIIUtilArg0,
                                                       &PhotonIIUtilArg1};
static const iocshFuncDef PhotonIIUtil = {"p2util", 2, PhotonIIUtilArgs};
static void PhotonIIUtilCallFunc(const iocshArgBuf *args)
{
    p2util(args[0].sval, args[1].sval);
}

static void PhotonIIRegister(void)
{
    iocshRegister(&configPhotonII, configPhotonIICallFunc);
    iocshRegister(&PhotonIIUtil, PhotonIIUtilCallFunc);
}

extern "C" {
epicsExportRegistrar(PhotonIIRegister);
}
