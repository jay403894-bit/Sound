#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <AssetManager.h>
#include "../include/miniaudio.h"

// winmm.h/Windows.h #defines PlaySound as PlaySoundA/PlaySoundW -- if a consumer includes
// Windows.h before this header (or, like SoundManager.cpp, after it), that macro silently
// mangles every "PlaySound" token below into "PlaySoundW", breaking declaration/definition
// matching. #undef it defensively; harmless if Windows.h was never included in this TU.
#ifdef PlaySound
#undef PlaySound
#endif
namespace JLib {
    struct Task;

    // Generational handle to a playing voice -- index into SoundManager's slot array plus a
    // generation counter, so a handle from a voice that already finished/was stopped can't
    // accidentally refer to a DIFFERENT voice that later reused the same slot (classic ABA problem
    // with plain indices). IsValid() only means "was constructed with a real index," not "still
    // playing" -- use SoundManager::IsPlaying(handle) for that.
    struct SoundHandle {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        bool IsValid() const { return index != UINT32_MAX; }
    };

    // Two threads are involved, and they have very different jobs:
    //
    //   1. miniaudio's own WASAPI device thread (spun up internally by ma_device_start(), fully
    //      outside JLib::TaskScheduler -- the pool has no idea it exists and can't schedule onto it).
    //      Its data callback does the absolute minimum: drain the ring buffer into the WASAPI
    //      buffer. ma_context_config.threadPriority = ma_thread_priority_realtime maps straight to
    //      THREAD_PRIORITY_TIME_CRITICAL on this thread (see miniaudio.h's Win32 backend), so the
    //      OS-facing side of playback is genuinely real-time-safe.
    //
    //   2. The MIXING thread -- this is the one that does actual work (decode/mix/DSP), and it's a
    //      JLib pool worker, pinned here via TaskScheduler::PushFork(). PushFork's immediate-task path
    //      runs a fastJob task inline on the worker's own OS-thread stack with NO fiber acquired
    //      (Thread.cpp's fast path), and marks that core's immediateCoresInUse permanently true for
    //      as long as the task runs -- so PickNextWorker() never routes ordinary pool work onto it.
    //      A never-returning fastJob task is an explicitly supported "long-running subsystem" use
    //      case (see PushToCore's comment in TaskScheduler.cpp). This thread sets its OWN priority
    //      to THREAD_PRIORITY_TIME_CRITICAL directly, since it fully owns that worker thread now.
    //
    // Shutdown is cooperative: Shutdown() calls the saved Task*'s Stop() (sets Task::stopFlag), and
    // MixLoop() checks that flag every iteration. Once MixLoop() returns, Execute() returns, and
    // Thread::Worker() runs its normal fastJob cleanup (~Task(), Free(), clears
    // immediateCoresInUse) -- the core rejoins the ordinary pool automatically. Shutdown() does NOT
    // touch the Task* again after calling Stop() (it may already be freed/reused the instant the
    // mix loop actually exits) -- it waits on m_MixThreadStopped instead, a plain atomic<bool> this
    // class owns, set by MixLoop() itself right before returning.
    class SoundManager
    {
    public:
        ~SoundManager();

        // coreID matches TaskScheduler::PushFork's cpu_affinity: 1-based, 1..workerCount. Must be
        // called after JLib::TaskScheduler::Init().
        bool Initialize(size_t coreID);
        // Cooperatively stops the mixing task and tears down the WASAPI device/context. Safe to call
        // even if Initialize() failed partway through.
        void Shutdown();

        // Fire-and-forget one-shot playback (sound effects) -- decodes whatever miniaudio's built-in
        // decoders support (WAV/MP3/FLAC) via ma_decoder, converted to the device's format/channels/
        // sample rate at load time so the mix loop never needs to resample per-voice. The file is
        // opened/probed on the CALLING thread (real disk I/O, not real-time-safe) -- only the fully-
        // initialized decoder gets handed to the mix thread. Returns an invalid handle
        // (!handle.IsValid()) if the file couldn't be opened/decoded (bad path, unsupported format).
        // The handle is still usable to Stop()/SetVolume() after a one-shot finishes on its own --
        // those calls just silently no-op once IsPlaying() would return false.
        SoundHandle PlaySound(const char* filePath, float volume = 1.0f);
        // Same as PlaySound, but loops forever (seeks back to frame 0 on EOF) -- for music/ambience.
        SoundHandle PlayLoop(const char* filePath, float volume = 1.0f);
        // Stops and frees a voice immediately (no fade). Safe to call with an already-invalid/
        // already-stopped/stale handle -- it's just checked and ignored.
        void Stop(SoundHandle handle);
        // Safe to call every frame from game logic (e.g. music ducking, distance attenuation) --
        // just a locked float write, no reallocation.
        void SetVolume(SoundHandle handle, float volume);
        // False for an invalid handle, a stopped/finished voice, or a stale handle whose slot got
        // reused by a different voice since.
        bool IsPlaying(SoundHandle handle) const;
        // True if ANY voice is currently active, no handle needed -- for "is something already
        // playing" checks that don't care which specific sound (e.g. don't stack a second music
        // track on top of whatever's already going). Distinct from IsPlaying(SoundHandle): that
        // answers "is THIS specific voice still alive," this answers "is the mixer doing anything
        // at all right now."
        bool IsAnythingPlaying() const;

    private:
        SoundHandle PlayFile(const char* filePath, float volume, bool loop);
        // Caller must hold m_VoicesMutex.
        bool IsValidLocked(SoundHandle handle) const;

        // Runs on the forked JLib worker thread -- see the class comment above. Never calls
        // WaitOnEvent*/anything that suspends (would violate the fastJob contract and throw).
        void MixLoop();

        // miniaudio's device data callback -- runs on miniaudio's OWN thread, not the JLib pool.
        // Just drains m_RingBuffer into pOutput; underrun (mix thread fell behind) is filled with
        // silence rather than stalling or reading garbage.
        static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

        ma_context m_Context{};
        ma_device m_Device{};
        ma_pcm_rb m_RingBuffer{};
        bool m_ContextInitialized = false;
        bool m_DeviceInitialized = false;
        bool m_RingBufferInitialized = false;

        JLib::Task* m_MixTask = nullptr;
        std::atomic<bool> m_MixThreadStopped{ false };

        // One playing sound. Heap-allocated (via unique_ptr) so growing m_VoiceSlots never
        // invalidates a decoder's address, even though every access still goes through
        // m_VoicesMutex regardless.
        struct Voice {
            ma_decoder decoder{};
            float volume = 1.0f;
            bool loop = false;
            bool decoderInitialized = false;
            ~Voice() { if (decoderInitialized) ma_decoder_uninit(&decoder); }
        };
        // nullptr voice = empty/free slot. generation increments every time a slot's occupant is
        // removed (finished naturally or Stop()'d) -- NOT when a slot is (re)allocated -- so a
        // SoundHandle captured for one voice can never validate against a different voice that
        // later reused the same index (see SoundHandle's comment).
        struct VoiceSlot {
            std::unique_ptr<Voice> voice;
            uint32_t generation = 0;
        };
        // Guards m_VoiceSlots against PlaySound()/PlayLoop()/Stop()/SetVolume() (called from
        // arbitrary game-logic threads) racing with MixLoop()'s read/mix/erase-finished pass. Held
        // only for the duration of a slot lookup or one chunk's worth of mixing -- short enough not
        // to meaningfully threaten the mix thread's real-time budget. mutable so IsPlaying() can
        // stay const.
        mutable std::mutex m_VoicesMutex;
        std::vector<VoiceSlot> m_VoiceSlots;

        // Shared cache of raw file bytes, keyed by path -- NOT per-voice decoders. Two simultaneous
        // PlaySound() calls for the SAME file (e.g. two gunshots) need INDEPENDENT decode positions,
        // so caching a stateful ma_decoder per key would be wrong (the second play would silently
        // fight the first over one shared read cursor). Caching the raw bytes instead is exactly
        // right: they're genuinely shareable read-only data. Each PlayFile() call decodes its OWN
        // ma_decoder via ma_decoder_init_memory() over this shared buffer -- repeated plays of the
        // same file skip disk I/O entirely after the first load, and LoadAsync() (unused today, but
        // available) lets a caller prefetch a level's sounds ahead of time.
        //
        // Lifetime note: ma_decoder_init_memory does NOT copy pData -- it just stores the pointer
        // (see miniaudio.h) -- so every live Voice decoder holds a raw, unowned pointer into this
        // cache's storage for as long as it plays. This is safe ONLY because: (1) AssetManager's
        // slots live in a deque (stable addresses, never moved/reallocated -- see AssetManager.h),
        // and (2) file-byte assets are never Unload()'d here, so a slot's bytes, once loaded, live
        // for m_FileBytes' entire lifetime (which itself must outlive every Voice -- true here since
        // it's a SoundManager member destructed after Shutdown() has already stopped the mix thread
        // and cleared every Voice).
        AssetManager<std::vector<uint8_t>> m_FileBytes;

        static constexpr ma_uint32 kSampleRate = 48000;
        static constexpr ma_uint32 kChannels = 2;
        static constexpr ma_format kFormat = ma_format_f32;
        // ~170ms of headroom at 48kHz stereo float -- comfortably more than one WASAPI callback
        // period, so the mix thread can run in bursts rather than having to keep up frame-by-frame.
        static constexpr ma_uint32 kRingBufferFrames = 8192;
    };
}