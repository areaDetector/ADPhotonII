/* PhotonII.cpp
 *
 * This is a driver for Bruker Instrument Service (PhotonII) detectors.
 *
 * Original Author: Jeff Gebhardt
 * Current Author:  Mark Rivers
 *         University of Chicago
 *
 * Created:  March 18, 2010
 *
 * Derived from pilatusDetector.cpp
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created:  June 11, 2008
 */
 
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

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

#include "ADDriver.h"

#include <epicsExport.h>

/** Frame type choices */
typedef enum {
    PhotonIIFrameNormal,
    PhotonIIFrameDark,
    PhotonIIFrameRaw,
    PhotonIIFrameDoubleCorrelation
} PhotonIIFrameType_t;

/** Messages to/from p2util */
#define MAX_MESSAGE_SIZE 512 
#define MAX_FILENAME_LEN 256
#define PHOTONII_SIZEX 768
#define PHOTONII_SIZEY 1024
/** Time to poll when reading from p2util */
#define ASYN_POLL_TIME .01 
#define PHOTONII_POLL_DELAY .01 
#define PHOTONII_DEFAULT_TIMEOUT 1.0 
/** Time between checking to see if .SFRM file is complete */
#define FILE_READ_DELAY .01

#define PhotonIIFileTimeoutString  "PHOTONII_FILE_TIMEOUT"
#define PhotonIINumDarksString     "PHOTONII_NUM_DARKS"
#define PhotonIIStatusString       "PHOTONII_STATUS"


static const char *driverName = "PhotonII";

/** Driver for Bruker Photon II detector using their p2util server over TCP/IP socket */
class PhotonII : public ADDriver {
public:
    PhotonII(const char *portName, const char *PhotonIICommandPort, 
                    int maxBuffers, size_t maxMemory,
                    int priority, int stackSize);
                 
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual void setShutter(int open);
    void report(FILE *fp, int details);
    /* These are new methods */
    void PhotonIITask();  /* This should be private but is called from C so must be public */
    void statusTask(); /* This should be private but is called from C so must be public */
    epicsEventId stopEventId_;   /**< This should be private but is accessed from C, must be public */
 
 protected:
    int PhotonIIFileTimeout;
#define FIRST_PHOTONII_PARAM PhotonIIFileTimeout
    int PhotonIINumDarks;
    int PhotonIIStatus;

 private:                                       
    /* These are the methods that are new to this class */
    asynStatus readRaw(const char *fileName, epicsTimeStamp *pStartTime, double timeout, NDArray *pImage);
    asynStatus writePhotonII(double timeout);
       
    /* Our data */
    epicsEventId startEventId_;
    epicsEventId readoutEventId_;
    epicsTimerId timerId_;
    char toPhotonII_[MAX_MESSAGE_SIZE];
    char fromPhotonII_[MAX_MESSAGE_SIZE];
    asynUser *pasynUserCommand_;
    int detSizeX_;
    int detSizeY_;
};

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
               priority, stackSize)
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
    status |= setIntegerParam(ADMaxSizeX, PHOTONII_SIZEX);
    status |= setIntegerParam(ADMaxSizeY, PHOTONII_SIZEY);
    status |= setIntegerParam(ADSizeX, PHOTONII_SIZEX);
    status |= setIntegerParam(ADSizeY, PHOTONII_SIZEY);
    status |= setIntegerParam(NDArraySizeX, 0);
    status |= setIntegerParam(NDArraySizeY, 0);
    status |= setIntegerParam(NDArraySize, 0);
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
    status = (epicsThreadCreate("PhotonIIStatusTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)statusTaskC,
                                this) == NULL);
    if (status) {
        printf("%s:%s epicsThreadCreate failure for status task\n", 
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

void PhotonII::setShutter(int open)
{
    ADShutterMode_t shutterMode;
    int itemp;
    double delay;
    double shutterOpenDelay, shutterCloseDelay;
    
    getIntegerParam(ADShutterMode, &itemp); shutterMode = (ADShutterMode_t)itemp;
    getDoubleParam(ADShutterOpenDelay, &shutterOpenDelay);
    getDoubleParam(ADShutterCloseDelay, &shutterCloseDelay);

    switch (shutterMode) {
        case ADShutterModeDetector:
            if (open) {
                /* Open the shutter */
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "[Shutter /Status=1]");
                writePhotonII(2.0);
                /* This delay is to get the exposure time correct.  
                * It is equal to the opening time of the shutter minus the
                * closing time.  If they are equal then no delay is needed, 
                * except use 1msec so delay is not negative and commands are 
                * not back-to-back */
                delay = shutterOpenDelay - shutterCloseDelay;
                if (delay < .001) delay=.001;
                epicsThreadSleep(delay);
            } else {
                /* Close shutter */
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "[Shutter /Status=0]");
                writePhotonII(2.0);
                epicsThreadSleep(shutterCloseDelay);
            }
            callParamCallbacks();
            break;
        default:
            ADDriver::setShutter(open);
            break;
    }
}

/** This thread reads status strings from the status socket, makes the string available to EPICS, and
  * sends an eveny when it detects acquisition complete, etc. */
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
    char fullFileName[MAX_FILENAME_LEN];
    char statusMessage[MAX_MESSAGE_SIZE];
    size_t dims[2];
    int itemp;
    int arrayCallbacks;
    
    this->lock();

    /* Loop forever */
    while (1) {
        /* Is acquisition active? */
        getIntegerParam(ADAcquire, &acquire);
        
        /* If we are not acquiring then wait for a semaphore that is given when acquisition is started */
        if (!acquire) {
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
        }
        
        /* Get current values of some parameters */
        getIntegerParam(ADFrameType, &frameType);
        /* Get the exposure parameters */
        getDoubleParam(ADAcquireTime, &acquireTime);
        getIntegerParam(ADShutterMode, &itemp);  shutterMode = (ADShutterMode_t)itemp;
        getDoubleParam(PhotonIIFileTimeout, &readFileTimeout);
        
        setIntegerParam(ADStatus, ADStatusAcquire);

        /* Create the full filename */
        createFileName(sizeof(fullFileName), fullFileName);
        
        setStringParam(ADStatusMessage, "Starting exposure");
        /* Call the callbacks to update any changes */
        setStringParam(NDFullFileName, fullFileName);
        callParamCallbacks();
        switch (frameType) {
            case PhotonIIFrameNormal:
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "[Scan /Filename=%s /scantime=%f /Rescan=0]", fullFileName, acquireTime);
                break;
            case PhotonIIFrameDark:
                getIntegerParam(PhotonIINumDarks, &numDarks);
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "[Dark /AddTime=%f /Repetitions=%d]", acquireTime, numDarks);
                break;
            case PhotonIIFrameRaw:
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "[Scan /Filename=%s /scantime=%f /Rescan=0 /DarkFlood=0]", fullFileName, acquireTime);
                break;
            case PhotonIIFrameDoubleCorrelation:
                epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                    "[Scan /Filename=%s /scantime=%f /Rescan=1]", fullFileName, acquireTime);
                break;
        }
        /* Send the acquire command to PhotonII */
        writePhotonII(2.0);

        setStringParam(ADStatusMessage, "Waiting for Acquisition");
        callParamCallbacks();
        /* Set the the start time for the TimeRemaining counter */
        epicsTimeGetCurrent(&startTime);
        timeRemaining = acquireTime;

        /* PhotonII will control the shutter if we are using the hardware shutter signal.
         * If we are using the EPICS shutter then tell it to open */
        if (shutterMode == ADShutterModeEPICS) ADDriver::setShutter(1);

        /* Wait for the exposure time using epicsEventWaitWithTimeout, 
         * so we can abort. */
        epicsTimerStartDelay(timerId_, acquireTime);
        while(1) {
            this->unlock();
            status = epicsEventWaitWithTimeout(stopEventId_, PHOTONII_POLL_DELAY);
            this->lock();
            if (status == epicsEventWaitOK) {
                /* The acquisition was stopped before the time was complete */
                epicsTimerCancel(timerId_);
                break;
            }
            epicsTimeGetCurrent(&currentTime);
            timeRemaining = acquireTime -  epicsTimeDiffInSeconds(&currentTime, &startTime);
            if (timeRemaining < 0.) timeRemaining = 0.;
            setDoubleParam(ADTimeRemaining, timeRemaining);
            callParamCallbacks();
        }
        if (shutterMode == ADShutterModeEPICS) ADDriver::setShutter(0);
        setDoubleParam(ADTimeRemaining, 0.0);
        callParamCallbacks();
        this->unlock();
        status = epicsEventWaitWithTimeout(readoutEventId_, 5.0);
        this->lock();
        /* If there was an error jump to bottom of loop */
        if (status != epicsEventWaitOK) {
            setIntegerParam(ADAcquire, 0);
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: error waiting for readout to complete\n",
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
            epicsSnprintf(statusMessage, sizeof(statusMessage), "Reading from File %s", fullFileName);
            setStringParam(ADStatusMessage, statusMessage);
            callParamCallbacks();
            status = readRaw(fullFileName, &startTime, acquireTime + readFileTimeout, pImage); 
            /* If there was an error jump to bottom of loop */
            if (status) {
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
        getIntegerParam(ADImageMode, &imageMode);
        if (imageMode == ADImageMultiple) {
            getIntegerParam(ADNumImages, &numImages);
            if (numImagesCounter >= numImages) setIntegerParam(ADAcquire, 0);
        }    
        if (imageMode == ADImageSingle) setIntegerParam(ADAcquire, 0);
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
    int maxSizeX;
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
    else if (function == ADBinX) {
        getIntegerParam(ADMaxSizeX, &maxSizeX);
        if ((value == 1) || (value == 2) || (value == 4) || (value == 8)) {
            /* There is only 1 binning, set X and Y the same */
            setIntegerParam(ADBinY, value);
            epicsSnprintf(toPhotonII_, sizeof(toPhotonII_), 
                "[ChangeFrameSize /FrameSize=%d]", maxSizeX/value);
            writePhotonII(PHOTONII_DEFAULT_TIMEOUT);
        } else {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: invalid binning=%d, must be 1,2,4 or 8\n",
                driverName, functionName, value);
            status = asynError;
        } 
    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PHOTONII_PARAM) status = ADDriver::writeInt32(pasynUser, value);
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

