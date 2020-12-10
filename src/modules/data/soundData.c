#include "data/soundData.h"
#include "data/blob.h"
#include "core/util.h"
#include "core/ref.h"
#include "lib/stb/stb_vorbis.h"
#include "lib/miniaudio/miniaudio.h"
#include "audio/audio_internal.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static uint32_t lovrSoundDataReadRaw(SoundData* soundData, uint32_t offset, uint32_t count, void* data) {
  uint8_t* p = soundData->blob->data;
  uint32_t n = MIN(count, soundData->frames - offset);
  size_t stride = bytesPerAudioFrame(soundData->channels, soundData->format);
  memcpy(data, p + offset * stride, n * stride);
  return n;
}

/*
static uint32_t lovrSoundDataReadWav(SoundData* soundData, uint32_t offset, uint32_t count, void* data) {
  return 0;
}
*/

static uint32_t lovrSoundDataReadOgg(SoundData* soundData, uint32_t offset, uint32_t count, void* data) {
  if (soundData->cursor != offset) {
    stb_vorbis_seek(soundData->decoder, (int) offset);
    soundData->cursor = offset;
  }

  uint32_t frames = 0;
  uint32_t channels = soundData->channels;
  float* p = data;
  int n;

  do {
    n = stb_vorbis_get_samples_float_interleaved(soundData->decoder, channels, p, count * channels);
    p += n * channels;
    frames += n;
    count -= n;
  } while (frames < count && n > 0);

  soundData->cursor += frames;
  return frames;
}

/*
static uint32_t lovrSoundDataReadMp3(SoundData* soundData, uint32_t offset, uint32_t count, void* data) {
  return 0;
}
*/

static uint32_t lovrSoundDataReadRing(SoundData* soundData, uint32_t offset, uint32_t count, void* data) {
  size_t bytesPerFrame = bytesPerAudioFrame(soundData->channels, soundData->format);
  size_t totalRead = 0;
  while(count > 0) {
    uint32_t availableFramesInRing = count;
    void *store;
    ma_result acquire_status = ma_pcm_rb_acquire_read(soundData->ring, &availableFramesInRing, &store);
    lovrAssert(acquire_status == MA_SUCCESS, "Failed to acquire ring buffer for read: %d\n", acquire_status);
    memcpy(data, store, availableFramesInRing * bytesPerFrame);
    ma_result commit_status = ma_pcm_rb_commit_read(soundData->ring, availableFramesInRing, store);
    lovrAssert(commit_status == MA_SUCCESS, "Failed to commit ring buffer for read: %d\n", acquire_status);

    if (availableFramesInRing == 0) {
      return totalRead;
    }

    count -= availableFramesInRing;
    data += availableFramesInRing * bytesPerFrame;
    totalRead += availableFramesInRing;
  }
  return totalRead;
}


SoundData* lovrSoundDataCreateRaw(uint32_t frameCount, uint32_t channelCount, uint32_t sampleRate, SampleFormat format, struct Blob* blob) {
  SoundData* soundData = lovrAlloc(SoundData);
  soundData->format = format;
  soundData->sampleRate = sampleRate;
  soundData->channels = channelCount;
  soundData->frames = frameCount;
  soundData->read = lovrSoundDataReadRaw;
  
  if (blob) {
    soundData->blob = blob;
    lovrRetain(blob);
  } else {
    size_t size = frameCount * bytesPerAudioFrame(channelCount, format);
    void* data = calloc(1, size);
    lovrAssert(data, "Out of memory");
    soundData->blob = lovrBlobCreate(data, size, "SoundData");
  }

  return soundData;
}

SoundData* lovrSoundDataCreateStream(uint32_t bufferSizeInFrames, uint32_t channels, uint32_t sampleRate, SampleFormat format) {
  SoundData* soundData = lovrAlloc(SoundData);
  soundData->format = format;
  soundData->sampleRate = sampleRate;
  soundData->channels = channels;
  soundData->frames = bufferSizeInFrames;
  soundData->read = lovrSoundDataReadRing;
  soundData->ring = calloc(1, sizeof(ma_pcm_rb));
  ma_result rbStatus = ma_pcm_rb_init(miniAudioFormatFromLovr[format], channels, bufferSizeInFrames, NULL, NULL, soundData->ring);
  lovrAssert(rbStatus == MA_SUCCESS, "Failed to create ring buffer for streamed SoundData");
  return soundData;
}

SoundData* lovrSoundDataCreateFromFile(struct Blob* blob, bool decode) {
  SoundData* soundData = lovrAlloc(SoundData);

  if (blob->size >= 4 && !memcmp(blob->data, "OggS", 4)) {
    soundData->decoder = stb_vorbis_open_memory(blob->data, (int) blob->size, NULL, NULL);
    lovrAssert(soundData->decoder, "Could not load sound from '%s'", blob->name);

    stb_vorbis_info info = stb_vorbis_get_info(soundData->decoder);
    soundData->frames = stb_vorbis_stream_length_in_samples(soundData->decoder);
    soundData->sampleRate = info.sample_rate;
    soundData->channels = info.channels;
    soundData->format = SAMPLE_F32;

    if (decode) {
      soundData->read = lovrSoundDataReadRaw;
      size_t size = soundData->frames * bytesPerAudioFrame(soundData->channels, soundData->format);
      void* data = calloc(1, size);
      lovrAssert(data, "Out of memory");
      soundData->blob = lovrBlobCreate(data, size, "SoundData");
      if (stb_vorbis_get_samples_float_interleaved(soundData->decoder, info.channels, data, size / 4) < (int) soundData->frames) {
        lovrThrow("Could not decode sound from '%s'", blob->name);
      }
      stb_vorbis_close(soundData->decoder);
      soundData->decoder = NULL;
    } else {
      soundData->read = lovrSoundDataReadOgg;
      soundData->blob = blob;
      lovrRetain(blob);
    }
  }

  return soundData;
}

size_t lovrSoundDataStreamAppendBlob(SoundData *dest, struct Blob* blob) {
  lovrAssert(dest->ring, "Data can only be appended to a SoundData stream");

  void *store;
  size_t blobOffset = 0;
  size_t bytesPerFrame = bytesPerAudioFrame(dest->channels, dest->format);
  size_t frameCount = blob->size / bytesPerFrame;
  size_t framesAppended = 0;
  while(frameCount > 0) {
    uint32_t availableFrames = frameCount;
    ma_result acquire_status = ma_pcm_rb_acquire_write(dest->ring, &availableFrames, &store);
    lovrAssert(acquire_status == MA_SUCCESS, "Failed to acquire ring buffer");
    memcpy(store, blob->data + blobOffset, availableFrames * bytesPerFrame);
    ma_result commit_status = ma_pcm_rb_commit_write(dest->ring, availableFrames, store);
    lovrAssert(commit_status == MA_SUCCESS, "Failed to commit to ring buffer");
    if (availableFrames == 0) {
      lovrLog(LOG_WARN, "audio", "SoundData's stream ring buffer is overrun; appended %d and dropping %d frames of data", framesAppended, frameCount);
      return framesAppended;
    }
    
    frameCount -= availableFrames;
    blobOffset += availableFrames * bytesPerFrame;
    framesAppended += availableFrames;
  }
  return framesAppended;
}

size_t lovrSoundDataStreamAppendSound(SoundData *dest, SoundData *src) {
  lovrAssert(dest->channels == src->channels && dest->sampleRate == src->sampleRate && dest->format == src->format, "Source and destination SoundData formats must match");
  lovrAssert(src->blob && src->read == lovrSoundDataReadRaw, "Source SoundData must have static PCM data and not be a stream");
  return lovrSoundDataStreamAppendBlob(dest, src->blob);
}

void lovrSoundDataSetSample(SoundData* soundData, size_t index, float value) {
  size_t byteIndex = index * bytesPerAudioFrame(soundData->channels, soundData->format);
  lovrAssert(byteIndex < soundData->blob->size, "Sample index out of range");
  switch (soundData->format) {
    case SAMPLE_I16: ((int16_t*) soundData->blob->data)[index] = value * SHRT_MAX; break;
    case SAMPLE_F32: ((float*) soundData->blob->data)[index] = value; break;
    default: lovrThrow("Unsupported SoundData format %d\n", soundData->format); break;
  }
}

bool lovrSoundDataIsStream(SoundData *soundData) {
  return soundData->read == lovrSoundDataReadRing;
}

uint32_t lovrSoundDataGetDuration(SoundData *soundData)
{
  if (lovrSoundDataIsStream(soundData)) {
    return ma_pcm_rb_available_read(soundData->ring);
  } else {
    return soundData->frames;
  }
}

void lovrSoundDataDestroy(void* ref) {
  SoundData* soundData = (SoundData*) ref;
  stb_vorbis_close(soundData->decoder);
  lovrRelease(Blob, soundData->blob);
  ma_pcm_rb_uninit(soundData->ring);
  free(soundData->ring);
}
