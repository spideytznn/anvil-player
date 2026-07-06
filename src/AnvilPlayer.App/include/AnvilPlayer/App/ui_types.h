#pragma once

namespace anvil::app {

// Which runtime backend the player uses for the current session.
enum class PlaybackBackend {
    NativeFfmpegD3D11,
    EmbeddedFfplay,
    RawFrameBridge,
};

// User-facing UI commands dispatched by the transport bar / inspector.
enum class Command {
    Open,
    Back,
    PlayPause,
    Stop,
    Forward,
    VolumeDown,
    VolumeUp,
    SubtitleMenu,
    Fullscreen,
    Settings,
    ToggleSidebar,
    InspectorMedia,
    InspectorDevice,
    InspectorLog,
    ToggleDolbyVisionHdr,
    ResetHdrToneCurve,
};

// Which icon asset a button shows. Mirrors assets/icons/*.png filenames.
enum class IconKind {
    None,
    Folder,
    Cog,
    Back10,
    Play,
    Pause,
    Stop,
    Forward10,
    VolumeDown,
    VolumeUp,
    Subtitles,
    Fullscreen,
    Windowed,
    ChevronLeft,
    ChevronRight,
};

// Visual style of a UI button.
enum class ButtonKind {
    Icon,
    TransportPrimary,
    Tab,
};

// Which inspector panel is currently visible.
enum class InspectorTab {
    Media,
    Device,
    Log,
    Settings,
};

}  // namespace anvil::app
