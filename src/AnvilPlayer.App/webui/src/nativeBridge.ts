export interface PlayerState {
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
  fullscreenTransportVisible: boolean
  subtitleMenuOpen: boolean
  inspectorTab: 'recent' | 'folder' | 'media' | 'system' | 'log' | 'settings'
  hdrAvailable: boolean
  hdrOutput: boolean
  cmv4Available: boolean
  cmv4Enabled: boolean
  audioSelectedTrack: number
  subtitleSelectedTrack: number
  subtitleDelayMs: number
  subtitleFontScale: number
  subtitleOffsetX: number
  subtitleOffsetY: number
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
  | { type: 'command'; command: 'debugLog'; message: string }
  | { type: 'command'; command: 'setWebUiRoute'; route: 'player' | 'library' }
  | { type: 'command'; command: 'setAllowInsecureCertificates'; enabled: boolean }
  | { type: 'command'; command: 'playPause' }
  | { type: 'command'; command: 'stop' }
  | { type: 'command'; command: 'back' }
  | { type: 'command'; command: 'forward' }
  | { type: 'command'; command: 'toggleSidebar' }
  | { type: 'command'; command: 'toggleFullscreen' }
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
  | { type: 'command'; command: 'openSubtitleFile' }
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
  fullscreenTransportVisible: true,
  subtitleMenuOpen: false,
  inspectorTab: 'recent',
  hdrAvailable: false,
  hdrOutput: false,
  cmv4Available: false,
  cmv4Enabled: false,
  audioSelectedTrack: -2,
  subtitleSelectedTrack: -2,
  subtitleDelayMs: 0,
  subtitleFontScale: 1,
  subtitleOffsetX: 0,
  subtitleOffsetY: 0,
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
