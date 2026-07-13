#pragma once

namespace anvil::app {

// Which runtime backend the player uses for the current session.
enum class PlaybackBackend {
    NativeFfmpegD3D12,
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
    InspectorRecent,
    InspectorFolder,
    InspectorMedia,
    InspectorSystem,
    InspectorLog,
    ToggleDolbyVisionHdr,
    ToggleDolbyVisionCmv4Approx,
    ResetHdrToneCurve,
    ToggleHdrToneCurveExpanded,
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
    HdrColor,
    DolbyVisionColor,
};

// Visual style of a UI button.
enum class ButtonKind {
    Icon,
    TransportPrimary,
    TransportIcon,
    TransportLabel,
    Tab,
    InspectorTab,
};

// Which inspector panel is currently visible.
enum class InspectorTab {
    Recent,
    Folder,
    Media,
    System,
    Log,
    Settings,
};

}  // namespace anvil::app
