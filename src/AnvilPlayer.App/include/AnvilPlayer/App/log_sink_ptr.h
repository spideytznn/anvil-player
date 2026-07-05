#pragma once

#include "AnvilPlayer/Playback/PlayerController.h"

#include <memory>

namespace anvil::app {

// Shared alias for the in-memory log sink injected into decoders/renderers.
using LogSinkPtr = std::shared_ptr<anvil::playback::InMemoryLogSink>;

}  // namespace anvil::app
