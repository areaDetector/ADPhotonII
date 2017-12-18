< envPaths
errlogInit(20000)

dbLoadDatabase("$(TOP)/dbd/PhotonIIApp.dbd")
PhotonIIApp_registerRecordDeviceDriver(pdbbase) 

# Prefix for all records
epicsEnvSet("PREFIX", "13PII_1:")
# The port name for the detector
epicsEnvSet("PORT",   "PII")
# The queue size for all plugins
epicsEnvSet("QSIZE",  "20")
# The maximim image width; used for row profiles in the NDPluginStats plugin
epicsEnvSet("XSIZE",  "768")
# The maximim image height; used for column profiles in the NDPluginStats plugin
epicsEnvSet("YSIZE",  "1024")
# The maximum number of time series points in the NDPluginStats plugin
epicsEnvSet("NCHANS", "2048")
# The maximum number of frames buffered in the NDPluginCircularBuff plugin
epicsEnvSet("CBUFFS", "500")
# The search path for database files
epicsEnvSet("EPICS_DB_INCLUDE_PATH", "$(ADCORE)/db")

# The name of the drvAsynIPPort to communicate with p2util
epicsEnvSet("PII_SERVER",   "PIIServer")
epicsEnvSet("PII_STARTUP_SCRIPT", "/home/bruker/p2util/scripts/prep_collection.cmd")

###
# Create the asyn port to talk to the p2util procServ port 20000
drvAsynIPPortConfigure("$(PII_SERVER)","localhost:20000")
# Set the input and output terminators.
asynOctetSetInputEos("$(PII_SERVER)", 0, "\r\n")
asynOctetSetOutputEos("$(PII_SERVER)", 0, "\n")
asynSetTraceIOMask("$(PII_SERVER)",0,2)
asynSetTraceIOTruncateSize("$(PII_SERVER)",0,150)
asynSetTraceMask("$(PII_SERVER)",0,9)

PhotonIIConfig("$(PORT)", "$(PII_SERVER)", 0, 0)
p2util $(PORT) "load --commands --filename "$(PII_STARTUP_SCRIPT)
dbLoadRecords("$(ADPHOTONII)/db/PhotonII.template","P=$(PREFIX),R=cam1:,PORT=$(PORT),ADDR=0,TIMEOUT=1,PII_SERVER_PORT=$(PII_SERVER)")

# Create a standard arrays plugin
NDStdArraysConfigure("Image1", 5, 0, "$(PORT)", 0, 0)
# Make NELEMENTS in the following be a little bigger than 2048*2048
dbLoadRecords("$(ADCORE)/db/NDStdArrays.template", "P=$(PREFIX),R=image1:,PORT=Image1,ADDR=0,TIMEOUT=1,NDARRAY_PORT=$(PORT),TYPE=Int32,FTVL=LONG,NELEMENTS=786432")

# Load all other plugins using commonPlugins.cmd
< $(ADCORE)/iocBoot/commonPlugins.cmd
set_requestfile_path("$(ADPHOTONII)/PhotonIIApp/Db")

#asynSetTraceMask("$(PORT)",0,3)
#asynSetTraceIOMask("$(PORT)",0,4)

iocInit()

# save things every thirty seconds
create_monitor_set("auto_settings.req", 30,"P=$(PREFIX)")
