/* PhotonII.h
 * This is a driver for Bruker Instrument Service (PhotonII) detectors.
 *
 * Author:  Mark Rivers
 *          University of Chicago
 *
 * Created:  June 11, 2017
 *
 */

#include <epicsEvent.h>
#include <epicsTimer.h>

#include <ADDriver.h>

#define DRIVER_VERSION      1
#define DRIVER_REVISION     0
#define DRIVER_MODIFICATION 0

#define MAX_MESSAGE_SIZE 512 

#define PhotonIINumDarksString     "PII_NUM_DARKS"
#define PhotonIIStatusString       "PII_STATUS"


/** Driver for Bruker Photon II detector using their p2util server over TCP/IP socket */
class PhotonII : public ADDriver {
public:
    PhotonII(const char *portName, const char *PhotonIICommandPort, 
                    int maxBuffers, size_t maxMemory,
                    int priority, int stackSize);
                 
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);
    void report(FILE *fp, int details);
    /* These are new methods.  These should be private but are called from C so must be public */
    void PhotonIITask();  
    asynStatus p2util(const char* command);
    epicsEventId stopEventId_;   /**< This should be private but is accessed from C, must be public */
 
 protected:
    int PhotonIINumDarks;
#define FIRST_PII_PARAM PhotonIINumDarks

 private:                                       
    /* These are the methods that are new to this class */
    asynStatus readRaw(const char *fileName, epicsTimeStamp *pStartTime, double timeout, NDArray *pImage);
    asynStatus writePhotonII(double timeout);
    asynStatus readPhotonII(double timeout);
       
    /* Our data */
    epicsEventId startEventId_;
    char toPhotonII_[MAX_MESSAGE_SIZE];
    char fromPhotonII_[MAX_MESSAGE_SIZE];
    asynUser *pasynUserCommand_;
    int detSizeX_;
    int detSizeY_;
};

