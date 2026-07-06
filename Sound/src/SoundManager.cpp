#include "../include/SoundManager.h"
#include <Windows.h>
// See SoundManager.h's comment -- Windows.h (included above, AFTER that header in this file)
// re-defines the PlaySound macro every time it's processed, so it needs undefining again here
// too, right before the out-of-line PlaySound/PlayFile/etc. definitions below.
#ifdef PlaySound
#undef PlaySound
#endif
#include <Task.h>
#include <TaskScheduler.h>
#include <cmath>
#include <fstream>

SoundManager::~SoundManager() {
    Shutdown();
}

void SoundManager::DataCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount) {
    // Runs on miniaudio's OWN thread (see the class comment) -- do the absolute minimum here.
    auto* self = static_cast<SoundManager*>(pDevice->pUserData);
    ma_uint32 framesRemaining = frameCount;
    ma_uint8* pOut = static_cast<ma_uint8*>(pOutput);
    const ma_uint32 bytesPerFrame = ma_get_bytes_per_frame(kFormat, kChannels);

    while (framesRemaining > 0) {
        ma_uint32 framesToRead = framesRemaining;
        void* pReadBuffer = nullptr;
        if (ma_pcm_rb_acquire_read(&self->m_RingBuffer, &framesToRead, &pReadBuffer) != MA_SUCCESS) {
            break;
        }
        if (framesToRead == 0) {
            // Underrun -- the mix thread fell behind. Fill the rest with silence rather than
            // stalling this thread or reading whatever garbage is left in the WASAPI buffer.
            memset(pOut, 0, (size_t)framesRemaining * bytesPerFrame);
            break;
        }
        memcpy(pOut, pReadBuffer, (size_t)framesToRead * bytesPerFrame);
        ma_pcm_rb_commit_read(&self->m_RingBuffer, framesToRead);
        pOut += (size_t)framesToRead * bytesPerFrame;
        framesRemaining -= framesToRead;
    }
}

bool SoundManager::Initialize(size_t coreID) {
    ma_context_config contextConfig = ma_context_config_init();
    // Maps straight to THREAD_PRIORITY_TIME_CRITICAL for miniaudio's WASAPI thread (see
    // ma_thread_priority_to_win32 in miniaudio.h) -- that thread only ever drains the ring
    // buffer, so running it at the highest priority is safe and exactly what a real-time audio
    // callback wants.
    contextConfig.threadPriority = ma_thread_priority_realtime;
    ma_backend backends[] = { ma_backend_wasapi };
    if (ma_context_init(backends, 1, &contextConfig, &m_Context) != MA_SUCCESS) {
        return false;
    }
    m_ContextInitialized = true;

    if (ma_pcm_rb_init(kFormat, kChannels, kRingBufferFrames, nullptr, nullptr, &m_RingBuffer) != MA_SUCCESS) {
        Shutdown();
        return false;
    }
    m_RingBufferInitialized = true;

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = kFormat;
    deviceConfig.playback.channels = kChannels;
    deviceConfig.sampleRate = kSampleRate;
    deviceConfig.dataCallback = DataCallback;
    deviceConfig.pUserData = this;

    if (ma_device_init(&m_Context, &deviceConfig, &m_Device) != MA_SUCCESS) {
        Shutdown();
        return false;
    }
    m_DeviceInitialized = true;

    if (ma_device_start(&m_Device) != MA_SUCCESS) {
        Shutdown();
        return false;
    }

    // Save the Task* BEFORE PushFork so it's visible to MixLoop() the moment the worker picks it
    // up (PushFork's internal notify/condition-variable synchronization provides the
    // happens-before -- this write is sequenced on OUR thread before PushFork is even called).
    JLib::Task* task = JLib::TaskScheduler::Instance().CreateTask([this]() { MixLoop(); });
    if (!task) {
        Shutdown();
        return false;
    }
    m_MixTask = task;
    if (!JLib::TaskScheduler::Instance().PushFork((uint8_t)coreID, task)) {
        m_MixTask = nullptr;
        Shutdown();
        return false;
    }

    return true;
}

SoundHandle SoundManager::PlaySound(const char* filePath, float volume) {
    return PlayFile(filePath, volume, false);
}

SoundHandle SoundManager::PlayLoop(const char* filePath, float volume) {
    return PlayFile(filePath, volume, true);
}

SoundHandle SoundManager::PlayFile(const char* filePath, float volume, bool loop) {
    // Real file I/O happens HERE, on whatever thread called this (game logic, main, etc.) --
    // never on the mix thread. m_FileBytes caches the raw bytes by path (shared, read-only,
    // safe to reuse across any number of simultaneous plays -- see its comment in
    // SoundManager.h for why this is the raw-bytes cache, not a cache of decoders).
    SoundHandle fail{};
    auto fileHandle = m_FileBytes.Load(filePath, [filePath](std::vector<uint8_t>& bytes) -> bool {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file) return false;
        std::streamsize size = file.tellg();
        if (size < 0) return false;
        file.seekg(0, std::ios::beg);
        bytes.resize((size_t)size);
        return (bool)file.read(reinterpret_cast<char*>(bytes.data()), size);
        });
    if (!fileHandle.IsValid()) return fail;
    const std::vector<uint8_t>& bytes = m_FileBytes.Resolve(fileHandle);

    // outputFormat/Channels/SampleRate match the device's fixed format, so ma_decoder does any
    // needed resampling/channel conversion once at read time instead of the mixer having to
    // handle mismatched voice formats. ma_decoder_init_memory does NOT copy `bytes` -- see
    // m_FileBytes' lifetime comment in SoundManager.h for why holding a raw pointer into it here
    // is safe.
    auto voice = std::make_unique<Voice>();
    ma_decoder_config decoderConfig = ma_decoder_config_init(kFormat, kChannels, kSampleRate);
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &decoderConfig, &voice->decoder) != MA_SUCCESS) {
        return fail;
    }
    voice->decoderInitialized = true;
    voice->volume = volume;
    voice->loop = loop;

    std::lock_guard<std::mutex> lock(m_VoicesMutex);
    for (size_t i = 0; i < m_VoiceSlots.size(); ++i) {
        if (!m_VoiceSlots[i].voice) {
            m_VoiceSlots[i].voice = std::move(voice);
            return SoundHandle{ (uint32_t)i, m_VoiceSlots[i].generation };
        }
    }
    m_VoiceSlots.push_back(VoiceSlot{});
    m_VoiceSlots.back().voice = std::move(voice);
    return SoundHandle{ (uint32_t)(m_VoiceSlots.size() - 1), m_VoiceSlots.back().generation };
}

bool SoundManager::IsValidLocked(SoundHandle handle) const {
    return handle.index < m_VoiceSlots.size()
        && m_VoiceSlots[handle.index].voice
        && m_VoiceSlots[handle.index].generation == handle.generation;
}

void SoundManager::Stop(SoundHandle handle) {
    std::lock_guard<std::mutex> lock(m_VoicesMutex);
    if (!IsValidLocked(handle)) return;
    m_VoiceSlots[handle.index].voice.reset(); // Voice's destructor uninits the decoder
    m_VoiceSlots[handle.index].generation++;
}

void SoundManager::SetVolume(SoundHandle handle, float volume) {
    std::lock_guard<std::mutex> lock(m_VoicesMutex);
    if (!IsValidLocked(handle)) return;
    m_VoiceSlots[handle.index].voice->volume = volume;
}

bool SoundManager::IsPlaying(SoundHandle handle) const {
    std::lock_guard<std::mutex> lock(m_VoicesMutex);
    return IsValidLocked(handle);
}

void SoundManager::MixLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    constexpr ma_uint32 kChunkFrames = 512;
    float mixBuffer[kChunkFrames * kChannels];
    float readBuffer[kChunkFrames * kChannels];

    while (!m_MixTask->stopFlag.load(std::memory_order_acquire)) {
        ma_uint32 framesToWrite = kChunkFrames;
        void* pWriteBuffer = nullptr;
        if (ma_pcm_rb_acquire_write(&m_RingBuffer, &framesToWrite, &pWriteBuffer) != MA_SUCCESS ||
            framesToWrite == 0) {
            // MA_SUCCESS-but-zero means the ring buffer is full -- comfortably ahead of
            // playback. Back off briefly instead of busy-spinning a TIME_CRITICAL thread.
            Sleep(1);
            continue;
        }

        memset(mixBuffer, 0, (size_t)framesToWrite * kChannels * sizeof(float));

        {
            std::lock_guard<std::mutex> lock(m_VoicesMutex);
            for (size_t i = 0; i < m_VoiceSlots.size(); ++i) {
                VoiceSlot& slot = m_VoiceSlots[i];
                if (!slot.voice) continue; // empty slot -- nothing playing here right now
                Voice* voice = slot.voice.get();
                ma_uint32 framesFilled = 0;
                bool finished = false;

                while (framesFilled < framesToWrite) {
                    ma_uint32 framesRequested = framesToWrite - framesFilled;
                    ma_uint64 framesRead = 0;
                    ma_decoder_read_pcm_frames(&voice->decoder, readBuffer, framesRequested, &framesRead);

                    for (ma_uint32 f = 0; f < (ma_uint32)framesRead; ++f) {
                        for (ma_uint32 c = 0; c < kChannels; ++c) {
                            mixBuffer[(framesFilled + f) * kChannels + c] +=
                                readBuffer[f * kChannels + c] * voice->volume;
                        }
                    }
                    framesFilled += (ma_uint32)framesRead;

                    if ((ma_uint32)framesRead < framesRequested) {
                        // Hit EOF before filling the requested amount.
                        if (voice->loop && framesRead > 0) {
                            // Made forward progress this pass -- safe to loop and keep filling
                            // the rest of this chunk from the start of the file.
                            ma_decoder_seek_to_pcm_frame(&voice->decoder, 0);
                            continue;
                        }
                        if (voice->loop) {
                            // framesRead == 0 even after seeking to 0 means a decoder that can't
                            // produce any frames at all (empty/corrupt file) -- bail out of this
                            // voice for this chunk rather than spinning forever on a 0-progress
                            // loop attempt.
                            ma_decoder_seek_to_pcm_frame(&voice->decoder, 0);
                        }
                        finished = !voice->loop;
                        break;
                    }
                }

                if (finished) {
                    slot.voice.reset(); // Voice's destructor uninits the decoder
                    slot.generation++;  // invalidates any handle still referring to this voice
                }
            }
        }

        // Clip -- summing multiple voices can exceed [-1, 1]; a hard clip is preferable to
        // wraparound distortion from an unclamped float being converted downstream.
        float* __restrict out = static_cast<float*>(pWriteBuffer);
        const float* __restrict mix = mixBuffer;
        for (ma_uint32 i = 0; i < framesToWrite * kChannels; ++i) {
            float s = mix[i];
            out[i] = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
        }


        ma_pcm_rb_commit_write(&m_RingBuffer, framesToWrite);
    }

    m_MixThreadStopped.store(true, std::memory_order_release);
}

void SoundManager::Shutdown() {
    if (m_MixTask) {
        m_MixTask->Stop(); // cooperative -- sets Task::stopFlag, MixLoop() checks it each iteration
        // Do NOT touch m_MixTask again past this point: the instant MixLoop() actually returns,
        // Thread::Worker()'s fastJob cleanup destructs and frees it back to the TaskAllocator
        // (possibly to be reused by an unrelated CreateTask call). Wait on our OWN flag instead,
        // which MixLoop() sets right before returning.
        while (!m_MixThreadStopped.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        m_MixTask = nullptr;
    }

    // Safe without the lock now -- the mix thread is confirmed stopped and touches m_VoiceSlots
    // nowhere else. Each Voice's destructor uninits its decoder.
    m_VoiceSlots.clear();

    if (m_DeviceInitialized) {
        ma_device_uninit(&m_Device);
        m_DeviceInitialized = false;
    }
    if (m_RingBufferInitialized) {
        ma_pcm_rb_uninit(&m_RingBuffer);
        m_RingBufferInitialized = false;
    }
    if (m_ContextInitialized) {
        ma_context_uninit(&m_Context);
        m_ContextInitialized = false;
    }
}
