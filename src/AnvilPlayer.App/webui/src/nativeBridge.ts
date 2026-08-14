export interface PlayerState {
  uiLanguage: 'en' | 'zh'
  playbackState: 'Empty' | 'Stopped' | 'Playing' | 'Paused' | 'Opening' | string
  lastError: string
  mediaName: string
  mediaPath: string
  hasMedia: boolean
  hasVideo: boolean
  hasAudio: boolean
  positionMs: number
  durationMs: number
  bufferedEndMs: number
  buffering: boolean
  networkKbps: number
  volume: number
  runtimeLabel: string
  backendLabel: string
  sidebarCollapsed: boolean
  fullscreen: boolean
  customTitleBar: boolean
  hardwareDecodeEnabled: boolean
  frameInterpolationEnabled: boolean
  frameInterpolationMaximumHeight: number
  frameInterpolationActive: boolean
  frameInterpolationMultiplier: number
  frameInterpolationBackend: string
  frameInterpolationReason: string
  refreshRateSyncEnabled: boolean
  refreshRateMaximumMultiple: boolean
  refreshRateSyncUnavailable: boolean
  refreshRateSyncActive: boolean
  refreshRateSyncHz: number
  fullscreenTransportVisible: boolean
  subtitleMenuOpen: boolean
  inspectorTab: 'recent' | 'folder' | 'media' | 'system' | 'log' | 'settings'
  hdrAvailable: boolean
  hdrOutput: boolean
  hdrOutputLocked: boolean
  windowsHdrEnabled: boolean
  hdrDisplayPeakAutomatic: boolean
  hdrDisplayPeakConfiguredNits: number
  hdrDisplayPeakDetectedNits: number
  hdrDisplayPeakEffectiveNits: number
  autoDisplayFormat: boolean
  displayMetadataPassthrough: boolean
  dolbyVisionSystemPipelineExperimental: boolean
  dolbyVisionSystemPipelineAvailable: boolean
  dolbyVisionMedia: boolean
  cmv4Available: boolean
  cmv4Enabled: boolean
  audioPassthroughRequested: boolean
  audioPassthroughActive: boolean
  audioPassthroughReason: string
  audioPassthroughCodec: string
  audioPassthroughOutput: string
  audioSelectedTrack: number
  subtitleSelectedTrack: number
  subtitleDelayMs: number
  subtitleFontScale: number
  subtitleOffsetX: number
  subtitleOffsetY: number
  assrtConfigured: boolean
  danmakuEnabled: boolean
  danmakuMode: number
  danmakuOpacityPercent: number
  danmakuSpeedPercent: number
  danmakuPath: string
  audioTracks: TrackOption[]
  subtitleTracks: TrackOption[]
  hdrToneCurveAvailable: boolean
  hdrToneCurvePeakNits: number
  hdrToneCurve: HdrToneCurvePoint[]
  container: string
  videoCodec: string
  audioCodec: string
  hdrFormat: string
  resolution: string
  frameRate: string
  streamCount: number
  recentMedia: MediaListItem[]
  folderMedia: MediaListItem[]
  logLines: string[]
}

export interface MediaListItem {
  name: string
  path: string
}

export interface LocalFolderPickItem {
  name: string
  path: string
  playbackPath?: string
  modifiedAt?: number
  sizeBytes?: number
}

export interface LocalFolderPickResult {
  type: 'localFolderPicked'
  folder: {
    name: string
    path: string
  }
  items: LocalFolderPickItem[]
  truncated?: boolean
  scanPending?: boolean
}

export interface LocalFolderPickCancelled {
  type: 'localFolderPickCancelled'
}

export interface LocalFolderPickFailed {
  type: 'localFolderPickFailed'
  message: string
}

export interface LocalFolderScanCompleted {
  type: 'localFolderScanCompleted'
  folder: {
    name: string
    path: string
  }
  items: LocalFolderPickItem[]
  truncated?: boolean
}

export interface LocalFolderScanFailed {
  type: 'localFolderScanFailed'
  folder: {
    name: string
    path: string
  }
  message: string
}

export interface SmbDirectoryEntry {
  name: string
  path: string
}

export interface SmbDirectoryListed {
  type: 'smbDirectoryListed'
  requestId: string
  path: string
  directories: SmbDirectoryEntry[]
}

export interface SmbDirectoryFailed {
  type: 'smbDirectoryFailed'
  requestId: string
  path: string
  message: string
}

export interface WebDavDirectoryEntry {
  name: string
  path: string
}

export interface WebDavDirectoryListed {
  type: 'webDavDirectoryListed'
  requestId: string
  path: string
  directories: WebDavDirectoryEntry[]
}

export interface WebDavDirectoryFailed {
  type: 'webDavDirectoryFailed'
  requestId: string
  path: string
  message: string
}

export interface MediaDetailsProbed {
  type: 'mediaDetailsProbed'
  requestId: string
  path: string
  videoSpec: string
  audioSpec: string
  streamSpecs: Array<{
    id: string
    type: 'video' | 'audio' | 'subtitle'
    title: string
    subtitle: string
    details: Array<{ label: string; value: string }>
  }>
}

export interface MediaDetailsProbeFailed {
  type: 'mediaDetailsProbeFailed'
  requestId: string
  message: string
}

export interface AssrtSubtitleCandidate {
  id: number
  name: string
  videoName: string
  language: string
  format: string
  releaseSite: string
  uploadTime: string
  score: number
}

export type NativeMessage =
  | { type?: 'state'; state?: PlayerState }
  | LocalFolderPickResult
  | LocalFolderPickCancelled
  | LocalFolderPickFailed
  | LocalFolderScanCompleted
  | LocalFolderScanFailed
  | SmbDirectoryListed
  | SmbDirectoryFailed
  | WebDavDirectoryListed
  | WebDavDirectoryFailed
  | MediaDetailsProbed
  | MediaDetailsProbeFailed
  | { type: 'windowChrome'; customTitleBar: boolean }
  | { type: 'globalVideoPassthroughSettings'; frameInterpolationEnabled: boolean; frameInterpolationMaximumHeight: number; autoDisplayFormat: boolean; displayMetadataPassthrough: boolean; dolbyVisionSystemPipelineExperimental: boolean; windowsHdrEnabled: boolean; displayPeakBrightnessNits: number; detectedDisplayPeakBrightnessNits: number }
  | { type: 'globalAudioPassthroughSettings'; enabled: boolean }
  | { type: 'deliverEmbyPlaybackReport'; report: unknown }
  | { type: 'playerClosing' }
  | { type: 'command'; command: 'localPlaybackProgress'; path: string; providerItemId?: string; positionMs: number; durationMs: number; playbackState: string }
  | { type: 'bilibiliTrailerSearchCompleted'; requestId: string; response: unknown }
  | { type: 'bilibiliTrailerSearchFailed'; requestId: string; message: string }
  | { type: 'assrtSubtitleSearchCompleted'; requestId: string; items: AssrtSubtitleCandidate[] }
  | { type: 'assrtSubtitleSearchFailed'; requestId: string; message: string }
  | { type: 'assrtSubtitleDownloadCompleted'; requestId: string; fileName: string }
  | { type: 'assrtSubtitleDownloadFailed'; requestId: string; message: string }
  | { type: 'assrtTokenSaveFailed'; requestId: string; message: string }

export interface TrackOption {
  index: number
  label: string
  detail: string
}

export interface HdrToneCurvePoint {
  index: number
  inputNits: number
  outputNits: number
}

export type NativeCommand =
  | { type: 'command'; command: 'open' }
  | { type: 'command'; command: 'openPath'; path: string; startPositionRatio?: number }
  | { type: 'command'; command: 'pickLocalFolder' }
  | { type: 'command'; command: 'scanLocalFolder'; path: string; username?: string; password?: string }
  | { type: 'command'; command: 'listSmbDirectory'; requestId: string; host: string; path?: string; username?: string; password?: string }
  | { type: 'command'; command: 'connectSmbShare'; path: string; username?: string; password?: string }
  | { type: 'command'; command: 'listWebDavDirectory'; requestId: string; url: string; username?: string; password?: string }
  | { type: 'command'; command: 'scanWebDavFolder'; url: string; name?: string; username?: string; password?: string }
  | { type: 'command'; command: 'cancelLibraryScan'; path: string; webDav: boolean }
  | { type: 'command'; command: 'cancelLibraryProbe'; requestId: string }
  | { type: 'command'; command: 'debugLog'; message: string }
  | { type: 'command'; command: 'searchBilibiliTrailers'; requestId: string; keyword: string }
  | { type: 'command'; command: 'probeMediaDetails'; requestId: string; path: string; username?: string; password?: string }
  | { type: 'command'; command: 'setWebUiRoute'; route: 'player' | 'library' }
  | { type: 'command'; command: 'requestPlayback'; path: string; startPositionRatio?: number; audioTrackIndex?: number; subtitleTrackIndex?: number }
  | { type: 'command'; command: 'localPlaybackProgress'; path: string; providerItemId?: string; positionMs: number; durationMs: number; playbackState: string }
  | { type: 'command'; command: 'openExternalUrl'; url: string }
  | { type: 'command'; command: 'focusPlayer' }
  | { type: 'command'; command: 'requestWindowChrome' }
  | { type: 'command'; command: 'beginWindowDrag' }
  | { type: 'command'; command: 'minimizeWindow' }
  | { type: 'command'; command: 'toggleMaximizeWindow' }
  | { type: 'command'; command: 'closeWindow' }
  | { type: 'command'; command: 'deliverEmbyPlaybackReport'; report: unknown }
  | { type: 'command'; command: 'requestEmbyPlaybackReport' }
  | { type: 'command'; command: 'acknowledgeEmbyPlaybackReport'; reportId: string }
  | { type: 'command'; command: 'setAllowInsecureCertificates'; enabled: boolean }
  | { type: 'command'; command: 'setLibraryWebViewMuted'; muted: boolean }
  | { type: 'command'; command: 'playPause' }
  | { type: 'command'; command: 'stop' }
  | { type: 'command'; command: 'back' }
  | { type: 'command'; command: 'forward' }
  | { type: 'command'; command: 'toggleSidebar' }
  | { type: 'command'; command: 'toggleFullscreen' }
  | { type: 'command'; command: 'setHardwareDecode'; enabled: boolean }
  | { type: 'command'; command: 'setFrameInterpolation'; enabled: boolean }
  | { type: 'command'; command: 'setFrameInterpolationMaximumHeight'; maximumHeight: number }
  | { type: 'command'; command: 'setRefreshRateSync'; enabled: boolean }
  | { type: 'command'; command: 'setRefreshRateMaximumMultiple'; enabled: boolean }
  | { type: 'command'; command: 'dismissRefreshRateSyncUnavailable' }
  | { type: 'command'; command: 'setGlobalRefreshRateSync'; enabled: boolean }
  | { type: 'command'; command: 'setGlobalRefreshRateMaximumMultiple'; enabled: boolean }
  | { type: 'command'; command: 'setGlobalFrameInterpolation'; enabled: boolean }
  | { type: 'command'; command: 'setGlobalFrameInterpolationMaximumHeight'; maximumHeight: number }
  | { type: 'command'; command: 'requestGlobalVideoPassthroughSettings' }
  | { type: 'command'; command: 'setGlobalAutoDisplayFormat'; enabled: boolean }
  | { type: 'command'; command: 'setGlobalDisplayMetadataPassthrough'; enabled: boolean }
  | { type: 'command'; command: 'setGlobalDisplayPeakBrightness'; peakNits: number }
  | { type: 'command'; command: 'setGlobalDolbyVisionSystemPipelineExperimental'; enabled: boolean }
  | { type: 'command'; command: 'requestGlobalAudioPassthroughSettings' }
  | { type: 'command'; command: 'setGlobalAudioPassthrough'; enabled: boolean }
  | { type: 'command'; command: 'setUiLanguage'; language: 'en' | 'zh' }
  | { type: 'command'; command: 'showFullscreenTransport' }
  | { type: 'command'; command: 'subtitleMenu' }
  | { type: 'command'; command: 'hideSubtitleMenu' }
  | { type: 'command'; command: 'settings' }
  | { type: 'command'; command: 'inspectorRecent' }
  | { type: 'command'; command: 'inspectorFolder' }
  | { type: 'command'; command: 'inspectorMedia' }
  | { type: 'command'; command: 'inspectorSystem' }
  | { type: 'command'; command: 'inspectorLog' }
  | { type: 'command'; command: 'toggleHdr' }
  | { type: 'command'; command: 'toggleCmv4' }
  | { type: 'command'; command: 'setAutoDisplayFormat'; enabled: boolean }
  | { type: 'command'; command: 'setDisplayPeakBrightness'; peakNits: number }
  | { type: 'command'; command: 'setDisplayMetadataPassthrough'; enabled: boolean }
  | { type: 'command'; command: 'setDolbyVisionSystemPipelineExperimental'; enabled: boolean }
  | { type: 'command'; command: 'setCurrentAudioPassthrough'; enabled: boolean }
  | { type: 'command'; command: 'setAudioTrack'; index: number }
  | { type: 'command'; command: 'setSubtitleTrack'; index: number }
  | { type: 'command'; command: 'setSubtitleDelay'; delayMs: number }
  | { type: 'command'; command: 'setSubtitleFontScale'; scale: number }
  | { type: 'command'; command: 'setSubtitleOffset'; x: number; y: number }
  | {
      type: 'command'
      command: 'subtitleGeometry'
      scale: number
      anchorLeft: number
      anchorTop: number
      anchorWidth: number
      anchorHeight: number
      popoverLeft: number
      popoverTop: number
      popoverWidth: number
      popoverHeight: number
    }
  | {
      type: 'command'
      command: 'transportGeometry'
      scale: number
      left: number
      top: number
      width: number
      height: number
    }
  | {
      type: 'command'
      command: 'videoGeometry'
      scale: number
      left: number
      top: number
      width: number
      height: number
    }
  | { type: 'command'; command: 'openSubtitleFile' }
  | { type: 'command'; command: 'setAssrtToken'; token: string }
  | { type: 'command'; command: 'searchAssrtSubtitles'; requestId: string; query: string }
  | { type: 'command'; command: 'downloadAssrtSubtitle'; requestId: string; subtitleId: number }
  | { type: 'command'; command: 'toggleDanmakuEnabled' }
  | { type: 'command'; command: 'cycleDanmakuMode' }
  | { type: 'command'; command: 'setDanmakuOpacity'; opacityPercent: number }
  | { type: 'command'; command: 'setDanmakuSpeed'; speedPercent: number }
  | { type: 'command'; command: 'openDanmakuFile' }
  | { type: 'command'; command: 'resetHdrToneCurve' }
  | { type: 'command'; command: 'toggleHdrToneCurveExpanded' }
  | { type: 'command'; command: 'setHdrToneCurvePoint'; index: number; outputNits: number }
  | {
      type: 'command'
      command: 'setHdrToneCurvePoints'
      output1?: number
      output2?: number
      output3?: number
      output4?: number
      output5?: number
      output6?: number
      output7?: number
      output8?: number
    }
  | { type: 'command'; command: 'setVolume'; volume: number }
  | { type: 'command'; command: 'seekToRatio'; ratio: number }

export const EMPTY_STATE: PlayerState = {
  uiLanguage: 'zh',
  playbackState: 'Empty',
  lastError: '',
  mediaName: 'No media loaded',
  mediaPath: '',
  hasMedia: false,
  hasVideo: false,
  hasAudio: false,
  positionMs: 0,
  durationMs: 0,
  bufferedEndMs: 0,
  buffering: false,
  networkKbps: 0,
  volume: 1,
  runtimeLabel: 'Native FFmpeg',
  backendLabel: 'Native FFmpeg / D3D11',
  sidebarCollapsed: false,
  fullscreen: false,
  customTitleBar: false,
  hardwareDecodeEnabled: true,
  frameInterpolationEnabled: false,
  frameInterpolationMaximumHeight: 1080,
  frameInterpolationActive: false,
  frameInterpolationMultiplier: 1,
  frameInterpolationBackend: 'inactive',
  frameInterpolationReason: '',
  refreshRateSyncEnabled: false,
  refreshRateMaximumMultiple: true,
  refreshRateSyncUnavailable: false,
  refreshRateSyncActive: false,
  refreshRateSyncHz: 0,
  fullscreenTransportVisible: true,
  subtitleMenuOpen: false,
  inspectorTab: 'recent',
  hdrAvailable: false,
  hdrOutput: false,
  hdrOutputLocked: false,
  windowsHdrEnabled: false,
  hdrDisplayPeakAutomatic: true,
  hdrDisplayPeakConfiguredNits: 0,
  hdrDisplayPeakDetectedNits: 0,
  hdrDisplayPeakEffectiveNits: 1000,
  autoDisplayFormat: false,
  displayMetadataPassthrough: false,
  dolbyVisionSystemPipelineExperimental: false,
  dolbyVisionSystemPipelineAvailable: false,
  dolbyVisionMedia: false,
  cmv4Available: false,
  cmv4Enabled: false,
  audioPassthroughRequested: false,
  audioPassthroughActive: false,
  audioPassthroughReason: 'disabled',
  audioPassthroughCodec: '',
  audioPassthroughOutput: 'wasapi shared pcm',
  audioSelectedTrack: -2,
  subtitleSelectedTrack: -2,
  subtitleDelayMs: 0,
  subtitleFontScale: 1,
  subtitleOffsetX: 0,
  subtitleOffsetY: 0,
  assrtConfigured: false,
  danmakuEnabled: false,
  danmakuMode: 0,
  danmakuOpacityPercent: 70,
  danmakuSpeedPercent: 100,
  danmakuPath: '',
  audioTracks: [],
  subtitleTracks: [],
  hdrToneCurveAvailable: false,
  hdrToneCurvePeakNits: 1000,
  hdrToneCurve: [],
  container: '',
  videoCodec: '',
  audioCodec: '',
  hdrFormat: '',
  resolution: '',
  frameRate: '',
  streamCount: 0,
  recentMedia: [],
  folderMedia: [],
  logLines: []
}

declare global {
  interface Window {
    chrome?: {
      webview?: {
        postMessage: (message: unknown) => void
        addEventListener: (type: 'message', listener: (event: MessageEvent) => void) => void
        removeEventListener: (type: 'message', listener: (event: MessageEvent) => void) => void
      }
    }
  }
}

export function postNativeCommand(command: NativeCommand): void {
  if (window.chrome?.webview) {
    window.chrome.webview.postMessage(command)
    return
  }
  window.dispatchEvent(new CustomEvent('anvil-debug-command', { detail: command }))
}

export function subscribeNativeMessages(onMessage: (message: NativeMessage) => void): () => void {
  const handler = (event: MessageEvent): void => {
    onMessage(event.data as NativeMessage)
  }

  window.chrome?.webview?.addEventListener('message', handler)
  return () => window.chrome?.webview?.removeEventListener('message', handler)
}

export function subscribeNativeState(onState: (state: PlayerState) => void): () => void {
  const handler = (event: MessageEvent): void => {
    const payload = event.data as NativeMessage
    if (payload?.type === 'state' && payload.state) {
      onState({ ...EMPTY_STATE, ...payload.state })
    }
  }

  window.chrome?.webview?.addEventListener('message', handler)
  window.chrome?.webview?.postMessage({ type: 'requestState' })
  return () => window.chrome?.webview?.removeEventListener('message', handler)
}
