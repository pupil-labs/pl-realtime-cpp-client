#include "liveMedia.hh"
#include "LoggingUsageEnvironment.hh"
#include <thread>
#include <mutex>
#include "RTSPService.hh"
#include <vector>

typedef std::vector<u_int8_t>(*DataPostprocessor)(unsigned int size, const u_int8_t* unit);

// Forward function definitions:
static std::vector<u_int8_t> processNalUnit(unsigned int size, const u_int8_t* unit);

// RTSP 'response handlers':
static void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
static void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
static void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

// Other event handler functions:
static void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
static void subsessionByeHandler(void* clientData, char const* reason);
// called when a RTCP "BYE" is received for a subsession

// The main streaming routine (for each "rtsp://" URL):
static RTSPClient* openURL(UsageEnvironment& env, char const* progName, char const* rtspURL, u_int8_t id, std::atomic<char>& statusRef, RawDataCallback dataCallback, void* userData);

// Used to iterate through each stream's 'subsessions', setting up each one:
static void setupNextSubsession(RTSPClient* rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
static void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

// A function that outputs a string that identifies each stream (for debugging output).
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
	return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
	return env << subsession.mediumName() << "/" << subsession.codecName();
}

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
	StreamClientState();
	virtual ~StreamClientState();

public:
	MediaSubsessionIterator* iter;
	MediaSession* session;
	MediaSubsession* subsession;
};

class ourRTSPClient : public RTSPClient {
public:
	static ourRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL, u_int8_t id, std::atomic<char>& statusRef, RawDataCallback dataCallback, void* userData,
		int verbosityLevel = 0,
		char const* applicationName = NULL,
		portNumBits tunnelOverHTTPPortNum = 0);

protected:
	ourRTSPClient(UsageEnvironment& env, char const* rtspURL, u_int8_t id, std::atomic<char>& statusRef, RawDataCallback dataCallback, void* userData,
		int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum);
	// called only by createNew();
	virtual ~ourRTSPClient();

public:
	RawDataCallback dataCallback = NULL;
	void* userData = NULL;
	StreamClientState scs;
	const u_int8_t id = 0;
	std::atomic<char>& statusRef;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').

class CallbackSink : public MediaSink {
public:
	static CallbackSink* createNew(UsageEnvironment& env,
		MediaSubsession& subsession, // identifies the kind of data that's being received
		DataPostprocessor dataPostprocessor);

private:
	CallbackSink(UsageEnvironment& env, MediaSubsession& subsession, DataPostprocessor dataPostprocessor);
	// called only by "createNew()"
	virtual ~CallbackSink();

	static void afterGettingFrame(void* clientData, unsigned frameSize,
		unsigned numTruncatedBytes,
		struct timeval presentationTime,
		unsigned durationInMicroseconds);
	void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
		struct timeval presentationTime, unsigned durationInMicroseconds);

private:
	// redefined virtual functions:
	virtual Boolean continuePlaying();
	DataPostprocessor dataPostprocessor;

private:
	u_int8_t* fReceiveBuffer;
	MediaSubsession& fSubsession;
};

#define RTSP_MAX_WORKER_COUNT 5
#define RTSP_MAX_CLIENT_COUNT 5 //imu, world, gaze, eye events, eyes
#define RTSP_CLIENT_VERBOSITY_LEVEL 1 // by default, print verbose output from each "RTSPClient"

static RTSPClient* openURL(UsageEnvironment& env, char const* progName, char const* rtspURL, u_int8_t id, std::atomic<char>& statusRef, RawDataCallback dataCallback, void* userData) {
	// Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
	// to receive (even if more than stream uses the same "rtsp://" URL).
	ourRTSPClient* rtspClient = ourRTSPClient::createNew(env, rtspURL, id, statusRef, dataCallback, userData, RTSP_CLIENT_VERBOSITY_LEVEL, progName);
	if (rtspClient == NULL) {
		env << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << env.getResultMsg() << "\n";
		return NULL;
	}

	rtspClient->statusRef = SST_CLIENT_CREATED;

	// Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
	// Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
	// Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
	rtspClient->sendDescribeCommand(continueAfterDESCRIBE);
	return rtspClient;
}


// Implementation of the RTSP 'response handlers':

static void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
	do {
		UsageEnvironment& env = rtspClient->envir(); // alias
		ourRTSPClient* oRtspClient = (ourRTSPClient*)rtspClient;
		StreamClientState& scs = oRtspClient->scs; // alias

		if (resultCode != 0) {
			env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
			delete[] resultString;
			break;
		}

		char* const sdpDescription = resultString;
		env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

		// Create a media session object from this SDP description:
		scs.session = MediaSession::createNew(env, sdpDescription);
		delete[] sdpDescription; // because we don't need it anymore
		if (scs.session == NULL) {
			env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
			break;
		}
		else if (!scs.session->hasSubsessions()) {
			env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
			break;
		}

		oRtspClient->statusRef = SST_DESCRIBE_SUCCESS;

		// Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
		// calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
		// (Each 'subsession' will have its own data source.)
		scs.iter = new MediaSubsessionIterator(*scs.session);
		setupNextSubsession(rtspClient);
		return;
	} while (0);

	// An unrecoverable error occurred with this stream.
	shutdownStream(rtspClient);
}

// By default, we request that the server stream its data using RTP/UDP.
// If, instead, you want to request that the server stream via RTP-over-TCP, change the following to True:
#define REQUEST_STREAMING_OVER_TCP False

static void setupNextSubsession(RTSPClient* rtspClient) {
	UsageEnvironment& env = rtspClient->envir(); // alias
	ourRTSPClient* oRtspClient = (ourRTSPClient*)rtspClient;
	StreamClientState& scs = oRtspClient->scs; // alias

	scs.subsession = scs.iter->next();
	if (scs.subsession != NULL) {
		if (!scs.subsession->initiate(0)) {
			env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
			setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
		}
		else {
			env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
			if (scs.subsession->rtcpIsMuxed()) {
				env << "client port " << scs.subsession->clientPortNum();
			}
			else {
				env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum() + 1;
			}
			env << ")\n";

			// Continue setting up this subsession, by sending a RTSP "SETUP" command:
			rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
		}
		return;
	}

	oRtspClient->statusRef = SST_SETUP_SUCCESS;

	// We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
	if (scs.session->absStartTime() != NULL) {
		// Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
		rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
	}
	else {
		rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
	}
}

static void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
	do {
		UsageEnvironment& env = rtspClient->envir(); // alias
		ourRTSPClient* oRtspClient = (ourRTSPClient*)rtspClient;
		StreamClientState& scs = oRtspClient->scs; // alias

		if (resultCode != 0) {
			env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
			break;
		}

		env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
		if (scs.subsession->rtcpIsMuxed()) {
			env << "client port " << scs.subsession->clientPortNum();
		}
		else {
			env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum() + 1;
		}
		env << ")\n";

		// Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
		// (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
		// after we've sent a RTSP "PLAY" command.)

		DataPostprocessor dataPostprocessor = NULL;
		const unsigned char payloadFormat = scs.subsession->rtpPayloadFormat();
		switch (payloadFormat)
		{
		case(PF_AUDIO):
		case(PF_IMU):
		case(PF_GAZE):
		case(PF_EYE_EVENTS):
			break;
		case(PF_VIDEO):
		{
			std::string codecName = scs.subsession->codecName();
			if (codecName != "H264") {
				continue;
			}

			//send SPS and PPS first
			unsigned int n;
			SPropRecord* record = parseSPropParameterSets(scs.subsession->fmtp_spropparametersets(), n);
			for (size_t i = 0; i < n; i++)
			{
				std::vector<u_int8_t> processed = processNalUnit(record[i].sPropLength, record[i].sPropBytes);
				oRtspClient->dataCallback(0, false, oRtspClient->id, payloadFormat, processed.size(), processed.data(), oRtspClient->userData);
			}
			delete[] record;
			dataPostprocessor = processNalUnit;
			break;
		}
		default: //not supported
			//env << "Unsupported payload format: " << scs.subsession->rtpPayloadFormat() << " with codec name: " << scs.subsession->codecName() << "\n";
			continue;
		}

		scs.subsession->sink = CallbackSink::createNew(env, *scs.subsession, dataPostprocessor);
		if (scs.subsession->sink == NULL) {
			env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
				<< "\" subsession: " << env.getResultMsg() << "\n";
			break;
		}

		env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
		scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession 
		scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
			subsessionAfterPlaying, scs.subsession);
		// Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
		if (scs.subsession->rtcpInstance() != NULL) {
			scs.subsession->rtcpInstance()->setByeWithReasonHandler(subsessionByeHandler, scs.subsession);
		}
	} while (0);
	delete[] resultString;

	// Set up the next subsession, if any:
	setupNextSubsession(rtspClient);
}

static void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
	Boolean success = False;

	do {
		UsageEnvironment& env = rtspClient->envir(); // alias
		ourRTSPClient* oRtspClient = (ourRTSPClient*)rtspClient;

		if (resultCode != 0) {
			env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
			break;
		}

		env << *rtspClient << "Started playing session\n";

		oRtspClient->statusRef = SST_PLAY_SUCCESS;
		success = True;
	} while (0);
	delete[] resultString;

	if (!success) {
		// An unrecoverable error occurred with this stream.
		shutdownStream(rtspClient);
	}
}


// Implementation of the other event handlers:

static void subsessionAfterPlaying(void* clientData) {
	MediaSubsession* subsession = (MediaSubsession*)clientData;
	RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

	// Begin by closing this subsession's stream:
	Medium::close(subsession->sink);
	subsession->sink = NULL;

	// Next, check whether *all* subsessions' streams have now been closed:
	MediaSession& session = subsession->parentSession();
	MediaSubsessionIterator iter(session);
	while ((subsession = iter.next()) != NULL) {
		if (subsession->sink != NULL) return; // this subsession is still active
	}

	// All subsessions' streams have now been closed, so shutdown the client:
	shutdownStream(rtspClient);
}

static void subsessionByeHandler(void* clientData, char const* reason) {
	MediaSubsession* subsession = (MediaSubsession*)clientData;
	RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
	UsageEnvironment& env = rtspClient->envir(); // alias

	env << *rtspClient << "Received RTCP \"BYE\"";
	if (reason != NULL) {
		env << " (reason:\"" << reason << "\")";
		delete[](char*)reason;
	}
	env << " on \"" << *subsession << "\" subsession\n";

	// Now act as if the subsession had closed:
	subsessionAfterPlaying(subsession);
}

static void shutdownStream(RTSPClient* rtspClient, int exitCode) {
	UsageEnvironment& env = rtspClient->envir(); // alias
	ourRTSPClient* oRtspClient = (ourRTSPClient*)rtspClient;
	StreamClientState& scs = oRtspClient->scs; // alias

	// First, check whether any subsessions have still to be closed:
	if (scs.session != NULL) {
		Boolean someSubsessionsWereActive = False;
		MediaSubsessionIterator iter(*scs.session);
		MediaSubsession* subsession;

		while ((subsession = iter.next()) != NULL) {
			if (subsession->sink != NULL) {
				Medium::close(subsession->sink);
				subsession->sink = NULL;

				if (subsession->rtcpInstance() != NULL) {
					subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
				}

				someSubsessionsWereActive = True;
			}
		}

		if (someSubsessionsWereActive) {
			// Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
			// Don't bother handling the response to the "TEARDOWN".
			rtspClient->sendTeardownCommand(*scs.session, NULL);
		}
	}

	oRtspClient->statusRef = SST_SHUTDOWN;

	env << *rtspClient << "Closing the stream.\n";
	Medium::close(rtspClient);
	// Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.
}


// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL, u_int8_t id, std::atomic<char>& statusRef, RawDataCallback dataCallback, void* userData,
	int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
	return new ourRTSPClient(env, rtspURL, id, statusRef, dataCallback, userData, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL, u_int8_t id, std::atomic<char>& statusRef, RawDataCallback dataCallback, void* userData,
	int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
	: RTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1), id(id), statusRef(statusRef) {
	this->dataCallback = dataCallback;
	this->userData = userData;
}

ourRTSPClient::~ourRTSPClient() {
}


// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
	: iter(NULL), session(NULL), subsession(NULL) {
}

StreamClientState::~StreamClientState() {
	delete iter;
	if (session != NULL) {
		Medium::close(session);
	}
}


// Implementation of "CallbackSink":

// Define the size of the buffer that we'll use:
#define CALLBACK_SINK_RECEIVE_BUFFER_SIZE 200000

CallbackSink* CallbackSink::createNew(UsageEnvironment& env, MediaSubsession& subsession, DataPostprocessor dataPostprocessor) {
	return new CallbackSink(env, subsession, dataPostprocessor);
}

CallbackSink::CallbackSink(UsageEnvironment& env, MediaSubsession& subsession, DataPostprocessor dataPostprocessor)
	: MediaSink(env),
	fSubsession(subsession) {
	fReceiveBuffer = new u_int8_t[CALLBACK_SINK_RECEIVE_BUFFER_SIZE];
	this->dataPostprocessor = dataPostprocessor;
}

CallbackSink::~CallbackSink() {
	delete[] fReceiveBuffer;
}

void CallbackSink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
	struct timeval presentationTime, unsigned durationInMicroseconds) {
	CallbackSink* sink = (CallbackSink*)clientData;
	sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void CallbackSink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
	struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
	if (dataPostprocessor != NULL) {
		std::vector<u_int8_t> processed = dataPostprocessor(frameSize, fReceiveBuffer);
		frameSize = processed.size();
		std::copy(processed.begin(), processed.end(), fReceiveBuffer);
	}
	ourRTSPClient* oRtspClient = (ourRTSPClient*)fSubsession.miscPtr;
	oRtspClient->dataCallback(presentationTime.tv_sec * 1000ll + presentationTime.tv_usec / 1000, fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP(), oRtspClient->id, fSubsession.rtpPayloadFormat(), frameSize, fReceiveBuffer, oRtspClient->userData);

	// Then continue, to request the next frame of data:
	continuePlaying();
}

Boolean CallbackSink::continuePlaying() {
	if (fSource == NULL) return False; // sanity check (should not happen)

	// Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
	fSource->getNextFrame(fReceiveBuffer, CALLBACK_SINK_RECEIVE_BUFFER_SIZE,
		afterGettingFrame, this,
		onSourceClosure, this);
	return True;
}

class RTSPWorker
{
public:
	~RTSPWorker();
	int Start(const char* baseUrl, u_int8_t streamMask, LogCallback logCallback, RawDataCallback dataCallback, void* userData);
	void Stop();
	std::atomic<char> clientStatus[RTSP_MAX_CLIENT_COUNT] = { SST_UNINITIALIZED };

private:
	std::string baseUrl; //rtsp://192.168.1.27:8086
	RawDataCallback dataCallback = NULL;
	TaskScheduler* scheduler = NULL;
	UsageEnvironment* env = NULL;
	void* userData = NULL;
	EventLoopWatchVariable watchVariable = 0;
	std::thread workerThread;
	u_int8_t streamMask = 0;
	bool isIdle = true;

	void DoWork();
};

RTSPWorker::~RTSPWorker()
{
	Stop();
}

int RTSPWorker::Start(const char* baseUrl, u_int8_t streamMask, LogCallback logCallback, RawDataCallback dataCallback, void* userData)
{
	if (isIdle == false) {
		return -1;
	}
	this->baseUrl = std::string(baseUrl);
	this->streamMask = streamMask;
	this->dataCallback = dataCallback;
	this->userData = userData;
	watchVariable = 0;
	scheduler = BasicTaskScheduler::createNew();
	env = LoggingUsageEnvironment::createNew(*scheduler, logCallback, userData);
	workerThread = std::thread(&RTSPWorker::DoWork, this);
	return 0;
}

void RTSPWorker::Stop()
{
	watchVariable = 1;
	if (workerThread.joinable())
	{
		workerThread.join();
	}
	if (env != NULL) {
		env->reclaim();
		env = NULL;
	}
	if (scheduler != NULL) {
		delete scheduler;
		scheduler = NULL;
	}
	isIdle = true;
}

void RTSPWorker::DoWork()
{
	char appName[128];
	const char* urlParameters[RTSP_MAX_CLIENT_COUNT] = { "/?camera=imu", "/?camera=world&audioenable=on", "/?camera=gaze", "/?camera=eye_events", "/?camera=eyes" };
	RTSPClient* clients[RTSP_MAX_CLIENT_COUNT] = { NULL };
	for (u_int8_t i = 0; i < RTSP_MAX_CLIENT_COUNT; i++)
	{
		if ((streamMask >> i) & 1) {
			appName[0] = snprintf(appName, sizeof(appName), "RTSPClient_%d", i);
			clients[i] = openURL(*env, appName, (baseUrl + urlParameters[i]).c_str(), i, clientStatus[i], dataCallback, userData);
		}
	}

	env->taskScheduler().doEventLoop(&watchVariable);

	for (u_int8_t i = 0; i < RTSP_MAX_CLIENT_COUNT; i++)
	{
		char status = clientStatus[i];
		if (status != SST_UNINITIALIZED && status != SST_SHUTDOWN) {
			shutdownStream(clients[i]);
		}
		clients[i] = NULL;
	}
}

class RTSPClientService
{
public:
	~RTSPClientService();
	short AcquireWorker();
	int StartWorker(u_int8_t id, const char* baseUrl, u_int8_t streamMask, LogCallback logCallback, RawDataCallback dataCallback, void* userData);
	void StopWorker(u_int8_t id, bool release);
	void Stop();

private:
	RTSPWorker workers[RTSP_MAX_WORKER_COUNT];
	bool acquired[RTSP_MAX_WORKER_COUNT];
};

RTSPClientService::~RTSPClientService()
{
	Stop();
}

short RTSPClientService::AcquireWorker() {
	for (u_int8_t i = 0; i < RTSP_MAX_WORKER_COUNT; i++)
	{
		if (acquired[i] == false) {
			acquired[i] = true;
			return i;
		}
	}
	return -1;
}

int RTSPClientService::StartWorker(u_int8_t id, const char* baseUrl, u_int8_t streamMask, LogCallback logCallback, RawDataCallback dataCallback, void* userData)
{
	return workers[id].Start(baseUrl, streamMask, logCallback, dataCallback, userData);
}

void RTSPClientService::StopWorker(u_int8_t id, bool release)
{
	workers[id].Stop();
	if (release) {
		acquired[id] = false;
	}
}

void RTSPClientService::Stop()
{
	for (u_int8_t i = 0; i < RTSP_MAX_WORKER_COUNT; i++)
	{
		StopWorker(i, true);
	}
}

static bool sysIsLittleEndian() {
	u_int16_t number = 1; //0x0001
	u_int8_t* firstByte = (u_int8_t*)&number;
	return firstByte[0] == 1;
}

template<typename T>
T convertBytes(const u_int8_t* bytes, bool srcIsLittleEndian) {
	T result;
	const int nbytes = sizeof(T);
	u_int8_t reorderedBytes[nbytes] = { 0 };

	if (srcIsLittleEndian ^ sysIsLittleEndian()) {
		for (unsigned int i = 0; i < nbytes; i++) {
			reorderedBytes[i] = bytes[nbytes - i - 1];
		}
	}
	else {
		std::memcpy(reorderedBytes, bytes, nbytes);
	}

	std::memcpy(&result, reorderedBytes, nbytes);
	return result;
}

template<typename T>
unsigned int bytesToArray(T* dest, const u_int8_t* src, unsigned int startSrc, unsigned int count, bool srcIsLittleEndian) {
	unsigned int currentPos = startSrc;
	for (unsigned int i = 0; i < count; i++)
	{
		if (dest != NULL) {
			dest[i] = convertBytes<T>(&src[currentPos], srcIsLittleEndian);
		}
		currentPos += sizeof(T);
	}
	return currentPos;
}

static unsigned int bytesToFloats(float* dest, const u_int8_t* src, unsigned int startSrc, unsigned int count, bool srcIsLittleEndian = false) {
	static_assert(sizeof(float) == 4, "This code requires float to be 4 byte");
	return bytesToArray<float>(dest, src, startSrc, count, srcIsLittleEndian);
}

static unsigned int bytesToBooleans(bool* dest, const u_int8_t* src, unsigned int startSrc, unsigned int count) {
	static_assert(sizeof(bool) == 1, "This code requires bool to be 1 byte");
	return bytesToArray<bool>(dest, src, startSrc, count, false);
}

static unsigned int bytesToInts(int* dest, const u_int8_t* src, unsigned int startSrc, unsigned int count, bool srcIsLittleEndian = false) {
	static_assert(sizeof(int) == 4, "This code requires int to be 4 bytes");
	return bytesToArray<int>(dest, src, startSrc, count, srcIsLittleEndian);
}

static unsigned int bytesToLongLongs(long long* dest, const u_int8_t* src, unsigned int startSrc, unsigned int count, bool srcIsLittleEndian = false) {
	static_assert(sizeof(long long) == 8, "This code requires long long to be 8 bytes");
	return bytesToArray<long long>(dest, src, startSrc, count, srcIsLittleEndian);
}

static unsigned int bytesToVarint64s(unsigned long long* dest, const u_int8_t* src, unsigned int startSrc, unsigned int count) {
	static_assert(sizeof(long long) == 8, "This code requires long long to be 8 bytes");
	unsigned int currentPos = startSrc;
	for (unsigned int i = 0; i < count; i++)
	{
		unsigned long long result = 0;
		unsigned int shift = 0;
		unsigned int posLimit = currentPos + 10; //max 10 bytes
		while (currentPos < posLimit) {
			unsigned long long byte = src[currentPos++];
			result |= (byte & 0x7F) << shift;
			shift += 7;
			if ((byte & 0x80) == 0) { //end of varint
				if (dest != NULL) {
					dest[i] = result;
				}
				break;
			}
		}
	}
	return currentPos;
}

unsigned int parseProtobufMsg(void** destPtrMap, const u_int8_t* src, unsigned int startSrc, unsigned int msgSize) {
	unsigned int currentPos = startSrc;
	unsigned int posLimit = currentPos + msgSize;
	while (currentPos < posLimit) {
		unsigned long long tag = 0;
		currentPos = bytesToVarint64s(&tag, src, currentPos, 1);
		unsigned int fieldNum = tag >> 3;
		int fieldId = fieldNum - 1;
		unsigned int wireType = tag & 7;
		switch (wireType) {
		case 0: // varint
			currentPos = bytesToVarint64s((unsigned long long*)destPtrMap[fieldId], src, currentPos, 1);
			break;
		case 2: // embedded message
		{
			unsigned long long msgLength = 0;
			currentPos = bytesToVarint64s(&msgLength, src, currentPos, 1);
			currentPos = parseProtobufMsg((void**)destPtrMap[fieldId], src, currentPos, msgLength);
			break;
		}
		case 5: //float
			currentPos = bytesToFloats((float*)destPtrMap[fieldId], src, currentPos, 1, true);
			break;
		}
	}
	return currentPos;
}

static std::vector<u_int8_t> processNalUnit(unsigned int size, const u_int8_t* unit) {
	const u_int8_t startCode[4] = { 0x00, 0x00, 0x00, 0x01 };
	std::vector<u_int8_t> result(startCode, startCode + sizeof(startCode));
	size_t offset = 0;

	u_int8_t firstByte = unit[0];

	// Check forbidden_zero_bit (first bit of the first byte must be 0)
	bool isFirstBitOne = firstByte & 0b10000000;
	if (isFirstBitOne) {
		throw std::invalid_argument("First bit must be zero (forbidden_zero_bit)");
	}

	// Extract the NAL type (lower 5 bits of the first byte)
	u_int8_t nalType = firstByte & 0b00011111;

	if (nalType == 28) {
		// Fragmentation Unit (FU-A)
		// Ensure the unit is long enough to have a second byte (FU header)
		if (size < 2) {
			throw std::invalid_argument("NAL unit too short for FU-A header");
		}

		u_int8_t fuHeader = unit[1];
		offset = 2;  // Skip first two bytes (NAL header and FU header)

		// Check the Start bit of the FU header (bit 8 of the second byte)
		bool isFuStartBitOne = fuHeader & 0b10000000;

		if (isFuStartBitOne) {
			// Reconstruct the original NAL unit header from the FU-A headers
			u_int8_t firstByteBits1to3 = firstByte & 0b11100000;   // First 3 bits of the original NAL unit
			u_int8_t fuHeaderBits4to8 = fuHeader & 0b00011111;     // Last 5 bits of FU header (original NAL type)

			u_int8_t reconstructedHeader = firstByteBits1to3 | fuHeaderBits4to8;
			result.push_back(reconstructedHeader);  // Append the reconstructed NAL header to the start code
		}
		else {
			// Do not prepend the start code, we are in the middle of a fragmented NAL unit
			result.clear();
		}
	}

	// Append the rest of the payload (after the FU headers) to the start code (or directly if no FU)
	result.insert(result.end(), unit + offset, unit + size);

	return result;
}

static RTSPClientService service;

short pl_acquire_worker()
{
	return service.AcquireWorker();
}

int pl_start_worker(u_int8_t id, const char* url, u_int8_t streamMask, LogCallback logCallback, RawDataCallback dataCallback, void* userData) {
	return service.StartWorker(id, url, streamMask, logCallback, dataCallback, userData);
}

void pl_stop_worker(u_int8_t id, bool release) {
	service.StopWorker(id, release);
}

void pl_stop_service() {
	service.Stop();
}

int pl_bytes_to_eye_tracking_data(
	const u_int8_t* bytes,
	unsigned int size,
	unsigned int offset,
	float* gazePoint, bool* worn,
	float* gazePointDualRight,
	float* eyeStateLeft, float* eyeStateRight,
	float* eyelidLeft, float* eyelidRight
) {
	int currentPos = offset;
	currentPos = bytesToFloats(gazePoint, bytes, currentPos, 2);
	currentPos = bytesToBooleans(worn, bytes, currentPos, 1);
	if (currentPos == size) {
		return EDT_GAZE_DATA;
	}
	if (currentPos + 8 == size) {
		currentPos = bytesToFloats(gazePointDualRight, bytes, currentPos, 2);
		return EDT_DUAL_MONOCULAR_GAZE_DATA;
	}
	currentPos = bytesToFloats(eyeStateLeft, bytes, currentPos, 7);
	currentPos = bytesToFloats(eyeStateRight, bytes, currentPos, 7);
	if (currentPos == size) {
		return EDT_EYE_STATE_GAZE_DATA;
	}
	currentPos = bytesToFloats(eyelidLeft, bytes, currentPos, 3);
	currentPos = bytesToFloats(eyelidRight, bytes, currentPos, 3);
	if (currentPos == size) {
		return EDT_EYE_STATE_EYELID_GAZE_DATA;
	}
	return EDT_UNKNOWN;
}

int pl_bytes_to_eye_event_data(
	const u_int8_t* bytes,
	unsigned int size,
	unsigned int offset,
	int* eventType, long long* startTime,
	long long* endTime,
	float* gazeEvent
) {
	int dataTypeByEventType[] = { EEDT_FIXATION_DATA, EEDT_FIXATION_DATA, EEDT_FIXATION_ONSET_DATA, EEDT_FIXATION_ONSET_DATA, EEDT_BLINK_DATA };
	int currentPos = offset;
	currentPos = bytesToInts(eventType, bytes, currentPos, 1);
	int et = *eventType;
	int eedt = EEDT_UNKNOWN;
	if (et >= 0 && et <= 4) {
		eedt = dataTypeByEventType[et];
	}
	else {
		return eedt; //unknown data
	}
	currentPos = bytesToLongLongs(startTime, bytes, currentPos, 1);
	if (eedt == EEDT_FIXATION_ONSET_DATA) {
		return eedt; //fixation onset data
	}
	currentPos = bytesToLongLongs(endTime, bytes, currentPos, 1);
	if (eedt == EEDT_BLINK_DATA) {
		return eedt; //blink data
	}
	currentPos = bytesToFloats(gazeEvent, bytes, currentPos, 10);
	return eedt; //fixation data
}

int pl_bytes_to_imu_data(
	const u_int8_t* bytes,
	unsigned int size,
	unsigned int offset,
	unsigned long long* tsNs,
	float* accelData,
	float* gyroData,
	float* quatData
) {
	unsigned long long tmpInt;
	float tmpFloat;
	void* accelPtr[4] = { NULL };
	if (accelData != NULL) {
		accelPtr[0] = &accelData[0];
		accelPtr[1] = &accelData[1];
		accelPtr[2] = &accelData[2];
		accelPtr[3] = &tmpInt;
	}
	void* gyroPtr[4] = { NULL };
	if (gyroData != NULL)
	{
		gyroPtr[0] = &gyroData[0];
		gyroPtr[1] = &gyroData[1];
		gyroPtr[2] = &gyroData[2];
		gyroPtr[3] = &tmpInt;
	};
	void* quatPtr[5] = { NULL };
	if (quatData != NULL)
	{
		quatPtr[0] = &quatData[0];
		quatPtr[1] = &quatData[1];
		quatPtr[2] = &quatData[2];
		quatPtr[3] = &quatData[3];
		quatPtr[4] = &tmpFloat;
	};
	void* ptrMap[] = { tsNs, accelPtr, gyroPtr, quatPtr };

	parseProtobufMsg(ptrMap, bytes + offset, 0, size - offset);
	return 0;
}