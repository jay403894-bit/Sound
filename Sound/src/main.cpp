/*

#include <iostream>
#include <chrono>
#include <thread>
#include <TaskScheduler.h>
#include "../include/SoundManager.h"

int main() {
    JGL::TaskScheduler::Init();

    SoundManager sound;
    // Core 1 (1-based, matches PushFork's cpu_affinity) is pinned for the mixing loop's whole
    // lifetime -- see SoundManager.h's class comment for why this doesn't fight the rest of the
    // pool.
    if (!sound.Initialize(1)) {
        std::cout << "SoundManager::Initialize failed -- is a playback device available?\n";
        return 1;
    }

    // Point these at real files to try it -- WAV/MP3/FLAC all decode via the same PlaySound/
    // PlayLoop call, miniaudio picks the right decoder from the file itself.
    SoundHandle music = sound.PlayLoop("music.mp3", 0.5f);
    if (!music.IsValid()) {
        std::cout << "PlayLoop(\"music.mp3\") failed to load -- put a real file next to the exe to test.\n";
    }
    SoundHandle effect = sound.PlaySound("effect.wav", 1.0f);
    if (!effect.IsValid()) {
        std::cout << "PlaySound(\"effect.wav\") failed to load -- put a real file next to the exe to test.\n";
    }

    std::cout << "Playing for 4 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // Handle-based control: fade the music down and stop it early, independent of the one-shot
    // effect (which either already finished on its own or keeps playing -- IsPlaying() tells you
    // which without needing to track duration yourself).
    sound.SetVolume(music, 0.1f);
    std::cout << "effect still playing: " << (sound.IsPlaying(effect) ? "yes" : "no") << "\n";

    std::cout << "Playing for 4 more seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(4));
    sound.Stop(music);

    std::cout << "Shutting down...\n";
    sound.Shutdown();

    JGL::TaskScheduler::Instance().Join();
    return 0;
}
*/
