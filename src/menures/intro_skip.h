#pragma once

// The start-up logos (intro_skip.cpp): skips the boot video and the Game Freak logo with the same calls the game's
// own click-to-skip makes, once the game allows skipping; and logs slow frames during start-up.
namespace IntroSkip
{
    // On the game thread at the first tick, after Engine::CheckRuntime passed (the boot map is loaded by then).
    void Setup();
    // Every tick after Setup, after the engine's own tick.
    void OnTick();
    // From the StaticConstructObject_Internal hook, on any thread: notes a new Game Freak logo widget.
    void OnConstruct(void* object);
}
