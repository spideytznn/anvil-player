import { useEffect, useLayoutEffect, useMemo, useRef, useState, type CSSProperties, type PointerEvent, type ReactNode, type Ref } from 'react'
import {
  Captions,
  ChevronLeft,
  ChevronRight,
  Folder,
  FolderOpen,
  History,
  Info,
  Library,
  Maximize2,
  Minimize2,
  Monitor,
  Pause,
  Play,
  AlertTriangle,
  RotateCcw,
  ScrollText,
  Settings,
  SkipBack,
  SkipForward,
  SlidersHorizontal,
  Square,
  Volume2
} from 'lucide-react'
import { applyAppearanceSettings } from './appearance'
import {
  clearPendingEmbyPlaybackReport,
  EMBY_PLAYBACK_REPORT_PENDING_EVENT,
  loadPendingEmbyPlaybackReport,
  reportEmbyPlaybackProgress,
  reportEmbyPlaybackStarted,
  reportEmbyPlaybackStopped,
  type EmbyPlaybackReportRecord
} from './manager/embyClient'
import { applyDocumentLanguage, getInitialLanguage, saveUiLanguage, type UiLanguage } from './uiSettings'
import {
  EMPTY_STATE,
  postNativeCommand,
  subscribeNativeState,
  type HdrToneCurvePoint,
  type MediaListItem,
  type NativeCommand,
  type PlayerState,
  type TrackOption
} from './nativeBridge'
import {
  clamp,
  compactPath,
  formatDelay,
  formatPercent,
  formatScale,
  formatSignedPixels,
  formatTime,
  HDR_CURVE_FOCUS_NITS,
  HDR_CURVE_FOCUS_UNIT,
  HDR_CURVE_MAX_NITS,
  rangeStyle,
  toneCurveNitsToUnit,
  toneCurveUnitToNits
} from './player/format'

const en = {
  appTitle: 'Anvil Player',
  none: 'None',
  noMediaLoaded: 'No media loaded',
  ready: 'Ready',
  openMedia: 'Open Media',
  showInspector: 'Show inspector',
  hideInspector: 'Hide inspector',
  openMediaLabel: 'Open media',
  settings: 'Settings',
  inspector: 'Inspector',
  recent: 'Recent',
  folder: 'Folder',
  media: 'Media',
  system: 'System',
  log: 'Log',
  currentFolder: 'Current Folder',
  mediaInfo: 'Media Info',
  recentPlayback: 'Recent Playback',
  noItems: 'No items',
  noLogEntries: 'No log entries',
  backend: 'Backend',
  volume: 'Volume',
  runtime: 'Runtime',
  container: 'Container',
  video: 'Video',
  audio: 'Audio',
  resolution: 'Resolution',
  frameRate: 'Frame rate',
  hdr: 'HDR',
  hdrControls: 'HDR controls',
  hdrOutput: 'HDR output',
  dolbyVision: 'Dolby Vision',
  streams: 'Streams',
  available: 'Available',
  unavailable: 'Unavailable',
  on: 'On',
  off: 'Off',
  auto: 'Auto',
  stream: 'Stream',
  empty: 'Empty',
  stopped: 'Stopped',
  playing: 'Playing',
  paused: 'Paused',
  opening: 'Opening',
  error: 'Error',
  playbackFailed: 'Playback failed',
  playbackFailedHint: 'The player could not open this stream. The source may have rejected the request or the URL may have expired.',
  errorDetails: 'Details',
  retry: 'Retry',
  backToLibrary: 'Back to library',
  hdrCurve: 'HDR Curve',
  reset: 'Reset',
  zoom: 'Zoom',
  peak: 'Peak',
  nits: 'nits',
  language: 'Language',
  mediaLibrary: 'Media library',
  interfaceLanguage: 'Interface language',
  english: 'English',
  chinese: 'Chinese',
  back10: 'Back 10 seconds',
  pause: 'Pause',
  play: 'Play',
  forward10: 'Forward 10 seconds',
  stop: 'Stop',
  subtitles: 'Subtitles',
  subtitleTracks: 'Subtitle tracks',
  audioTracks: 'Audio tracks',
  danmaku: 'Danmaku',
  addSubtitleFile: 'Add subtitle file...',
  addDanmakuFile: 'Add danmaku file...',
  delay: 'Delay',
  subtitleSize: 'Subtitle size',
  horizontalOffset: 'Horizontal offset',
  verticalOffset: 'Vertical offset',
  enableDanmaku: 'Enable danmaku',
  displayMode: 'Display mode',
  opacity: 'Opacity',
  speed: 'Speed',
  danmakuModeScroll: 'Scroll',
  danmakuModeTop: 'Top',
  danmakuModeBottom: 'Bottom',
  fileTypes: 'XML / JSON / ASS',
  enhanced: 'Enhanced',
  exitFullscreen: 'Exit fullscreen',
  fullscreen: 'Fullscreen',
  chineseSimplified: 'Chinese Simplified',
  chineseTraditional: 'Chinese Traditional',
  chineseTrack: 'Chinese',
  englishTrack: 'English',
  japaneseTrack: 'Japanese',
  koreanTrack: 'Korean',
  buffering: 'Buffering',
  waitingForNetwork: 'Waiting for network'
} as const

const zh: Record<keyof typeof en, string> = {
  appTitle: 'Anvil Player',
  none: '无',
  noMediaLoaded: '未加载媒体',
  ready: '就绪',
  openMedia: '打开媒体',
  showInspector: '显示检查器',
  hideInspector: '隐藏检查器',
  openMediaLabel: '打开媒体',
  settings: '设置',
  inspector: '检查器',
  recent: '最近',
  folder: '文件夹',
  media: '媒体',
  system: '系统',
  log: '日志',
  currentFolder: '当前文件夹',
  mediaInfo: '媒体信息',
  recentPlayback: '最近播放',
  noItems: '没有项目',
  noLogEntries: '没有日志',
  backend: '后端',
  volume: '音量',
  runtime: '运行时',
  container: '封装',
  video: '视频',
  audio: '音频',
  resolution: '分辨率',
  frameRate: '帧率',
  hdr: 'HDR',
  hdrControls: 'HDR 控制',
  hdrOutput: 'HDR 输出',
  dolbyVision: '杜比视界',
  streams: '流',
  available: '可用',
  unavailable: '不可用',
  on: '开',
  off: '关',
  auto: '自动',
  stream: '流',
  empty: '空',
  stopped: '已停止',
  playing: '播放中',
  paused: '已暂停',
  opening: '打开中',
  error: '错误',
  playbackFailed: '播放失败',
  playbackFailedHint: '播放器无法打开这个视频流，可能是片源拒绝访问或播放地址已经失效。',
  errorDetails: '详情',
  retry: '重试',
  backToLibrary: '返回媒体库',
  hdrCurve: 'HDR 曲线',
  reset: '重置',
  zoom: '放大',
  peak: '峰值',
  nits: '尼特',
  language: '语言',
  mediaLibrary: '媒体库',
  interfaceLanguage: '界面语言',
  english: 'English',
  chinese: '中文',
  back10: '后退 10 秒',
  pause: '暂停',
  play: '播放',
  forward10: '前进 10 秒',
  stop: '停止',
  subtitles: '字幕',
  subtitleTracks: '字幕轨',
  audioTracks: '音轨',
  danmaku: '弹幕',
  addSubtitleFile: '添加字幕文件...',
  addDanmakuFile: '添加弹幕文件...',
  delay: '延迟',
  subtitleSize: '字幕大小',
  horizontalOffset: '水平偏移',
  verticalOffset: '垂直偏移',
  enableDanmaku: '启用弹幕',
  displayMode: '显示模式',
  opacity: '透明度',
  speed: '速度',
  danmakuModeScroll: '滚动',
  danmakuModeTop: '顶部',
  danmakuModeBottom: '底部',
  fileTypes: 'XML / JSON / ASS',
  enhanced: '增强',
  exitFullscreen: '退出全屏',
  fullscreen: '全屏',
  chineseSimplified: '简体中文',
  chineseTraditional: '繁体中文',
  chineseTrack: '中文',
  englishTrack: '英语',
  japaneseTrack: '日语',
  koreanTrack: '韩语',
  buffering: '缓冲中',
  waitingForNetwork: '等待网络'
}

const copy = { en, zh }

type Copy = Record<keyof typeof en, string>
type SubtitlePanel = 'subtitles' | 'audio' | 'danmaku'

const EMBY_PROGRESS_REPORT_INTERVAL_MS = 5_000

interface ActiveEmbyPlaybackReport {
  report: EmbyPlaybackReportRecord
  matched: boolean
  started: boolean
  stopped: boolean
  lastProgressAt: number
  lastPositionMs: number
  lastPlaybackState: string
}

interface PlaybackUrlIdentity {
  normalized: string
  itemId: string
  mediaSourceId: string
}

function normalizePlaybackUrl(value: string): string {
  try {
    const url = new URL(value)
    for (const key of [...url.searchParams.keys()]) {
      if (/^(api_key|x-emby-token)$/i.test(key)) url.searchParams.delete(key)
    }
    return `${url.origin}${url.pathname}?${[...url.searchParams.entries()]
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, entryValue]) => `${key}=${entryValue}`)
      .join('&')}`.replace(/\?$/, '').toLowerCase()
  } catch {
    return value.trim().replace(/\\/g, '/').toLowerCase()
  }
}

function urlSearchParam(url: URL, name: string): string {
  const lowerName = name.toLowerCase()
  for (const [key, value] of url.searchParams.entries()) {
    if (key.toLowerCase() === lowerName) return value.trim().toLowerCase()
  }
  return ''
}

function decodeUrlPart(value: string): string {
  try {
    return decodeURIComponent(value)
  } catch {
    return value
  }
}

function playbackUrlIdentity(value: string): PlaybackUrlIdentity {
  const normalized = normalizePlaybackUrl(value)
  try {
    const url = new URL(value)
    const pathMatch = /\/(?:videos|items)\/([^/?#]+)|\/users\/[^/?#]+\/items\/([^/?#]+)/i.exec(url.pathname)
    const itemId = decodeUrlPart(pathMatch?.[1] ?? pathMatch?.[2] ?? '').trim().toLowerCase()
    return {
      normalized,
      itemId: itemId || urlSearchParam(url, 'ItemId'),
      mediaSourceId: urlSearchParam(url, 'MediaSourceId')
    }
  } catch {
    return {
      normalized,
      itemId: '',
      mediaSourceId: ''
    }
  }
}

function sameToken(left?: string, right?: string): boolean {
  const a = left?.trim().toLowerCase() ?? ''
  const b = right?.trim().toLowerCase() ?? ''
  return Boolean(a && b && a === b)
}

function playbackPathMatchesEmbyReport(mediaPath: string, report: EmbyPlaybackReportRecord): boolean {
  const media = playbackUrlIdentity(mediaPath)
  const target = playbackUrlIdentity(report.target.url)
  if (!media.normalized || !target.normalized) return false
  if (media.normalized === target.normalized) return true

  const itemId = report.target.itemId.toLowerCase()
  if (sameToken(itemId, media.itemId) || sameToken(itemId, target.itemId)) return true
  if (sameToken(media.itemId, target.itemId)) {
    const mediaSourceId = report.target.mediaSourceId?.toLowerCase() ?? ''
    return !mediaSourceId || !media.mediaSourceId || sameToken(mediaSourceId, media.mediaSourceId)
  }
  return Boolean(itemId && media.normalized.includes(itemId) && target.normalized.includes(itemId))
}

function embyReportEventName(previousState: string, nextState: string): string {
  if (previousState === 'Playing' && nextState === 'Paused') return 'Pause'
  if (previousState === 'Paused' && nextState === 'Playing') return 'Unpause'
  return 'TimeUpdate'
}

function debugEmbyPlaybackReport(message: string): void {
  postNativeCommand({
    type: 'command',
    command: 'debugLog',
    message: `emby playback report ${message}`
  })
}

function stopActiveEmbyPlaybackReport(
  active: ActiveEmbyPlaybackReport | null,
  positionMs: number,
  failed: boolean,
  keepalive = false
): void {
  if (!active || active.stopped) return
  active.stopped = true
  const stoppedAt = Math.max(positionMs, active.lastPositionMs)
  active.lastPositionMs = stoppedAt
  clearPendingEmbyPlaybackReport(active.report.id)
  if (!active.started) return
  const finalProgress = stoppedAt > 0 && !keepalive
    ? reportEmbyPlaybackProgress(active.report, stoppedAt, false, failed ? 'PlaybackError' : 'TimeUpdate')
      .catch((error) => debugEmbyPlaybackReport(`final progress failed itemId=${active.report.target.itemId} error=${error instanceof Error ? error.message : String(error)}`))
    : Promise.resolve()
  void finalProgress
    .then(() => reportEmbyPlaybackStopped(active.report, stoppedAt, failed, keepalive))
    .then(() => debugEmbyPlaybackReport(`stopped itemId=${active.report.target.itemId} positionMs=${Math.round(stoppedAt)}`))
    .catch((error) => debugEmbyPlaybackReport(`stop failed itemId=${active.report.target.itemId} error=${error instanceof Error ? error.message : String(error)}`))
}

function createActiveEmbyPlaybackReport(report: EmbyPlaybackReportRecord): ActiveEmbyPlaybackReport {
  return {
    report,
    matched: false,
    started: false,
    stopped: false,
    lastProgressAt: 0,
    lastPositionMs: 0,
    lastPlaybackState: ''
  }
}

function useEmbyPlaybackReporting(state: PlayerState): void {
  const activeRef = useRef<ActiveEmbyPlaybackReport | null>(null)
  const [pendingPulse, setPendingPulse] = useState(0)

  useEffect(() => {
    activeRef.current = null
    return () => {
      stopActiveEmbyPlaybackReport(activeRef.current, activeRef.current?.lastPositionMs ?? 0, false, true)
    }
  }, [])

  useEffect(() => {
    const refreshPending = (): void => setPendingPulse((value) => value + 1)
    window.addEventListener(EMBY_PLAYBACK_REPORT_PENDING_EVENT, refreshPending)
    const retryTimer = window.setInterval(refreshPending, 1000)
    refreshPending()
    return () => {
      window.removeEventListener(EMBY_PLAYBACK_REPORT_PENDING_EVENT, refreshPending)
      window.clearInterval(retryTimer)
    }
  }, [])

  useEffect(() => {
    let active = activeRef.current
    if (active?.stopped) {
      activeRef.current = null
      active = null
    }
    const pending = loadPendingEmbyPlaybackReport()
    const pendingPathMatches = Boolean(pending && state.hasMedia && playbackPathMatchesEmbyReport(state.mediaPath, pending))
    const activePathMatches = Boolean(active && state.hasMedia && playbackPathMatchesEmbyReport(state.mediaPath, active.report))

    if (active && pending && pending.id !== active.report.id && (!active.started || !active.matched || pendingPathMatches || (active.matched && state.hasMedia && !activePathMatches))) {
      if (active.started) {
        stopActiveEmbyPlaybackReport(active, active.lastPositionMs, false)
      }
      active = createActiveEmbyPlaybackReport(pending)
      activeRef.current = active
      debugEmbyPlaybackReport(`pending itemId=${pending.target.itemId}`)
    }

    if (!active) {
      if (!pending) return
      active = createActiveEmbyPlaybackReport(pending)
      activeRef.current = active
    }

    if (active.stopped) {
      activeRef.current = null
      return
    }

    if (active.matched && state.hasMedia && !activePathMatches) {
      const replacement = pending && pending.id !== active.report.id && pendingPathMatches ? pending : undefined
      stopActiveEmbyPlaybackReport(active, active.lastPositionMs, false)
      if (!replacement) {
        activeRef.current = null
        return
      }
      active = createActiveEmbyPlaybackReport(replacement)
      activeRef.current = active
      debugEmbyPlaybackReport(`switched itemId=${replacement.target.itemId}`)
    }

    const nextActivePathMatches = state.hasMedia && playbackPathMatchesEmbyReport(state.mediaPath, active.report)
    const matched = active.matched || nextActivePathMatches
    if (!matched) return
    active.matched = true

    const positionMs = Math.max(0, state.positionMs)
    if (state.playbackState !== 'Stopped' && state.playbackState !== 'Empty') {
      active.lastPositionMs = positionMs
    }

    if (!state.hasMedia || state.playbackState === 'Stopped' || state.playbackState === 'Empty' || state.playbackState === 'Error') {
      stopActiveEmbyPlaybackReport(active, positionMs, state.playbackState === 'Error')
      return
    }

    const now = Date.now()
    const paused = state.playbackState === 'Paused'
    const eventName = embyReportEventName(active.lastPlaybackState, state.playbackState)

    if (!active.started && (state.playbackState === 'Playing' || state.playbackState === 'Paused' || state.playbackState === 'Ready')) {
      active.started = true
      active.lastProgressAt = now
      active.lastPlaybackState = state.playbackState
      void reportEmbyPlaybackStarted(active.report, positionMs)
        .then(() => reportEmbyPlaybackProgress(active.report, positionMs, paused, 'TimeUpdate'))
        .then(() => debugEmbyPlaybackReport(`started itemId=${active.report.target.itemId}`))
        .catch((error) => debugEmbyPlaybackReport(`start failed itemId=${active.report.target.itemId} error=${error instanceof Error ? error.message : String(error)}`))
      return
    }

    if (!active.started) return

    const stateChanged = eventName !== 'TimeUpdate'
    const due = now - active.lastProgressAt >= EMBY_PROGRESS_REPORT_INTERVAL_MS
    if (stateChanged || due) {
      active.lastProgressAt = now
      void reportEmbyPlaybackProgress(active.report, positionMs, paused, eventName)
        .catch((error) => debugEmbyPlaybackReport(`progress failed itemId=${active.report.target.itemId} error=${error instanceof Error ? error.message : String(error)}`))
    }

    active.lastPlaybackState = state.playbackState
  }, [pendingPulse, state])
}

function displayTrackLabel(label: string, t: Copy): string {
  switch (label) {
    case 'Off': return t.off
    case 'Auto': return t.auto
    case 'Chinese Simplified': return t.chineseSimplified
    case 'Chinese Traditional': return t.chineseTraditional
    case 'Chinese': return t.chineseTrack
    case 'English': return t.englishTrack
    case 'Japanese': return t.japaneseTrack
    case 'Korean': return t.koreanTrack
    default: {
      const streamMatch = /^Stream\s+(.+)$/.exec(label)
      return streamMatch ? `${t.stream} ${streamMatch[1]}` : label
    }
  }
}

interface RectSnapshot {
  left: number
  top: number
  width: number
  height: number
}

const DEFAULT_SUBTITLE_ANCHOR: RectSnapshot = {
  left: 0,
  top: 0,
  width: 30,
  height: 30
}

function rectSnapshotFromElement(element: HTMLElement | null): RectSnapshot {
  if (!element) return DEFAULT_SUBTITLE_ANCHOR
  const rect = element.getBoundingClientRect()
  return {
    left: rect.left,
    top: rect.top,
    width: rect.width,
    height: rect.height
  }
}

function layoutSnapshotFromElement(element: HTMLElement | null): RectSnapshot {
  if (!element) return DEFAULT_SUBTITLE_ANCHOR
  if (element.offsetWidth > 0 && element.offsetHeight > 0) {
    return {
      left: element.offsetLeft,
      top: element.offsetTop,
      width: element.offsetWidth,
      height: element.offsetHeight
    }
  }
  return rectSnapshotFromElement(element)
}

function clampPanelPosition(value: number, size: number, viewportSize: number, margin: number): number {
  const max = Math.max(margin, viewportSize - size - margin)
  return clamp(value, margin, max)
}

function subtitlePopoverLayout(anchor: RectSnapshot): { left: number; top: number; width: number; height: number } {
  const viewportWidth = window.innerWidth || 1280
  const viewportHeight = window.innerHeight || 720
  const compact = viewportWidth <= 900 || viewportHeight <= 560
  const margin = compact ? 10 : 16
  const width = Math.min(compact ? 380 : 420, Math.max(280, viewportWidth - margin * 2))
  const height = Math.min(compact ? 480 : 430, Math.max(260, viewportHeight - (compact ? 126 : 152)))
  return {
    left: clampPanelPosition(anchor.left + anchor.width - width, width, viewportWidth, margin),
    top: clampPanelPosition(anchor.top + anchor.height - height, height, viewportHeight, margin),
    width,
    height
  }
}

function postSubtitleGeometry(anchor: RectSnapshot): void {
  const popover = subtitlePopoverLayout(anchor)
  postNativeCommand({
    type: 'command',
    command: 'subtitleGeometry',
    scale: window.devicePixelRatio || 1,
    anchorLeft: anchor.left,
    anchorTop: anchor.top,
    anchorWidth: anchor.width,
    anchorHeight: anchor.height,
    popoverLeft: popover.left,
    popoverTop: popover.top,
    popoverWidth: popover.width,
    popoverHeight: popover.height
  })
}

function postTransportGeometry(bounds: RectSnapshot): void {
  postNativeCommand({
    type: 'command',
    command: 'transportGeometry',
    scale: window.devicePixelRatio || 1,
    left: bounds.left,
    top: bounds.top,
    width: bounds.width,
    height: bounds.height
  })
}

function danmakuModeLabel(mode: number, t: Copy): string {
  switch (mode) {
    case 1: return t.danmakuModeTop
    case 2: return t.danmakuModeBottom
    default: return t.danmakuModeScroll
  }
}

function formatNits(nits: number, t: Copy): string {
  return `${Math.round(nits)} ${t.nits}`
}

function IconButton({
  label,
  children,
  active = false,
  primary = false,
  disabled = false,
  subtitleToggle = false,
  buttonRef,
  onClick
}: {
  label: string
  children: ReactNode
  active?: boolean
  primary?: boolean
  disabled?: boolean
  subtitleToggle?: boolean
  buttonRef?: Ref<HTMLButtonElement>
  onClick: () => void
}): JSX.Element {
  return (
    <button
      ref={buttonRef}
      type="button"
      className={`line-button icon-button ${primary ? 'is-primary' : ''} ${active ? 'is-active' : ''}`}
      aria-label={label}
      title={label}
      data-subtitle-toggle={subtitleToggle ? 'true' : undefined}
      disabled={disabled}
      onClick={onClick}
    >
      {children}
    </button>
  )
}

function InfoRow({ label, value, emptyLabel }: { label: string; value: ReactNode; emptyLabel: string }): JSX.Element {
  const displayValue = value === null || value === undefined || value === '' ? emptyLabel : value
  return (
    <div className="info-row">
      <span>{label}</span>
      <strong>{displayValue}</strong>
    </div>
  )
}

type HdrCurveBatchCommand = Extract<NativeCommand, { command: 'setHdrToneCurvePoints' }>

interface HdrCurveDrag {
  pointIndex: number
  group: boolean
  selectedIndexes: number[]
  startPointerNits: number
  startOutputs: Map<number, number>
}

interface HdrCurveRangeSelection {
  startX: number
  currentX: number
}

function HdrCurveEditor({ state, t }: { state: PlayerState; t: Copy }): JSX.Element {
  const svgRef = useRef<SVGSVGElement | null>(null)
  const [dragging, setDragging] = useState<HdrCurveDrag | null>(null)
  const [selectedPointIndexes, setSelectedPointIndexes] = useState<number[]>([])
  const [rangeSelection, setRangeSelection] = useState<HdrCurveRangeSelection | null>(null)
  const points = [...state.hdrToneCurve].sort((a, b) => a.index - b.index)

  useEffect(() => {
    setSelectedPointIndexes((current) => {
      const next = current.filter((index) => index > 0 && points.some((point) => point.index === index))
      return next.length === current.length ? current : next
    })
  }, [points])

  if (!state.hdrToneCurveAvailable || points.length === 0) {
    return (
      <div className="control-group">
        <div className="control-title">{t.hdrCurve}</div>
        <div className="empty-list">{t.unavailable}</div>
      </div>
    )
  }

  const viewWidth = 320
  const viewHeight = 178
  const plot = { left: 42, top: 14, width: 252, height: 118 }
  const gridTicks = [0, 250, 500, 750, 1000, 2000, 4000]
  const defaultCurve = [
    { inputNits: 0, outputNits: 0 },
    { inputNits: 50, outputNits: 50 },
    { inputNits: 100, outputNits: 100 },
    { inputNits: 250, outputNits: 235 },
    { inputNits: 400, outputNits: 360 },
    { inputNits: 700, outputNits: 540 },
    { inputNits: 1000, outputNits: 700 },
    { inputNits: 2000, outputNits: 880 },
    { inputNits: 4000, outputNits: 1000 }
  ]

  const pointLimits = (pointIndex: number): { min: number; max: number } => {
    const point = points.find((candidate) => candidate.index === pointIndex)
    if (!point) return { min: 0, max: HDR_CURVE_MAX_NITS }
    const previous = points.find((candidate) => candidate.index === point.index - 1)
    const next = points.find((candidate) => candidate.index === point.index + 1)
    return {
      min: previous ? previous.outputNits : 0,
      max: next ? next.outputNits : HDR_CURVE_MAX_NITS
    }
  }

  const xForNits = (nits: number): number => plot.left + toneCurveNitsToUnit(nits) * plot.width
  const yForNits = (nits: number): number => plot.top + (1 - toneCurveNitsToUnit(nits)) * plot.height
  const pointByIndex = (pointIndex: number): HdrToneCurvePoint | undefined => points.find((point) => point.index === pointIndex)
  const svgPoint = (event: PointerEvent<Element>): { x: number; y: number } => {
    const rect = svgRef.current?.getBoundingClientRect()
    if (!rect || rect.width <= 0 || rect.height <= 0) {
      return { x: plot.left, y: plot.top + plot.height }
    }
    return {
      x: ((event.clientX - rect.left) / rect.width) * viewWidth,
      y: ((event.clientY - rect.top) / rect.height) * viewHeight
    }
  }
  const pointerOutputNits = (event: PointerEvent<Element>): number => {
    const local = svgPoint(event)
    const unitY = 1 - clamp((local.y - plot.top) / plot.height, 0, 1)
    return toneCurveUnitToNits(unitY)
  }
  const sendPointFromPointer = (pointIndex: number, event: PointerEvent<Element>): void => {
    const limits = pointLimits(pointIndex)
    const outputNits = clamp(Math.round(pointerOutputNits(event)), limits.min, Math.max(limits.min, limits.max))
    postNativeCommand({ type: 'command', command: 'setHdrToneCurvePoint', index: pointIndex, outputNits })
  }
  const postPointOutputs = (outputs: Map<number, number>): void => {
    if (outputs.size === 0) return
    const command: HdrCurveBatchCommand = { type: 'command', command: 'setHdrToneCurvePoints' }
    outputs.forEach((outputNits, pointIndex) => {
      switch (pointIndex) {
      case 1:
        command.output1 = outputNits
        break
      case 2:
        command.output2 = outputNits
        break
      case 3:
        command.output3 = outputNits
        break
      case 4:
        command.output4 = outputNits
        break
      case 5:
        command.output5 = outputNits
        break
      case 6:
        command.output6 = outputNits
        break
      case 7:
        command.output7 = outputNits
        break
      case 8:
        command.output8 = outputNits
        break
      default:
        break
      }
    })
    postNativeCommand(command)
  }
  const clampSelectionDelta = (selectedIndexes: number[], startOutputs: Map<number, number>, desiredDelta: number): number => {
    const selectedSet = new Set(selectedIndexes)
    let minDelta = -HDR_CURVE_MAX_NITS
    let maxDelta = HDR_CURVE_MAX_NITS

    selectedIndexes.forEach((pointIndex) => {
      const startOutput = startOutputs.get(pointIndex) ?? pointByIndex(pointIndex)?.outputNits ?? 0
      const previous = pointByIndex(pointIndex - 1)
      const next = pointByIndex(pointIndex + 1)
      const lower = previous && !selectedSet.has(previous.index) ? previous.outputNits : 0
      const upper = next && !selectedSet.has(next.index) ? next.outputNits : HDR_CURVE_MAX_NITS
      minDelta = Math.max(minDelta, lower - startOutput)
      maxDelta = Math.min(maxDelta, upper - startOutput)
    })

    return clamp(desiredDelta, minDelta, Math.max(minDelta, maxDelta))
  }
  const sendDragFromPointer = (drag: HdrCurveDrag, event: PointerEvent<Element>): void => {
    if (!drag.group) {
      sendPointFromPointer(drag.pointIndex, event)
      return
    }

    const desiredDelta = Math.round(pointerOutputNits(event) - drag.startPointerNits)
    const delta = clampSelectionDelta(drag.selectedIndexes, drag.startOutputs, desiredDelta)
    const outputs = new Map<number, number>()
    drag.selectedIndexes.forEach((pointIndex) => {
      const startOutput = drag.startOutputs.get(pointIndex)
      if (startOutput !== undefined) {
        outputs.set(pointIndex, Math.round(clamp(startOutput + delta, 0, HDR_CURVE_MAX_NITS)))
      }
    })
    postPointOutputs(outputs)
  }
  const pointIndexesInRange = (leftX: number, rightX: number): number[] => {
    const rangeLeft = Math.min(leftX, rightX) - 4
    const rangeRight = Math.max(leftX, rightX) + 4
    return points
      .filter((point) => point.index > 0)
      .filter((point) => {
        const x = xForNits(point.inputNits)
        return x >= rangeLeft && x <= rangeRight
      })
      .map((point) => point.index)
  }
  const rangeXFromPointer = (event: PointerEvent<Element>): number => {
    const local = svgPoint(event)
    return clamp(local.x, plot.left, plot.left + plot.width)
  }
  const beginRangeSelection = (event: PointerEvent<Element>): boolean => {
    if (event.button !== 0 || !event.shiftKey) return false
    const local = svgPoint(event)
    if (
      local.x < plot.left ||
      local.x > plot.left + plot.width ||
      local.y < plot.top ||
      local.y > plot.top + plot.height
    ) {
      return false
    }

    event.preventDefault()
    event.stopPropagation()
    svgRef.current?.setPointerCapture(event.pointerId)
    const x = rangeXFromPointer(event)
    setDragging(null)
    setRangeSelection({ startX: x, currentX: x })
    setSelectedPointIndexes(pointIndexesInRange(x, x))
    return true
  }
  const updateRangeSelection = (event: PointerEvent<Element>): void => {
    if (!rangeSelection) return
    const x = rangeXFromPointer(event)
    setRangeSelection({ ...rangeSelection, currentX: x })
    setSelectedPointIndexes(pointIndexesInRange(rangeSelection.startX, x))
  }
  const beginPointDrag = (point: HdrToneCurvePoint, event: PointerEvent<Element>): void => {
    if (point.index === 0 || event.button !== 0 || event.shiftKey) return
    event.preventDefault()
    event.stopPropagation()

    const clickedSelected = selectedPointIndexes.includes(point.index)
    const group = clickedSelected && selectedPointIndexes.length > 1
    const selectedIndexes = group ? selectedPointIndexes : [point.index]
    const startOutputs = new Map(
      selectedIndexes.map((pointIndex) => [pointIndex, pointByIndex(pointIndex)?.outputNits ?? 0] as const)
    )

    svgRef.current?.setPointerCapture(event.pointerId)
    if (!group) {
      setSelectedPointIndexes([point.index])
    }
    const nextDrag = {
      pointIndex: point.index,
      group,
      selectedIndexes,
      startPointerNits: pointerOutputNits(event),
      startOutputs
    }
    setDragging(nextDrag)
    if (!group) {
      sendPointFromPointer(point.index, event)
    }
  }
  const curvePath = points.map((point) => `${xForNits(point.inputNits)},${yForNits(point.outputNits)}`).join(' ')
  const referencePath = defaultCurve.map((point) => `${xForNits(point.inputNits)},${yForNits(point.outputNits)}`).join(' ')
  const selectedSet = new Set(selectedPointIndexes)
  const selectionBand = rangeSelection
    ? {
        x: Math.min(rangeSelection.startX, rangeSelection.currentX),
        width: Math.abs(rangeSelection.currentX - rangeSelection.startX)
      }
    : null

  return (
    <div className="control-group hdr-curve">
      <div className="control-title with-action">
        <span>{t.hdrCurve}</span>
        <div className="curve-actions">
          <button className="line-button mini-text-button" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'resetHdrToneCurve' })}>
            <RotateCcw size={13} />
            <span>{t.reset}</span>
          </button>
          <button className="line-button mini-text-button" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'toggleHdrToneCurveExpanded' })}>
            <Maximize2 size={13} />
            <span>{t.zoom}</span>
          </button>
        </div>
      </div>
      <div className="curve-summary">
        <span>{t.peak}</span>
        <strong>{formatNits(state.hdrToneCurvePeakNits, t)}</strong>
      </div>
      <svg
        ref={svgRef}
        className="hdr-chart"
        viewBox={`0 0 ${viewWidth} ${viewHeight}`}
        role="img"
        aria-label={t.hdrCurve}
        onPointerDownCapture={(event) => {
          beginRangeSelection(event)
        }}
        onPointerDown={(event) => {
          if (event.button !== 0 || event.shiftKey || selectedPointIndexes.length === 0) return
          const target = event.target
          if (target instanceof Element && target.closest('[data-hdr-point="true"]')) return
          setSelectedPointIndexes([])
        }}
        onPointerMove={(event) => {
          if (rangeSelection) {
            updateRangeSelection(event)
            return
          }
          if (dragging) {
            sendDragFromPointer(dragging, event)
          }
        }}
        onPointerUp={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) {
            event.currentTarget.releasePointerCapture(event.pointerId)
          }
          if (rangeSelection) {
            updateRangeSelection(event)
            setRangeSelection(null)
            return
          }
          if (dragging) {
            sendDragFromPointer(dragging, event)
            setDragging(null)
          }
        }}
        onPointerCancel={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) {
            event.currentTarget.releasePointerCapture(event.pointerId)
          }
          setRangeSelection(null)
          setDragging(null)
        }}
      >
        <rect x={plot.left} y={plot.top} width={plot.width} height={plot.height} rx="6" />
        {gridTicks.map((tick) => {
          const x = xForNits(tick)
          const y = yForNits(tick)
          return (
            <g key={tick} className={tick === 0 || tick === 1000 ? 'major' : undefined}>
              <line x1={x} y1={plot.top} x2={x} y2={plot.top + plot.height} />
              <line x1={plot.left} y1={y} x2={plot.left + plot.width} y2={y} />
              <text x={x} y={plot.top + plot.height + 17} textAnchor="middle">{tick >= 1000 ? `${tick / 1000}k` : tick}</text>
              <text x={plot.left - 8} y={y + 4} textAnchor="end">{tick >= 1000 ? `${tick / 1000}k` : tick}</text>
            </g>
          )
        })}
        {selectionBand && selectionBand.width > 0 && <rect className="selection-band" x={selectionBand.x} y={plot.top} width={selectionBand.width} height={plot.height} rx="4" />}
        <polyline className="reference" points={referencePath} />
        <polyline className="curve" points={curvePath} />
        {points.map((point) => {
          const active = dragging?.pointIndex === point.index
          const selected = selectedSet.has(point.index)
          return (
            <g key={point.index}>
              <circle
                className="hit-target"
                data-hdr-point="true"
                cx={xForNits(point.inputNits)}
                cy={yForNits(point.outputNits)}
                r={12}
                onPointerDown={(event) => beginPointDrag(point, event)}
              />
              <circle
                className={`${point.index === 0 ? 'is-anchor' : ''} ${selected ? 'is-selected' : ''} ${active ? 'is-active' : ''}`}
                data-hdr-point="true"
                cx={xForNits(point.inputNits)}
                cy={yForNits(point.outputNits)}
                r={active ? 5.5 : (selected ? 5 : 4.25)}
                onPointerDown={(event) => beginPointDrag(point, event)}
              >
                <title>{`${Math.round(point.inputNits)} -> ${Math.round(point.outputNits)} ${t.nits}`}</title>
              </circle>
            </g>
          )
        })}
      </svg>
    </div>
  )
}

function TopBar({ state, t }: { state: PlayerState; t: Copy }): JSX.Element {
  return (
    <header className="topbar line-panel">
      <div className="brand-area">
        <img className="brand-mark" src="./app-icon.png" alt="" aria-hidden="true" draggable={false} />
        <div className="brand-copy">
          <div className="brand-name">Anvil Player</div>
          <div className="brand-runtime">{state.runtimeLabel}</div>
        </div>
      </div>

      <div className="top-media">
        <div className="media-name">{state.hasMedia ? state.mediaName : t.noMediaLoaded}</div>
        <div className="media-meta">{state.hasMedia ? compactPath(state.mediaPath, t.none) : state.backendLabel}</div>
      </div>

      <IconButton
        label={state.sidebarCollapsed ? t.showInspector : t.hideInspector}
        onClick={() => postNativeCommand({ type: 'command', command: 'toggleSidebar' })}
      >
        {state.sidebarCollapsed ? <ChevronLeft size={17} /> : <ChevronRight size={17} />}
      </IconButton>
      <IconButton label={t.openMediaLabel} onClick={() => postNativeCommand({ type: 'command', command: 'open' })}>
        <FolderOpen size={17} />
      </IconButton>
      <IconButton label={t.mediaLibrary} onClick={() => { window.location.hash = '#/library' }}>
        <Library size={17} />
      </IconButton>
      <IconButton
        label={t.settings}
        active={state.inspectorTab === 'settings'}
        onClick={() => postNativeCommand({ type: 'command', command: 'settings' })}
      >
        <Settings size={17} />
      </IconButton>
    </header>
  )
}

function VideoStage({ state, t }: { state: PlayerState; t: Copy }): JSX.Element {
  const playbackFailed = state.hasMedia && state.playbackState === 'Error'
  const failureMessage = state.lastError || t.playbackFailedHint
  const nativeRuntime = state.backendLabel.toLowerCase().includes('native') || state.runtimeLabel.toLowerCase().includes('native')

  return (
    <main className="video-shell">
      <div className="video-stage">
        {playbackFailed && (
          <div className="playback-error-overlay" aria-live="assertive">
            <div className="playback-error-panel">
              <div className="playback-error-icon" aria-hidden="true">
                <AlertTriangle size={30} />
              </div>
              <div className="playback-error-copy">
                <strong>{t.playbackFailed}</strong>
                <p>{t.playbackFailedHint}</p>
                <small><span>{t.errorDetails}</span>{failureMessage}</small>
              </div>
              <div className="playback-error-actions">
                <button className="line-button text-button" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'playPause' })}>
                  <RotateCcw size={15} />
                  <span>{t.retry}</span>
                </button>
                <button className="line-button text-button" type="button" onClick={() => { window.location.hash = '#/library' }}>
                  <Library size={15} />
                  <span>{t.backToLibrary}</span>
                </button>
              </div>
            </div>
          </div>
        )}
        {state.hasMedia && state.buffering && !playbackFailed && !nativeRuntime && (
          <div className="buffering-overlay" aria-live="polite">
            <span className="buffering-spinner" aria-hidden="true" />
            <span className="buffering-copy">
              <strong>{t.buffering}</strong>
              <small>{state.networkKbps > 0 ? `${state.networkKbps} KB/s` : t.waitingForNetwork}</small>
            </span>
          </div>
        )}
        {!state.hasMedia && (
          <div className="empty-stage">
            <div className="empty-play">
              <Play size={32} fill="currentColor" />
            </div>
            <div className="empty-title">{t.noMediaLoaded}</div>
            <div className="empty-subtitle">{t.ready}</div>
            <button className="line-button text-button" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'open' })}>
              <FolderOpen size={16} />
              <span>{t.openMedia}</span>
            </button>
          </div>
        )}
      </div>
    </main>
  )
}

function inspectorTabs(t: Copy) {
  return [
    { key: 'recent', label: t.recent, icon: History, command: 'inspectorRecent' },
    { key: 'folder', label: t.folder, icon: Folder, command: 'inspectorFolder' },
    { key: 'media', label: t.media, icon: Info, command: 'inspectorMedia' },
    { key: 'system', label: t.system, icon: Monitor, command: 'inspectorSystem' },
    { key: 'log', label: t.log, icon: ScrollText, command: 'inspectorLog' }
  ] as const
}

function MediaList({ items, t }: { items: MediaListItem[]; t: Copy }): JSX.Element {
  if (items.length === 0) {
    return <div className="empty-list">{t.noItems}</div>
  }

  return (
    <div className="media-list">
      {items.map((item) => (
        <button
          key={item.path}
          className="media-list-item"
          type="button"
          title={item.path}
          onClick={() => postNativeCommand({ type: 'command', command: 'openPath', path: item.path })}
        >
          <span>{item.name}</span>
          <small>{compactPath(item.path, t.none)}</small>
        </button>
      ))}
    </div>
  )
}

function InspectorContent({
  state,
  t
}: {
  state: PlayerState
  t: Copy
}): JSX.Element {
  if (state.inspectorTab === 'settings') {
    return (
      <div className="inspector-content">
        <div className="section-title">
          <SlidersHorizontal size={15} />
          <span>{t.settings}</span>
        </div>
        <HdrCurveEditor state={state} t={t} />
        <InfoRow label={t.backend} value={state.backendLabel} emptyLabel={t.none} />
        <InfoRow label={t.volume} value={`${Math.round(state.volume * 100)}%`} emptyLabel={t.none} />
      </div>
    )
  }

  if (state.inspectorTab === 'folder') {
    return (
      <div className="inspector-content">
        <div className="section-title">
          <Folder size={15} />
          <span>{t.currentFolder}</span>
        </div>
        <MediaList items={state.folderMedia} t={t} />
      </div>
    )
  }

  if (state.inspectorTab === 'media') {
    return (
      <div className="inspector-content">
        <div className="section-title">
          <Info size={15} />
          <span>{t.mediaInfo}</span>
        </div>
        <InfoRow label={t.container} value={state.container} emptyLabel={t.none} />
        <InfoRow label={t.video} value={state.hasVideo ? state.videoCodec : t.off} emptyLabel={t.none} />
        <InfoRow label={t.audio} value={state.hasAudio ? state.audioCodec : t.off} emptyLabel={t.none} />
        <InfoRow label={t.resolution} value={state.resolution} emptyLabel={t.none} />
        <InfoRow label={t.frameRate} value={state.frameRate} emptyLabel={t.none} />
        <InfoRow label={t.hdr} value={state.hdrFormat} emptyLabel={t.none} />
        <InfoRow label={t.streams} value={state.streamCount || 0} emptyLabel={t.none} />
      </div>
    )
  }

  if (state.inspectorTab === 'system') {
    return (
      <div className="inspector-content">
        <div className="section-title">
          <Monitor size={15} />
          <span>{t.system}</span>
        </div>
        <InfoRow label={t.runtime} value={state.runtimeLabel} emptyLabel={t.none} />
        <InfoRow label={t.backend} value={state.backendLabel} emptyLabel={t.none} />
        <InfoRow label={t.hdrControls} value={state.hdrAvailable ? t.available : t.unavailable} emptyLabel={t.none} />
        <InfoRow label={t.hdrOutput} value={state.hdrOutput ? t.on : t.off} emptyLabel={t.none} />
        <InfoRow label={t.dolbyVision} value={state.cmv4Available ? t.available : t.unavailable} emptyLabel={t.none} />
      </div>
    )
  }

  if (state.inspectorTab === 'log') {
    return (
      <div className="inspector-content">
        <div className="section-title">
          <ScrollText size={15} />
          <span>{t.log}</span>
        </div>
        <div className="log-list">
          {state.logLines.length === 0 ? <div className="empty-list">{t.noLogEntries}</div> : state.logLines.map((line, index) => <div key={`${index}-${line}`}>{line}</div>)}
        </div>
      </div>
    )
  }

  return (
    <div className="inspector-content">
      <div className="section-title">
        <History size={15} />
        <span>{t.recentPlayback}</span>
      </div>
      <MediaList items={state.recentMedia} t={t} />
    </div>
  )
}

function Inspector({
  state,
  t
}: {
  state: PlayerState
  t: Copy
}): JSX.Element {
  const tabs = inspectorTabs(t)

  return (
    <aside className={`inspector line-panel ${state.sidebarCollapsed ? 'is-collapsed' : ''}`} aria-hidden={state.sidebarCollapsed}>
      <div className="inspector-head">
        <div>
          <div className="panel-title">{state.inspectorTab === 'settings' ? t.settings : t.inspector}</div>
          <div className="panel-subtitle">{state.hasMedia ? state.mediaName : t.noMediaLoaded}</div>
        </div>
      </div>

      {state.inspectorTab !== 'settings' && (
        <div className="inspector-tabs">
          {tabs.map((tab) => {
            const Icon = tab.icon
            const selected = state.inspectorTab === tab.key
            return (
              <button
                key={tab.key}
                className={`tab-button ${selected ? 'is-selected' : ''}`}
                type="button"
                title={tab.label}
                onClick={() => postNativeCommand({ type: 'command', command: tab.command })}
              >
                <Icon size={14} />
                <span>{tab.label}</span>
              </button>
            )
          })}
        </div>
      )}

      <InspectorContent state={state} t={t} />
    </aside>
  )
}

function TrackList({
  tracks,
  selectedTrack,
  t,
  onSelect
}: {
  tracks: TrackOption[]
  selectedTrack: number
  t: Copy
  onSelect: (index: number) => void
}): JSX.Element {
  if (tracks.length === 0) {
    return <div className="empty-list">{t.noItems}</div>
  }

  return (
    <div className="track-list">
      {tracks.map((track) => (
        <button
          key={track.index}
          className={`track-option ${track.index === selectedTrack ? 'is-selected' : ''}`}
          type="button"
          onClick={() => onSelect(track.index)}
        >
          <span>{displayTrackLabel(track.label, t)}</span>
          {track.detail && <small>{track.detail}</small>}
        </button>
      ))}
    </div>
  )
}

function RangeControl({
  label,
  valueLabel,
  children
}: {
  label: string
  valueLabel: string
  children: ReactNode
}): JSX.Element {
  return (
    <label className="control-slider">
      <div className="control-slider-head">
        <span>{label}</span>
        <strong>{valueLabel}</strong>
      </div>
      {children}
    </label>
  )
}

function SubtitlePopover({
  state,
  t,
  open,
  anchor
}: {
  state: PlayerState
  t: Copy
  open: boolean
  anchor: RectSnapshot
}): JSX.Element {
  const [activePanel, setActivePanel] = useState<SubtitlePanel>('subtitles')
  const selectedSubtitle = state.subtitleTracks.find((track) => track.index === state.subtitleSelectedTrack)
  const selectedSubtitleLabel = selectedSubtitle ? displayTrackLabel(selectedSubtitle.label, t) : t.off
  const selectedAudio = state.audioTracks.find((track) => track.index === state.audioSelectedTrack)
  const selectedAudioLabel = selectedAudio ? displayTrackLabel(selectedAudio.label, t) : t.auto
  const danmakuLabel = state.danmakuEnabled ? t.on : t.off
  const layout = subtitlePopoverLayout(anchor)
  const style = {
    '--subtitle-anchor-left': `${anchor.left}px`,
    '--subtitle-anchor-top': `${anchor.top}px`,
    '--subtitle-anchor-width': `${anchor.width}px`,
    '--subtitle-anchor-height': `${anchor.height}px`,
    '--subtitle-anchor-right': `${anchor.left + anchor.width}px`,
    '--subtitle-open-left': `${layout.left}px`,
    '--subtitle-open-top': `${layout.top}px`,
    '--subtitle-popover-width': `${layout.width}px`,
    '--subtitle-popover-height': `${layout.height}px`
  } as CSSProperties
  const tabs = [
    { key: 'subtitles' as SubtitlePanel, label: t.subtitles, value: selectedSubtitleLabel, icon: Captions },
    { key: 'audio' as SubtitlePanel, label: t.audio, value: selectedAudioLabel, icon: Volume2 },
    { key: 'danmaku' as SubtitlePanel, label: t.danmaku, value: danmakuLabel, icon: ScrollText }
  ]

  return (
    <div
      className={`subtitle-popover line-panel ${open ? 'is-open' : 'is-closing'}`}
      style={style}
      data-subtitle-popover="true"
      role="dialog"
      aria-label={t.subtitles}
    >
      <div className="subtitle-popover-content">
        <div className="subtitle-tabs" role="tablist" aria-label={t.subtitles}>
          {tabs.map((tab) => {
            const Icon = tab.icon
            const selected = activePanel === tab.key
            return (
              <button
                key={tab.key}
                className={`subtitle-tab ${selected ? 'is-selected' : ''}`}
                type="button"
                role="tab"
                aria-selected={selected}
                onClick={() => setActivePanel(tab.key)}
              >
                <Icon size={14} />
                <span>{tab.label}</span>
                <small>{tab.value}</small>
              </button>
            )
          })}
        </div>

        <div className="subtitle-page-scroll">
          {activePanel === 'subtitles' && (
            <div className="subtitle-page">
              <section className="subtitle-section">
                <div className="control-title">{t.subtitleTracks}</div>
                <TrackList
                  tracks={state.subtitleTracks}
                  selectedTrack={state.subtitleSelectedTrack}
                  t={t}
                  onSelect={(index) => postNativeCommand({ type: 'command', command: 'setSubtitleTrack', index })}
                />
              </section>

              <button className="line-button text-button full-width-button" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'openSubtitleFile' })}>
                <FolderOpen size={15} />
                <span>{t.addSubtitleFile}</span>
              </button>

              <section className="subtitle-section">
                <RangeControl label={t.delay} valueLabel={formatDelay(state.subtitleDelayMs)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.subtitleDelayMs, -5000, 5000), -5000, 5000)}
                    type="range"
                    min={-5000}
                    max={5000}
                    step={100}
                    value={clamp(state.subtitleDelayMs, -5000, 5000)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setSubtitleDelay', delayMs: Number(event.currentTarget.value) })}
                  />
                </RangeControl>
                <RangeControl label={t.subtitleSize} valueLabel={formatScale(state.subtitleFontScale)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.subtitleFontScale, 0.5, 2), 0.5, 2)}
                    type="range"
                    min={0.5}
                    max={2}
                    step={0.05}
                    value={clamp(state.subtitleFontScale, 0.5, 2)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setSubtitleFontScale', scale: Number(event.currentTarget.value) })}
                  />
                </RangeControl>
                <RangeControl label={t.horizontalOffset} valueLabel={formatSignedPixels(state.subtitleOffsetX)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.subtitleOffsetX, -200, 200), -200, 200)}
                    type="range"
                    min={-200}
                    max={200}
                    step={5}
                    value={clamp(state.subtitleOffsetX, -200, 200)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setSubtitleOffset', x: Number(event.currentTarget.value), y: state.subtitleOffsetY })}
                  />
                </RangeControl>
                <RangeControl label={t.verticalOffset} valueLabel={formatSignedPixels(state.subtitleOffsetY)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.subtitleOffsetY, -200, 200), -200, 200)}
                    type="range"
                    min={-200}
                    max={200}
                    step={5}
                    value={clamp(state.subtitleOffsetY, -200, 200)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setSubtitleOffset', x: state.subtitleOffsetX, y: Number(event.currentTarget.value) })}
                  />
                </RangeControl>
              </section>
            </div>
          )}

          {activePanel === 'audio' && (
            <div className="subtitle-page">
              <section className="subtitle-section">
                <div className="control-title">{t.audioTracks}</div>
                <TrackList
                  tracks={state.audioTracks}
                  selectedTrack={state.audioSelectedTrack}
                  t={t}
                  onSelect={(index) => postNativeCommand({ type: 'command', command: 'setAudioTrack', index })}
                />
              </section>

              <section className="subtitle-section">
                <RangeControl label={t.volume} valueLabel={formatPercent(state.volume * 100)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.volume * 100, 0, 100), 0, 100)}
                    type="range"
                    min={0}
                    max={100}
                    step={1}
                    value={clamp(Math.round(state.volume * 100), 0, 100)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setVolume', volume: Number(event.currentTarget.value) / 100 })}
                  />
                </RangeControl>
              </section>
            </div>
          )}

          {activePanel === 'danmaku' && (
            <div className="subtitle-page">
              <section className="subtitle-section">
                <button
                  className={`menu-action-row ${state.danmakuEnabled ? 'is-selected' : ''}`}
                  type="button"
                  onClick={() => postNativeCommand({ type: 'command', command: 'toggleDanmakuEnabled' })}
                >
                  <span>{t.enableDanmaku}</span>
                  <strong>{state.danmakuEnabled ? t.on : t.off}</strong>
                </button>
                <button className="menu-action-row" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'cycleDanmakuMode' })}>
                  <span>{t.displayMode}</span>
                  <strong>{danmakuModeLabel(state.danmakuMode, t)}</strong>
                </button>
              </section>

              <section className="subtitle-section">
                <RangeControl label={t.opacity} valueLabel={formatPercent(state.danmakuOpacityPercent)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.danmakuOpacityPercent, 20, 100), 20, 100)}
                    type="range"
                    min={20}
                    max={100}
                    step={5}
                    value={clamp(state.danmakuOpacityPercent, 20, 100)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setDanmakuOpacity', opacityPercent: Number(event.currentTarget.value) })}
                  />
                </RangeControl>
                <RangeControl label={t.speed} valueLabel={formatPercent(state.danmakuSpeedPercent)}>
                  <input
                    className="simple-range"
                    style={rangeStyle(clamp(state.danmakuSpeedPercent, 50, 200), 50, 200)}
                    type="range"
                    min={50}
                    max={200}
                    step={10}
                    value={clamp(state.danmakuSpeedPercent, 50, 200)}
                    onChange={(event) => postNativeCommand({ type: 'command', command: 'setDanmakuSpeed', speedPercent: Number(event.currentTarget.value) })}
                  />
                </RangeControl>
              </section>

              <button className="line-button text-button full-width-button subtitle-file-button" type="button" onClick={() => postNativeCommand({ type: 'command', command: 'openDanmakuFile' })}>
                <FolderOpen size={15} />
                <span>{t.addDanmakuFile}</span>
                <small>{state.danmakuPath ? compactPath(state.danmakuPath, t.none) : t.fileTypes}</small>
              </button>
            </div>
          )}
        </div>
        <div className="subtitle-footer" aria-hidden="true" />
      </div>
    </div>
  )
}

function Transport({
  state,
  t,
  transportRef,
  subtitleButtonRef,
  subtitleVisualActive,
  onSubtitleMenu
}: {
  state: PlayerState
  t: Copy
  transportRef: Ref<HTMLElement>
  subtitleButtonRef: Ref<HTMLButtonElement>
  subtitleVisualActive: boolean
  onSubtitleMenu: () => void
}): JSX.Element {
  const [volumeHover, setVolumeHover] = useState(false)
  const [volumeDragging, setVolumeDragging] = useState(false)
  const [progressDragging, setProgressDragging] = useState(false)
  const [progressDraft, setProgressDraft] = useState<number | null>(null)
  const progressDraggingRef = useRef(false)
  const progress = useMemo(() => {
    if (state.durationMs <= 0) return 0
    return Math.max(0, Math.min(1000, Math.round((state.positionMs / state.durationMs) * 1000)))
  }, [state.durationMs, state.positionMs])
  const bufferedProgress = useMemo(() => {
    if (state.durationMs <= 0) return progress
    const buffered = Math.round((Math.max(state.bufferedEndMs, state.positionMs) / state.durationMs) * 1000)
    return Math.max(progress, Math.min(1000, buffered))
  }, [progress, state.bufferedEndMs, state.durationMs, state.positionMs])
  const displayProgress = progressDragging && progressDraft !== null ? progressDraft : progress
  const displayPositionMs = progressDragging && state.durationMs > 0
    ? Math.round((state.durationMs * displayProgress) / 1000)
    : state.positionMs
  const playing = state.playbackState === 'Playing'
  const hdrButtonLabel = state.cmv4Available ? t.dolbyVision : t.hdr
  const volumePercent = Math.round(state.volume * 100)
  const showVolumePercent = volumeHover || volumeDragging
  const progressStyle = {
    '--progress': `${displayProgress / 10}%`,
    '--buffered': `${Math.max(displayProgress, bufferedProgress) / 10}%`
  } as CSSProperties
  const volumeStyle = {
    '--volume-x': `${volumePercent}%`
  } as CSSProperties
  const clampProgress = (value: number): number => Math.max(0, Math.min(1000, Math.round(value)))
  const commitProgress = (value: number): void => {
    const next = clampProgress(value)
    progressDraggingRef.current = false
    setProgressDragging(false)
    setProgressDraft(null)
    postNativeCommand({
      type: 'command',
      command: 'seekToRatio',
      ratio: next / 1000
    })
  }

  return (
    <footer ref={transportRef} className="transport line-panel">
      <div className="time-row">
        <span>{formatTime(displayPositionMs)}</span>
        <span>{formatTime(state.durationMs)}</span>
      </div>
      <div className="progress-control" style={progressStyle}>
        <div className="progress-track" aria-hidden="true">
          <span className="progress-buffered" />
          <span className="progress-played" />
        </div>
        <input
          className="progress-slider"
          type="range"
          min={0}
          max={1000}
          value={displayProgress}
          disabled={!state.hasMedia || state.durationMs <= 0}
          onPointerDown={(event) => {
            const next = clampProgress(Number(event.currentTarget.value))
            progressDraggingRef.current = true
            setProgressDragging(true)
            setProgressDraft(next)
            event.currentTarget.setPointerCapture(event.pointerId)
          }}
          onPointerUp={(event) => {
            if (!progressDraggingRef.current) return
            if (event.currentTarget.hasPointerCapture(event.pointerId)) {
              event.currentTarget.releasePointerCapture(event.pointerId)
            }
            commitProgress(Number(event.currentTarget.value))
          }}
          onPointerCancel={(event) => {
            if (event.currentTarget.hasPointerCapture(event.pointerId)) {
              event.currentTarget.releasePointerCapture(event.pointerId)
            }
            progressDraggingRef.current = false
            setProgressDragging(false)
            setProgressDraft(null)
          }}
          onBlur={(event) => {
            if (progressDraggingRef.current) {
              commitProgress(Number(event.currentTarget.value))
            }
          }}
          onChange={(event) => {
            const next = clampProgress(Number(event.currentTarget.value))
            if (progressDraggingRef.current) {
              setProgressDraft(next)
            } else {
              postNativeCommand({
                type: 'command',
                command: 'seekToRatio',
                ratio: next / 1000
              })
            }
          }}
        />
      </div>
      <div className="transport-row">
        <div className="transport-left">
          <IconButton label={t.back10} disabled={!state.hasMedia} onClick={() => postNativeCommand({ type: 'command', command: 'back' })}>
            <SkipBack size={18} />
          </IconButton>
          <IconButton label={playing ? t.pause : t.play} primary disabled={!state.hasMedia} onClick={() => postNativeCommand({ type: 'command', command: 'playPause' })}>
            {playing ? <Pause size={22} fill="currentColor" /> : <Play size={22} fill="currentColor" />}
          </IconButton>
          <IconButton label={t.forward10} disabled={!state.hasMedia} onClick={() => postNativeCommand({ type: 'command', command: 'forward' })}>
            <SkipForward size={18} />
          </IconButton>
          <IconButton label={t.stop} disabled={!state.hasMedia} onClick={() => postNativeCommand({ type: 'command', command: 'stop' })}>
            <Square size={17} fill="currentColor" />
          </IconButton>
          <div
            className={`volume-control ${showVolumePercent ? 'is-active' : ''}`}
            onPointerEnter={() => setVolumeHover(true)}
            onPointerLeave={() => setVolumeHover(false)}
          >
            <Volume2 size={17} />
            <div className="volume-slider-wrap" data-volume={`${volumePercent}%`} style={volumeStyle}>
              <input
                aria-label={t.volume}
                type="range"
                min={0}
                max={100}
                value={volumePercent}
                onPointerDown={() => setVolumeDragging(true)}
                onPointerUp={() => setVolumeDragging(false)}
                onPointerCancel={() => setVolumeDragging(false)}
                onBlur={() => setVolumeDragging(false)}
                onChange={(event) => {
                  postNativeCommand({
                    type: 'command',
                    command: 'setVolume',
                    volume: Number(event.currentTarget.value) / 100
                  })
                }}
              />
            </div>
          </div>
        </div>

        <div className="transport-right">
          {state.hdrAvailable && (
            <button className={`line-button label-button ${state.hdrOutput ? 'is-active' : ''}`} type="button" onClick={() => postNativeCommand({ type: 'command', command: 'toggleHdr' })}>
              {hdrButtonLabel}
            </button>
          )}
          {state.cmv4Available && (
            <button className={`line-button label-button ${state.cmv4Enabled ? 'is-active' : ''}`} type="button" onClick={() => postNativeCommand({ type: 'command', command: 'toggleCmv4' })}>
              {t.enhanced}
            </button>
          )}
          <IconButton label={t.subtitles} subtitleToggle buttonRef={subtitleButtonRef} onClick={onSubtitleMenu}>
            {state.fullscreen && !subtitleVisualActive ? (
              <Captions size={18} />
            ) : (
              <span className="subtitle-button-icon-space" aria-hidden="true" />
            )}
          </IconButton>
          <IconButton label={state.fullscreen ? t.exitFullscreen : t.fullscreen} onClick={() => postNativeCommand({ type: 'command', command: 'toggleFullscreen' })}>
            {state.fullscreen ? <Minimize2 size={18} /> : <Maximize2 size={18} />}
          </IconButton>
        </div>
      </div>
    </footer>
  )
}

export default function App(): JSX.Element {
  const [state, setState] = useState<PlayerState>(EMPTY_STATE)
  const [language, setLanguage] = useState<UiLanguage>(() => getInitialLanguage())
  const [subtitlePopoverMounted, setSubtitlePopoverMounted] = useState(false)
  const [subtitlePopoverOpen, setSubtitlePopoverOpen] = useState(false)
  const [subtitleAnchor, setSubtitleAnchor] = useState<RectSnapshot>(DEFAULT_SUBTITLE_ANCHOR)
  const [subtitleAnchorReady, setSubtitleAnchorReady] = useState(false)
  const transportRef = useRef<HTMLElement | null>(null)
  const subtitleButtonRef = useRef<HTMLButtonElement | null>(null)
  const lastFullscreenTransportRevealAt = useRef(0)
  const t = copy[language]
  const subtitleVisualActive = subtitlePopoverMounted || subtitlePopoverOpen || state.subtitleMenuOpen
  useEmbyPlaybackReporting(state)

  const captureSubtitleAnchor = (): RectSnapshot => {
    const element = subtitleButtonRef.current
    const anchor = rectSnapshotFromElement(element)
    setSubtitleAnchor(anchor)
    setSubtitleAnchorReady(Boolean(element))
    return anchor
  }

  const syncSubtitleAnchor = (): void => {
    const anchor = captureSubtitleAnchor()
    if (subtitleButtonRef.current) {
      postSubtitleGeometry(anchor)
    }
  }

  useEffect(() => {
    applyAppearanceSettings()
    return subscribeNativeState(setState)
  }, [])

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent): void => {
      if (!state.fullscreen || event.key !== 'Escape') return
      event.preventDefault()
      postNativeCommand({ type: 'command', command: 'toggleFullscreen' })
    }
    window.addEventListener('keydown', onKeyDown, true)
    return () => window.removeEventListener('keydown', onKeyDown, true)
  }, [state.fullscreen])

  useEffect(() => {
    const updateAnchor = (): void => {
      syncSubtitleAnchor()
    }
    updateAnchor()
    window.addEventListener('resize', updateAnchor)
    return () => window.removeEventListener('resize', updateAnchor)
  }, [])

  useLayoutEffect(() => {
    syncSubtitleAnchor()
  }, [state.fullscreen, state.fullscreenTransportVisible, state.sidebarCollapsed, subtitleVisualActive])

  useEffect(() => {
    const firstFrame = window.requestAnimationFrame(syncSubtitleAnchor)
    const secondFrame = window.requestAnimationFrame(() => {
      window.requestAnimationFrame(syncSubtitleAnchor)
    })
    const settleTimer = window.setTimeout(syncSubtitleAnchor, 280)
    return () => {
      window.cancelAnimationFrame(firstFrame)
      window.cancelAnimationFrame(secondFrame)
      window.clearTimeout(settleTimer)
    }
  }, [state.fullscreen, state.fullscreenTransportVisible, state.sidebarCollapsed, subtitleVisualActive])

  useEffect(() => {
    const updateTransportGeometry = (): void => {
      if (!transportRef.current) return
      postTransportGeometry(layoutSnapshotFromElement(transportRef.current))
    }
    const frame = window.requestAnimationFrame(updateTransportGeometry)
    window.addEventListener('resize', updateTransportGeometry)
    return () => {
      window.cancelAnimationFrame(frame)
      window.removeEventListener('resize', updateTransportGeometry)
    }
  }, [state.fullscreen, state.fullscreenTransportVisible])

  useEffect(() => {
    applyDocumentLanguage(language)
    saveUiLanguage(language)
  }, [language])

  useEffect(() => {
    if (state.subtitleMenuOpen) {
      syncSubtitleAnchor()
      setSubtitlePopoverOpen(true)
    } else {
      setSubtitlePopoverOpen(false)
    }
  }, [state.subtitleMenuOpen])

  useEffect(() => {
    if (subtitlePopoverOpen) {
      setSubtitlePopoverMounted(true)
      return undefined
    }

    const timeout = window.setTimeout(() => setSubtitlePopoverMounted(false), 260)
    return () => window.clearTimeout(timeout)
  }, [subtitlePopoverOpen])

  const toggleSubtitleMenu = (): void => {
    syncSubtitleAnchor()
    setSubtitlePopoverOpen((open) => !open)
    postNativeCommand({ type: 'command', command: 'subtitleMenu' })
  }

  const hideSubtitleMenu = (): void => {
    syncSubtitleAnchor()
    setSubtitlePopoverOpen(false)
    postNativeCommand({ type: 'command', command: 'hideSubtitleMenu' })
  }

  const revealFullscreenTransport = (event: PointerEvent<HTMLDivElement>): void => {
    if (!state.fullscreen) return

    const activationHeight = Math.max(96, Math.min(140, window.innerHeight * 0.14))
    if (event.clientY < window.innerHeight - activationHeight) return

    const now = window.performance.now()
    if (now - lastFullscreenTransportRevealAt.current < 220) return

    lastFullscreenTransportRevealAt.current = now
    postNativeCommand({ type: 'command', command: 'showFullscreenTransport' })
  }

  return (
    <div
      className={`app-shell ${state.sidebarCollapsed ? 'inspector-collapsed' : ''} ${state.fullscreen ? 'is-fullscreen' : ''} ${state.fullscreenTransportVisible ? 'fullscreen-transport-visible' : ''}`}
      onPointerMoveCapture={revealFullscreenTransport}
      onPointerDownCapture={(event) => {
        if (!state.subtitleMenuOpen && !subtitlePopoverOpen) return
        const target = event.target
        if (target instanceof Element && target.closest('[data-subtitle-toggle="true"]')) return
        if (target instanceof Element && target.closest('[data-subtitle-popover="true"]')) return
        hideSubtitleMenu()
      }}
    >
      <TopBar state={state} t={t} />
      <section className="content-grid">
        <VideoStage state={state} t={t} />
        <Inspector state={state} t={t} />
      </section>
      {subtitlePopoverMounted && <SubtitlePopover state={state} t={t} open={subtitlePopoverOpen} anchor={subtitleAnchor} />}
      {subtitleAnchorReady && (!state.fullscreen || subtitleVisualActive) && (
        <button
          className="subtitle-top-icon"
          type="button"
          aria-label={t.subtitles}
          title={t.subtitles}
          data-subtitle-toggle="true"
          style={{
            '--subtitle-anchor-left': `${subtitleAnchor.left}px`,
            '--subtitle-anchor-top': `${subtitleAnchor.top}px`,
            '--subtitle-anchor-width': `${subtitleAnchor.width}px`,
            '--subtitle-anchor-height': `${subtitleAnchor.height}px`
          } as CSSProperties}
          onClick={toggleSubtitleMenu}
        >
          <Captions size={18} />
        </button>
      )}
      <Transport
        state={state}
        t={t}
        transportRef={transportRef}
        subtitleButtonRef={subtitleButtonRef}
        subtitleVisualActive={subtitleVisualActive}
        onSubtitleMenu={toggleSubtitleMenu}
      />
    </div>
  )
}
