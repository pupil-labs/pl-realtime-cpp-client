#pragma once

#if defined(_WIN32) || defined(_WIN64)

  #ifdef PLRTSPSERVICE_EXPORTS
    #define PLRTSPSERVICE_API __declspec(dllexport)
  #else
    #define PLRTSPSERVICE_API __declspec(dllimport)
  #endif

#elif defined(__GNUC__) || defined(__clang__)
  #define PLRTSPSERVICE_API __attribute__((visibility("default")))

#else
  #define PLRTSPSERVICE_API
#endif

#include <stdint.h>
#include <stdbool.h>

typedef unsigned char u_int8_t;

/**
 * @brief Function pointer type for callbacks handling log messages.
 *
 * @param[in] message   Null-terminated string containing the log message.
 * @param[in] userData  Pointer to user-defined data or context.
 */
typedef void (*LogCallback)(const char* message, void* userData);

/**
 * @brief Function pointer type for callbacks handling raw data received by a worker.
 *
 * @param[in] timestampMs       Timestamp of the data in milliseconds.
 * @param[in] rtcpSynchronized  Indicates whether timestampMs is synchronized using RTCP.
 * @param[in] streamId          ID of the source stream (see StreamId).
 * @param[in] payloadFormat     Format of the payload (see RTPPayloadFormat).
 * @param[in] dataSize          Size of the data in bytes.
 * @param[in] data              Pointer to the raw data buffer.
 * @param[in] userData          Pointer to user-defined data or context.
 */
typedef void (*RawDataCallback)(int64_t timestampMs, bool rtcpSynchronized, u_int8_t streamId, u_int8_t payloadFormat, unsigned int dataSize, const u_int8_t* data, void* userData);

enum EyeEventType {
	EET_SACCADE = 0,
	EET_FIXATION = 1,
	EET_SACCADE_ONSET = 2,
	EET_FIXATION_ONSET = 3,
	EET_BLINK = 4,
	EET_KEEPALIVE = 5
};

enum EyeEventsDataType {
	EEDT_FIXATION_DATA = 0,
	EEDT_FIXATION_ONSET_DATA = 1,
	EEDT_BLINK_DATA = 2,
	EEDT_UNKNOWN = -1
};

enum EtDataType {
	EDT_GAZE_DATA = 0,
	EDT_DUAL_MONOCULAR_GAZE_DATA = 1,
	EDT_EYE_STATE_GAZE_DATA = 2,
	EDT_EYE_STATE_EYELID_GAZE_DATA = 3,
	EDT_UNKNOWN = -1
};

enum StreamId {
	SID_IMU = 0,
	SID_WORLD = 1,
	SID_GAZE = 2,
	SID_EYE_EVENTS = 3,
	SID_EYES = 4
};

enum StreamStatus {
	SST_UNINITIALIZED = 0,
	SST_CLIENT_CREATED = 1,
	SST_DESCRIBE_SUCCESS = 2,
	SST_SETUP_SUCCESS = 3,
	SST_PLAY_SUCCESS = 4,
	SST_SHUTDOWN = 5
};

enum RTPPayloadFormat {
	PF_VIDEO = 96,
	PF_AUDIO = 97,
	PF_GAZE = 99,
	PF_IMU = 100,
	PF_EYE_EVENTS = 101
};


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decodes raw data received from gaze stream.
 *
 * @param[in]  bytes                Pointer to a byte array containing raw data.
 * @param[in]  size                 Size of the data buffer in bytes.
 *
 * @param[out] gazePoint            Pointer to a float array where gaze point (x, y) will be set (binocular, or left eye in case of dual-monocular gaze data).
 * @param[out] worn                 Pointer to a bool where the worn flag will be set.
 * @param[out] gazePointDualRight   Pointer to a float array where gaze point (x, y) for right eye will be set (in case of dual-monocular gaze data).
 * @param[out] eyeStateLeft         Pointer to float array of 7 elements where left eye state (pupil diameter, eyeball center [x, y, z], optical axis [x, y, z]) will be set.
 * @param[out] eyeStateRight        Same as eyeStateLeft but for the right eye.
 * @param[out] eyelidLeft           Pointer to float array of 3 elements where left eyelid state (angle top, angle bottom, aperture) will be set.
 * @param[out] eyelidRight          Same as eyelidLeft but for the right eyelid.
 *
 * @return type of received data (see EtDataType).
 */
PLRTSPSERVICE_API int pl_bytes_to_eye_tracking_data(
	const u_int8_t* bytes,
	unsigned int size,
	unsigned int offset,
	float* gazePoint, bool* worn,
	float* gazePointDualRight,
	float* eyeStateLeft, float* eyeStateRight,
	float* eyelidLeft, float* eyelidRight
);

/**
 * @brief Decodes raw data received from eye events stream.
 *
 * @param[in]  bytes                Pointer to a byte array containing raw data.
 * @param[in]  size                 Size of the data buffer in bytes.
 *
 * @param[out] eventType            Pointer to a int where event type will be set (see EyeEventType).
 * @param[out] startTime            Pointer to a long long where start time will be set.
 * @param[out] endTime              Pointer to a long long where end time will be set.
 * @param[out] gazeEvent            Pointer to float array of 10 elements where gaze event data (start gaze [x, y], end gaze [x, y], mean gaze [x, y], amplitude pixels, amplitude deg, mean velocity, max velocity) will be set (if available for given type).
 *
 * @return type of received data (see EyeEventsDataType).
 */
PLRTSPSERVICE_API int pl_bytes_to_eye_event_data(
	const u_int8_t* bytes,
	unsigned int size,
	unsigned int offset,
	int* eventType, long long* startTime,
	long long* endTime,
	float* gazeEvent
);

/**
 * @brief Decodes raw data received from IMU stream.
 *
 * @param[in]  bytes                Pointer to a byte array containing raw data.
 * @param[in]  size                 Size of the data buffer in bytes.
 *
 * @param[out] tsNs                 Pointer to a long long where timestamp in ns will be set.
 * @param[out] accelData            Pointer to float array of 3 elements where acceleration data (x, y, z) will be set.
 * @param[out] gyroData             Pointer to float array of 3 elements where gyroscope data (x, y, z) will be set.
 * @param[out] quatData             Pointer to float array of 4 elements where quaternion data (w, x, y, z) will be set.
 *
 * @return type of received data (always 0).
 */
PLRTSPSERVICE_API int pl_bytes_to_imu_data(
	const u_int8_t* bytes,
	unsigned int size,
	unsigned int offset,
	unsigned long long* tsNs,
	float* accelData,
	float* gyroData,
	float* quatData
);

/**
 * @brief Acquires a free worker thread.
 *
 * @return The ID of a free worker thread, or -1 if none are available.
 */
PLRTSPSERVICE_API short pl_acquire_worker();

/**
 * @brief Starts a worker thread that handles multiple data streams.
 *
 * @param[in] id            Identifier of the idle worker thread.
 * @param[in] url           Pointer to a null-terminated string representing the RTSP stream URL.
 * @param[in] streamMask    Bitmask indicating which streams to enable (LSB first: [imu | world | gaze | eye_events | eyes | x | x | x]).
 * @param[in] logCallback   Callback function invoked for logging events.
 * @param[in] dataCallback  Callback function invoked when new data is received.
 * @param[in] userData      Pointer to user-defined data or context that will be passed to both callbacks.
 *
 * @return 0 on success, or a non-zero error code on failure.
 */
PLRTSPSERVICE_API int pl_start_worker(u_int8_t id, const char* url, u_int8_t streamMask, LogCallback logCallback, RawDataCallback dataCallback, void* userData);

/**
 * @brief Stops a specific worker thread.
 *
 * @param[in] id       Identifier of the worker thread to stop.
 * @param[in] release  If true, the worker will be marked as available after stopping.
 */
PLRTSPSERVICE_API void pl_stop_worker(u_int8_t id, bool release);

/**
 * @brief Stops all worker threads and frees allocated resources.
 */
PLRTSPSERVICE_API void pl_stop_service();

/**
 * @brief Returns the number of milliseconds elapsed since the Unix epoch (January 1, 1970, 00:00:00 UTC).
 *
 * @return Milliseconds since the Unix epoch.
 */
PLRTSPSERVICE_API int64_t pl_time_ms();


#ifdef __cplusplus
}
#endif
