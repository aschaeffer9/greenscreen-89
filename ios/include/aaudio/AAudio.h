/* Minimal aaudio/AAudio.h for the iOS port: implemented over AudioQueue. */
#ifndef IOS_AAUDIO_H
#define IOS_AAUDIO_H
#include <stdint.h>

typedef struct AAudioStream AAudioStream;
typedef struct AAudioStreamBuilder AAudioStreamBuilder;

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_stream_state_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_usage_t;
typedef int32_t aaudio_content_type_t;

#define AAUDIO_OK 0
#define AAUDIO_ERROR_BASE (-900)
#define AAUDIO_ERROR_DISCONNECTED (AAUDIO_ERROR_BASE + 18)
#define AAUDIO_ERROR_INVALID_STATE (AAUDIO_ERROR_BASE + 11)
#define AAUDIO_ERROR_TIMEOUT (AAUDIO_ERROR_BASE + 21)

enum {
    AAUDIO_DIRECTION_OUTPUT = 0,
    AAUDIO_DIRECTION_INPUT = 1,
};

enum {
    AAUDIO_FORMAT_INVALID = -1,
    AAUDIO_FORMAT_UNSPECIFIED = 0,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_FORMAT_PCM_FLOAT = 2,
};

enum {
    AAUDIO_PERFORMANCE_MODE_NONE = 10,
    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
};

enum {
    AAUDIO_SHARING_MODE_EXCLUSIVE = 0,
    AAUDIO_SHARING_MODE_SHARED = 1,
};

enum {
    AAUDIO_USAGE_MEDIA = 1,
    AAUDIO_USAGE_GAME = 14,
};

enum {
    AAUDIO_CONTENT_TYPE_SPEECH = 1,
    AAUDIO_CONTENT_TYPE_MUSIC = 2,
    AAUDIO_CONTENT_TYPE_SONIFICATION = 4,
};

enum {
    AAUDIO_STREAM_STATE_UNINITIALIZED = 0,
    AAUDIO_STREAM_STATE_OPEN = 1,
    AAUDIO_STREAM_STATE_STARTING = 2,
    AAUDIO_STREAM_STATE_STARTED = 3,
    AAUDIO_STREAM_STATE_PAUSING = 4,
    AAUDIO_STREAM_STATE_PAUSED = 5,
    AAUDIO_STREAM_STATE_STOPPING = 6,
    AAUDIO_STREAM_STATE_STOPPED = 7,
    AAUDIO_STREAM_STATE_CLOSING = 8,
    AAUDIO_STREAM_STATE_CLOSED = 9,
    AAUDIO_STREAM_STATE_DISCONNECTED = 10,
};

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *builder, aaudio_direction_t direction);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *builder, int32_t sampleRate);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *builder, int32_t channelCount);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *builder, aaudio_format_t format);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *builder, aaudio_performance_mode_t mode);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *builder, aaudio_sharing_mode_t mode);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *builder, aaudio_usage_t usage);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *builder, aaudio_content_type_t contentType);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *builder, AAudioStream **stream);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *builder);

int32_t AAudioStream_getSampleRate(AAudioStream *stream);
int32_t AAudioStream_getChannelCount(AAudioStream *stream);
aaudio_format_t AAudioStream_getFormat(AAudioStream *stream);
int32_t AAudioStream_getFramesPerBurst(AAudioStream *stream);
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *stream, int32_t numFrames);
aaudio_stream_state_t AAudioStream_getState(AAudioStream *stream);
int64_t AAudioStream_getFramesRead(AAudioStream *stream);
aaudio_result_t AAudioStream_requestStart(AAudioStream *stream);
aaudio_result_t AAudioStream_requestPause(AAudioStream *stream);
aaudio_result_t AAudioStream_requestStop(AAudioStream *stream);
aaudio_result_t AAudioStream_close(AAudioStream *stream);
aaudio_result_t AAudioStream_write(AAudioStream *stream, const void *buffer, int32_t numFrames, int64_t timeoutNanoseconds);

#endif
