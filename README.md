# pl-realtime-cpp-client
This is a lightweight library built on Live555 that runs inside your application, serving as the Pupil Labs Real-Time client. It exhibits service-like behavior by managing RTSP-based live streaming across one or more background threads with start/stop control, delivering data asynchronously via callbacks. This is a library component, not a standalone OS service or daemon.

## Building Live555 library
1. clone https://github.com/melchi45/live555
2. cd live555
3. mkdir build
4. cd build
5. cmake .. -B vs2022 -G "Visual Studio 17 2022" -DLIVE555_ENABLE_OPENSSL=OFF -DLIVE555_BUILD_EXAMPLES=OFF -DLIVE555_MONOLITH_BUILD=ON<br/>
6. open generated VS solution that can be found in build folder and build the project
7. built dll can be found in build\vs2022\Debug

## Building the RTSP service
1. clone this repository (pl-releatime-cpp-client and live555 repositories should be in the same folder)
2. open VS solution
3. build

## Usage Examples

### C++
```cpp
#include <opencv2/opencv.hpp>
#include <RTSPService.hh>
#include <concurrent_queue.h>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

extern "C"
{
#include<libswscale/swscale.h>
#include<libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

static float gazePoint[2] = { 0 };
static float accelData[3] = { 0 };
static std::atomic<bool> fixationOnSet = false;
static std::mutex imuMutex;
static std::mutex gazePointMutex;
static Concurrency::concurrent_queue<std::vector<u_int8_t>> imuQueue;
static Concurrency::concurrent_queue<std::vector<u_int8_t>> videoQueue;
static Concurrency::concurrent_queue<std::vector<u_int8_t>> audioQueue;

void logCallback(const char* message, void* /*userData*/) {
	std::cout << message;
}

void imuCallback(int64_t timestampMs, unsigned int dataSize, const u_int8_t* data) {
	imuMutex.lock();
	unsigned long long tsNs = 0;
	pl_bytes_to_imu_data(data, dataSize, 0, &tsNs, accelData, NULL, NULL);
	imuMutex.unlock();
	//std::cout << "RECEIVED IMU DATA AT: " << timestampMs << " NS: " << tsNs << std::endl;
}

void gazeCallback(int64_t timestampMs, unsigned int dataSize, const u_int8_t* data) {
	gazePointMutex.lock();
	pl_bytes_to_eye_tracking_data(data, dataSize, 0, gazePoint, NULL, NULL, NULL, NULL, NULL, NULL);
	gazePointMutex.unlock();
	//std::cout << "RECEIVED GAZE DATA AT: " << timestampMs << std::endl;
}

void videoCallback(int64_t timestampMs, unsigned int dataSize, const u_int8_t* data) {
	std::vector<u_int8_t> v(data, data + dataSize);
	videoQueue.push(v);
	//std::cout << "RECEIVED VIDEO DATA AT: " << timestampMs << std::endl;
}

void audioCallback(int64_t timestampMs, unsigned int dataSize, const u_int8_t* data) {
	std::vector<u_int8_t> v(data, data + dataSize);
	audioQueue.push(v);
	//std::cout << "RECEIVED AUDIO DATA AT: " << timestampMs << std::endl;
}

void eyeEventsCallback(int64_t timestampMs, unsigned int dataSize, const u_int8_t* data) {
	int eyeEventType = 0;
	pl_bytes_to_eye_event_data(data, dataSize, 0, &eyeEventType, NULL, NULL, NULL);
	if (eyeEventType == EyeEventType::EET_FIXATION_ONSET) {
		fixationOnSet = true;
	}
	else if (eyeEventType == EyeEventType::EET_SACCADE_ONSET) {
		fixationOnSet = false;
	}
	//std::cout << "RECEIVED EYE EVENT DATA AT: " << timestampMs << std::endl;
}

void dataCallback(int64_t timestampMs, bool rtcpSynchronized, u_int8_t streamId, u_int8_t payloadFormat, unsigned int dataSize, const u_int8_t* data, void* /*userData*/) {
	//std::cout << "RECEIVED DATA FROM STREAM: " << unsigned(streamId) << " FORMAT: " << unsigned(payloadFormat) << " SIZE: " << dataSize << std::endl;
	if (streamId == StreamId::SID_WORLD) {
		if (payloadFormat == RTPPayloadFormat::PF_VIDEO) {
			videoCallback(timestampMs, dataSize, data);
		}
		else if (payloadFormat == RTPPayloadFormat::PF_AUDIO) {
			audioCallback(timestampMs, dataSize, data);
		}
	}
	else if (streamId == StreamId::SID_GAZE) {
		gazeCallback(timestampMs, dataSize, data);
	}
	else if (streamId == StreamId::SID_IMU) {
		imuCallback(timestampMs, dataSize, data);
	}
	else if (streamId == StreamId::SID_EYE_EVENTS) {
		eyeEventsCallback(timestampMs, dataSize, data);
	}
}

cv::Mat avframeToCvmat(const AVFrame* frame) {
	int width = frame->width;
	int height = frame->height;
	cv::Mat image(height, width, CV_8UC3);
	int cvLinesizes[1];
	cvLinesizes[0] = image.step1();
	SwsContext* conversion = sws_getContext(
		width, height, (AVPixelFormat)frame->format, width, height,
		AVPixelFormat::AV_PIX_FMT_BGR24, SWS_FAST_BILINEAR, NULL, NULL, NULL);
	sws_scale(conversion, frame->data, frame->linesize, 0, height, &image.data,
		cvLinesizes);
	sws_freeContext(conversion);
	return image;
}

class AudioTools
{
public:
	static AudioTools* createNew();
	bool submitFrame(AVFrame* frame);
	bool readPlaybackData(void* dest, ma_uint32 sizeInBytes);
	AVFrame* convertToPcm(AVFrame* frame);
	bool startPlayback();
	~AudioTools();
protected:
	AudioTools(AVFrame* pcmFrame, ma_rb* rb);
private:
	AVFrame* pcmFrame;
	SwrContext* swrContext = NULL;
	bool swrInitialized = false;
	ma_device device{};
	ma_rb* playbackBuffer = NULL;
	static const size_t playbackBufferSize = 10240;
};

AudioTools::AudioTools(AVFrame* pcmFrame, ma_rb* rb) : pcmFrame(pcmFrame), playbackBuffer(rb)
{
}

AudioTools* AudioTools::createNew()
{
	AVFrame* pcmFrame = av_frame_alloc();
	if (pcmFrame == NULL) {
		return NULL;
	}
	ma_rb* rb = new ma_rb();
	if (ma_rb_init(playbackBufferSize, NULL, NULL, rb) != MA_SUCCESS) {
		av_frame_free(&pcmFrame);
		delete rb;
		return NULL;
	}
	return new AudioTools(pcmFrame, rb);
}

AVFrame* AudioTools::convertToPcm(AVFrame* frame)
{
	if (swrInitialized == false) {
		pcmFrame->ch_layout = frame->ch_layout;
		pcmFrame->sample_rate = frame->sample_rate;
		pcmFrame->format = AV_SAMPLE_FMT_S16;
		pcmFrame->nb_samples = frame->nb_samples;

		swr_alloc_set_opts2(&swrContext,
			&pcmFrame->ch_layout, (AVSampleFormat)pcmFrame->format, pcmFrame->sample_rate,
			&frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate,
			0, nullptr);

		swrInitialized = (swrContext != NULL) && (swr_init(swrContext) == 0);
	}

	if (swrInitialized == false) {
		return NULL;
	}

	if (swr_convert_frame(swrContext, pcmFrame, frame) != 0) {
		return NULL;
	}

	return pcmFrame;
}

void maDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	ma_uint32 bytesNeeded = frameCount * pDevice->playback.channels * pDevice->playback.format;
	AudioTools* at = (AudioTools*)pDevice->pUserData;
	if (at->readPlaybackData(pOutput, bytesNeeded) == false)
	{
		memset(pOutput, 0, bytesNeeded);
	}
}

bool AudioTools::readPlaybackData(void* dest, ma_uint32 sizeInBytes) {
	size_t toCopy = sizeInBytes;
	void* srcBuffer = NULL;
	ma_result result = ma_rb_acquire_read(playbackBuffer, &toCopy, &srcBuffer);
	if (result != MA_SUCCESS) {
		return false;
	}
	memcpy(dest, srcBuffer, toCopy);
	memset(dest, 0, sizeInBytes - toCopy);
	return ma_rb_commit_read(playbackBuffer, toCopy) == MA_SUCCESS;
}

bool AudioTools::submitFrame(AVFrame* frame) {
	AVFrame* pcm = convertToPcm(frame);
	if (pcm == NULL) {
		return false;
	}
	int dataSize = av_get_bytes_per_sample((AVSampleFormat)pcm->format);
	size_t sizeInBytes = dataSize * pcm->nb_samples * pcm->ch_layout.nb_channels;
	void* destBuffer = NULL;
	ma_result result = ma_rb_acquire_write(playbackBuffer, &sizeInBytes, &destBuffer);
	if (result != MA_SUCCESS) {
		return false;
	}
	memcpy(destBuffer, pcm->data[0], sizeInBytes);
	return ma_rb_commit_write(playbackBuffer, sizeInBytes) == MA_SUCCESS;
}

bool AudioTools::startPlayback()
{
	if (swrInitialized) {
		ma_device_config config = ma_device_config_init(ma_device_type_playback);
		config.playback.format = ma_format_s16;
		config.playback.channels = (pcmFrame->ch_layout).nb_channels;
		config.sampleRate = pcmFrame->sample_rate;
		config.dataCallback = maDataCallback;
		config.pUserData = this;

		if (ma_device_init(NULL, &config, &device) == MA_SUCCESS) {
			return ma_device_start(&device) == MA_SUCCESS;
		}
	}
	return false;
}

AudioTools::~AudioTools()
{
	av_frame_free(&pcmFrame);
	if (swrContext != NULL) {
		swr_free(&swrContext);
	}
	ma_device_uninit(&device);
	if (playbackBuffer != NULL) {
		ma_rb_uninit(playbackBuffer);
		delete playbackBuffer;
		playbackBuffer = NULL;
	}
}


class DataDecoder
{
public:
	static DataDecoder* createNew(AVCodecID codecId);
	static DataDecoder* createNew(AVCodecID codecId, u_int8_t* extraData, int extraDataSize);
	~DataDecoder();
	AVFrame* decodeData(std::vector<u_int8_t>& data);
protected:
	DataDecoder(const AVCodec* codec, AVCodecContext* ctx, AVPacket* packet, AVFrame* frame);
private:
	const AVCodec* const codec;
	AVCodecContext* codecCtx;
	AVPacket* packet;
	AVFrame* frame;
};

DataDecoder::DataDecoder(const AVCodec* codec, AVCodecContext* ctx, AVPacket* packet, AVFrame* frame) : codec(codec), codecCtx(ctx), packet(packet), frame(frame)
{
}

DataDecoder* DataDecoder::createNew(AVCodecID codecId)
{
	return createNew(codecId, NULL, 0);
}

DataDecoder* DataDecoder::createNew(AVCodecID codecId, u_int8_t* extraData, int extraDataSize)
{
	const AVCodec* codec = avcodec_find_decoder(codecId);
	AVPacket* packet = av_packet_alloc();
	if (!codec || !packet) {
		return NULL;
	}
	AVFrame* frame = av_frame_alloc();
	if (!frame) {
		av_packet_free(&packet);
		return NULL;
	}
	AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
	if (!codecCtx) {
		av_packet_free(&packet);
		av_frame_free(&frame);
		return NULL;
	}
	if (extraData != NULL) {
		codecCtx->extradata = (u_int8_t*)av_malloc(extraDataSize + AV_INPUT_BUFFER_PADDING_SIZE);
		codecCtx->extradata_size = extraDataSize;
		memcpy(codecCtx->extradata, extraData, extraDataSize);
		memset(codecCtx->extradata + extraDataSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	}
	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		av_packet_free(&packet);
		av_frame_free(&frame);
		avcodec_free_context(&codecCtx);
		return NULL;
	}
	return new DataDecoder(codec, codecCtx, packet, frame);
}

DataDecoder::~DataDecoder()
{
	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&codecCtx);
}

AVFrame* DataDecoder::decodeData(std::vector<u_int8_t>& data)
{
	packet->data = data.data();
	packet->size = data.size();

	int ret = avcodec_send_packet(codecCtx, packet);
	if (ret == 0) {
		ret = avcodec_receive_frame(codecCtx, frame);
	}
	return ret == 0 ? frame : NULL;
}

int main() {

	int gpRadius = 20;
	cv::Scalar gpColor(0, 0, 255);
	cv::Scalar fixColor(0, 255, 0);
	int gpThickness = 2;

	int baseline = 0;
	int fontFace = cv::FONT_HERSHEY_SIMPLEX;
	double fontScale = 1;
	int thickness = 2;
	cv::Size txtSize = cv::getTextSize("Ag", fontFace, fontScale, 2, &baseline);
	int lineHeight = txtSize.height + baseline;

	float gazePointCopy[2] = { 0 };
	float accelDataCopy[3] = { 0 };

	short workerId = pl_acquire_worker();
	int res = pl_start_worker(workerId, "rtsp://192.168.1.27:8086", (1 << StreamId::SID_EYE_EVENTS) | (1 << StreamId::SID_IMU) | (1 << StreamId::SID_GAZE) | (1 << StreamId::SID_WORLD), logCallback, dataCallback, NULL);

	if (workerId < 0) {
		return -1;
	}

	cv::Mat cvFrameResized;
	DataDecoder* videoDecoder = DataDecoder::createNew(AV_CODEC_ID_H264);
	u_int8_t extraData[] = { 0x15, 0x88 };
	//00010 1011 0001 000
	//type  hz   n_ch ext
	DataDecoder* audioDecoder = DataDecoder::createNew(AV_CODEC_ID_AAC, extraData, sizeof(extraData));
	AudioTools* audioTools = AudioTools::createNew();
	bool started = false;
	while (true) {

		std::vector<u_int8_t> unit;

		while (audioQueue.try_pop(unit)) {
			AVFrame* aFrame = audioDecoder->decodeData(unit);
			if (aFrame != NULL) {
				//std::cout << "Decoded audio frame with " << aFrame->nb_samples << " samples at " << aFrame->sample_rate << "HZ " << aFrame->ch_layout.nb_channels << "CH" << std::endl;
				if (audioTools->submitFrame(aFrame) && started == false)
				{
					started = audioTools->startPlayback();
					//std::cout << "AUDIO PLAYBACK STARTED" << std::endl;
				}
			}
		}

		if (videoQueue.try_pop(unit) == false) {
			continue;
		}

		AVFrame* frame = videoDecoder->decodeData(unit);
		if (frame != NULL) {
			cv::Mat cvFrame = avframeToCvmat(frame);

			gazePointMutex.lock();
			memcpy(gazePointCopy, gazePoint, 8);
			gazePointMutex.unlock();
			cv::Point gp(static_cast<int>(gazePointCopy[0]), static_cast<int>(gazePointCopy[1]));
			cv::circle(cvFrame, gp, gpRadius, fixationOnSet ? fixColor : gpColor, gpThickness);

			imuMutex.lock();
			memcpy(accelDataCopy, accelData, 12);
			imuMutex.unlock();
			cv::Point imuDataPoint(100, 100);
			cv::putText(cvFrame, "IMU accel", imuDataPoint, fontFace, fontScale, gpColor, thickness);
			imuDataPoint.y += lineHeight;
			imuDataPoint.x += txtSize.width;
			char buffer[64];
			char labels[] = { 'X', 'Y', 'Z' };
			for (int i = 0; i < sizeof(labels); i++)
			{
				snprintf(buffer, sizeof(buffer), "%c: %+8.4fg", labels[i], accelData[i]);
				cv::putText(cvFrame, buffer, imuDataPoint, fontFace, fontScale, gpColor, thickness);
				imuDataPoint.y += lineHeight;
			}

			cv::resize(cvFrame, cvFrameResized, cv::Size(), 0.5, 0.5);
			cv::imshow("World and gaze", cvFrameResized);
		}

		if (cv::waitKey(10) >= 0)
			break;
	}

	delete videoDecoder;
	videoDecoder = NULL;
	delete audioDecoder;
	audioDecoder = NULL;
	cv::destroyAllWindows();

	pl_stop_worker(workerId, true);

	return 0;
}
```

### Python
```python
import os
from ctypes import c_void_p, c_char_p, c_int64, c_uint, c_bool, c_uint8, c_int, c_short, c_float, c_ulonglong, POINTER, cdll, cast, CFUNCTYPE

class NeonClient:
    
    def __init__(self, url):
        self.lib = cdll.LoadLibrary(os.path.join(os.path.dirname(__file__), r"libs\pl-rtsp-service.dll"))
        
        self._logCallbackType = CFUNCTYPE(None, c_char_p, c_void_p)
        self._rawDataCallbackType = CFUNCTYPE(None, c_int64, c_bool, c_uint8, c_uint8, c_uint, POINTER(c_uint8), c_void_p)
        self._logCallbackFunc = None
        self._dataCallbackFunc = None
        
        self.lib.pl_acquire_worker.restype = c_short
        self.lib.pl_start_worker.argtypes = [c_uint8, c_char_p, c_uint8, self._logCallbackType, self._rawDataCallbackType, c_void_p]
        self.lib.pl_start_worker.restype = c_int
        self.lib.pl_stop_service.argtypes = []
        self.lib.pl_stop_service.restype = None
        self.lib.pl_bytes_to_imu_data.argtypes = [POINTER(c_uint8), c_uint, c_uint, POINTER(c_ulonglong), POINTER(c_float), POINTER(c_float), POINTER(c_float)]
        self.lib.pl_bytes_to_imu_data.restype = c_int
        
        self.url = url
        return
        
    def bytesToImuData(self, data, size, offset, tsNsPtr, accelData, gyroData, quatData):
        return self.lib.pl_bytes_to_imu_data(data, size, offset, tsNsPtr, accelData, gyroData, quatData)
        
    def startWorker(self, mask, logCallback, dataCallback, userData):
        self._logCallbackFunc = self._logCallbackType(logCallback) if logCallback is not None else cast(logCallback, self._logCallbackType)
        self._dataCallbackFunc = self._rawDataCallbackType(dataCallback) if dataCallback is not None else cast(dataCallback, self._rawDataCallbackType)
        workerId = self.lib.pl_acquire_worker()
        if workerId >= 0:
            res = self.lib.pl_start_worker(
                workerId,
                self.url.encode('utf-8'),
                mask,
                self._logCallbackFunc,
                self._dataCallbackFunc,
                userData
            )
            if res == 0:
                return workerId
        return -1
        
    def stop(self):
        self.lib.pl_stop_service()
        return

if __name__=="__main__":
    import matplotlib.pyplot as plt
    import time
    from ctypes import byref
    
    nc = NeonClient("rtsp://192.168.1.27:8086")
    accel = [[],[],[]]
    gyro = [[],[],[]]
    
    def logCallback(message, userData):
        print(message)
        return
    
    def dataCallback(timestampMs, rtcpSynchronized, streamId, payloadFormat, dataSize, data, userData):
        if streamId == 0: #recording IMU data only
            tsNs = c_ulonglong(0)
            accelData = (c_float * 3)()
            gyroData = (c_float * 3)()
            quatData = (c_float * 4)()
            res = nc.bytesToImuData(data, dataSize, 0, byref(tsNs), accelData, gyroData, quatData)
            if res == 0:
                for i in range(3):
                    accel[i].append(accelData[i])
                    gyro[i].append(gyroData[i])
        return
    
    #record data for ~5 seconds
    nc.startWorker(1, logCallback, dataCallback, None)
    time.sleep(5)
    nc.stop()
    
    #plot results
    tm = list(range(len(accel[0])))
    
    fig, (axAcc, axGyro) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
    
    axAcc.set_ylim(-1, 2)
    axAcc.plot(tm, accel[0], label='X-axis')
    axAcc.plot(tm, accel[1], label='Y-axis')
    axAcc.plot(tm, accel[2], label='Z-axis')
    axAcc.set_ylabel('Acceleration (m/s²)')
    axAcc.set_title('Acceleration Data Over Time')
    axAcc.legend()
    axAcc.grid(True)
    
    #axGyro.set_ylim(-7, 7)
    axGyro.plot(tm, gyro[0], label='X-axis')
    axGyro.plot(tm, gyro[1], label='Y-axis')
    axGyro.plot(tm, gyro[2], label='Z-axis')
    axGyro.set_xlabel('Sample Index')
    axGyro.set_ylabel('Gyro (deg/s)')
    axGyro.set_title('Gyro Data Over Time')
    axGyro.legend()
    axGyro.grid(True)
    
    fig.tight_layout()
    plt.show()
```
