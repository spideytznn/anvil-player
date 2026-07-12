import { useEffect, useId, useLayoutEffect, useMemo, useRef, useState, type CSSProperties, type Dispatch, type FormEvent, type ReactNode, type SetStateAction } from 'react'
import {
  Activity,
  ArrowDownWideNarrow,
  ArrowLeft,
  ArrowUpNarrowWide,
  Check,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  Clock3,
  Cloud,
  Database,
  Eraser,
  Film,
  FolderOpen,
  Grid3X3,
  HardDrive,
  Heart,
  Languages,
  LockKeyhole,
  ListFilter,
  ListPlus,
  MoreHorizontal,
  MonitorCog,
  Play,
  Plus,
  Search,
  SearchCheck,
  Server,
  Settings2,
  ShieldOff,
  Star,
  Trash2,
  Tv,
  Volume2,
  VolumeX,
  Wand2,
  X
} from 'lucide-react'
import { applyAppearanceSettings } from './appearance'
import { loadSavedEmbyConnections, removeSavedEmbyConnection, saveSavedEmbyConnection, type SavedEmbyConnection } from './manager/connectionStorage'
import { createEmptyLibraryClient } from './manager/mediaLibraryClient'
import {
  listEmbyLibraryView,
  loadEmbyItemDetails,
  loadEmbyLibrary,
  notifyPendingEmbyPlaybackReport,
  refreshEmbyLibrary,
  resolveEmbyPlaybackTarget,
  savePendingEmbyPlaybackReport,
  searchEmbyLibrary,
  searchEmbyPersonLibrary,
  type EmbySession
} from './manager/embyClient'
import { buildLocalFolderLibrary, buildLocalFolderSource } from './manager/localFolderLibrary'
import {
  fetchTmdbTrailerUrls,
  scrapeTmdbItem,
  scrapeTmdbCandidate,
  searchTmdbCandidates,
  tmdbApiBaseUrl,
  type TmdbMatchCandidate,
  type TmdbAuthMode,
  type TmdbNetworkMode,
  type TmdbSettings
} from './manager/tmdbClient'
import {
  loadTrailerSettings,
  saveTrailerSettings,
  searchBilibiliTrailerUrls,
  trailerUrlMatchesSource,
  type TrailerSettings,
  type TrailerSource
} from './manager/trailerClient'
import {
  credentialedWebDavUrl,
  loadWebDavCredentials,
  removeWebDavCredentials,
  saveWebDavCredentials
} from './manager/webdavClient'
import { loadSmbCredentials, removeSmbCredentials, saveSmbCredentials } from './manager/smbCredentials'
import type { LibraryHomeCard, LibraryHomeSection, LibrarySource, LibraryView, MediaFilterKey, MediaItem, NavKey, PersonCredit, SortKey, SortOrder, SourceKind } from './manager/types'
import { postNativeCommand, subscribeNativeMessages, type LocalFolderPickResult, type LocalFolderScanCompleted, type LocalFolderScanFailed } from './nativeBridge'
import {
  applyDocumentLanguage,
  getInitialLanguage,
  saveUiLanguage,
  tmdbLanguageForUi,
  type UiLanguage
} from './uiSettings'
import './library.css'
import {
  buildServerAddress,
  directoryDisplayName,
  fileServiceNameFromLocation,
  locationKey,
  mediaItemBelongsToScanRoot,
  normalizeSmbHost,
  normalizeSmbPath,
  normalizeWebDavSourceUrl,
  parseServerAddress,
  scanLocationsForSource,
  scanProgressSummary,
  smbParentPath,
  sourceScanKind,
  tryNormalizeWebDavSourceUrl,
  uniqueLocationRows,
  webDavBaseUrlForSource,
  webDavParentUrl,
  webDavSelectedPathsForSource,
  type ServerAddressParts,
  type ServerProtocol,
  type SourceScanProgress
} from './library/paths'
import {
  buildManualMetadataItem,
  clearLocalMetadata,
  compareMediaIndex,
  duplicateMetadataKey,
  dissolveMediaCollection,
  hasImageBackground,
  imageBackgroundUrl,
  itemMergeRank,
  localFileName,
  mediaVersionLabel,
  mediaPathKey,
  mergeDuplicateMetadataItems,
  mergeLocalScannedItem,
  metadataFormFromItem,
  type MetadataEditForm
} from './library/merge'
import { useTmdbSettings } from './library/useTmdbSettings'
import { MetadataMatchDialog } from './library/MetadataMatchDialog'
import { MetadataDeleteDialog } from './library/MetadataDeleteDialog'
import { MediaManagementDialog } from './library/MediaManagementDialog'
import { TaskCenter, type BackgroundTask } from './library/TaskCenter'

const LIBRARY_LISTING_PAGE_SIZE = 60
const DISSOLVED_COLLECTION_PATHS_KEY = 'anvil-player.library.dissolved-paths.v1'
const PLAYLISTS_KEY = 'anvil-player.library.playlists.v1'
const WATCH_LATER_PLAYLIST_ID = 'watch-later'

interface MediaPlaylist {
  id: string
  name: string
}

function loadMediaPlaylists(): MediaPlaylist[] {
  try {
    const parsed: unknown = JSON.parse(localStorage.getItem(PLAYLISTS_KEY) || '[]')
    const custom = Array.isArray(parsed)
      ? parsed.filter((row): row is MediaPlaylist => Boolean(row && typeof row === 'object' && typeof (row as MediaPlaylist).id === 'string' && typeof (row as MediaPlaylist).name === 'string'))
      : []
    return [{ id: WATCH_LATER_PLAYLIST_ID, name: '稍后观看' }, ...custom.filter((row) => row.id !== WATCH_LATER_PLAYLIST_ID)]
  } catch {
    return [{ id: WATCH_LATER_PLAYLIST_ID, name: '稍后观看' }]
  }
}

function saveMediaPlaylists(playlists: MediaPlaylist[]): void {
  localStorage.setItem(PLAYLISTS_KEY, JSON.stringify(playlists.filter((row) => row.id !== WATCH_LATER_PLAYLIST_ID)))
}

function loadDissolvedCollectionPaths(): Set<string> {
  try {
    const parsed: unknown = JSON.parse(localStorage.getItem(DISSOLVED_COLLECTION_PATHS_KEY) || '[]')
    return new Set(Array.isArray(parsed) ? parsed.filter((value): value is string => typeof value === 'string') : [])
  } catch {
    return new Set()
  }
}

function saveDissolvedCollectionPaths(paths: Set<string>): void {
  localStorage.setItem(DISSOLVED_COLLECTION_PATHS_KEY, JSON.stringify([...paths]))
}

interface NavItem {
  key: NavKey
  label: string
  icon: ReactNode
}

type SourceSetupMode = 'hidden' | 'select' | 'emby' | 'localFolder' | 'smb' | 'webdav' | 'settings'

interface FileServiceDirectory {
  name: string
  path: string
}

const primaryNav: NavItem[] = [
  { key: 'continue', label: 'continue', icon: <Clock3 size={16} /> },
  { key: 'recent', label: 'recent', icon: <Grid3X3 size={16} /> },
  { key: 'movies', label: 'movies', icon: <Film size={16} /> },
  { key: 'series', label: 'series', icon: <Tv size={16} /> }
]

const smartNav: NavItem[] = [
  { key: 'unwatched', label: 'unwatched', icon: <Clock3 size={16} /> },
  { key: 'watched', label: 'watched', icon: <Check size={16} /> },
  { key: 'favorites', label: 'favorites', icon: <Heart size={16} /> },
  { key: 'playlist', label: 'playlist', icon: <ListPlus size={16} /> },
  { key: 'genre', label: 'genre', icon: <ListFilter size={16} /> },
  { key: 'rating', label: 'rating', icon: <Star size={16} /> },
  { key: 'release', label: 'release', icon: <Database size={16} /> }
]

const navTranslations: Record<string, Record<UiLanguage, string>> = {
  continue: { zh: '继续观看', en: 'Continue watching' },
  recent: { zh: '最近添加', en: 'Recently added' },
  movies: { zh: '电影', en: 'Movies' },
  series: { zh: '剧集', en: 'Series' },
  unwatched: { zh: '未看', en: 'Unwatched' },
  watched: { zh: '已看', en: 'Watched' },
  favorites: { zh: '收藏', en: 'Favorites' },
  playlist: { zh: '片单', en: 'Playlists' },
  genre: { zh: '类型', en: 'Genres' },
  rating: { zh: '评分', en: 'Rating' },
  release: { zh: '发行年份', en: 'Release year' }
}

let currentLibraryLanguage: UiLanguage = 'zh'

function playbackLocationKey(value: string): string {
  try {
    const url = new URL(value)
    url.username = ''
    url.password = ''
    return locationKey(url.toString())
  } catch {
    return locationKey(value)
  }
}

function applyPlaybackProgress(item: MediaItem, path: string, ratio: number, ended: boolean): { item: MediaItem; changed: boolean } {
  const matches = [item.path, item.playbackPath].some((value) => value && playbackLocationKey(value) === playbackLocationKey(path))
  let nestedChanged = false
  const updateNested = (nested: MediaItem | undefined): MediaItem | undefined => {
    if (!nested) return nested
    const updated = applyPlaybackProgress(nested, path, ratio, ended)
    nestedChanged ||= updated.changed
    return updated.item
  }
  const seasons = item.seasons?.map((season) => ({
    ...season,
    episodes: season.episodes.map((episode) => ({ ...episode, item: updateNested(episode.item) }))
  }))
  const episodes = item.episodes?.map((episode) => ({ ...episode, item: updateNested(episode.item) }))
  const versions = item.versions?.map((version) => updateNested(version) ?? version)
  if (!matches && !nestedChanged) return { item, changed: false }
  const watched = ended || ratio >= 0.9
  const progress = watched ? 1 : ratio
  return {
    changed: true,
    item: {
      ...item,
      seasons,
      episodes,
      versions,
      progress,
      watched,
      continueWatching: !watched && progress >= 0.005
    }
  }
}

function localizedNav(items: NavItem[], language: UiLanguage): NavItem[] {
  return items.map((item) => ({ ...item, label: navTranslations[item.label]?.[language] ?? item.label }))
}

function localizedViewTabs(language: UiLanguage): Array<{ key: LibraryView; label: string }> {
  return [
    { key: 'home', label: language === 'zh' ? '全部' : 'All' },
    { key: 'movies', label: navTranslations.movies[language] },
    { key: 'series', label: navTranslations.series[language] }
  ]
}

function localizedSortOptions(language: UiLanguage): Array<{ key: SortKey; label: string }> {
  return [
    { key: 'recent', label: navTranslations.recent[language] },
    { key: 'title', label: language === 'zh' ? '标题' : 'Title' },
    { key: 'rating', label: navTranslations.rating[language] },
    { key: 'year', label: navTranslations.release[language] }
  ]
}

function mediaTypeLabel(type: MediaItem['type']): string {
  switch (type) {
    case 'movie': return currentLibraryLanguage === 'zh' ? '电影' : 'Movie'
    case 'series': return currentLibraryLanguage === 'zh' ? '剧集' : 'Series'
    default: return currentLibraryLanguage === 'zh' ? '文件夹' : 'Folder'
  }
}

function sourceKindLabel(kind: SourceKind): string {
  switch (kind) {
    case 'Emby': return 'Emby'
    case 'WebDAV': return 'WebDAV'
    case 'SMB': return 'SMB'
    default: return currentLibraryLanguage === 'zh' ? '文件系统' : 'File system'
  }
}

function EmbyIcon({ size = 16 }: { size?: number }): JSX.Element {
  return (
    <svg
      className="library-emby-icon"
      width={size}
      height={size}
      viewBox="0 0 512 512"
      aria-hidden="true"
      focusable="false"
    >
      <path
        className="library-emby-icon-mark"
        d="m97.1 229.4 26.5 26.5L0 379.5l132.4 132.4 26.5-26.5L282.5 609l141.2-141.2-26.5-26.5L512 326.5 379.6 194.1l-26.5 26.5L229.5 97z"
        transform="translate(0 -97)"
      />
      <path className="library-emby-icon-play" d="M196.8 351.2v-193L366 254.7 281.4 303z" />
    </svg>
  )
}

function sourceIcon(kind: SourceKind, size = 16): ReactNode {
  switch (kind) {
    case 'Emby': return <EmbyIcon size={size} />
    case 'WebDAV': return <Database className="library-source-icon" size={size} />
    case 'SMB': return <Server className="library-source-icon" size={size} />
    default: return <HardDrive className="library-source-icon" size={size} />
  }
}

function navLabel(navKey: NavKey, sourceMap: Map<string, LibrarySource>, language: UiLanguage = 'zh'): string {
  if (navKey.startsWith('source:')) {
    return sourceMap.get(navKey.slice('source:'.length))?.name ?? '媒体源'
  }
  const item = [...primaryNav, ...smartNav].find((candidate) => candidate.key === navKey)
  return item ? navTranslations[item.label]?.[language] ?? item.label : (language === 'zh' ? '媒体库' : 'Library')
}

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : '连接 Emby 失败'
}

function posterMark(item: MediaItem): string {
  if (item.type === 'folder') return '合集'
  return item.title.slice(0, 2)
}

function MediaArtwork(props: {
  background: string
  className: string
  children?: ReactNode
}): JSX.Element {
  const imageUrl = imageBackgroundUrl(props.background)
  const [imageFailed, setImageFailed] = useState(false)
  const showImage = Boolean(imageUrl && !imageFailed)

  useEffect(() => {
    setImageFailed(false)
  }, [imageUrl])

  return (
    <div className={`${props.className} ${showImage ? 'has-media-image' : 'has-default-media-poster'}`}>
      <div className="library-default-film" style={{ backgroundImage: 'url("./default-media-poster.png")' }} aria-hidden="true" />
      {showImage ? (
        <img
          className="library-art-image"
          src={imageUrl}
          alt=""
          loading="lazy"
          decoding="async"
          draggable={false}
          onError={() => setImageFailed(true)}
        />
      ) : null}
      {props.children}
    </div>
  )
}

function homeCardViewId(card: LibraryHomeCard): string {
  if (card.viewId) return card.viewId
  return card.kind === 'view' && card.id.startsWith('view:') ? card.id.slice('view:'.length) : ''
}

function cleanJoin(values: string[], separator = ' · '): string {
  return values.map((value) => value.trim()).filter(Boolean).join(separator)
}

const LIBRARY_VIEW_STATE_STORAGE_KEY = 'anvil-player.library.view-state.v1'

interface LibraryViewState {
  activeNav?: string
}

function defaultLibraryNav(connections: SavedEmbyConnection[]): NavKey {
  const firstSourceId = connections.find((connection) => connection.sourceId)?.sourceId
  return firstSourceId ? `source:${firstSourceId}` as NavKey : 'recent'
}

function isSavedLibraryNav(value: string, connections: SavedEmbyConnection[]): value is NavKey {
  if (value.startsWith('source:')) {
    const sourceId = value.slice('source:'.length)
    return Boolean(sourceId && connections.some((connection) => connection.sourceId === sourceId))
  }
  return [...primaryNav, ...smartNav].some((item) => item.key === value)
}

function loadSavedLibraryNav(connections: SavedEmbyConnection[]): NavKey {
  const fallback = defaultLibraryNav(connections)
  try {
    const raw = window.localStorage.getItem(LIBRARY_VIEW_STATE_STORAGE_KEY)
    if (!raw) return fallback
    const parsed: unknown = JSON.parse(raw)
    if (!parsed || typeof parsed !== 'object') return fallback
    const activeNav = (parsed as LibraryViewState).activeNav
    return typeof activeNav === 'string' && isSavedLibraryNav(activeNav, connections) ? activeNav : fallback
  } catch {
    return fallback
  }
}

function saveLibraryNav(navKey: NavKey): void {
  try {
    window.localStorage.setItem(LIBRARY_VIEW_STATE_STORAGE_KEY, JSON.stringify({ activeNav: navKey }))
  } catch {
    // Restoring the last library source is a convenience only.
  }
}

function SidebarButton(props: {
  item: NavItem
  activeNav: NavKey
  suppressActive?: boolean
  onSelect: (navKey: NavKey) => void
}): JSX.Element {
  return (
    <button
      className={`library-nav-button ${!props.suppressActive && props.activeNav === props.item.key ? 'is-active' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.key)}
    >
      {props.item.icon}
      <span>{props.item.label}</span>
    </button>
  )
}

const TOOLBAR_SELECT_CLOSED_HEIGHT = 36
const TOOLBAR_SELECT_DURATION_MS = 460
const TOOLBAR_SELECT_EASING = 'cubic-bezier(0.19, 1, 0.22, 1)'

type ToolbarSelectState = 'closed' | 'opening' | 'open' | 'closing'

function ToolbarSelect<T extends string>(props: {
  value: T
  options: Array<{ key: T; label: string }>
  ariaLabel: string
  className?: string
  statusIcon?: ReactNode
  onChange: (value: T) => void
}): JSX.Element {
  const [selectState, setSelectState] = useState<ToolbarSelectState>('closed')
  const menuId = useId()
  const rootRef = useRef<HTMLDivElement | null>(null)
  const panelRef = useRef<HTMLDivElement | null>(null)
  const menuContentRef = useRef<HTMLDivElement | null>(null)
  const animationRef = useRef<Animation | null>(null)
  const selected = props.options.find((option) => option.key === props.value) ?? props.options[0]
  const isMounted = selectState !== 'closed'
  const isExpanded = selectState === 'opening' || selectState === 'open'
  const isActive = selectState !== 'closed'
  const className = ['toolbar-select', props.className ?? ''].filter(Boolean).join(' ')

  function measureOpenHeight(): number {
    return TOOLBAR_SELECT_CLOSED_HEIGHT + (menuContentRef.current?.offsetHeight ?? 0)
  }

  function openSelect(): void {
    setSelectState((current) => current === 'open' || current === 'opening' ? current : 'opening')
  }

  function closeSelect(): void {
    setSelectState((current) => current === 'closed' || current === 'closing' ? current : 'closing')
  }

  function toggleSelect(): void {
    setSelectState((current) => current === 'open' || current === 'opening' ? 'closing' : 'opening')
  }

  function animatePanel(expand: boolean): void {
    const panel = panelRef.current
    if (!panel) return

    animationRef.current?.cancel()
    const fromHeight = panel.getBoundingClientRect().height || TOOLBAR_SELECT_CLOSED_HEIGHT
    const toHeight = expand ? measureOpenHeight() : TOOLBAR_SELECT_CLOSED_HEIGHT
    panel.style.height = `${fromHeight}px`

    const prefersReducedMotion = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches ?? false
    const duration = prefersReducedMotion ? 1 : TOOLBAR_SELECT_DURATION_MS
    const animation = panel.animate(
      [
        { height: `${fromHeight}px` },
        { height: `${toHeight}px` }
      ],
      {
        duration,
        easing: TOOLBAR_SELECT_EASING,
        fill: 'forwards'
      }
    )

    animationRef.current = animation
    animation.onfinish = () => {
      animationRef.current = null
      panel.style.height = expand ? `${toHeight}px` : ''
      setSelectState(expand ? 'open' : 'closed')
    }
  }

  useLayoutEffect(() => {
    const panel = panelRef.current
    if (!panel) return undefined

    if (selectState === 'opening') {
      animatePanel(true)
    } else if (selectState === 'closing') {
      animatePanel(false)
    } else if (selectState === 'open') {
      panel.style.height = `${measureOpenHeight()}px`
    } else {
      animationRef.current?.cancel()
      animationRef.current = null
      panel.style.height = ''
    }

    return undefined
  }, [selectState, props.options])

  useEffect(() => {
    return () => animationRef.current?.cancel()
  }, [])

  useEffect(() => {
    if (!isActive) return undefined
    const onPointerDown = (event: PointerEvent): void => {
      const target = event.target
      if (target instanceof Node && rootRef.current?.contains(target)) return
      closeSelect()
    }
    const onKeyDown = (event: KeyboardEvent): void => {
      if (event.key === 'Escape') closeSelect()
    }
    window.addEventListener('pointerdown', onPointerDown)
    window.addEventListener('keydown', onKeyDown)
    return () => {
      window.removeEventListener('pointerdown', onPointerDown)
      window.removeEventListener('keydown', onKeyDown)
    }
  }, [isActive])

  return (
    <div
      className={className}
      data-active={isActive}
      data-expanded={isExpanded}
      data-state={selectState}
      ref={rootRef}
    >
      <div className="toolbar-select-panel" ref={panelRef}>
        <button
          className="toolbar-select-trigger"
          type="button"
          aria-label={props.ariaLabel}
          aria-controls={menuId}
          aria-haspopup="listbox"
          aria-expanded={isExpanded}
          onClick={toggleSelect}
          onKeyDown={(event) => {
            if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
              event.preventDefault()
              openSelect()
            }
          }}
        >
          <span className="toolbar-select-label">{selected?.label ?? ''}</span>
          {props.statusIcon ? (
            <span className="toolbar-select-status" aria-hidden="true">
              {props.statusIcon}
            </span>
          ) : null}
          <ChevronDown className="toolbar-select-chevron" size={16} />
        </button>
        {isMounted ? (
          <div
            className="toolbar-select-list"
            id={menuId}
            role="listbox"
            aria-label={props.ariaLabel}
            aria-hidden={!isExpanded}
          >
            <div className="toolbar-select-options" ref={menuContentRef}>
              {props.options.map((option) => (
                <button
                  className={option.key === props.value ? 'is-selected' : ''}
                  type="button"
                  role="option"
                  aria-selected={option.key === props.value}
                  tabIndex={isExpanded ? 0 : -1}
                  key={option.key}
                  onClick={() => {
                    props.onChange(option.key)
                    closeSelect()
                  }}
                >
                  <span>{option.label}</span>
                </button>
              ))}
            </div>
          </div>
        ) : null}
      </div>
    </div>
  )
}

function EmptyState(props: { icon: ReactNode; title: string; caption: string }): JSX.Element {
  return (
    <div className="library-empty-state">
      {props.icon}
      <strong>{props.title}</strong>
      <span>{props.caption}</span>
    </div>
  )
}

function HorizontalScroller(props: {
  className: string
  children: ReactNode
  pageRatio?: number
}): JSX.Element {
  const scrollerRef = useRef<HTMLDivElement | null>(null)
  const [scrollState, setScrollState] = useState({ canLeft: false, canRight: false })

  function updateScrollState(): void {
    const scroller = scrollerRef.current
    if (!scroller) return
    const maxScrollLeft = scroller.scrollWidth - scroller.clientWidth
    setScrollState({
      canLeft: scroller.scrollLeft > 2,
      canRight: scroller.scrollLeft < maxScrollLeft - 2
    })
  }

  function scrollPage(direction: -1 | 1): void {
    const scroller = scrollerRef.current
    if (!scroller) return
    const distance = Math.max(160, scroller.clientWidth * (props.pageRatio ?? 0.72))
    scroller.scrollBy({ left: direction * distance, behavior: 'smooth' })
  }

  useLayoutEffect(() => {
    const scroller = scrollerRef.current
    if (!scroller) return undefined

    let frame = 0
    const scheduleUpdate = (): void => {
      window.cancelAnimationFrame(frame)
      frame = window.requestAnimationFrame(updateScrollState)
    }

    scheduleUpdate()
    scroller.addEventListener('scroll', scheduleUpdate, { passive: true })
    window.addEventListener('resize', scheduleUpdate)
    const observer = typeof ResizeObserver === 'undefined' ? undefined : new ResizeObserver(scheduleUpdate)
    observer?.observe(scroller)

    return () => {
      window.cancelAnimationFrame(frame)
      scroller.removeEventListener('scroll', scheduleUpdate)
      window.removeEventListener('resize', scheduleUpdate)
      observer?.disconnect()
    }
  }, [props.children])

  const isScrollable = scrollState.canLeft || scrollState.canRight
  const shellClassName = [
    'horizontal-scroll-shell',
    props.className.includes('library-home-grid') ? 'is-home-grid' : '',
    props.className.includes('library-continue-row') ? 'is-continue-row' : '',
    props.className.includes('library-episode-row') ? 'is-episode-row' : '',
    props.className.includes('library-cast-row') ? 'is-cast-row' : '',
    props.className.includes('library-similar-row') ? 'is-similar-row' : '',
    props.className.includes('library-season-tabs') ? 'is-season-tabs' : ''
  ].filter(Boolean).join(' ')

  return (
    <div
      className={shellClassName}
      data-scrollable={isScrollable}
      data-can-left={scrollState.canLeft}
      data-can-right={scrollState.canRight}
      onPointerEnter={updateScrollState}
    >
      <div className={props.className} ref={scrollerRef}>
        {props.children}
      </div>
      <div className="horizontal-scroll-edge is-left">
        <button
          className="horizontal-scroll-button"
          type="button"
          title="上一页"
          aria-label="上一页"
          disabled={!scrollState.canLeft}
          onClick={() => scrollPage(-1)}
        >
          <ChevronLeft size={18} />
        </button>
      </div>
      <div className="horizontal-scroll-edge is-right">
        <button
          className="horizontal-scroll-button"
          type="button"
          title="下一页"
          aria-label="下一页"
          disabled={!scrollState.canRight}
          onClick={() => scrollPage(1)}
        >
          <ChevronRight size={18} />
        </button>
      </div>
    </div>
  )
}

function ContinueCard(props: {
  item: MediaItem
  selected: boolean
  onSelect: (id: string) => void
}): JSX.Element {
  const art = hasImageBackground(props.item.backdrop) ? props.item.backdrop : props.item.poster
  return (
    <button
      className={`library-continue-card ${props.selected ? 'is-selected' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.id)}
    >
      <MediaArtwork background={art} className="library-continue-art">
        <span className="library-continue-progress" style={{ width: `${Math.round(props.item.progress * 100)}%` }} />
      </MediaArtwork>
      <strong>{props.item.title}</strong>
      <small>{Math.round(props.item.progress * 100)}%</small>
    </button>
  )
}

function MediaPoster(props: {
  item: MediaItem
  source: LibrarySource
  selected: boolean
  onSelect: (id: string) => void
}): JSX.Element {
  return (
    <button
      className={`library-poster-card ${props.selected ? 'is-selected' : ''} ${props.item.availability === 'missing' ? 'is-missing' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.id)}
    >
      <MediaArtwork background={props.item.poster} className="library-poster-art">
        <div className="library-poster-shine" />
        <span className="library-poster-type">{mediaTypeLabel(props.item.type)}</span>
        {props.item.availability === 'missing' ? <span className="library-poster-warning">{currentLibraryLanguage === 'zh' ? '缺失' : 'Missing'}</span> : null}
        {(props.item.versions?.length ?? 0) > 0 ? <span className="library-poster-versions">{(props.item.versions?.length ?? 0) + 1}×</span> : null}
      </MediaArtwork>
      {props.item.progress > 0 && props.item.progress < 1 ? (
        <span className="library-progress-track">
          <span style={{ width: `${Math.round(props.item.progress * 100)}%` }} />
        </span>
      ) : null}
      <span className="library-poster-title">{props.item.title}</span>
      <span className="library-poster-meta">
        {props.item.year} · {props.item.quality} · {props.source.name}
      </span>
    </button>
  )
}

function HomeCard(props: {
  card: LibraryHomeCard
  layout: LibraryHomeSection['layout']
  selected: boolean
  onSelectItem: (id: string) => void
  onSelectView: (id: string) => void
}): JSX.Element {
  const viewId = homeCardViewId(props.card)
  const selectable = Boolean(props.card.itemId || viewId)

  return (
    <button
      className={`library-home-card is-${props.layout} ${props.selected ? 'is-selected' : ''}`}
      type="button"
      onClick={() => {
        if (props.card.itemId) props.onSelectItem(props.card.itemId)
        else if (viewId) props.onSelectView(viewId)
      }}
      aria-disabled={!selectable}
    >
      {props.card.kind === 'view' ? (
        <div className="library-home-card-art" style={{ background: props.card.image }}>
          <span className="library-home-view-icon">{sourceIcon('Emby', 16)}</span>
        </div>
      ) : (
        <MediaArtwork background={props.card.image} className="library-home-card-art">
          <div className="library-poster-shine" />
          {props.card.mediaType ? (
            <span className="library-poster-type">{mediaTypeLabel(props.card.mediaType)}</span>
          ) : null}
        </MediaArtwork>
      )}
      <span className="library-home-card-title">{props.card.title}</span>
      <span className="library-home-card-meta">{props.card.subtitle}</span>
    </button>
  )
}

function LibraryHomeSections(props: {
  sections: LibraryHomeSection[]
  selectedId: string
  selectedViewId: string
  onSelectItem: (id: string) => void
  onSelectView: (id: string) => void
}): JSX.Element {
  return (
    <>
      {props.sections.map((section) => (
        <section className="library-home-section" key={section.id}>
          <div className="library-section-heading">
            <span>{section.title}</span>
            <small>{section.cards.length}</small>
          </div>
          <HorizontalScroller className={`library-home-grid is-${section.layout}`}>
            {section.cards.map((card) => (
              <HomeCard
                key={card.id}
                card={card}
                layout={section.layout}
                selected={card.itemId === props.selectedId || homeCardViewId(card) === props.selectedViewId}
                onSelectItem={props.onSelectItem}
                onSelectView={props.onSelectView}
              />
            ))}
          </HorizontalScroller>
        </section>
      ))}
    </>
  )
}

function embeddedTrailerUrl(value: string | undefined): string | undefined {
  if (!value) return undefined
  try {
    const url = new URL(value)
    const host = url.hostname.toLowerCase().replace(/^www\./, '')
    let videoId = ''
    if (host === 'youtu.be') {
      videoId = url.pathname.split('/').filter(Boolean)[0] ?? ''
    } else if (host === 'youtube.com' || host === 'm.youtube.com' || host === 'youtube-nocookie.com') {
      if (url.pathname === '/watch') videoId = url.searchParams.get('v') ?? ''
      else videoId = url.pathname.match(/^\/(?:embed|shorts)\/([^/?#]+)/)?.[1] ?? ''
    }
    if (videoId) {
      const origin = encodeURIComponent(window.location.origin)
      return `https://www.youtube.com/embed/${encodeURIComponent(videoId)}?autoplay=1&rel=0&playsinline=1&enablejsapi=1&origin=${origin}`
    }
    if (host === 'bilibili.com' || host.endsWith('.bilibili.com')) {
      const bvid = url.pathname.match(/\/video\/(BV[0-9A-Za-z]+)/i)?.[1]
      if (bvid) {
        return `https://player.bilibili.com/player.html?bvid=${encodeURIComponent(bvid)}&autoplay=1&muted=0&danmaku=0&poster=1`
      }
    }
    const vimeoId = host === 'vimeo.com' || host === 'player.vimeo.com'
      ? url.pathname.match(/\/(?:video\/)?(\d+)/)?.[1]
      : undefined
    if (vimeoId) return `https://player.vimeo.com/video/${vimeoId}?autoplay=1`
  } catch {
    return undefined
  }
  return undefined
}

function youtubeVideoId(value: string | undefined): string | undefined {
  if (!value) return undefined
  try {
    const url = new URL(value)
    const host = url.hostname.toLowerCase().replace(/^www\./, '')
    if (host === 'youtu.be') return url.pathname.split('/').filter(Boolean)[0]
    if (host !== 'youtube.com' && host !== 'm.youtube.com' && host !== 'youtube-nocookie.com') return undefined
    return url.pathname === '/watch'
      ? url.searchParams.get('v') ?? undefined
      : url.pathname.match(/^\/(?:embed|shorts)\/([^/?#]+)/)?.[1]
  } catch {
    return undefined
  }
}

interface YouTubePlayerInstance {
  destroy: () => void
  mute: () => void
  unMute: () => void
  playVideo: () => void
}

interface YouTubeApi {
  Player: new (element: HTMLElement, options: Record<string, unknown>) => YouTubePlayerInstance
}

declare global {
  interface Window {
    YT?: YouTubeApi
    onYouTubeIframeAPIReady?: () => void
  }
}

let youtubeApiPromise: Promise<YouTubeApi> | undefined

function loadYouTubeApi(): Promise<YouTubeApi> {
  if (window.YT?.Player) return Promise.resolve(window.YT)
  if (youtubeApiPromise) return youtubeApiPromise
  const attempt = new Promise<YouTubeApi>((resolve, reject) => {
    const previousReady = window.onYouTubeIframeAPIReady
    window.onYouTubeIframeAPIReady = () => {
      previousReady?.()
      if (window.YT?.Player) resolve(window.YT)
      else reject(new Error('YouTube player API unavailable'))
    }
    let script = document.querySelector<HTMLScriptElement>('script[data-anvil-youtube-api]')
    if (!script) {
      script = document.createElement('script')
      script.src = 'https://www.youtube.com/iframe_api'
      script.async = true
      script.dataset.anvilYoutubeApi = 'true'
      script.onerror = () => {
        script?.remove()
        reject(new Error('YouTube player API failed to load'))
      }
      document.head.appendChild(script)
    }
    window.setTimeout(() => reject(new Error('YouTube player API timed out')), 12000)
  })
  youtubeApiPromise = attempt.catch((error) => {
    youtubeApiPromise = undefined
    throw error
  })
  return youtubeApiPromise
}

function YouTubeTrailer(props: {
  videoId: string
  backdropStyle: CSSProperties
  muted: boolean
  onPlaybackStarted: () => void
  onAutoplayMuted: () => void
  onError: () => void
}): JSX.Element {
  const playerHostRef = useRef<HTMLDivElement>(null)
  const playerRef = useRef<YouTubePlayerInstance | undefined>(undefined)
  const [ready, setReady] = useState(false)
  const [playbackStarted, setPlaybackStarted] = useState(false)

  useEffect(() => {
    let disposed = false
    let player: YouTubePlayerInstance | undefined
    const timeout = window.setTimeout(() => {
      if (!disposed && !ready) {
        postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer youtube ready timeout id=${props.videoId}` })
        props.onError()
      }
    }, 15000)
    void loadYouTubeApi().then((api) => {
      if (disposed || !playerHostRef.current) return
      const reactHost = playerHostRef.current
      const playerFrame = document.createElement('iframe')
      const origin = encodeURIComponent(window.location.origin)
      playerFrame.src = `https://www.youtube.com/embed/${encodeURIComponent(props.videoId)}?autoplay=1&playsinline=1&rel=0&controls=0&disablekb=1&fs=0&iv_load_policy=3&enablejsapi=1&origin=${origin}`
      playerFrame.allow = 'autoplay; encrypted-media; picture-in-picture; fullscreen'
      playerFrame.allowFullscreen = true
      playerFrame.referrerPolicy = 'origin'
      playerFrame.style.width = '100%'
      playerFrame.style.height = '100%'
      playerFrame.style.border = '0'
      reactHost.replaceChildren(playerFrame)
      player = new api.Player(playerFrame, {
        events: {
          onReady: (event: { target: YouTubePlayerInstance }) => {
            if (disposed) return
            window.clearTimeout(timeout)
            setReady(true)
            postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer youtube ready id=${props.videoId}` })
            playerRef.current = event.target
            if (props.muted) event.target.mute()
            else event.target.unMute()
            event.target.playVideo()
          },
          onError: (event: { data: number }) => {
            postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer youtube error id=${props.videoId} code=${event.data}` })
            if (!disposed) props.onError()
          },
          onStateChange: (event: { data: number }) => {
            if (disposed || event.data !== 1 || playbackStarted) return
            postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer youtube playing id=${props.videoId}` })
            setPlaybackStarted(true)
            props.onPlaybackStarted()
          },
          onAutoplayBlocked: () => {
            if (disposed) return
            window.clearTimeout(timeout)
            setReady(true)
            postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer youtube autoplay blocked id=${props.videoId}` })
            player?.mute()
            player?.playVideo()
            props.onAutoplayMuted()
          }
        }
      })
    }).catch((error) => {
      postNativeCommand({
        type: 'command',
        command: 'debugLog',
        message: `trailer youtube api failed id=${props.videoId} error=${error instanceof Error ? error.message : String(error)}`
      })
      if (!disposed) props.onError()
    })
    return () => {
      disposed = true
      playerRef.current = undefined
      window.clearTimeout(timeout)
      try {
        player?.destroy()
      } catch {
        // The iframe may already have been removed during WebView navigation.
      }
      playerHostRef.current?.replaceChildren()
    }
  }, [props.videoId])

  useEffect(() => {
    if (props.muted) playerRef.current?.mute()
    else playerRef.current?.unMute()
  }, [props.muted])

  return (
    <div className={`library-detail-trailer-shell ${playbackStarted ? 'is-playing' : ''}`}>
      <div className="library-detail-backdrop-image is-trailer-placeholder" style={props.backdropStyle} />
      <div className={`library-detail-trailer-host ${ready ? 'is-ready' : ''}`}>
        <div ref={playerHostRef} />
      </div>
      {!ready ? <div className="library-detail-trailer-loading"><span /></div> : null}
    </div>
  )
}

function DetailPanel(props: {
  item: MediaItem
  source: LibrarySource
  language: UiLanguage
  trailerSource: TrailerSource
  trailerSoundEnabled: boolean
  trailerAutoPlay: boolean
  trailerAutoPlayReady: boolean
  playIntent: string
  isResolvingPlayback: boolean
  canScrapeMetadata: boolean
  isScrapingMetadata: boolean
  canEditMetadata: boolean
  onPlayIntent: (item: MediaItem, audioTrackIndex: number, subtitleTrackIndex: number) => void
  onTrailerIntent: (item: MediaItem) => Promise<string[]>
  onRemoveFromLibrary: (item: MediaItem) => void
  onToggleFavorite: (item: MediaItem) => void
  playlists: MediaPlaylist[]
  onTogglePlaylist: (item: MediaItem, playlistId: string) => void
  onCreatePlaylist: (item: MediaItem) => void
  onScrapeIntent: (item: MediaItem) => void
  onRematchIntent: (item: MediaItem) => void
  onSearchMatch: (item: MediaItem) => void
  onToggleMetadataLock: (item: MediaItem) => void
  onEditMetadata: (item: MediaItem) => void
  onClearMetadata: (item: MediaItem) => void
  onSelectItem: (item: MediaItem) => void
  onSearchPerson: (person: PersonCredit) => void
  onClose: () => void
}): JSX.Element {
  const backdropStyle: CSSProperties = { background: props.item.backdrop }
  const seasons = useMemo(
    () => [...(props.item.seasons ?? [])].sort((left, right) => compareMediaIndex(left.index, right.index)),
    [props.item.seasons]
  )
  const [activeSeasonId, setActiveSeasonId] = useState('')
  const [expandedEpisodeId, setExpandedEpisodeId] = useState('')
  const [selectedEpisodeVersionId, setSelectedEpisodeVersionId] = useState('')
  const [selectedVersionId, setSelectedVersionId] = useState(props.item.id)
  const [selectedAudioStreamId, setSelectedAudioStreamId] = useState('')
  const [selectedSubtitleStreamId, setSelectedSubtitleStreamId] = useState('subtitle-auto')
  const [metadataMenuOpen, setMetadataMenuOpen] = useState(false)
  const [playlistMenuOpen, setPlaylistMenuOpen] = useState(false)
  const [trailerPlaying, setTrailerPlaying] = useState(false)
  const [trailerPlaybackStarted, setTrailerPlaybackStarted] = useState(false)
  const initialTrailerUrls = (): string[] => [...new Set([
    props.item.trailerUrl,
    ...(props.item.trailerUrls ?? [])
  ].filter((url): url is string => typeof url === 'string' && trailerUrlMatchesSource(url, props.trailerSource)))]
  const [resolvedTrailerUrls, setResolvedTrailerUrls] = useState<string[]>(initialTrailerUrls)
  const [trailerUrlIndex, setTrailerUrlIndex] = useState(0)
  const [isResolvingTrailer, setIsResolvingTrailer] = useState(false)
  const [trailerPlaybackFailed, setTrailerPlaybackFailed] = useState(false)
  const [trailerMuted, setTrailerMuted] = useState(
    !props.trailerSoundEnabled
  )
  const autoPlayedTrailerRef = useRef('')
  const [overviewExpanded, setOverviewExpanded] = useState(false)
  const [overviewOverflowing, setOverviewOverflowing] = useState(false)
  const [overviewExpandedHeight, setOverviewExpandedHeight] = useState(0)
  const overviewRef = useRef<HTMLParagraphElement>(null)
  const resolvedTrailerUrl = resolvedTrailerUrls[trailerUrlIndex]
  const trailerEmbedUrl = useMemo(() => embeddedTrailerUrl(resolvedTrailerUrl), [resolvedTrailerUrl])
  const trailerYouTubeId = useMemo(() => youtubeVideoId(resolvedTrailerUrl), [resolvedTrailerUrl])
  const activeSeason = seasons.find((season) => season.id === activeSeasonId) ?? seasons[0]
  const seasonLabel = (index: string): string => index === 'SP'
    ? props.language === 'zh' ? '外传' : 'Specials'
    : index
  const seasonOptions = seasons.map((season) => ({ key: season.id, label: seasonLabel(season.index) }))
  const activeSeasonSelectId = activeSeason?.id ?? seasonOptions[0]?.key ?? ''
  const episodeRows = activeSeason?.episodes ?? props.item.episodes ?? []
  const episodeCount = seasons.length
    ? seasons.reduce((total, season) => total + (season.episodes.length || season.episodeCount), 0)
    : props.item.episodes?.length ?? 0
  const detailRuntime = props.item.type === 'series'
    ? episodeCount > 0
      ? props.language === 'zh' ? `共${episodeCount}集` : `${episodeCount} episodes`
      : props.item.runtime
    : props.item.runtime
  const expandedEpisode = episodeRows.find((episode) => episode.id === expandedEpisodeId)
  const expandedEpisodeVersions = expandedEpisode?.item ? [expandedEpisode.item, ...(expandedEpisode.item.versions ?? [])] : []
  const selectedEpisodeVersion = expandedEpisodeVersions.find((version) => version.id === selectedEpisodeVersionId) ?? expandedEpisodeVersions[0]
  const playbackVersions = useMemo(() => [props.item, ...(props.item.versions ?? [])], [props.item])
  const selectedPlaybackVersion = playbackVersions.find((version) => version.id === selectedVersionId) ?? playbackVersions[0]
  const versionOptions = playbackVersions.map((version, index) => ({
    key: version.id,
    label: mediaVersionLabel(version) || `${props.language === 'zh' ? '版本' : 'Version'} ${index + 1}`
  }))
  const audioStreams = selectedPlaybackVersion.streamSpecs?.filter((stream) => stream.type === 'audio') ?? []
  const subtitleStreams = selectedPlaybackVersion.streamSpecs?.filter((stream) => stream.type === 'subtitle') ?? []
  const selectedAudioStream = audioStreams.find((stream) => stream.id === selectedAudioStreamId)
    ?? audioStreams.find((stream) => stream.isDefault)
    ?? audioStreams[0]
  const selectedSubtitleStream = subtitleStreams.find((stream) => stream.id === selectedSubtitleStreamId)
  const audioOptions = audioStreams.map((stream) => ({
    key: stream.id,
    label: cleanJoin([stream.title, stream.subtitle], ' · ')
  }))
  const subtitleOptions = [
    { key: 'subtitle-auto', label: props.language === 'zh' ? '自动' : 'Auto' },
    { key: 'subtitle-off', label: props.language === 'zh' ? '关' : 'Off' },
    ...subtitleStreams.map((stream) => ({ key: stream.id, label: cleanJoin([stream.title, stream.subtitle], ' · ') }))
  ]
  const streamIndex = (id: string | undefined, fallback: number): number => {
    const match = id?.match(/(\d+)$/)
    return match ? Number(match[1]) : fallback
  }

  useEffect(() => {
    if (!seasons.length) {
      if (activeSeasonId) setActiveSeasonId('')
      return
    }
    if (!activeSeasonId || !seasons.some((season) => season.id === activeSeasonId)) {
      setActiveSeasonId(seasons[0].id)
    }
  }, [activeSeasonId, seasons])

  useEffect(() => {
    setSelectedVersionId(props.item.id)
    setTrailerPlaying(false)
    setTrailerPlaybackStarted(false)
    setResolvedTrailerUrls(initialTrailerUrls())
    setTrailerUrlIndex(0)
    setIsResolvingTrailer(false)
    setTrailerPlaybackFailed(false)
    const defaultMuted = !props.trailerSoundEnabled
    setTrailerMuted(defaultMuted)
    postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: defaultMuted })
    setOverviewExpanded(false)
    setOverviewOverflowing(false)
    setOverviewExpandedHeight(0)
  }, [props.item.id, props.trailerSource])

  useEffect(() => {
    const defaultMuted = !props.trailerSoundEnabled
    setTrailerMuted(defaultMuted)
    postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: defaultMuted })
  }, [props.trailerSoundEnabled])

  useEffect(() => {
    const urls = initialTrailerUrls()
    if (urls.length) setResolvedTrailerUrls(urls)
  }, [props.item.trailerUrl, props.item.trailerUrls, props.trailerSource])

  async function toggleTrailer(): Promise<void> {
    setTrailerPlaybackFailed(false)
    if (trailerPlaying) {
      setTrailerPlaying(false)
      setTrailerPlaybackStarted(false)
      setTrailerMuted(false)
      postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: false })
      return
    }
    setTrailerPlaybackStarted(false)
    if (trailerEmbedUrl && resolvedTrailerUrls.length > 1) {
      const defaultMuted = !props.trailerSoundEnabled
      setTrailerMuted(defaultMuted)
      postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: defaultMuted })
      setTrailerPlaying((playing) => !playing)
      return
    }
    setIsResolvingTrailer(true)
    try {
      const urls = await props.onTrailerIntent(props.item)
      if (urls.length) {
        setResolvedTrailerUrls(urls)
        setTrailerUrlIndex(0)
        const defaultMuted = !props.trailerSoundEnabled
        setTrailerMuted(defaultMuted)
        postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: defaultMuted })
        setTrailerPlaying(true)
      } else if (trailerEmbedUrl) {
        const defaultMuted = !props.trailerSoundEnabled
        setTrailerMuted(defaultMuted)
        postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: defaultMuted })
        setTrailerPlaying(true)
      }
    } finally {
      setIsResolvingTrailer(false)
    }
  }

  useEffect(() => () => {
    postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: false })
  }, [])

  useEffect(() => {
    if (!props.trailerAutoPlay) {
      autoPlayedTrailerRef.current = ''
      return
    }
    if (!props.trailerAutoPlayReady) return
    const autoPlayKey = `${props.item.id}:${props.trailerSource}`
    if (autoPlayedTrailerRef.current === autoPlayKey) return
    autoPlayedTrailerRef.current = autoPlayKey
    postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer auto start item=${props.item.id} source=${props.trailerSource}` })
    void toggleTrailer()
  }, [props.item.id, props.trailerSource, props.trailerAutoPlay, props.trailerAutoPlayReady])

  useEffect(() => {
    const nextAudioStreams = selectedPlaybackVersion.streamSpecs?.filter((stream) => stream.type === 'audio') ?? []
    setSelectedAudioStreamId((nextAudioStreams.find((stream) => stream.isDefault) ?? nextAudioStreams[0])?.id ?? '')
    setSelectedSubtitleStreamId('subtitle-auto')
  }, [selectedPlaybackVersion.id])

  useLayoutEffect(() => {
    const overview = overviewRef.current
    if (!overview) return undefined
    const updateOverflow = (): void => {
      setOverviewExpandedHeight(overview.scrollHeight)
      if (overviewExpanded) return
      setOverviewOverflowing(overview.scrollHeight > overview.clientHeight + 1)
    }
    updateOverflow()
    const observer = new ResizeObserver(updateOverflow)
    observer.observe(overview)
    return () => observer.disconnect()
  }, [overviewExpanded, props.item.id, props.item.overview])

  function playMediaAndCloseTrailer(item: MediaItem, audioTrackIndex: number, subtitleTrackIndex: number): void {
    setTrailerPlaying(false)
    setTrailerPlaybackStarted(false)
    setTrailerMuted(false)
    postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: false })
    props.onPlayIntent(item, audioTrackIndex, subtitleTrackIndex)
  }

  return (
    <aside className="library-detail">
      <div className="library-detail-backdrop">
        {trailerPlaying && trailerYouTubeId ? (
          <YouTubeTrailer
            videoId={trailerYouTubeId}
            backdropStyle={backdropStyle}
            muted={trailerMuted}
            onPlaybackStarted={() => setTrailerPlaybackStarted(true)}
            onAutoplayMuted={() => {
              setTrailerMuted(true)
              postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: true })
            }}
            onError={() => {
              if (trailerUrlIndex + 1 < resolvedTrailerUrls.length) {
                setTrailerUrlIndex((index) => index + 1)
                return
              }
              setTrailerPlaybackFailed(true)
              setTrailerPlaying(false)
              setTrailerPlaybackStarted(false)
            }}
          />
        ) : trailerPlaying && trailerEmbedUrl ? (
          <iframe
            className={`library-detail-trailer ${trailerPlaybackStarted ? 'is-playing' : ''}`}
            src={trailerEmbedUrl}
            title={`${props.item.title} trailer`}
            allow="autoplay; encrypted-media; picture-in-picture; fullscreen"
            referrerPolicy="strict-origin-when-cross-origin"
            allowFullScreen
            onLoad={() => {
              postNativeCommand({ type: 'command', command: 'debugLog', message: `trailer iframe loaded item=${props.item.id} source=${props.trailerSource}` })
              setTrailerPlaybackStarted(true)
            }}
          />
        ) : (
          <div className="library-detail-backdrop-image" style={backdropStyle} />
        )}
        {trailerPlaying ? (
          <button
            className={`library-trailer-mute ${trailerMuted ? 'is-muted' : ''}`}
            type="button"
            title={trailerMuted
              ? (props.language === 'zh' ? '恢复声音' : 'Unmute')
              : (props.language === 'zh' ? '静音' : 'Mute')}
            aria-label={trailerMuted
              ? (props.language === 'zh' ? '恢复声音' : 'Unmute')
              : (props.language === 'zh' ? '静音' : 'Mute')}
            onClick={() => {
              const nextMuted = !trailerMuted
              setTrailerMuted(nextMuted)
              postNativeCommand({ type: 'command', command: 'setLibraryWebViewMuted', muted: nextMuted })
            }}
          >
            {trailerMuted ? <VolumeX size={17} /> : <Volume2 size={17} />}
          </button>
        ) : null}
        {!trailerPlaying ? (
          <button
            className="library-detail-close"
            type="button"
            title={props.language === 'zh' ? '关闭详情' : 'Close details'}
            onPointerDown={(event) => event.stopPropagation()}
            onClick={(event) => {
              event.stopPropagation()
              props.onClose()
            }}
          >
            <X size={15} />
          </button>
        ) : null}
        {!trailerPlaybackStarted ? <div className="library-detail-gradient" /> : null}
        {!trailerPlaybackStarted ? (
          <div className="library-detail-copy">
          <span className="library-source-pill">
            {sourceIcon(props.source.kind, 13)}
            {props.source.name}
          </span>
          <h2>{props.item.title}</h2>
          <div className="library-metadata-line">
            <span>
              <Star size={13} />
              {props.item.rating.toFixed(1)}
            </span>
            <span>{props.item.year}</span>
            <span>{detailRuntime}</span>
          </div>
            <p>{props.item.originalTitle}</p>
          </div>
        ) : null}
      </div>

      <div className="library-detail-body">
        {props.item.type !== 'series' ? (
          <><div className="library-version-row">
          <span>{props.language === 'zh' ? '视频' : 'Video'}</span>
          {playbackVersions.length > 1 ? (
            <ToolbarSelect
              className="library-version-select"
              value={selectedPlaybackVersion.id}
              options={versionOptions}
              ariaLabel={props.language === 'zh' ? '选择播放版本' : 'Select playback version'}
              onChange={setSelectedVersionId}
            />
          ) : (
            <strong>{versionOptions[0]?.label}</strong>
          )}
        </div>
        <div className="library-version-row">
          <span>{props.language === 'zh' ? '音频' : 'Audio'}</span>
          {audioOptions.length > 1 ? (
            <ToolbarSelect
              className="library-version-select"
              value={selectedAudioStream?.id ?? audioOptions[0]?.key ?? ''}
              options={audioOptions}
              ariaLabel={props.language === 'zh' ? '预选音轨' : 'Preselect audio track'}
              onChange={setSelectedAudioStreamId}
            />
          ) : (
            <strong>{audioOptions[0]?.label || (props.language === 'zh' ? '自动' : 'Auto')}</strong>
          )}
        </div>
        <div className="library-version-row">
          <span>{props.language === 'zh' ? '字幕' : 'Subtitles'}</span>
          {subtitleOptions.length > 1 ? (
            <ToolbarSelect
              className="library-version-select"
              value={selectedSubtitleStream?.id ?? selectedSubtitleStreamId}
              options={subtitleOptions}
              ariaLabel={props.language === 'zh' ? '预选字幕' : 'Preselect subtitles'}
              onChange={setSelectedSubtitleStreamId}
            />
          ) : (
            <strong>{props.language === 'zh' ? '关' : 'Off'}</strong>
          )}
          </div></>
        ) : null}
        <div className={`library-detail-actions ${props.item.type === 'series' ? 'is-series' : ''}`}>
          <button
            className="library-primary-action"
            type="button"
            disabled={props.isResolvingPlayback || props.item.availability === 'missing'}
            onClick={() => playMediaAndCloseTrailer(
              selectedPlaybackVersion,
              props.item.type === 'series' ? -2 : streamIndex(selectedAudioStream?.id, -2),
              props.item.type === 'series'
                ? -2
                : selectedSubtitleStreamId === 'subtitle-auto'
                  ? -2
                  : selectedSubtitleStreamId === 'subtitle-off'
                    ? -1
                    : selectedSubtitleStream ? streamIndex(selectedSubtitleStream.id, -2) : -2
            )}
          >
            <Play size={17} />
            <span>{props.item.progress > 0 && props.item.progress < 1 ? '继续播放' : '播放'}</span>
          </button>
          <button
            className={`library-trailer-action ${trailerPlaying ? 'is-active' : ''}`}
            type="button"
            disabled={isResolvingTrailer}
            title={trailerPlaybackFailed
              ? (props.language === 'zh' ? '预告片加载失败，点击重试' : 'Trailer failed to load; click to retry')
              : !trailerEmbedUrl
                ? props.trailerSource === 'bilibili'
                  ? (props.language === 'zh' ? '从 B 站搜索预告片' : 'Find trailer on Bilibili')
                  : (props.language === 'zh' ? '从 TMDB 查找预告片' : 'Find trailer on TMDB')
                : undefined}
            onClick={() => { void toggleTrailer() }}
          >
            <Film size={17} />
            <span>{isResolvingTrailer ? (props.language === 'zh' ? '查找中…' : 'Finding…') : (props.language === 'zh' ? '预告片' : 'Trailer')}</span>
          </button>
        </div>
        <div className="library-utility-actions">
          <button
            className={`library-tool-action library-favorite-action ${props.item.favorite ? 'is-favorite' : ''}`}
            type="button"
            title={props.item.favorite ? (props.language === 'zh' ? '已收藏' : 'Favorited') : (props.language === 'zh' ? '收藏' : 'Favorite')}
            aria-pressed={props.item.favorite}
            onClick={() => props.onToggleFavorite(props.item)}
          >
            <Heart size={18} fill={props.item.favorite ? 'currentColor' : 'none'} />
            <span>{props.item.favorite ? (props.language === 'zh' ? '已收藏' : 'Favorited') : (props.language === 'zh' ? '收藏' : 'Favorite')}</span>
          </button>
          <div className="library-detail-menu">
            <button
              className="library-tool-action"
              type="button"
              title={props.language === 'zh' ? '管理片单' : 'Manage playlists'}
              aria-expanded={playlistMenuOpen}
              onClick={() => setPlaylistMenuOpen((open) => !open)}
            >
              <ListPlus size={18} />
              <span>{props.language === 'zh' ? '片单' : 'Playlists'}</span>
            </button>
            {playlistMenuOpen ? (
              <div className="library-detail-menu-panel library-playlist-menu-panel">
                {props.playlists.map((playlist) => {
                  const selected = props.item.playlistIds?.includes(playlist.id) || (props.item.inPlaylist && playlist.id === WATCH_LATER_PLAYLIST_ID)
                  return <button className={selected ? 'is-active' : ''} type="button" key={playlist.id} onClick={() => props.onTogglePlaylist(props.item, playlist.id)}>
                    <span>{selected ? '✓' : '+'}</span><span>{playlist.name}</span>
                  </button>
                })}
                <button type="button" onClick={() => props.onCreatePlaylist(props.item)}><span>+</span><span>{props.language === 'zh' ? '新建片单' : 'New playlist'}</span></button>
              </div>
            ) : null}
          </div>
          {props.canScrapeMetadata ? (
            <button
              className="library-tool-action"
              type="button"
              title="TMDB 刮削"
              disabled={props.isScrapingMetadata}
              onClick={() => props.onScrapeIntent(props.item)}
            >
              <Wand2 size={18} />
              <span>{props.language === 'zh' ? '刮削' : 'Scrape'}</span>
            </button>
          ) : null}
          <button
            className="library-tool-action is-danger"
            type="button"
            title={props.language === 'zh' ? '从媒体库移除' : 'Remove from library'}
            disabled={props.source.kind === 'Emby'}
            onClick={() => props.onRemoveFromLibrary(props.item)}
          >
            <Trash2 size={18} />
            <span>{props.language === 'zh' ? '删除' : 'Remove'}</span>
          </button>
          {props.canEditMetadata ? (
            <div className="library-detail-menu">
              <button
                className={`library-tool-action ${metadataMenuOpen ? 'is-active' : ''}`}
                type="button"
                title="更多"
                aria-expanded={metadataMenuOpen}
                onClick={() => setMetadataMenuOpen((open) => !open)}
              >
                <MoreHorizontal size={18} />
                <span>{props.language === 'zh' ? '更多' : 'More'}</span>
              </button>
              {metadataMenuOpen ? (
                <div className="library-detail-menu-panel">
                  <button type="button" onClick={() => { setMetadataMenuOpen(false); props.onEditMetadata(props.item) }}>修改元数据</button>
                  <button type="button" onClick={() => { setMetadataMenuOpen(false); props.onClearMetadata(props.item) }}>删除元数据</button>
                </div>
              ) : null}
            </div>
          ) : (
            <button className="library-tool-action" type="button" title="更多" disabled={props.source.kind === 'Emby'}>
              <MoreHorizontal size={18} />
              <span>{props.language === 'zh' ? '更多' : 'More'}</span>
            </button>
          )}
        </div>

        {props.playIntent ? <div className="library-intent">{props.playIntent}</div> : null}

        {props.canScrapeMetadata ? (
          <div className={`library-match-status ${props.item.metadataLocked ? 'is-locked' : ''}`}>
            <div>
              <SearchCheck size={15} />
              <span>{props.item.metadataProvider
                ? `${props.language === 'zh' ? '识别为' : 'Matched as'}：${props.item.metadataMatchTitle || props.item.title}`
                : (props.language === 'zh' ? '尚未识别元数据' : 'Metadata not identified')}</span>
            </div>
            <div>
              <button type="button" disabled={props.isScrapingMetadata || props.item.metadataLocked} onClick={() => props.onRematchIntent(props.item)}>{props.language === 'zh' ? '重新识别' : 'Re-identify'}</button>
              <button type="button" disabled={props.isScrapingMetadata || props.item.metadataLocked} onClick={() => props.onSearchMatch(props.item)}>{props.language === 'zh' ? '搜索并选择' : 'Search & select'}</button>
              <button type="button" className={props.item.metadataLocked ? 'is-active' : ''} onClick={() => props.onToggleMetadataLock(props.item)}>
                <LockKeyhole size={13} />
                {props.item.metadataLocked ? (props.language === 'zh' ? '已锁定' : 'Locked') : (props.language === 'zh' ? '锁定元数据' : 'Lock metadata')}
              </button>
            </div>
          </div>
        ) : null}

        <div className="library-genre-row">
          {props.item.genres.map((genre) => <span key={genre}>{genre}</span>)}
        </div>

        <p className="library-tagline">{props.item.tagline}</p>
        <div className="library-overview-block">
          <p
            ref={overviewRef}
            className={`library-overview ${overviewExpanded ? 'is-expanded' : 'is-collapsed'}`}
            style={{ '--overview-expanded-height': `${overviewExpandedHeight}px` } as CSSProperties}
          >
            {props.item.overview}
          </p>
          {overviewOverflowing || overviewExpanded ? (
            <button type="button" onClick={() => setOverviewExpanded((expanded) => !expanded)}>
              {overviewExpanded
                ? props.language === 'zh' ? '收起' : 'Collapse'
                : props.language === 'zh' ? '展开' : 'More'}
            </button>
          ) : null}
        </div>

        {props.item.availability === 'missing' ? (
          <div className="library-missing-warning">{props.language === 'zh' ? '扫描时未找到这个文件，播放已禁用。重新出现后会自动恢复。' : 'This file was not found during the last scan. Playback will return when it becomes available.'}</div>
        ) : null}

        <div className="library-info-list">
          <div>
            <span>来源</span>
            <strong>{sourceKindLabel(props.source.kind)} · {props.source.location || '未配置'}</strong>
          </div>
          <div>
            <span>国家</span>
            <strong>{props.item.country}</strong>
          </div>
          <div>
            <span>状态</span>
            <strong>{props.item.watched ? '已看完' : props.item.progress > 0 ? '观看中' : '未观看'}</strong>
          </div>
        </div>

        {seasons.length || props.item.episodes?.length ? (
          <section className="library-episode-section">
            <div className="library-section-heading">
              <span>{seasons.length ? '分季分集' : '剧集'}</span>
              <span className="library-section-count">共 {episodeRows.length} 集</span>
            </div>
            {seasons.length ? (
              <div className="library-season-select-row">
                {seasons.length > 1 ? (
                  <ToolbarSelect
                    className="library-season-select"
                    value={activeSeasonSelectId}
                    options={seasonOptions}
                    ariaLabel="选择季"
                    onChange={setActiveSeasonId}
                  />
                ) : (
                  <span className="library-season-badge">{seasonLabel(activeSeason?.index ?? 'S1')}</span>
                )}
              </div>
            ) : null}
            <HorizontalScroller className="library-episode-row">
              {episodeRows.map((episode) => (
                <button
                  className="library-episode-card"
                  type="button"
                  disabled={props.isResolvingPlayback || !episode.item}
                  key={episode.id}
                  onClick={() => {
                    if (!episode.item) return
                    if (episode.item.versions?.length) {
                      setExpandedEpisodeId((current) => current === episode.id ? '' : episode.id)
                      setSelectedEpisodeVersionId(episode.item.id)
                      return
                    }
                    playMediaAndCloseTrailer(episode.item, -2, -2)
                  }}
                >
                  <MediaArtwork background={episode.poster} className="library-episode-thumb">
                    <Play size={18} />
                  </MediaArtwork>
                  <span>{episode.index}</span>
                  <strong>{episode.title}</strong>
                  <small>{episode.duration}</small>
                </button>
              ))}
            </HorizontalScroller>
            {expandedEpisode?.item?.versions?.length ? (
              <div className="library-episode-version-picker">
                <span>{props.language === 'zh' ? `${expandedEpisode.title} · 选择版本` : `${expandedEpisode.title} · Select version`}</span>
                <div>
                  <button className="library-episode-version-play" type="button" onClick={() => selectedEpisodeVersion && playMediaAndCloseTrailer(selectedEpisodeVersion, -2, -2)}>
                    <Play size={14} />{props.language === 'zh' ? '播放' : 'Play'}
                  </button>
                  <ToolbarSelect
                    className="library-version-select"
                    value={selectedEpisodeVersion?.id ?? expandedEpisode.item.id}
                    options={expandedEpisodeVersions.map((version) => ({ key: version.id, label: mediaVersionLabel(version) }))}
                    ariaLabel={props.language === 'zh' ? '选择剧集版本' : 'Select episode version'}
                    onChange={setSelectedEpisodeVersionId}
                  />
                </div>
              </div>
            ) : null}
          </section>
        ) : null}

        {props.item.cast?.length ? (
          <section className="library-detail-section">
            <div className="library-section-heading">
              <span>演职人员</span>
            </div>
            <HorizontalScroller className="library-cast-row">
              {props.item.cast.map((person) => (
                <button className="library-person-card" type="button" key={person.id} onClick={() => props.onSearchPerson(person)}>
                  <div style={{ background: person.image }} />
                  <strong>{person.name}</strong>
                  <small>{person.role}</small>
                </button>
              ))}
            </HorizontalScroller>
          </section>
        ) : null}

        {props.item.similarItems?.length ? (
          <section className="library-detail-section">
            <div className="library-section-heading">
              <span>更多类似</span>
            </div>
            <HorizontalScroller className="library-similar-row">
              {props.item.similarItems.map((item) => (
                <button className="library-similar-card" type="button" key={item.id} onClick={() => props.onSelectItem(item)}>
                  <MediaArtwork background={item.poster} className="library-similar-art" />
                  <strong>{item.title}</strong>
                  <small>{item.year || item.runtime}</small>
                </button>
              ))}
            </HorizontalScroller>
          </section>
        ) : null}

        {props.item.streamSpecs?.length ? (
          <section className="library-detail-section">
            <div className="library-section-heading">
              <span>媒体流信息</span>
            </div>
            <div className="library-stream-grid">
              {props.item.streamSpecs.map((stream) => (
                <div className="library-stream-card" key={stream.id}>
                  <div className="library-stream-card-title">
                    {stream.type === 'video' ? <Tv size={14} /> : stream.type === 'audio' ? <Database size={14} /> : <Languages size={14} />}
                    <div>
                      <strong>{stream.title}</strong>
                      <span>{stream.subtitle}</span>
                    </div>
                  </div>
                  <dl>
                    {stream.details.map((detail) => (
                      <div key={`${stream.id}:${detail.label}`}>
                        <dt>{detail.label}</dt>
                        <dd>{detail.value}</dd>
                      </div>
                    ))}
                  </dl>
                </div>
              ))}
            </div>
          </section>
        ) : null}
      </div>

      {props.isResolvingPlayback ? (
        <div className="library-detail-loading" role="status" aria-label="正在解析播放">
          <span />
        </div>
      ) : null}
    </aside>
  )
}

function MetadataEditorDialog(props: {
  item: MediaItem
  saving: boolean
  onClose: () => void
  onSave: (item: MediaItem) => void
}): JSX.Element {
  const [form, setForm] = useState<MetadataEditForm>(() => metadataFormFromItem(props.item))

  useEffect(() => {
    setForm(metadataFormFromItem(props.item))
  }, [props.item])

  function updateField<Key extends keyof MetadataEditForm>(key: Key, value: MetadataEditForm[Key]): void {
    setForm((current) => ({ ...current, [key]: value }))
  }

  function submitForm(event: FormEvent<HTMLFormElement>): void {
    event.preventDefault()
    props.onSave(buildManualMetadataItem(props.item, form))
  }

  return (
    <div className="library-metadata-editor-backdrop" role="presentation" onMouseDown={props.onClose}>
      <form
        className="library-metadata-editor"
        onSubmit={submitForm}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="library-metadata-editor-heading">
          <div>
            <span>文件系统</span>
            <h2>修改元数据</h2>
          </div>
          <button className="library-icon-action" type="button" title="关闭" onClick={props.onClose}>
            <X size={16} />
          </button>
        </div>

        <div className="library-metadata-editor-grid">
          <label className="library-metadata-editor-field">
            <span>标题</span>
            <input autoFocus value={form.title} onChange={(event) => updateField('title', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>原始标题</span>
            <input value={form.originalTitle} onChange={(event) => updateField('originalTitle', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>类型</span>
            <select value={form.type} onChange={(event) => updateField('type', event.target.value as MediaItem['type'])}>
              <option value="movie">电影</option>
              <option value="series">剧集</option>
              <option value="folder">文件夹</option>
            </select>
          </label>
          <label className="library-metadata-editor-field">
            <span>年份</span>
            <input value={form.year} inputMode="numeric" onChange={(event) => updateField('year', event.target.value.replace(/[^\d]/g, ''))} />
          </label>
          <label className="library-metadata-editor-field">
            <span>评分</span>
            <input value={form.rating} inputMode="decimal" onChange={(event) => updateField('rating', event.target.value.replace(/[^\d.]/g, ''))} />
          </label>
          <label className="library-metadata-editor-field">
            <span>时长</span>
            <input value={form.runtime} onChange={(event) => updateField('runtime', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>分类</span>
            <input value={form.genres} onChange={(event) => updateField('genres', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>国家/地区</span>
            <input value={form.country} onChange={(event) => updateField('country', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>清晰度</span>
            <input value={form.quality} onChange={(event) => updateField('quality', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field is-wide">
            <span>海报 URL</span>
            <input value={form.poster} onChange={(event) => updateField('poster', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field is-wide">
            <span>背景图 URL</span>
            <input value={form.backdrop} onChange={(event) => updateField('backdrop', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field is-wide">
            <span>标语</span>
            <input value={form.tagline} onChange={(event) => updateField('tagline', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field is-wide">
            <span>简介</span>
            <textarea value={form.overview} onChange={(event) => updateField('overview', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>TMDB ID</span>
            <input value={form.tmdbId} onChange={(event) => updateField('tmdbId', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>IMDB ID</span>
            <input value={form.imdbId} onChange={(event) => updateField('imdbId', event.target.value)} />
          </label>
          <label className="library-metadata-editor-field">
            <span>TVDB ID</span>
            <input value={form.tvdbId} onChange={(event) => updateField('tvdbId', event.target.value)} />
          </label>
        </div>

        <div className="library-metadata-editor-actions">
          <button className="library-secondary-action" type="button" onClick={props.onClose}>
            <X size={15} />
            <span>取消</span>
          </button>
          <button className="library-connect-action" type="submit" disabled={props.saving}>
            <Check size={15} />
            <span>{props.saving ? '保存中' : '保存'}</span>
          </button>
        </div>
      </form>
    </div>
  )
}

function EmptyDetail(): JSX.Element {
  return (
    <aside className="library-detail library-detail-empty">
      <div>
        <Film size={28} />
        <strong>未选择媒体</strong>
        <span>接入真实媒体源后会显示详情</span>
      </div>
    </aside>
  )
}

function FileServiceDirectoryBrowser(props: {
  currentPath: string
  directories: FileServiceDirectory[]
  selectedPaths: string[]
  loading: boolean
  emptyLabel: string
  parentPath: string
  onBrowse: (path?: string) => void
  onToggle: (path: string) => void
}): JSX.Element {
  return (
    <div className="library-folder-browser">
      <div className="library-folder-browser-toolbar">
        <button
          className="library-secondary-action"
          type="button"
          disabled={props.loading || !props.parentPath}
          onClick={() => props.onBrowse(props.parentPath)}
        >
          <ArrowLeft size={15} />
          <span>上级</span>
        </button>
        <button
          className="library-secondary-action"
          type="button"
          disabled={props.loading}
          onClick={() => props.onBrowse(props.currentPath || undefined)}
        >
          <Search size={15} />
          <span>{props.loading ? '读取中' : '刷新'}</span>
        </button>
        <button
          className="library-secondary-action"
          type="button"
          disabled={props.loading || !props.currentPath}
          onClick={() => props.onToggle(props.currentPath)}
        >
          <Check size={15} />
          <span>{props.selectedPaths.some((path) => locationKey(path) === locationKey(props.currentPath)) ? '取消当前' : '选择当前'}</span>
        </button>
        <span className="library-folder-browser-path">{props.currentPath || '尚未连接'}</span>
      </div>

      <div className="library-folder-browser-list">
        {props.directories.length ? props.directories.map((directory) => (
          <div className="library-folder-browser-row" key={directory.path}>
            <label>
              <input
                type="checkbox"
                checked={props.selectedPaths.some((path) => locationKey(path) === locationKey(directory.path))}
                onChange={() => props.onToggle(directory.path)}
              />
              <FolderOpen size={16} />
              <span>{directory.name}</span>
            </label>
            <button className="library-icon-action" type="button" onClick={() => props.onBrowse(directory.path)}>
              <ChevronRight size={16} />
            </button>
          </div>
        )) : (
          <div className="library-folder-browser-empty">{props.loading ? '正在读取...' : props.emptyLabel}</div>
        )}
      </div>

      {props.selectedPaths.length ? (
        <div className="library-folder-selected-list">
          {props.selectedPaths.map((path) => (
            <button type="button" key={path} onClick={() => props.onToggle(path)}>
              <Check size={13} />
              <span>{directoryDisplayName(path, path)}</span>
              <X size={13} />
            </button>
          ))}
        </div>
      ) : null}
    </div>
  )
}

function SourceTypePicker(props: {
  onSelectEmby: () => void
  onSelectLocalFolder: () => void
  onSelectSmb: () => void
  onSelectWebDav: () => void
}): JSX.Element {
  return (
    <section className="library-source-setup">
      <div className="library-source-setup-heading">
        <span>媒体源</span>
        <h2>添加媒体源</h2>
      </div>

      <div className="library-source-type-sections">
        <section className="library-source-type-section">
          <span>文件系统</span>
          <div className="library-source-type-grid">
            <button className="library-source-type-button" type="button" onClick={props.onSelectLocalFolder}>
              {sourceIcon('Local', 22)}
              <strong>本地文件夹</strong>
              <span>扫描本机目录，后续可自行刮削</span>
            </button>
            <button className="library-source-type-button" type="button" onClick={props.onSelectSmb}>
              {sourceIcon('SMB', 22)}
              <strong>SMB</strong>
              <span>扫描 Windows 共享或映射盘路径</span>
            </button>
            <button className="library-source-type-button" type="button" onClick={props.onSelectWebDav}>
              {sourceIcon('WebDAV', 22)}
              <strong>WebDAV</strong>
              <span>连接常见 WebDAV 文件服务</span>
            </button>
          </div>
        </section>
        <section className="library-source-type-section">
          <span>媒体库服务</span>
          <div className="library-source-type-grid">
            <button className="library-source-type-button" type="button" onClick={props.onSelectEmby}>
              {sourceIcon('Emby', 22)}
              <strong>Emby</strong>
              <span>服务器媒体库</span>
            </button>
          </div>
        </section>
      </div>
    </section>
  )
}

const settingsCopy: Record<UiLanguage, {
  settings: string
  globalSettings: string
  interfaceLanguage: string
  languageCaption: string
  appearance: string
  tone: string
  playerTone: string
  playbackHint: string
  displayPassthrough: string
  displayPassthroughCaption: string
  autoDisplayFormat: string
  autoDisplayFormatCaption: string
  displayMetadataPassthrough: string
  displayMetadataPassthroughCaption: string
  hdrDisplayPeak: string
  hdrDisplayPeakCaption: string
  automatic: string
  nits: string
  dolbyVisionSystemPipelineExperimental: string
  dolbyVisionSystemPipelineExperimentalCaption: string
  refreshRateSync: string
  refreshRateSyncCaption: string
  refreshRateSyncRequirement: string
  enabled: string
  disabled: string
  maximumRefreshMultiple: string
  tmdb: string
  tmdbCaption: string
  networkMode: string
  officialNetwork: string
  alternateNetwork: string
  customNetwork: string
  apiBaseUrl: string
  imageBaseUrl: string
  authMode: string
  apiKey: string
  readToken: string
  credential: string
  metadataLanguage: string
  testConnection: string
  trailerSource: string
  trailerSourceCaption: string
  experimental: string
  youtube: string
  bilibili: string
  bilibiliKeyword: string
  bilibiliKeywordHint: string
  trailerDefaultSound: string
  autoPlayTrailer: string
  chinese: string
  english: string
}> = {
  zh: {
    settings: '设置',
    globalSettings: '全局设置',
    interfaceLanguage: '界面语言',
    languageCaption: '用于媒体库和播放器的界面文本。',
    appearance: '外观',
    tone: '色调',
    playerTone: '跟随播放器',
    playbackHint: '播放相关设置仍在播放器侧边栏中调整。',
    displayPassthrough: '显示直通',
    displayPassthroughCaption: '全局默认。播放器会根据片源规格选择显示输出，并在停止播放时恢复。',
    autoDisplayFormat: '自动匹配显示格式',
    autoDisplayFormatCaption: '根据片源自动切换 Windows 与播放器 HDR，播放结束后恢复原始显示状态。',
    displayMetadataPassthrough: '显示元数据直通',
    displayMetadataPassthroughCaption: '向电视传递 HDR 母版色域、MaxCLL 和 MaxFALL 元数据。',
    hdrDisplayPeak: 'HDR 显示峰值',
    hdrDisplayPeakCaption: '全局默认。自动读取 Windows 当前显示器峰值，读取失败时使用 1000 尼特。直通开启时由显示设备负责。',
    automatic: '自动',
    nits: '尼特',
    dolbyVisionSystemPipelineExperimental: '杜比视界直通（实验性）',
    dolbyVisionSystemPipelineExperimentalCaption: '强制杜比视界片源使用 Windows MediaEngine 与 Dolby Vision Extensions；默认关闭。',
    refreshRateSync: '智能刷新率同步',
    refreshRateSyncCaption: '全局默认。播放视频进入全屏时，自动选择与帧率整数倍匹配的最高刷新率。',
    refreshRateSyncRequirement: '请先在显卡控制面板创建片源所需的精确刷新率，例如 23.976 Hz 或 119.880 Hz。',
    enabled: '开启',
    disabled: '关闭',
    maximumRefreshMultiple: '使用最大倍率刷新率',
    tmdb: 'TMDB 刮削',
    tmdbCaption: '用于本地文件系统媒体的海报、简介、评分和演职员信息。',
    networkMode: '网络',
    officialNetwork: '官方',
    alternateNetwork: '备用域名',
    customNetwork: '自定义',
    apiBaseUrl: 'API 地址',
    imageBaseUrl: '图片地址',
    authMode: '认证',
    apiKey: 'API Key',
    readToken: 'Read Token',
    credential: '凭据',
    metadataLanguage: '语言',
    testConnection: '测试连接',
    trailerSource: '预告片来源',
    trailerSourceCaption: '选择详情页预告片的搜索与播放来源。B 站搜索为实验性功能，结果可能受网络和平台风控影响。',
    experimental: '实验性',
    youtube: 'YouTube',
    bilibili: '哔哩哔哩',
    bilibiliKeyword: 'B 站搜索关键词',
    bilibiliKeywordHint: '支持 {title}、{originalTitle} 和 {year} 占位符',
    trailerDefaultSound: '预告片默认开启声音',
    autoPlayTrailer: '打开媒体详情时自动播放预告片',
    chinese: '中文',
    english: 'English'
  },
  en: {
    settings: 'Settings',
    globalSettings: 'Global Settings',
    interfaceLanguage: 'Interface language',
    languageCaption: 'Used by the media library and player interface.',
    appearance: 'Appearance',
    tone: 'Tone',
    playerTone: 'Follow player',
    playbackHint: 'Playback-specific settings remain in the player sidebar.',
    displayPassthrough: 'Display passthrough',
    displayPassthroughCaption: 'Global default. Match the display output to the source and restore it when playback stops.',
    autoDisplayFormat: 'Automatically match display format',
    autoDisplayFormatCaption: 'Switch Windows and player HDR for the current media, then restore the original display state.',
    displayMetadataPassthrough: 'Display metadata passthrough',
    displayMetadataPassthroughCaption: 'Pass HDR mastering primaries, MaxCLL, and MaxFALL metadata to the TV.',
    hdrDisplayPeak: 'HDR display peak',
    hdrDisplayPeakCaption: 'Global default. Auto reads the current Windows display and falls back to 1000 nits. Passthrough delegates this work to the display.',
    automatic: 'Auto',
    nits: 'nits',
    dolbyVisionSystemPipelineExperimental: 'Dolby Vision passthrough (experimental)',
    dolbyVisionSystemPipelineExperimentalCaption: 'Force Dolby Vision sources through Windows MediaEngine and Dolby Vision Extensions. Disabled by default.',
    refreshRateSync: 'Smart refresh-rate sync',
    refreshRateSyncCaption: 'Global default. In fullscreen, select the highest refresh rate that is an integer multiple of the video frame rate.',
    refreshRateSyncRequirement: 'Create the exact required mode in the GPU control panel first, such as 23.976 Hz or 119.880 Hz.',
    enabled: 'Enabled',
    disabled: 'Disabled',
    maximumRefreshMultiple: 'Use maximum refresh multiple',
    tmdb: 'TMDB scraping',
    tmdbCaption: 'Used for posters, overview, ratings, and cast on local filesystem media.',
    networkMode: 'Network',
    officialNetwork: 'Official',
    alternateNetwork: 'Alternate',
    customNetwork: 'Custom',
    apiBaseUrl: 'API base URL',
    imageBaseUrl: 'Image base URL',
    authMode: 'Auth',
    apiKey: 'API Key',
    readToken: 'Read Token',
    credential: 'Credential',
    metadataLanguage: 'Language',
    testConnection: 'Test connection',
    trailerSource: 'Trailer source',
    trailerSourceCaption: 'Choose the trailer search and playback provider. Bilibili search is experimental and may be affected by network or platform restrictions.',
    experimental: 'Experimental',
    youtube: 'YouTube',
    bilibili: 'Bilibili',
    bilibiliKeyword: 'Bilibili search keywords',
    bilibiliKeywordHint: 'Supports {title}, {originalTitle}, and {year} placeholders',
    trailerDefaultSound: 'Enable trailer sound by default',
    autoPlayTrailer: 'Autoplay trailers when opening media details',
    chinese: '中文',
    english: 'English'
  }
}

function LibrarySettingsPage(props: {
  language: UiLanguage
  onLanguageChange: (language: UiLanguage) => void
  tmdbSettings: TmdbSettings
  tmdbStatus: string
  isTestingTmdb: boolean
  onTmdbSettingsChange: (settings: TmdbSettings) => void
  onTestTmdb: () => void
  trailerSettings: TrailerSettings
  onTrailerSettingsChange: (settings: TrailerSettings) => void
  refreshRateSyncEnabled: boolean
  onRefreshRateSyncChange: (enabled: boolean) => void
  refreshRateMaximumMultiple: boolean
  onRefreshRateMaximumMultipleChange: (enabled: boolean) => void
  displayMetadataPassthrough: boolean
  displayPeakBrightnessNits: number
  autoDisplayFormat: boolean
  dolbyVisionSystemPipelineExperimental: boolean
  windowsHdrEnabled: boolean
  onDisplayMetadataPassthroughChange: (enabled: boolean) => void
  onDisplayPeakBrightnessChange: (peakNits: number) => void
  onAutoDisplayFormatChange: (enabled: boolean) => void
  onDolbyVisionSystemPipelineExperimentalChange: (enabled: boolean) => void
}): JSX.Element {
  const t = settingsCopy[props.language]
  const languageOptions: Array<{ key: UiLanguage; label: string }> = [
    { key: 'zh', label: t.chinese },
    { key: 'en', label: t.english }
  ]
  const tmdbNetworkOptions: Array<{ value: TmdbNetworkMode; label: string }> = [
    { value: 'official', label: t.officialNetwork },
    { value: 'alternate', label: t.alternateNetwork },
    { value: 'custom', label: t.customNetwork }
  ]
  const tmdbAuthOptions: Array<{ value: TmdbAuthMode; label: string }> = [
    { value: 'apiKey', label: t.apiKey },
    { value: 'readToken', label: t.readToken }
  ]
  const peakPassthroughActive = props.autoDisplayFormat ||
    props.displayMetadataPassthrough ||
    props.dolbyVisionSystemPipelineExperimental
  const displayPeakValue = props.displayPeakBrightnessNits > 0
    ? Math.max(100, Math.min(10000, props.displayPeakBrightnessNits))
    : 1000
  const updateTmdbSettings = (patch: Partial<TmdbSettings>): void => {
    props.onTmdbSettingsChange({ ...props.tmdbSettings, ...patch })
  }

  return (
    <section className="library-settings-page">
      <div className="library-settings-heading">
        <Settings2 size={20} />
        <div>
          <span>{t.globalSettings}</span>
          <h2>{t.settings}</h2>
        </div>
      </div>

      <section className="library-settings-panel library-language-panel">
        <div className="library-settings-panel-title">
          <Languages size={16} />
          <span>{t.interfaceLanguage}</span>
        </div>
        <p>{t.languageCaption}</p>
        <ToolbarSelect
          className="library-settings-select"
          value={props.language}
          options={languageOptions}
          ariaLabel={t.interfaceLanguage}
          onChange={props.onLanguageChange}
        />
      </section>

      <section className="library-settings-panel">
        <div className="library-settings-panel-title">
          <Cloud size={16} />
          <span>{t.tmdb}</span>
        </div>
        <p>{t.tmdbCaption}</p>
        <div className="library-settings-form-grid">
          <label className="library-settings-field">
            <span>{t.networkMode}</span>
            <div className="library-setting-options" role="group" aria-label={t.networkMode}>
              {tmdbNetworkOptions.map((option) => (
                <button
                  key={option.value}
                  className={option.value === props.tmdbSettings.networkMode ? 'is-selected' : ''}
                  type="button"
                  onClick={() => updateTmdbSettings({ networkMode: option.value })}
                >
                  <span>{option.label}</span>
                </button>
              ))}
            </div>
          </label>
          <label className="library-settings-field">
            <span>{t.authMode}</span>
            <div className="library-setting-options" role="group" aria-label={t.authMode}>
              {tmdbAuthOptions.map((option) => (
                <button
                  key={option.value}
                  className={option.value === props.tmdbSettings.authMode ? 'is-selected' : ''}
                  type="button"
                  onClick={() => updateTmdbSettings({ authMode: option.value })}
                >
                  <span>{option.label}</span>
                </button>
              ))}
            </div>
          </label>
          <label className="library-settings-field">
            <span>{t.credential}</span>
            <input
              value={props.tmdbSettings.credential}
              type="password"
              onChange={(event) => updateTmdbSettings({ credential: event.target.value })}
              autoComplete="off"
            />
          </label>
          <label className="library-settings-field">
            <span>{t.metadataLanguage}</span>
            <input value={props.tmdbSettings.language} readOnly aria-readonly="true" />
          </label>
          <label className="library-settings-field">
            <span>{t.apiBaseUrl}</span>
            <input
              value={props.tmdbSettings.networkMode === 'custom' ? props.tmdbSettings.customApiBaseUrl : tmdbApiBaseUrl(props.tmdbSettings)}
              disabled={props.tmdbSettings.networkMode !== 'custom'}
              onChange={(event) => updateTmdbSettings({ customApiBaseUrl: event.target.value })}
              placeholder="https://api.themoviedb.org/3"
            />
          </label>
          <label className="library-settings-field">
            <span>{t.imageBaseUrl}</span>
            <input
              value={props.tmdbSettings.imageBaseUrl}
              onChange={(event) => updateTmdbSettings({ imageBaseUrl: event.target.value })}
              placeholder="https://image.tmdb.org/t/p 或代理模板"
            />
          </label>
        </div>
        <button
          className="library-secondary-action"
          type="button"
          disabled={props.isTestingTmdb}
          onClick={props.onTestTmdb}
        >
          <Server size={15} />
          <span>{props.isTestingTmdb ? '测试中' : t.testConnection}</span>
        </button>
        {props.tmdbStatus ? (
          <div className={`library-source-status ${props.tmdbStatus.includes('成功') || props.tmdbStatus.includes('success') ? 'is-success' : ''} ${props.tmdbStatus.includes('失败') || props.tmdbStatus.includes('failed') ? 'is-error' : ''}`}>
            {props.tmdbStatus}
          </div>
        ) : null}
      </section>

      <section className="library-settings-panel">
        <div className="library-settings-panel-title">
          <MonitorCog size={16} />
          <span>{t.displayPassthrough}</span>
        </div>
        <p>{t.displayPassthroughCaption}</p>
        <div className="library-settings-form-grid">
          <label className="library-settings-field">
            <span>{t.autoDisplayFormat}</span>
            <small>{t.autoDisplayFormatCaption}</small>
            <div className="library-setting-options" role="group" aria-label={t.autoDisplayFormat}>
              <button className={props.autoDisplayFormat ? 'is-selected' : ''} type="button" onClick={() => props.onAutoDisplayFormatChange(true)}>
                <span>{t.enabled}</span>
              </button>
              <button className={!props.autoDisplayFormat ? 'is-selected' : ''} type="button" onClick={() => props.onAutoDisplayFormatChange(false)}>
                <span>{t.disabled}</span>
              </button>
            </div>
          </label>
          <label className={`library-settings-field library-hdr-peak ${peakPassthroughActive ? 'is-disabled' : ''}`}>
            <span>{t.hdrDisplayPeak}</span>
            <small>{t.hdrDisplayPeakCaption}</small>
            <div className="library-hdr-peak-controls">
              <button
                className={props.displayPeakBrightnessNits === 0 ? 'is-selected' : ''}
                type="button"
                disabled={peakPassthroughActive}
                onClick={() => props.onDisplayPeakBrightnessChange(
                  props.displayPeakBrightnessNits === 0 ? displayPeakValue : 0
                )}
              >
                {t.automatic}
              </button>
              <input
                type="range"
                min="100"
                max="4000"
                step="50"
                value={Math.min(4000, displayPeakValue)}
                disabled={peakPassthroughActive || props.displayPeakBrightnessNits === 0}
                onChange={(event) => props.onDisplayPeakBrightnessChange(Number(event.currentTarget.value))}
              />
              <input
                className="library-hdr-peak-number"
                type="number"
                min="100"
                max="10000"
                step="50"
                value={displayPeakValue}
                disabled={peakPassthroughActive || props.displayPeakBrightnessNits === 0}
                onChange={(event) => props.onDisplayPeakBrightnessChange(
                  Math.max(100, Math.min(10000, Number(event.currentTarget.value)))
                )}
              />
              <span>{t.nits}</span>
            </div>
          </label>
          <label className="library-settings-field">
            <span>{t.displayMetadataPassthrough}</span>
            <small>{t.displayMetadataPassthroughCaption}</small>
            <div className="library-setting-options" role="group" aria-label={t.displayMetadataPassthrough}>
              <button className={(props.autoDisplayFormat || props.displayMetadataPassthrough) ? 'is-selected' : ''} type="button" disabled={props.autoDisplayFormat || !props.windowsHdrEnabled} onClick={() => props.onDisplayMetadataPassthroughChange(true)}>
                <span>{t.enabled}</span>
              </button>
              <button className={!props.autoDisplayFormat && !props.displayMetadataPassthrough ? 'is-selected' : ''} type="button" disabled={props.autoDisplayFormat || !props.windowsHdrEnabled} onClick={() => props.onDisplayMetadataPassthroughChange(false)}>
                <span>{t.disabled}</span>
              </button>
            </div>
          </label>
          <label className="library-settings-field">
            <span>{t.dolbyVisionSystemPipelineExperimental}</span>
            <small>{t.dolbyVisionSystemPipelineExperimentalCaption}</small>
            <div className="library-setting-options" role="group" aria-label={t.dolbyVisionSystemPipelineExperimental}>
              <button className={(props.autoDisplayFormat || props.dolbyVisionSystemPipelineExperimental) ? 'is-selected' : ''} type="button" disabled={props.autoDisplayFormat || !props.windowsHdrEnabled} onClick={() => props.onDolbyVisionSystemPipelineExperimentalChange(true)}>
                <span>{t.enabled}</span>
              </button>
              <button className={!props.autoDisplayFormat && !props.dolbyVisionSystemPipelineExperimental ? 'is-selected' : ''} type="button" disabled={props.autoDisplayFormat || !props.windowsHdrEnabled} onClick={() => props.onDolbyVisionSystemPipelineExperimentalChange(false)}>
                <span>{t.disabled}</span>
              </button>
            </div>
          </label>
        </div>
      </section>

      <section className="library-settings-panel">
        <div className="library-settings-panel-title">
          <Tv size={16} />
          <span>{t.refreshRateSync}</span>
        </div>
        <p>{t.refreshRateSyncCaption}</p>
        <p className="library-settings-warning">{t.refreshRateSyncRequirement}</p>
        <div className="library-refresh-sync-controls">
          <div className="library-setting-options" role="group" aria-label={t.refreshRateSync}>
            <button className={props.refreshRateSyncEnabled ? 'is-selected' : ''} type="button" onClick={() => props.onRefreshRateSyncChange(true)}>
              <span>{t.enabled}</span>
            </button>
            <button className={!props.refreshRateSyncEnabled ? 'is-selected' : ''} type="button" onClick={() => props.onRefreshRateSyncChange(false)}>
              <span>{t.disabled}</span>
            </button>
          </div>
          <label className={`library-checkbox-option ${props.refreshRateSyncEnabled ? '' : 'is-disabled'}`}>
            <input
              type="checkbox"
              checked={props.refreshRateMaximumMultiple}
              disabled={!props.refreshRateSyncEnabled}
              onChange={(event) => props.onRefreshRateMaximumMultipleChange(event.currentTarget.checked)}
            />
            <span>{t.maximumRefreshMultiple}</span>
          </label>
        </div>
      </section>

      <section className="library-settings-panel">
        <div className="library-settings-panel-title">
          <Film size={16} />
          <span>{t.trailerSource}</span>
          <small className="library-settings-experimental">{t.experimental}</small>
        </div>
        <p>{t.trailerSourceCaption}</p>
        <div className="library-setting-options" role="group" aria-label={t.trailerSource}>
          {([
            { value: 'youtube', label: t.youtube },
            { value: 'bilibili', label: t.bilibili }
          ] as Array<{ value: TrailerSource; label: string }>).map((option) => (
            <button
              key={option.value}
              className={props.trailerSettings.source === option.value ? 'is-selected' : ''}
              type="button"
              onClick={() => props.onTrailerSettingsChange({ ...props.trailerSettings, source: option.value })}
            >
              <span>{option.label}</span>
            </button>
          ))}
        </div>
        {props.trailerSettings.source === 'bilibili' ? (
          <>
            <label className="library-settings-field library-trailer-keyword-field">
              <span>{t.bilibiliKeyword}</span>
              <input
                value={props.trailerSettings.bilibiliKeywordTemplate}
                onChange={(event) => props.onTrailerSettingsChange({
                  ...props.trailerSettings,
                  bilibiliKeywordTemplate: event.target.value
                })}
                placeholder="{title} {year} 预告"
              />
              <small>{t.bilibiliKeywordHint}</small>
            </label>
          </>
        ) : null}
        <label className="library-checkbox-option library-trailer-preference">
          <input
            type="checkbox"
            checked={props.trailerSettings.soundEnabled}
            onChange={(event) => props.onTrailerSettingsChange({
              ...props.trailerSettings,
              soundEnabled: event.currentTarget.checked
            })}
          />
          <span>{t.trailerDefaultSound}</span>
        </label>
        <label className="library-checkbox-option library-trailer-preference">
          <input
            type="checkbox"
            checked={props.trailerSettings.autoPlayOnDetails}
            onChange={(event) => props.onTrailerSettingsChange({
              ...props.trailerSettings,
              autoPlayOnDetails: event.currentTarget.checked
            })}
          />
          <span>{t.autoPlayTrailer}</span>
        </label>
      </section>

      <section className="library-settings-panel">
        <div className="library-settings-panel-title">
          <Settings2 size={16} />
          <span>{t.appearance}</span>
        </div>
        <div className="library-settings-row">
          <span>{t.tone}</span>
          <strong>{t.playerTone}</strong>
        </div>
        <p>{t.playbackHint}</p>
      </section>
    </section>
  )
}

export default function LibraryApp(): JSX.Element {
  const savedConnections = useMemo(() => loadSavedEmbyConnections(), [])
  const savedConnection = savedConnections[0]
  const initialActiveNav = useMemo(() => loadSavedLibraryNav(savedConnections), [savedConnections])
  const savedAddress = useMemo(
    () => savedConnection ? parseServerAddress(savedConnection.serverUrl, 'http') : undefined,
    [savedConnection]
  )
  const client = useMemo(() => createEmptyLibraryClient(), [])
  const [language, setLanguage] = useState<UiLanguage>(() => getInitialLanguage())
  const [playlists, setPlaylists] = useState<MediaPlaylist[]>(() => loadMediaPlaylists())
  const [newPlaylistItem, setNewPlaylistItem] = useState<MediaItem | undefined>()
  const [newPlaylistName, setNewPlaylistName] = useState('')
  const [customTitleBarEnabled, setCustomTitleBarEnabled] = useState(false)
  currentLibraryLanguage = language
  const [displayMetadataPassthrough, setDisplayMetadataPassthrough] = useState(true)
  const [displayPeakBrightnessNits, setDisplayPeakBrightnessNits] = useState(0)
  const [autoDisplayFormat, setAutoDisplayFormat] = useState(false)
  const [dolbyVisionSystemPipelineExperimental, setDolbyVisionSystemPipelineExperimental] = useState(false)
  const [windowsHdrEnabled, setWindowsHdrEnabled] = useState(false)
  const [videoPassthroughSettingsLoaded, setVideoPassthroughSettingsLoaded] = useState(false)
  const [refreshRateSyncEnabled, setRefreshRateSyncEnabled] = useState(() => localStorage.getItem('anvil-player.refresh-rate-sync') === 'true')
  const [refreshRateMaximumMultiple, setRefreshRateMaximumMultiple] = useState(() => localStorage.getItem('anvil-player.refresh-rate-maximum-multiple') !== 'false')
  const { settings: tmdbSettings, status: tmdbStatus, isTesting: isTestingTmdb, updateSettings: updateTmdbSettings, testConnection: testTmdbSettingsConnection } = useTmdbSettings()
  const [trailerSettings, setTrailerSettings] = useState<TrailerSettings>(() => loadTrailerSettings())
  const changeInterfaceLanguage = (nextLanguage: UiLanguage): void => {
    setLanguage(nextLanguage)
    updateTmdbSettings({ ...tmdbSettings, language: tmdbLanguageForUi(nextLanguage) })
  }
  const [sources, setSources] = useState<LibrarySource[]>([])
  const [allItems, setAllItems] = useState<MediaItem[]>([])
  const [visibleItems, setVisibleItems] = useState<MediaItem[]>([])
  const [listingPage, setListingPage] = useState(1)
  const [detailItemsById, setDetailItemsById] = useState<Map<string, MediaItem>>(() => new Map())
  const [loadedDetailIds, setLoadedDetailIds] = useState<Set<string>>(() => new Set())
  const [continueItems, setContinueItems] = useState<MediaItem[]>([])
  const [embyRefreshPulse, setEmbyRefreshPulse] = useState(0)
  const [homeSections, setHomeSections] = useState<LibraryHomeSection[]>([])
  const [activeNav, setActiveNav] = useState<NavKey>(initialActiveNav)
  const [activeView, setActiveView] = useState<LibraryView>('home')
  const [sortKey, setSortKey] = useState<SortKey>('recent')
  const [sortOrder, setSortOrder] = useState<SortOrder>('descending')
  const [mediaFilter, setMediaFilter] = useState<MediaFilterKey>('all')
  const [query, setQuery] = useState('')
  const [debouncedQuery, setDebouncedQuery] = useState('')
  const [personSearch, setPersonSearch] = useState<{ sourceId: string; personId: string; name: string } | undefined>()
  const [activeLibraryViewId, setActiveLibraryViewId] = useState('')
  const [selectedId, setSelectedId] = useState('')
  const [isDetailOpen, setIsDetailOpen] = useState(false)
  const listingSectionRef = useRef<HTMLElement | null>(null)
  const [sourceSetupMode, setSourceSetupMode] = useState<SourceSetupMode>(() => savedConnection ? 'hidden' : 'select')
  const [connectionName, setConnectionName] = useState(() => savedConnection?.name ?? '')
  const [serverProtocol, setServerProtocol] = useState<ServerProtocol>(() => savedAddress?.protocol ?? 'http')
  const [serverHost, setServerHost] = useState(() => savedAddress?.host ?? '')
  const [serverPort, setServerPort] = useState(() => savedAddress?.port ?? '')
  const [serverPath, setServerPath] = useState(() => savedAddress?.path ?? '')
  const [username, setUsername] = useState(() => savedConnection?.username ?? '')
  const [password, setPassword] = useState('')
  const [fileServiceName, setFileServiceName] = useState('')
  const [smbHost, setSmbHost] = useState('')
  const [smbUsername, setSmbUsername] = useState('')
  const [smbPassword, setSmbPassword] = useState('')
  const [smbCurrentPath, setSmbCurrentPath] = useState('')
  const [smbDirectories, setSmbDirectories] = useState<FileServiceDirectory[]>([])
  const [smbSelectedPaths, setSmbSelectedPaths] = useState<Set<string>>(() => new Set())
  const [isBrowsingSmb, setIsBrowsingSmb] = useState(false)
  const [webDavUrl, setWebDavUrl] = useState('')
  const [webDavUsername, setWebDavUsername] = useState('')
  const [webDavPassword, setWebDavPassword] = useState('')
  const [webDavCurrentPath, setWebDavCurrentPath] = useState('')
  const [webDavDirectories, setWebDavDirectories] = useState<FileServiceDirectory[]>([])
  const [webDavSelectedPaths, setWebDavSelectedPaths] = useState<Set<string>>(() => new Set())
  const [isBrowsingWebDav, setIsBrowsingWebDav] = useState(false)
  const [ignoreCertificateErrors, setIgnoreCertificateErrors] = useState(() => savedConnection?.ignoreCertificateErrors ?? false)
  const [embySessionsBySourceId, setEmbySessionsBySourceId] = useState<Map<string, EmbySession>>(() => new Map(
    savedConnections
      .filter((connection) => connection.session)
      .map((connection) => [connection.sourceId, connection.session as EmbySession])
  ))
  const [playIntent, setPlayIntent] = useState('')
  const [resolvingPlayItemId, setResolvingPlayItemId] = useState('')
  const [scrapingMetadataItemId, setScrapingMetadataItemId] = useState('')
  const [scrapingSourceMetadataId, setScrapingSourceMetadataId] = useState('')
  const [scanningLocalFolderSourceIds, setScanningLocalFolderSourceIds] = useState<Set<string>>(() => new Set())
  const [metadataEditorItem, setMetadataEditorItem] = useState<MediaItem | undefined>()
  const [metadataDeleteItem, setMetadataDeleteItem] = useState<MediaItem | undefined>()
  const [metadataMatchItem, setMetadataMatchItem] = useState<MediaItem | undefined>()
  const [metadataCandidates, setMetadataCandidates] = useState<TmdbMatchCandidate[]>([])
  const [isSearchingMetadata, setIsSearchingMetadata] = useState(false)
  const [applyingMetadataCandidateId, setApplyingMetadataCandidateId] = useState('')
  const [backgroundTasks, setBackgroundTasks] = useState<BackgroundTask[]>([])
  const [taskCenterOpen, setTaskCenterOpen] = useState(false)
  const [mediaManagementOpen, setMediaManagementOpen] = useState(false)

  useEffect(() => {
    if (!taskCenterOpen && !mediaManagementOpen) return undefined
    const closeOnOutsidePointer = (event: PointerEvent): void => {
      const target = event.target
      if (target instanceof Element && target.closest('.library-toolbar-popover-anchor')) return
      setTaskCenterOpen(false)
      setMediaManagementOpen(false)
    }
    const closeOnEscape = (event: KeyboardEvent): void => {
      if (event.key !== 'Escape') return
      setTaskCenterOpen(false)
      setMediaManagementOpen(false)
    }
    window.addEventListener('pointerdown', closeOnOutsidePointer)
    window.addEventListener('keydown', closeOnEscape)
    return () => {
      window.removeEventListener('pointerdown', closeOnOutsidePointer)
      window.removeEventListener('keydown', closeOnEscape)
    }
  }, [mediaManagementOpen, taskCenterOpen])
  const [isSavingManualMetadata, setIsSavingManualMetadata] = useState(false)
  const [isConnecting, setIsConnecting] = useState(false)
  const [isPickingLocalFolder, setIsPickingLocalFolder] = useState(false)
  const [connectTone, setConnectTone] = useState<'idle' | 'success' | 'error'>('idle')
  const [connectMessage, setConnectMessage] = useState('')
  const [editingSourceId, setEditingSourceId] = useState('')
  const [scanProgressBySourceId, setScanProgressBySourceId] = useState<Map<string, SourceScanProgress>>(() => new Map())
  const ignoredLocalFolderScanSourceIdsRef = useRef<Set<string>>(new Set())
  const fileSystemSourceByLocationRef = useRef<Map<string, LibrarySource>>(new Map())
  const homeSectionRefreshSourceIdsRef = useRef<Set<string>>(new Set())
  const activeNavRef = useRef<NavKey>(initialActiveNav)
  const smbBrowseRequestIdRef = useRef('')
  const webDavBrowseRequestIdRef = useRef('')
  const localScanUpdateChainRef = useRef<Promise<void>>(Promise.resolve())
  const pendingFolderScanCountsRef = useRef<Map<string, number>>(new Map())
  const allItemsRef = useRef<MediaItem[]>([])
  const mediaProbeRequestedIdsRef = useRef<Set<string>>(new Set())
  const mediaProbeTimeoutsRef = useRef<Map<string, number>>(new Map())
  const castRefreshRequestedIdsRef = useRef<Set<string>>(new Set())
  const taskStatusRef = useRef<Map<string, BackgroundTask['status']>>(new Map())
  const taskRetryRef = useRef<Map<string, () => void>>(new Map())
  const taskNativeCancelRef = useRef<Map<string, () => void>>(new Map())
  const dissolvedCollectionPathsRef = useRef<Set<string>>(loadDissolvedCollectionPaths())

  useEffect(() => {
    allItemsRef.current = allItems
  }, [allItems])

  function setTask(task: BackgroundTask, retry?: () => void): void {
    taskStatusRef.current.set(task.id, task.status)
    if (retry) taskRetryRef.current.set(task.id, retry)
    setBackgroundTasks((current) => [task, ...current.filter((row) => row.id !== task.id)].slice(0, 100))
  }

  function updateTask(id: string, patch: Partial<BackgroundTask>): void {
    if (patch.status) taskStatusRef.current.set(id, patch.status)
    setBackgroundTasks((current) => current.map((task) => task.id === id
      ? { ...task, ...patch, updatedAt: Date.now() }
      : task))
  }

  function advanceTask(id: string, patch: Partial<BackgroundTask>): void {
    if (patch.status) taskStatusRef.current.set(id, patch.status)
    setBackgroundTasks((current) => current.map((task) => task.id === id
      ? { ...task, ...patch, completed: Math.min(task.total, task.completed + 1), updatedAt: Date.now() }
      : task))
  }

  function startTask(id: string, kind: BackgroundTask['kind'], title: string, detail: string, total: number, retry?: () => void): void {
    const now = Date.now()
    setTask({ id, kind, title, detail, status: 'running', completed: 0, total, startedAt: now, updatedAt: now }, retry)
  }

  async function waitWhileTaskPaused(id: string): Promise<boolean> {
    while (taskStatusRef.current.get(id) === 'paused') {
      await new Promise((resolve) => window.setTimeout(resolve, 120))
    }
    return taskStatusRef.current.get(id) === 'cancelled'
  }

  function pauseTask(task: BackgroundTask): void {
    updateTask(task.id, { status: 'paused' })
    taskNativeCancelRef.current.get(task.id)?.()
    if (task.kind === 'probe' && task.id.startsWith('probe:')) {
      const requestId = task.id.slice('probe:'.length)
      window.clearTimeout(mediaProbeTimeoutsRef.current.get(requestId))
      mediaProbeTimeoutsRef.current.delete(requestId)
    }
  }

  function resumeTask(task: BackgroundTask): void {
    if (task.kind === 'scan' || task.kind === 'probe') {
      taskRetryRef.current.get(task.id)?.()
      return
    }
    updateTask(task.id, { status: 'running' })
  }

  function retryTask(task: BackgroundTask): void {
    taskRetryRef.current.get(task.id)?.()
  }

  function cancelTask(task: BackgroundTask): void {
    taskNativeCancelRef.current.get(task.id)?.()
    updateTask(task.id, { status: 'cancelled' })
    if (task.kind === 'probe' && task.id.startsWith('probe:')) {
      const requestId = task.id.slice('probe:'.length)
      window.clearTimeout(mediaProbeTimeoutsRef.current.get(requestId))
      mediaProbeTimeoutsRef.current.delete(requestId)
      mediaProbeRequestedIdsRef.current.delete(requestId)
    }
    if (task.kind === 'scan' && task.id.startsWith('scan:')) {
      const sourceId = task.id.slice('scan:'.length)
      pendingFolderScanCountsRef.current.delete(sourceId)
      setScanningLocalFolderSourceIds((current) => {
        const next = new Set(current)
        next.delete(sourceId)
        return next
      })
      setScanProgressBySourceId((current) => {
        const existing = current.get(sourceId)
        if (!existing) return current
        const next = new Map(current)
        next.set(sourceId, { ...existing, active: false })
        return next
      })
    }
  }

  function beginSourceScan(source: LibrarySource, totalFolders: number, foundItems = 0, lastPath = source.location): void {
    startTask(`scan:${source.id}`, 'scan', `${language === 'zh' ? '扫描' : 'Scan'} · ${source.name}`, lastPath, Math.max(1, totalFolders), () => rescanFileSystemSource(source))
    taskNativeCancelRef.current.set(`scan:${source.id}`, () => {
      scanLocationsForSource(source).forEach((path) => postNativeCommand({
        type: 'command',
        command: 'cancelLibraryScan',
        path,
        webDav: source.kind === 'WebDAV'
      }))
    })
    setScanProgressBySourceId((current) => {
      const next = new Map(current)
      next.set(source.id, {
        sourceId: source.id,
        sourceName: source.name,
        kind: sourceScanKind(source),
        totalFolders: Math.max(1, totalFolders),
        completedFolders: 0,
        foundItems,
        failedFolders: 0,
        active: true,
        truncated: false,
        lastPath
      })
      return next
    })
  }

  function completeSourceScanFolder(source: LibrarySource, foundItems: number, stillActive: boolean, path: string, truncated?: boolean): void {
    const taskId = `scan:${source.id}`
    advanceTask(taskId, {
      detail: `${path} · ${foundItems}`,
      status: stillActive ? 'running' : 'completed'
    })
    setScanProgressBySourceId((current) => {
      const existing = current.get(source.id)
      const totalFolders = Math.max(1, existing?.totalFolders ?? pendingFolderScanCountsRef.current.get(source.id) ?? 1)
      const next = new Map(current)
      next.set(source.id, {
        sourceId: source.id,
        sourceName: source.name,
        kind: sourceScanKind(source),
        totalFolders,
        completedFolders: Math.min(totalFolders, (existing?.completedFolders ?? 0) + 1),
        foundItems,
        failedFolders: existing?.failedFolders ?? 0,
        active: stillActive,
        truncated: Boolean(existing?.truncated || truncated),
        lastPath: path || existing?.lastPath || source.location
      })
      return next
    })
  }

  function failSourceScanFolder(source: LibrarySource, stillActive: boolean, path: string): void {
    updateTask(`scan:${source.id}`, {
      detail: path,
      status: stillActive ? 'running' : 'failed',
      error: language === 'zh' ? '扫描失败，可重试' : 'Scan failed. Retry available.'
    })
    setScanProgressBySourceId((current) => {
      const existing = current.get(source.id)
      const totalFolders = Math.max(1, existing?.totalFolders ?? pendingFolderScanCountsRef.current.get(source.id) ?? 1)
      const next = new Map(current)
      next.set(source.id, {
        sourceId: source.id,
        sourceName: source.name,
        kind: sourceScanKind(source),
        totalFolders,
        completedFolders: Math.min(totalFolders, (existing?.completedFolders ?? 0) + 1),
        foundItems: existing?.foundItems ?? source.itemCount,
        failedFolders: (existing?.failedFolders ?? 0) + 1,
        active: stillActive,
        truncated: existing?.truncated ?? false,
        lastPath: path || existing?.lastPath || source.location
      })
      return next
    })
  }

  async function mergeCachedDuplicateMetadata(
    sourceRows: LibrarySource[],
    allRows: MediaItem[]
  ): Promise<{ sourceRows: LibrarySource[]; allRows: MediaItem[]; mergedCount: number }> {
    let mergedCount = 0
    for (const source of sourceRows) {
      if (source.kind === 'Emby') continue
      const sourceItems = allRows.filter((item) => item.sourceId === source.id)
      const mergeResult = mergeDuplicateMetadataItems(sourceItems)
      if (!mergeResult.mergedCount) continue
      mergedCount += mergeResult.mergedCount
      await client.upsertSourceItems({ ...source, itemCount: mergeResult.items.length }, mergeResult.items)
      debugLibraryPlayback(`tmdb cached duplicates merged source=${source.id} merged=${mergeResult.mergedCount}`)
    }
    if (!mergedCount) return { sourceRows, allRows, mergedCount }
    const [nextSourceRows, nextAllRows] = await Promise.all([
      client.listSources(),
      client.listAllItems()
    ])
    return { sourceRows: nextSourceRows, allRows: nextAllRows, mergedCount }
  }

  useEffect(() => {
    applyAppearanceSettings()
  }, [])

  useEffect(() => {
    applyDocumentLanguage(language)
    saveUiLanguage(language)
    postNativeCommand({ type: 'command', command: 'setUiLanguage', language })
  }, [language])

  useEffect(() => {
    localStorage.setItem('anvil-player.refresh-rate-sync', String(refreshRateSyncEnabled))
    postNativeCommand({ type: 'command', command: 'setGlobalRefreshRateSync', enabled: refreshRateSyncEnabled })
  }, [refreshRateSyncEnabled])

  useEffect(() => {
    localStorage.setItem('anvil-player.refresh-rate-maximum-multiple', String(refreshRateMaximumMultiple))
    postNativeCommand({ type: 'command', command: 'setGlobalRefreshRateMaximumMultiple', enabled: refreshRateMaximumMultiple })
  }, [refreshRateMaximumMultiple])

  useEffect(() => {
    if (!videoPassthroughSettingsLoaded) return
    postNativeCommand({ type: 'command', command: 'setGlobalAutoDisplayFormat', enabled: autoDisplayFormat })
  }, [autoDisplayFormat, videoPassthroughSettingsLoaded])

  useEffect(() => {
    if (!videoPassthroughSettingsLoaded || autoDisplayFormat) return
    postNativeCommand({ type: 'command', command: 'setGlobalDisplayMetadataPassthrough', enabled: displayMetadataPassthrough })
  }, [displayMetadataPassthrough, videoPassthroughSettingsLoaded, autoDisplayFormat])

  useEffect(() => {
    if (!videoPassthroughSettingsLoaded) return
    postNativeCommand({ type: 'command', command: 'setGlobalDisplayPeakBrightness', peakNits: displayPeakBrightnessNits })
  }, [displayPeakBrightnessNits, videoPassthroughSettingsLoaded])

  useEffect(() => {
    if (!videoPassthroughSettingsLoaded || autoDisplayFormat) return
    postNativeCommand({ type: 'command', command: 'setGlobalDolbyVisionSystemPipelineExperimental', enabled: dolbyVisionSystemPipelineExperimental })
  }, [dolbyVisionSystemPipelineExperimental, videoPassthroughSettingsLoaded, autoDisplayFormat])

  useEffect(() => {
    saveTrailerSettings(trailerSettings)
  }, [trailerSettings])

  useEffect(() => {
    activeNavRef.current = activeNav
    if (sourceSetupMode === 'hidden') {
      saveLibraryNav(activeNav)
    }
  }, [activeNav, sourceSetupMode])

  useEffect(() => {
    postNativeCommand({
      type: 'command',
      command: 'setAllowInsecureCertificates',
      enabled: ignoreCertificateErrors
    })
  }, [ignoreCertificateErrors])

  useEffect(() => {
    let cancelled = false

    async function loadInitialLibrary(): Promise<void> {
      const [rawSourceRows, rawAllRows] = await Promise.all([
        client.listSources(),
        client.listAllItems()
      ])
      const {
        sourceRows,
        allRows,
        mergedCount
      } = await mergeCachedDuplicateMetadata(rawSourceRows, rawAllRows)
      const [visibleRows, continueRows, homeRows] = await Promise.all([
        client.listItems({ navKey: initialActiveNav, view: 'home', search: '', sortKey: 'recent' }),
        client.getContinueWatching(),
        client.listHomeSections()
      ])
      if (cancelled) return
      if (mergedCount > 0) {
        setConnectTone('success')
        setConnectMessage(`已合并 ${mergedCount} 个重复媒体。`)
      }
      setSources(sourceRows)
      setAllItems(allRows)
      setContinueItems(continueRows)
      setHomeSections(homeRows)
      setVisibleItems(visibleRows)
      if (sourceRows.length && !savedConnection) {
        setSourceSetupMode('hidden')
      }
    }

    void loadInitialLibrary()
    return () => {
      cancelled = true
    }
  }, [client, initialActiveNav, savedConnection])

  useEffect(() => {
    async function refreshLibraryRows(options: {
      navKey?: NavKey
      view?: LibraryView
      search?: string
      filterKey?: MediaFilterKey
      sortKey?: SortKey
      sortOrder?: SortOrder
      libraryViewId?: string
    } = {}): Promise<MediaItem[]> {
      const navKey = options.navKey ?? activeNav
      const sourceRows = await client.listSources()
      const activeSourceId = navKey.startsWith('source:') ? navKey.slice('source:'.length) : ''
      const activeSourceKind = activeSourceId ? sourceRows.find((source) => source.id === activeSourceId)?.kind : undefined
      const [allRows, continueRows, visibleRows, homeRows] = await Promise.all([
        client.listAllItems(),
        client.getContinueWatching(),
        client.listItems({
          navKey,
          view: options.view ?? activeView,
          search: options.search ?? debouncedQuery,
          sortKey: options.sortKey ?? sortKey,
          sortOrder: options.sortOrder ?? sortOrder,
          filterKey: options.filterKey ?? mediaFilter,
          libraryViewId: activeSourceKind === 'Emby' ? options.libraryViewId ?? activeLibraryViewId : undefined
        }),
        client.listHomeSections()
      ])

      setSources(sourceRows)
      setAllItems(allRows)
      setContinueItems(continueRows)
      setVisibleItems(visibleRows)
      setHomeSections(homeRows)
      return visibleRows
    }

    async function applyLocalFolderScan(result: LocalFolderScanCompleted | LocalFolderPickResult, activate: boolean): Promise<void> {
      const knownSource = sources.find((source) => locationKey(source.location) === locationKey(result.folder.path))
        ?? fileSystemSourceByLocationRef.current.get(locationKey(result.folder.path))
      const rawSnapshot = buildLocalFolderLibrary(result, knownSource ? { source: knownSource } : undefined)
      const dissolvedPaths = dissolvedCollectionPathsRef.current
      const expandedItems = rawSnapshot.items.flatMap((item): MediaItem[] => {
        const episodeItems = (item.seasons ?? []).flatMap((season) => season.episodes.map((episode) => episode.item))
          .filter((episode): episode is MediaItem => Boolean(episode?.path || episode?.playbackPath))
        if (episodeItems.some((episode) => dissolvedPaths.has(locationKey(episode.path || episode.playbackPath || '')))) {
          return episodeItems.map((episode) => ({
            ...episode,
            type: 'movie',
            seasons: undefined,
            episodes: undefined,
            collectionDissolved: true
          }))
        }
        if (dissolvedPaths.has(locationKey(item.path || item.playbackPath || ''))) {
          return [{ ...item, type: 'movie', collectionDissolved: true }]
        }
        return [item]
      })
      const snapshot = {
        ...rawSnapshot,
        source: { ...rawSnapshot.source, itemCount: expandedItems.length },
        items: expandedItems
      }
      if (!activate && ignoredLocalFolderScanSourceIdsRef.current.has(snapshot.source.id)) {
        return
      }
      const existingRows = await client.listAllItems()
      const existingSourceItems = existingRows.filter((item) => item.sourceId === snapshot.source.id)
      const existingById = new Map(existingSourceItems.map((item) => [item.id, item]))
      const scannedItems = snapshot.items.map((item) => mergeLocalScannedItem(existingById.get(item.id), item))
      const scannedPathKeys = new Set(result.items.map((item) => locationKey(item.path)))
      const itemPathKeys = (item: MediaItem): string[] => [
        item.path,
        item.playbackPath,
        ...(item.versions ?? []).flatMap((version) => [version.path, version.playbackPath]),
        ...(item.seasons ?? []).flatMap((season) => season.episodes.flatMap((episode) => [episode.item?.path, episode.item?.playbackPath]))
      ].filter((path): path is string => Boolean(path)).map(locationKey)
      const missingItems = existingSourceItems
        .filter((item) => mediaItemBelongsToScanRoot(item, result.folder.path))
        .filter((item) => {
          const paths = itemPathKeys(item)
          return paths.length > 0 && paths.every((path) => !scannedPathKeys.has(path))
        })
        .map((item): MediaItem => ({
          ...item,
          availability: 'missing',
          missingSince: item.missingSince ?? Date.now()
        }))
      const mergesPartialWebDavFolder = Boolean(
        knownSource?.kind === 'WebDAV' &&
        ((knownSource.folders?.length ?? 0) > 1 || (loadWebDavCredentials(knownSource.id)?.selectedPaths?.length ?? 0) > 1)
      )
      const mergedItems = mergesPartialWebDavFolder
        ? [
            ...existingSourceItems.filter((item) => !mediaItemBelongsToScanRoot(item, result.folder.path)),
            ...scannedItems,
            ...missingItems
          ]
        : [...scannedItems, ...missingItems]

      const addedCount = snapshot.items.filter((item) => !existingById.has(item.id)).length
      const changedCount = snapshot.items.filter((item) => {
        const existing = existingById.get(item.id)
        return Boolean(existing?.fileFingerprint && item.fileFingerprint && existing.fileFingerprint !== item.fileFingerprint)
      }).length
      const unchangedCount = Math.max(0, scannedItems.length - addedCount - changedCount)

      await client.upsertSourceItems({ ...snapshot.source, itemCount: mergedItems.length }, mergedItems)
      const pendingScanCount = pendingFolderScanCountsRef.current.get(snapshot.source.id) ?? 0
      const stillScanningSource = pendingScanCount > 1
      if (stillScanningSource) {
        pendingFolderScanCountsRef.current.set(snapshot.source.id, pendingScanCount - 1)
      } else {
        pendingFolderScanCountsRef.current.delete(snapshot.source.id)
      }
      completeSourceScanFolder(snapshot.source, mergedItems.length, stillScanningSource, result.folder.path, result.truncated)
      setScanningLocalFolderSourceIds((current) => {
        const next = new Set(current)
        if (!stillScanningSource) {
          next.delete(snapshot.source.id)
        }
        return next
      })

      const sourceNav = `source:${snapshot.source.id}` as NavKey
      const visibleRows = await refreshLibraryRows(activate
        ? { navKey: sourceNav, view: 'home', search: '', sortKey: 'recent', sortOrder: 'descending', filterKey: 'all', libraryViewId: '' }
        : {})

      if (activate) {
        setActiveNav(sourceNav)
        setActiveView('home')
        setActiveLibraryViewId('')
        setMediaFilter('all')
        setSortKey('recent')
        setSortOrder('descending')
        setPersonSearch(undefined)
        setQuery('')
        setDebouncedQuery('')
        setSelectedId(visibleRows[0]?.id ?? '')
        setIsDetailOpen(false)
        setSourceSetupMode('hidden')
        setEditingSourceId('')
        setPlayIntent('')
      } else if (activeNav === sourceNav && selectedId && !visibleRows.some((item) => item.id === selectedId)) {
        setSelectedId(visibleRows[0]?.id ?? '')
      }

      setConnectTone('success')
      setConnectMessage(result.truncated
        ? `已扫描 ${snapshot.source.name}，达到上限，加入 ${mergedItems.length} 个媒体文件`
        : `已扫描 ${snapshot.source.name}，加入 ${mergedItems.length} 个媒体文件`)
      if (!result.truncated) {
        setConnectMessage(`增量扫描完成：新增 ${addedCount}，变化 ${changedCount}，未变化 ${unchangedCount}，缺失 ${missingItems.length}`)
      }
    }

    async function importLocalFolder(result: LocalFolderPickResult): Promise<void> {
      setIsPickingLocalFolder(false)
      setConnectTone('idle')

      try {
        if (!result.scanPending) {
          const source = buildLocalFolderSource(result.folder)
          fileSystemSourceByLocationRef.current.set(locationKey(source.location), source)
          ignoredLocalFolderScanSourceIdsRef.current.delete(source.id)
          await applyLocalFolderScan(result, true)
          return
        }

        const source = buildLocalFolderSource(result.folder)
        fileSystemSourceByLocationRef.current.set(locationKey(source.location), source)
        ignoredLocalFolderScanSourceIdsRef.current.delete(source.id)
        setScanningLocalFolderSourceIds((current) => new Set(current).add(source.id))
        const sourceItems = (await client.listAllItems()).filter((item) => item.sourceId === source.id)
        beginSourceScan(source, 1, sourceItems.length)
        await client.upsertSourceItems({ ...source, itemCount: sourceItems.length }, sourceItems)

        const sourceNav = `source:${source.id}` as NavKey
        const visibleRows = await refreshLibraryRows({
          navKey: sourceNav,
          view: 'home',
          search: '',
          sortKey: 'recent',
          sortOrder: 'descending',
          filterKey: 'all',
          libraryViewId: ''
        })

        setActiveNav(sourceNav)
        setActiveView('home')
        setActiveLibraryViewId('')
        setMediaFilter('all')
        setSortKey('recent')
        setSortOrder('descending')
        setPersonSearch(undefined)
        setQuery('')
        setDebouncedQuery('')
        setSelectedId(visibleRows[0]?.id ?? '')
        setIsDetailOpen(false)
        setSourceSetupMode('hidden')
        setEditingSourceId('')
        setPlayIntent('')
        setConnectMessage(`已添加 ${source.name}，正在后台扫描媒体文件...`)
      } catch (error) {
        if (result.scanPending) {
          const source = buildLocalFolderSource(result.folder)
          setScanningLocalFolderSourceIds((current) => {
            const next = new Set(current)
            next.delete(source.id)
            return next
          })
        }
        setConnectTone('error')
        setConnectMessage(error instanceof Error ? error.message : '导入本地文件夹失败')
      }
    }

    async function completeLocalFolderScan(result: LocalFolderScanCompleted): Promise<void> {
      try {
        await applyLocalFolderScan(result, false)
      } catch (error) {
        setConnectTone('error')
        setConnectMessage(error instanceof Error ? error.message : '更新本地文件夹扫描结果失败')
      }
    }

    function failLocalFolderScan(result: LocalFolderScanFailed): void {
      const source = sources.find((candidate) => locationKey(candidate.location) === locationKey(result.folder.path))
        ?? fileSystemSourceByLocationRef.current.get(locationKey(result.folder.path))
        ?? buildLocalFolderSource(result.folder)
      if (ignoredLocalFolderScanSourceIdsRef.current.has(source.id)) {
        return
      }
      const pendingScanCount = pendingFolderScanCountsRef.current.get(source.id) ?? 0
      const stillScanningSource = pendingScanCount > 1
      if (stillScanningSource) {
        pendingFolderScanCountsRef.current.set(source.id, pendingScanCount - 1)
      } else {
        pendingFolderScanCountsRef.current.delete(source.id)
      }
      failSourceScanFolder(source, stillScanningSource, result.folder.path)
      setScanningLocalFolderSourceIds((current) => {
        const next = new Set(current)
        if (!stillScanningSource) {
          next.delete(source.id)
        }
        return next
      })
      setConnectTone('error')
      setConnectMessage(result.message || `扫描 ${source.name} 失败`)
    }

    const unsubscribe = subscribeNativeMessages((message) => {
      if (message.type === 'command' && message.command === 'localPlaybackProgress') {
        const ratio = message.durationMs > 0 ? Math.max(0, Math.min(1, message.positionMs / message.durationMs)) : 0
        const ended = message.playbackState === 'Ended'
        const changed: MediaItem[] = []
        const nextItems = allItemsRef.current.map((candidate) => {
          if (sources.find((source) => source.id === candidate.sourceId)?.kind === 'Emby') return candidate
          const updated = applyPlaybackProgress(candidate, message.path, ratio, ended)
          if (updated.changed) changed.push(updated.item)
          return updated.item
        })
        if (!changed.length) return
        allItemsRef.current = nextItems
        setAllItems(nextItems)
        setVisibleItems((current) => current.map((candidate) => changed.find((item) => item.id === candidate.id) ?? candidate))
        setContinueItems(nextItems.filter((item) => item.continueWatching || (item.progress > 0 && item.progress < 1)))
        setDetailItemsById((current) => {
          const next = new Map(current)
          changed.forEach((item) => next.set(item.id, item))
          return next
        })
        changed.forEach((item) => { void client.updateItem(item) })
      } else if (message.type === 'windowChrome') {
        setCustomTitleBarEnabled(message.customTitleBar)
      } else if (message.type === 'globalVideoPassthroughSettings') {
        setAutoDisplayFormat(message.autoDisplayFormat)
        setDisplayMetadataPassthrough(message.displayMetadataPassthrough)
        setDisplayPeakBrightnessNits(message.displayPeakBrightnessNits)
        setDolbyVisionSystemPipelineExperimental(message.dolbyVisionSystemPipelineExperimental)
        setWindowsHdrEnabled(message.windowsHdrEnabled)
        setVideoPassthroughSettingsLoaded(true)
      } else if (message.type === 'localFolderPicked') {
        void importLocalFolder(message)
      } else if (message.type === 'localFolderScanCompleted') {
        localScanUpdateChainRef.current = localScanUpdateChainRef.current.then(() => completeLocalFolderScan(message))
      } else if (message.type === 'localFolderScanFailed') {
        failLocalFolderScan(message)
      } else if (message.type === 'localFolderPickCancelled') {
        setIsPickingLocalFolder(false)
        setConnectTone('idle')
        setConnectMessage('已取消选择本地文件夹')
      } else if (message.type === 'localFolderPickFailed') {
        setIsPickingLocalFolder(false)
        setConnectTone('error')
        setConnectMessage(message.message || '读取本地文件夹失败')
      } else if (message.type === 'smbDirectoryListed') {
        if (message.requestId !== smbBrowseRequestIdRef.current) return
        setIsBrowsingSmb(false)
        setSmbCurrentPath(message.path)
        setSmbDirectories(message.directories)
        setConnectTone('idle')
        setConnectMessage(message.directories.length ? '' : '当前 SMB 文件夹下没有子文件夹。')
      } else if (message.type === 'smbDirectoryFailed') {
        if (message.requestId !== smbBrowseRequestIdRef.current) return
        setIsBrowsingSmb(false)
        setSmbCurrentPath(message.path)
        setSmbDirectories([])
        setConnectTone('error')
        setConnectMessage(message.message || '读取 SMB 文件夹失败')
      } else if (message.type === 'webDavDirectoryListed') {
        if (message.requestId !== webDavBrowseRequestIdRef.current) return
        setIsBrowsingWebDav(false)
        setWebDavCurrentPath(message.path)
        setWebDavDirectories(message.directories)
        setConnectTone('idle')
        setConnectMessage(message.directories.length ? '' : '当前 WebDAV 文件夹下没有子文件夹。')
      } else if (message.type === 'webDavDirectoryFailed') {
        if (message.requestId !== webDavBrowseRequestIdRef.current) return
        setIsBrowsingWebDav(false)
        setWebDavCurrentPath(message.path)
        setWebDavDirectories([])
        setConnectTone('error')
        setConnectMessage(message.message || '读取 WebDAV 文件夹失败')
      } else if (message.type === 'mediaDetailsProbed') {
        mediaProbeRequestedIdsRef.current.delete(message.requestId)
        window.clearTimeout(mediaProbeTimeoutsRef.current.get(message.requestId))
        mediaProbeTimeoutsRef.current.delete(message.requestId)
        if (['cancelled', 'paused'].includes(taskStatusRef.current.get(`probe:${message.requestId}`) ?? '')) return
        updateTask(`probe:${message.requestId}`, { status: 'completed', completed: 1 })
        const item = allItemsRef.current.find((candidate) => candidate.id === message.requestId)
          ?? detailItemsById.get(message.requestId)
        if (!item) return
        const updatedItem: MediaItem = {
          ...item,
          videoSpec: message.videoSpec || item.videoSpec,
          audioSpec: message.audioSpec || item.audioSpec,
          streamSpecs: message.streamSpecs
        }
        const replace = (candidate: MediaItem): MediaItem => candidate.id === updatedItem.id ? updatedItem : candidate
        allItemsRef.current = allItemsRef.current.map(replace)
        setAllItems(allItemsRef.current)
        setVisibleItems((current) => current.map(replace))
        setContinueItems((current) => current.map(replace))
        setDetailItemsById((current) => new Map(current).set(updatedItem.id, updatedItem))
        void client.updateItem(updatedItem)
      } else if (message.type === 'mediaDetailsProbeFailed') {
        mediaProbeRequestedIdsRef.current.delete(message.requestId)
        window.clearTimeout(mediaProbeTimeoutsRef.current.get(message.requestId))
        mediaProbeTimeoutsRef.current.delete(message.requestId)
        if (['cancelled', 'paused'].includes(taskStatusRef.current.get(`probe:${message.requestId}`) ?? '')) return
        updateTask(`probe:${message.requestId}`, { status: 'failed', error: message.message })
        if (message.requestId === selectedId) {
          setPlayIntent(`媒体信息读取失败：${message.message}`)
        }
      }
    })
    postNativeCommand({ type: 'command', command: 'requestWindowChrome' })
    postNativeCommand({ type: 'command', command: 'requestGlobalVideoPassthroughSettings' })
    return unsubscribe
  }, [activeLibraryViewId, activeNav, activeView, client, debouncedQuery, mediaFilter, selectedId, sortKey, sortOrder, sources])

  useEffect(() => {
    const requestRefresh = (): void => {
      if (document.visibilityState === 'visible') {
        setEmbyRefreshPulse((value) => value + 1)
      }
    }
    window.addEventListener('focus', requestRefresh)
    document.addEventListener('visibilitychange', requestRefresh)
    return () => {
      window.removeEventListener('focus', requestRefresh)
      document.removeEventListener('visibilitychange', requestRefresh)
    }
  }, [])

  useEffect(() => {
    const connections = savedConnections.filter((connection) => connection.session)
    if (!connections.length) return undefined
    let cancelled = false

    async function refreshSavedConnections(): Promise<void> {
      for (const connection of connections) {
        const savedSession = connection.session as EmbySession
        try {
          postNativeCommand({
            type: 'command',
            command: 'setAllowInsecureCertificates',
            enabled: connection.ignoreCertificateErrors
          })
          const snapshot = await refreshEmbyLibrary(savedSession, connection.name)
          await client.upsertSourceItems(snapshot.source, snapshot.items, snapshot.homeSections)

          saveSavedEmbyConnection({
            sourceId: snapshot.source.id,
            name: snapshot.source.name,
            serverUrl: snapshot.session.apiBaseUrl,
            username: snapshot.session.userName || connection.username,
            ignoreCertificateErrors: connection.ignoreCertificateErrors,
            session: snapshot.session,
            savedAt: Date.now()
          })

          const [sourceRows, allRows, continueRows, homeRows] = await Promise.all([
            client.listSources(),
            client.listAllItems(),
            client.getContinueWatching(),
            client.listHomeSections()
          ])

          if (cancelled) return
          setSources(sourceRows)
          setAllItems(allRows)
          setContinueItems(continueRows)
          setHomeSections(homeRows)
          setEmbySessionsBySourceId((current) => {
            const next = new Map(current)
            next.set(snapshot.source.id, snapshot.session)
            return next
          })

          if (connection.sourceId && connection.sourceId !== snapshot.source.id && activeNavRef.current === `source:${connection.sourceId}`) {
            setActiveNav(`source:${snapshot.source.id}` as NavKey)
          }
        } catch (error) {
          if (cancelled) return
          debugLibraryPlayback(`emby saved refresh failed source=${connection.sourceId || connection.name} error=${errorText(error)}`)
        }
      }
    }

    void refreshSavedConnections()
    return () => {
      cancelled = true
    }
  }, [client, savedConnections, embyRefreshPulse])

  useEffect(() => {
    const timer = window.setTimeout(() => setDebouncedQuery(query), 220)
    return () => window.clearTimeout(timer)
  }, [query])

  useEffect(() => {
    let cancelled = false
    const activeSourceId = activeNav.startsWith('source:') ? activeNav.slice('source:'.length) : ''
    const activeSourceKind = activeSourceId ? sources.find((source) => source.id === activeSourceId)?.kind : undefined
    const activeSession = activeSourceId ? embySessionsBySourceId.get(activeSourceId) : undefined

    async function loadVisibleItems(): Promise<void> {
      if (activeSourceKind === 'Emby' && activeSession && activeLibraryViewId && !debouncedQuery.trim()) {
        try {
          const rows = await listEmbyLibraryView(activeSession, {
            libraryViewId: activeLibraryViewId,
            view: activeView,
            filterKey: mediaFilter,
            sortKey,
            sortOrder
          })
          if (!cancelled) setVisibleItems(rows)
          return
        } catch (error) {
          debugLibraryPlayback(`emby view load failed source=${activeSourceId} view=${activeLibraryViewId} error=${errorText(error)}`)
        }
      }

      if (activeSourceKind === 'Emby' && activeSession && debouncedQuery.trim()) {
        try {
          const rows = personSearch && personSearch.sourceId === activeSourceId && personSearch.name === debouncedQuery.trim()
            ? await searchEmbyPersonLibrary(activeSession, {
                id: personSearch.personId,
                name: personSearch.name
              }, {
                libraryViewId: activeLibraryViewId || undefined,
                view: activeView,
                filterKey: mediaFilter,
                sortKey,
                sortOrder
              })
            : await searchEmbyLibrary(activeSession, debouncedQuery, {
                libraryViewId: activeLibraryViewId || undefined,
                view: activeView,
                filterKey: mediaFilter,
                sortKey,
                sortOrder
              })
          if (!cancelled) setVisibleItems(rows)
          return
        } catch (error) {
          debugLibraryPlayback(`emby search failed source=${activeSourceId} error=${errorText(error)}`)
        }
      }

      const rows = await client.listItems({
        navKey: activeNav,
        view: activeView,
        search: debouncedQuery,
        sortKey,
        sortOrder,
        filterKey: mediaFilter,
        libraryViewId: activeSourceKind === 'Emby' ? activeLibraryViewId : undefined
      })
      if (!cancelled) setVisibleItems(rows)
    }

    void loadVisibleItems()
    return () => {
      cancelled = true
    }
  }, [activeLibraryViewId, activeNav, activeView, client, debouncedQuery, embySessionsBySourceId, mediaFilter, personSearch, sortKey, sortOrder, sources])

  useEffect(() => {
    if (!visibleItems.length) {
      if (selectedId) setSelectedId('')
      return
    }
    if (!isDetailOpen) {
      if (selectedId && !visibleItems.some((item) => item.id === selectedId)) setSelectedId('')
      return
    }
    if (!visibleItems.some((item) => item.id === selectedId)) {
      setSelectedId(visibleItems[0].id)
    }
  }, [activeNav, isDetailOpen, selectedId, sources, visibleItems])

  useEffect(() => {
    setListingPage(1)
  }, [activeLibraryViewId, activeNav, activeView, debouncedQuery, mediaFilter, sortKey, sortOrder])

  const listingPageCount = Math.max(1, Math.ceil(visibleItems.length / LIBRARY_LISTING_PAGE_SIZE))
  const effectiveListingPage = Math.min(listingPage, listingPageCount)
  const pagedVisibleItems = useMemo(() => {
    const start = (effectiveListingPage - 1) * LIBRARY_LISTING_PAGE_SIZE
    return visibleItems.slice(start, start + LIBRARY_LISTING_PAGE_SIZE)
  }, [effectiveListingPage, visibleItems])
  const listingRangeStart = visibleItems.length
    ? (effectiveListingPage - 1) * LIBRARY_LISTING_PAGE_SIZE + 1
    : 0
  const listingRangeEnd = Math.min(
    effectiveListingPage * LIBRARY_LISTING_PAGE_SIZE,
    visibleItems.length
  )
  const classificationGroups = useMemo(() => {
    if (!['playlist', 'genre', 'rating', 'release'].includes(activeNav)) return [] as Array<{ label: string; items: MediaItem[] }>
    const groups = new Map<string, MediaItem[]>()
    visibleItems.forEach((item) => {
      const labels = activeNav === 'playlist'
        ? playlists.filter((playlist) => item.playlistIds?.includes(playlist.id) || (item.inPlaylist && playlist.id === WATCH_LATER_PLAYLIST_ID)).map((playlist) => playlist.name)
        : activeNav === 'genre'
        ? (item.genres.length ? item.genres : ['未分类'])
        : activeNav === 'rating'
          ? [item.rating > 0 ? `${Math.floor(item.rating)}.0–${Math.floor(item.rating)}.9` : '暂无评分']
          : [item.year > 0 ? String(item.year) : '未知年份']
      labels.forEach((label) => groups.set(label, [...(groups.get(label) ?? []), item]))
    })
    const rows = [...groups].map(([label, items]) => ({ label, items }))
    return activeNav === 'genre' || activeNav === 'playlist'
      ? rows.sort((a, b) => a.label.localeCompare(b.label, 'zh-Hans-CN'))
      : rows.sort((a, b) => (Number.parseFloat(b.label) || -1) - (Number.parseFloat(a.label) || -1))
  }, [activeNav, playlists, visibleItems])

  useEffect(() => {
    if (listingPage > listingPageCount) setListingPage(listingPageCount)
  }, [listingPage, listingPageCount])

  const sourceMap = useMemo(() => new Map(sources.map((source) => [source.id, source])), [sources])
  const embySourceIds = useMemo(
    () => new Set(sources.filter((source) => source.kind === 'Emby').map((source) => source.id)),
    [sources]
  )
  const fileSystemSources = useMemo(
    () => sources.filter((source) => source.kind !== 'Emby'),
    [sources]
  )
  const mediaServiceSources = useMemo(
    () => sources.filter((source) => source.kind === 'Emby'),
    [sources]
  )
  const activeSource = activeNav.startsWith('source:') ? sourceMap.get(activeNav.slice('source:'.length)) : undefined
  const editingSource = editingSourceId ? sourceMap.get(editingSourceId) : undefined
  const editingFileServiceItems = editingSource && editingSource.kind !== 'Emby'
    ? allItems.filter((item) => item.sourceId === editingSource.id)
    : []
  const editingFileServiceScanning = Boolean(editingSource && editingSource.kind !== 'Emby' && scanningLocalFolderSourceIds.has(editingSource.id))
  const editingFileServiceScanProgress = editingSource ? scanProgressBySourceId.get(editingSource.id) : undefined
  const editingFileSystemSource = sourceSetupMode === 'localFolder' && editingSource?.kind !== 'Emby'
    ? editingSource
    : undefined
  const editingFileSystemItems = editingFileSystemSource
    ? allItems.filter((item) => item.sourceId === editingFileSystemSource.id)
    : []
  const editingFileSystemScanning = Boolean(editingFileSystemSource && scanningLocalFolderSourceIds.has(editingFileSystemSource.id))
  const isSourceSetup = sourceSetupMode !== 'hidden'
  const isActiveEmby = activeSource?.kind === 'Emby'
  const isActiveFileSystemScanning = Boolean(activeSource && activeSource.kind !== 'Emby' && scanningLocalFolderSourceIds.has(activeSource.id))
  const activeSourceScanProgress = activeSource ? scanProgressBySourceId.get(activeSource.id) : undefined
  const selectedItem = isSourceSetup
    ? undefined
    : visibleItems.find((item) => item.id === selectedId)
      ?? allItems.find((item) => item.id === selectedId && (!activeSource || item.sourceId === activeSource.id))
      ?? detailItemsById.get(selectedId)
  const selectedDetailItem = selectedItem ? detailItemsById.get(selectedItem.id) ?? selectedItem : undefined
  useEffect(() => {
    if (!isDetailOpen || !selectedDetailItem || selectedDetailItem.streamSpecs?.length) return
    const source = sourceMap.get(selectedDetailItem.sourceId)
    if (!source || source.kind === 'Emby' || mediaProbeRequestedIdsRef.current.has(selectedDetailItem.id)) return
    const nestedItem = selectedDetailItem.versions?.find((item) => item.playbackPath || item.path)
      ?? selectedDetailItem.seasons?.flatMap((season) => season.episodes).find((episode) => episode.item?.playbackPath || episode.item?.path)?.item
      ?? selectedDetailItem.episodes?.find((episode) => episode.item?.playbackPath || episode.item?.path)?.item
    let probePath = selectedDetailItem.playbackPath || selectedDetailItem.path || nestedItem?.playbackPath || nestedItem?.path || ''
    if (!probePath) return
    if (source.kind === 'WebDAV') {
      const credentials = loadWebDavCredentials(source.id)
      if (credentials) probePath = credentialedWebDavUrl(probePath, credentials.username, credentials.password)
    }
    mediaProbeRequestedIdsRef.current.add(selectedDetailItem.id)
    const taskId = `probe:${selectedDetailItem.id}`
    const requestProbe = (): void => {
      startTask(taskId, 'probe', `${language === 'zh' ? '媒体探测' : 'Media probe'} · ${selectedDetailItem.title}`, probePath, 1, requestProbe)
      postNativeCommand({ type: 'command', command: 'probeMediaDetails', requestId: selectedDetailItem.id, path: probePath })
    }
    taskNativeCancelRef.current.set(taskId, () => postNativeCommand({ type: 'command', command: 'cancelLibraryProbe', requestId: selectedDetailItem.id }))
    requestProbe()
    const timeout = window.setTimeout(() => {
      mediaProbeRequestedIdsRef.current.delete(selectedDetailItem.id)
      updateTask(taskId, { status: 'failed', error: language === 'zh' ? '媒体探测超时' : 'Media probe timed out' })
      if (selectedId === selectedDetailItem.id) {
        setPlayIntent('媒体信息读取超时，请关闭详情后重试')
      }
    }, 25000)
    mediaProbeTimeoutsRef.current.set(selectedDetailItem.id, timeout)
    return () => {
      window.clearTimeout(timeout)
      mediaProbeTimeoutsRef.current.delete(selectedDetailItem.id)
      mediaProbeRequestedIdsRef.current.delete(selectedDetailItem.id)
    }
  }, [isDetailOpen, language, selectedDetailItem, selectedId, sourceMap])
  useEffect(() => {
    if (!isDetailOpen || !selectedDetailItem || selectedDetailItem.cast?.length ||
        selectedDetailItem.metadataProvider !== 'tmdb' || !selectedDetailItem.externalIds?.tmdb ||
        castRefreshRequestedIdsRef.current.has(selectedDetailItem.id)) return
    castRefreshRequestedIdsRef.current.add(selectedDetailItem.id)
    void scrapeTmdbItem(selectedDetailItem, tmdbSettings)
      .then((updatedItem) => applyUpdatedMediaItem(updatedItem))
      .catch(() => castRefreshRequestedIdsRef.current.delete(selectedDetailItem.id))
  }, [isDetailOpen, selectedDetailItem, tmdbSettings])
  const activeMetadataEditorItem = metadataEditorItem
    ? detailItemsById.get(metadataEditorItem.id)
      ?? allItems.find((item) => item.id === metadataEditorItem.id)
      ?? visibleItems.find((item) => item.id === metadataEditorItem.id)
      ?? metadataEditorItem
    : undefined
  const selectedDetailLoaded = Boolean(selectedItem && loadedDetailIds.has(selectedItem.id))
  const sourceScopedContinueItems = activeSource
    ? continueItems.filter((item) => item.sourceId === activeSource.id)
    : continueItems.filter((item) => !embySourceIds.has(item.sourceId))
  const scopedContinueItems = activeLibraryViewId && sourceScopedContinueItems.some((item) => item.libraryViewId)
    ? sourceScopedContinueItems.filter((item) => item.libraryViewId === activeLibraryViewId)
    : sourceScopedContinueItems
  const scopedHomeSections = activeSource
    ? homeSections.filter((section) => section.sourceId === activeSource.id)
    : homeSections
  const libraryViewCards = scopedHomeSections.flatMap((section) => section.cards).filter((card) => card.kind === 'view')
  const activeLibraryCard = libraryViewCards.find((card) => homeCardViewId(card) === activeLibraryViewId)
  const showHomeSections = Boolean(
    isActiveEmby
    && !activeLibraryViewId
    && activeView === 'home'
    && !query.trim()
    && mediaFilter === 'all'
    && sortKey === 'recent'
    && sortOrder === 'descending'
    && scopedHomeSections.length
  )
  const showDetailPanel = Boolean(!isSourceSetup && selectedItem && isDetailOpen)
  const useMainOnlyLayout = Boolean(isSourceSetup || !showDetailPanel)
  const isInsideEmbyLibraryView = Boolean(isActiveEmby && activeLibraryViewId)
  const showLibraryViewTabs = !isInsideEmbyLibraryView
  const showContinueSection = Boolean(!isInsideEmbyLibraryView && (!isActiveEmby || mediaFilter === 'all'))
  const showSortControl = Boolean(activeLibraryViewId || (!isActiveEmby && activeView !== 'home'))
  const listingBaseTitle = activeLibraryCard?.title ?? (activeSource ? `${activeSource.name} ${language === 'zh' ? '主页' : 'Home'}` : navLabel(activeNav, sourceMap, language))
  const listingTitle = mediaFilter === 'all'
    ? listingBaseTitle
    : `${listingBaseTitle} · ${mediaFilter === 'inProgress' ? '继续观看' : '筛选'}`
  const serverUrl = buildServerAddress(serverProtocol, serverHost, serverPort, serverPath)
  const smbSelectedRows = selectedPathRows(smbSelectedPaths)
  const webDavSelectedRows = selectedPathRows(webDavSelectedPaths)
  const smbParent = smbCurrentPath ? smbParentPath(smbCurrentPath) : ''
  const webDavParent = webDavCurrentPath ? webDavParentUrl(webDavCurrentPath) : ''
  const editingSavedConnection = editingSourceId
    ? savedConnections.find((connection) => connection.sourceId === editingSourceId)
    : undefined
  const editingSession = editingSourceId
    ? embySessionsBySourceId.get(editingSourceId) ?? editingSavedConnection?.session
    : undefined
  const canConnectEmby = Boolean(serverHost.trim() && username.trim() && (password || editingSession))
  const settingsLabels = settingsCopy[language]
  const localizedPrimaryNav = localizedNav(primaryNav, language)
  const localizedSmartNav = localizedNav(smartNav, language)
  const viewTabs = localizedViewTabs(language)
  const sortOptions = localizedSortOptions(language)
  const pageEyebrow = sourceSetupMode === 'settings'
    ? settingsLabels.globalSettings
    : sourceSetupMode === 'select'
    ? language === 'zh' ? '媒体源' : 'Media source'
    : sourceSetupMode === 'emby'
      ? `${language === 'zh' ? '媒体源' : 'Media source'} · Emby`
      : sourceSetupMode === 'localFolder'
      ? editingFileSystemSource ? `${language === 'zh' ? '文件系统' : 'File system'} · ${editingFileSystemSource.location}` : `${language === 'zh' ? '媒体源' : 'Media source'} · ${language === 'zh' ? '文件系统' : 'File system'}`
      : sourceSetupMode === 'smb'
      ? editingSource ? `${language === 'zh' ? '文件系统' : 'File system'} · ${editingSource.location}` : `${language === 'zh' ? '媒体源' : 'Media source'} · SMB`
      : sourceSetupMode === 'webdav'
      ? editingSource ? `${language === 'zh' ? '文件系统' : 'File system'} · ${editingSource.location}` : `${language === 'zh' ? '媒体源' : 'Media source'} · WebDAV`
      : activeSource
        ? activeLibraryCard
          ? `${sourceKindLabel(activeSource.kind)} · ${activeSource.name}`
          : `${sourceKindLabel(activeSource.kind)} · ${activeSource.location}`
        : `${language === 'zh' ? '独立管理器' : 'Library manager'} · ${navLabel(activeNav, sourceMap, language)}`
  const pageTitle = sourceSetupMode === 'settings'
    ? settingsLabels.settings
    : sourceSetupMode === 'select'
    ? language === 'zh' ? '添加媒体源' : 'Add media source'
    : sourceSetupMode === 'emby'
      ? language === 'zh' ? '添加 Emby 源' : 'Add Emby source'
      : sourceSetupMode === 'localFolder'
      ? editingFileSystemSource?.name ?? (language === 'zh' ? '添加本地文件夹' : 'Add local folder')
      : sourceSetupMode === 'smb'
      ? editingSource?.name ?? (language === 'zh' ? '添加 SMB' : 'Add SMB')
      : sourceSetupMode === 'webdav'
      ? editingSource?.name ?? (language === 'zh' ? '添加 WebDAV' : 'Add WebDAV')
      : activeLibraryCard?.title ?? activeSource?.name ?? (language === 'zh' ? '媒体库' : 'Library')

  const fallbackSource: LibrarySource = {
    id: 'unknown',
    name: '未归档',
    kind: 'Local',
    status: 'draft',
    itemCount: 0,
    location: ''
  }
  const sourceFor = (sourceId: string): LibrarySource => sourceMap.get(sourceId) ?? fallbackSource

  useEffect(() => {
    if (!selectedItem || !isDetailOpen || selectedDetailLoaded) return undefined
    const source = sourceMap.get(selectedItem.sourceId)
    const session = embySessionsBySourceId.get(selectedItem.sourceId)
    if (source?.kind !== 'Emby' || !session) return undefined

    const detailItem = selectedItem
    const detailSession = session
    let cancelled = false

    async function loadSelectedDetail(): Promise<void> {
      try {
        debugLibraryPlayback(`emby detail load start item=${detailItem.id}`)
        const detail = await loadEmbyItemDetails(detailSession, detailItem)
        if (cancelled) return
        setDetailItemsById((current) => {
          const next = new Map(current)
          next.set(detail.id, detail)
          return next
        })
        setLoadedDetailIds((current) => {
          const next = new Set(current)
          next.add(detail.id)
          return next
        })
        debugLibraryPlayback(`emby detail load ok item=${detailItem.id}`)
      } catch (error) {
        debugLibraryPlayback(`emby detail load failed item=${detailItem.id} error=${errorText(error)}`)
      }
    }

    void loadSelectedDetail()
    return () => {
      cancelled = true
    }
  }, [embySessionsBySourceId, isDetailOpen, selectedDetailLoaded, selectedItem, sourceMap])

  useEffect(() => {
    if (!activeSource || activeSource.kind !== 'Emby') return undefined
    if (activeLibraryViewId || activeView !== 'home' || query.trim() || mediaFilter !== 'all') return undefined
    if (scopedHomeSections.length > 0) return undefined

    const sourceId = activeSource.id
    const session = embySessionsBySourceId.get(sourceId)
    if (!session || homeSectionRefreshSourceIdsRef.current.has(sourceId)) return undefined

    const refreshSession = session
    const refreshSourceName = activeSource.name
    homeSectionRefreshSourceIdsRef.current.add(sourceId)
    let cancelled = false

    async function refreshMissingHomeSections(): Promise<void> {
      try {
        debugLibraryPlayback(`emby home refresh start source=${sourceId}`)
        const snapshot = await refreshEmbyLibrary(refreshSession, refreshSourceName)
        await client.upsertSourceItems(snapshot.source, snapshot.items, snapshot.homeSections)

        const saved = savedConnections.find((connection) => connection.sourceId === snapshot.source.id)
        saveSavedEmbyConnection({
          sourceId: snapshot.source.id,
          name: snapshot.source.name,
          serverUrl: snapshot.session.apiBaseUrl,
          username: snapshot.session.userName || saved?.username || '',
          ignoreCertificateErrors: saved?.ignoreCertificateErrors ?? ignoreCertificateErrors,
          session: snapshot.session,
          savedAt: Date.now()
        })

        const [sourceRows, allRows, continueRows, allHomeRows] = await Promise.all([
          client.listSources(),
          client.listAllItems(),
          client.getContinueWatching(),
          client.listHomeSections()
        ])

        if (cancelled) return
        setSources(sourceRows)
        setAllItems(allRows)
        setContinueItems(continueRows)
        setHomeSections(allHomeRows)
        setEmbySessionsBySourceId((current) => {
          const next = new Map(current)
          next.set(snapshot.source.id, snapshot.session)
          return next
        })
        if (sourceId !== snapshot.source.id && activeNavRef.current === `source:${sourceId}`) {
          setActiveNav(`source:${snapshot.source.id}` as NavKey)
        }
        debugLibraryPlayback(`emby home refresh ok source=${sourceId} sections=${snapshot.homeSections.length}`)
      } catch (error) {
        debugLibraryPlayback(`emby home refresh failed source=${sourceId} error=${errorText(error)}`)
      }
    }

    void refreshMissingHomeSections()
    return () => {
      cancelled = true
    }
  }, [
    activeLibraryViewId,
    activeSource,
    activeView,
    client,
    embySessionsBySourceId,
    ignoreCertificateErrors,
    mediaFilter,
    query,
    savedConnections,
    scopedHomeSections.length
  ])

  function debugLibraryPlayback(message: string): void {
    postNativeCommand({
      type: 'command',
      command: 'debugLog',
      message
    })
  }

  function redactPlaybackUrl(url: string): string {
    return url.replace(/([?&](?:api_key|X-Emby-Token)=)[^&]+/gi, '$1<redacted>')
  }

  async function refreshLibraryRowsFor(options: {
    navKey: NavKey
    view?: LibraryView
    search?: string
    filterKey?: MediaFilterKey
    sortKey?: SortKey
    sortOrder?: SortOrder
    libraryViewId?: string
  }): Promise<MediaItem[]> {
    const sourceRows = await client.listSources()
    const activeSourceId = options.navKey.startsWith('source:') ? options.navKey.slice('source:'.length) : ''
    const activeSourceKind = activeSourceId ? sourceRows.find((source) => source.id === activeSourceId)?.kind : undefined
    const [allRows, continueRows, visibleRows, homeRows] = await Promise.all([
      client.listAllItems(),
      client.getContinueWatching(),
      client.listItems({
        navKey: options.navKey,
        view: options.view ?? activeView,
        search: options.search ?? debouncedQuery,
        sortKey: options.sortKey ?? sortKey,
        sortOrder: options.sortOrder ?? sortOrder,
        filterKey: options.filterKey ?? mediaFilter,
        libraryViewId: activeSourceKind === 'Emby' ? options.libraryViewId ?? activeLibraryViewId : undefined
      }),
      client.listHomeSections()
    ])

    setSources(sourceRows)
    setAllItems(allRows)
    setContinueItems(continueRows)
    setVisibleItems(visibleRows)
    setHomeSections(homeRows)
    return visibleRows
  }

  function activateFileSystemSource(source: LibrarySource, visibleRows: MediaItem[]): void {
    setActiveNav(`source:${source.id}` as NavKey)
    setActiveView('home')
    setActiveLibraryViewId('')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setDebouncedQuery('')
    setSelectedId(visibleRows[0]?.id ?? '')
    setIsDetailOpen(false)
    setSourceSetupMode('hidden')
    setEditingSourceId('')
    setPlayIntent('')
  }

  function toggleSelectedPath(
    setSelectedPaths: Dispatch<SetStateAction<Set<string>>>,
    path: string
  ): void {
    setSelectedPaths((current) => {
      const next = new Set(current)
      const existing = [...next].find((selectedPath) => locationKey(selectedPath) === locationKey(path))
      if (existing) {
        next.delete(existing)
      } else {
        next.add(path)
      }
      return next
    })
  }

  function selectedPathRows(selectedPaths: Set<string>): string[] {
    return [...selectedPaths].sort((left, right) =>
      directoryDisplayName(left, left).localeCompare(directoryDisplayName(right, right), 'zh-Hans-CN')
    )
  }

  function browseSmbDirectory(path?: string): void {
    const host = normalizeSmbHost(smbHost || path || smbCurrentPath)
    if (!host) {
      setConnectTone('error')
      setConnectMessage('请填写 SMB 主机或 IP。')
      return
    }
    if (!window.chrome?.webview) {
      setConnectTone('error')
      setConnectMessage('请在 Anvil Player 桌面应用中浏览 SMB。')
      return
    }

    const requestId = `${Date.now()}-${Math.random().toString(16).slice(2)}`
    smbBrowseRequestIdRef.current = requestId
    setIsBrowsingSmb(true)
    setConnectTone('idle')
    setConnectMessage('正在读取 SMB 文件夹...')
    postNativeCommand({
      type: 'command',
      command: 'listSmbDirectory',
      requestId,
      host,
      path: path ?? smbCurrentPath,
      username: smbUsername,
      password: smbPassword
    })
  }

  function browseWebDavDirectory(path?: string): void {
    let targetUrl = ''
    try {
      targetUrl = normalizeWebDavSourceUrl(path || webDavCurrentPath || webDavUrl)
    } catch {
      setConnectTone('error')
      setConnectMessage('请填写有效的 WebDAV 地址。')
      return
    }

    if (!window.chrome?.webview) {
      setConnectTone('error')
      setConnectMessage('请在 Anvil Player 桌面应用中浏览 WebDAV。')
      return
    }

    const requestId = `${Date.now()}-${Math.random().toString(16).slice(2)}`
    webDavBrowseRequestIdRef.current = requestId
    setIsBrowsingWebDav(true)
    setConnectTone('idle')
    setConnectMessage('正在读取 WebDAV 文件夹...')
    postNativeCommand({
      type: 'command',
      command: 'listWebDavDirectory',
      requestId,
      url: targetUrl,
      username: webDavUsername,
      password: webDavPassword
    })
  }

  async function activatePendingFileServiceSource(source: LibrarySource): Promise<void> {
    const visibleRows = await refreshLibraryRowsFor({
      navKey: `source:${source.id}` as NavKey,
      view: 'home',
      search: '',
      sortKey: 'recent',
      sortOrder: 'descending',
      filterKey: 'all',
      libraryViewId: ''
    })
    activateFileSystemSource(source, visibleRows)
  }

  async function applyUpdatedMediaItem(updatedItem: MediaItem): Promise<void> {
    const source = sourceFor(updatedItem.sourceId)
    const nextAllItems = allItems.some((item) => item.id === updatedItem.id)
      ? allItems.map((item) => item.id === updatedItem.id ? updatedItem : item)
      : [...allItems, updatedItem]
    const rawSourceItems = nextAllItems.filter((item) => item.sourceId === updatedItem.sourceId)
    const {
      items: sourceItems,
      representativeIdByMergedId
    } = mergeDuplicateMetadataItems(rawSourceItems)
    const selectedUpdatedId = representativeIdByMergedId.get(updatedItem.id) ?? updatedItem.id
    await client.upsertSourceItems({ ...source, itemCount: sourceItems.length }, sourceItems)

    const activeSourceId = activeNav.startsWith('source:') ? activeNav.slice('source:'.length) : ''
    const activeSourceKind = activeSourceId ? sourceMap.get(activeSourceId)?.kind : undefined
    const [sourceRows, allRows, continueRows, visibleRows, homeRows] = await Promise.all([
      client.listSources(),
      client.listAllItems(),
      client.getContinueWatching(),
      client.listItems({
        navKey: activeNav,
        view: activeView,
        search: debouncedQuery,
        sortKey,
        sortOrder,
        filterKey: mediaFilter,
        libraryViewId: activeSourceKind === 'Emby' ? activeLibraryViewId : undefined
      }),
      client.listHomeSections()
    ])

    setSources(sourceRows)
    setAllItems(allRows)
    setContinueItems(continueRows)
    setVisibleItems(visibleRows)
    setHomeSections(homeRows)
    setSelectedId(selectedUpdatedId)
    setDetailItemsById((current) => {
      const next = new Map(current)
      representativeIdByMergedId.forEach((representativeId, mergedId) => {
        if (representativeId !== mergedId) next.delete(mergedId)
      })
      sourceItems.forEach((item) => next.set(item.id, item))
      return next
    })
  }

  function openMetadataEditor(item: MediaItem): void {
    const source = sourceFor(item.sourceId)
    if (source.kind === 'Emby') return
    setMetadataEditorItem(detailItemsById.get(item.id) ?? item)
  }

  function closeMetadataEditor(): void {
    setMetadataEditorItem(undefined)
  }

  async function saveManualMetadata(updatedItem: MediaItem): Promise<void> {
    const source = sourceFor(updatedItem.sourceId)
    if (source.kind === 'Emby') return

    setIsSavingManualMetadata(true)
    try {
      await applyUpdatedMediaItem(updatedItem)
      setMetadataEditorItem(undefined)
      setPlayIntent(`已保存 ${updatedItem.title} 的元数据`)
    } catch (error) {
      const message = error instanceof Error ? error.message : '保存元数据失败'
      setPlayIntent(message)
    } finally {
      setIsSavingManualMetadata(false)
    }
  }

  async function cacheArtworkRows(items: MediaItem[], taskId: string, title: string): Promise<void> {
    const urls = Array.from(new Set(items.flatMap((item) => [
      imageBackgroundUrl(item.poster),
      imageBackgroundUrl(item.backdrop),
      ...(item.cast ?? []).map((person) => imageBackgroundUrl(person.image))
    ]).filter((url): url is string => Boolean(url))))
    if (!urls.length) return
    startTask(taskId, 'artwork', title, language === 'zh' ? '下载海报、背景和人物图片' : 'Downloading posters, backdrops and cast images', urls.length, () => { void cacheArtworkRows(items, taskId, title) })
    let failed = 0
    for (const url of urls) {
      if (await waitWhileTaskPaused(taskId)) return
      await new Promise<void>((resolve) => {
        const image = new Image()
        image.onload = () => resolve()
        image.onerror = () => { failed += 1; resolve() }
        image.src = url
      })
      advanceTask(taskId, {})
    }
    if (taskStatusRef.current.get(taskId) === 'cancelled') return
    updateTask(taskId, failed
      ? { status: 'failed', error: `${failed} ${language === 'zh' ? '张图片下载失败' : 'images failed'}` }
      : { status: 'completed', completed: urls.length })
  }

  async function updateLibraryFlags(item: MediaItem, flags: Pick<Partial<MediaItem>, 'favorite' | 'inPlaylist'>): Promise<void> {
    await applyUpdatedMediaItem({ ...item, ...flags })
  }

  async function toggleItemPlaylist(item: MediaItem, playlistId: string): Promise<void> {
    const current = new Set(item.playlistIds ?? (item.inPlaylist ? [WATCH_LATER_PLAYLIST_ID] : []))
    if (current.has(playlistId)) current.delete(playlistId)
    else current.add(playlistId)
    await applyUpdatedMediaItem({
      ...item,
      inPlaylist: false,
      playlistIds: [...current]
    })
  }

  function createPlaylistForItem(item: MediaItem): void {
    setNewPlaylistItem(item)
    setNewPlaylistName('')
  }

  function confirmCreatePlaylist(): void {
    const item = newPlaylistItem
    const name = newPlaylistName.trim()
    if (!item || !name) return
    if (!name) return
    const id = `playlist-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`
    const next = [...playlists, { id, name }]
    setPlaylists(next)
    saveMediaPlaylists(next)
    setNewPlaylistItem(undefined)
    setNewPlaylistName('')
    void toggleItemPlaylist(item, id)
  }

  async function scrapeMediaMetadata(item: MediaItem, ignoreSavedMatch = false): Promise<void> {
    const source = sourceFor(item.sourceId)
    if (source.kind === 'Emby') return
    if (item.metadataLocked && !ignoreSavedMatch) {
      setPlayIntent(language === 'zh' ? '元数据已锁定，请先解锁。' : 'Metadata is locked. Unlock it first.')
      return
    }

    const taskId = `metadata:${item.id}`
    startTask(taskId, 'metadata', `${language === 'zh' ? '元数据刮削' : 'Metadata'} · ${item.title}`, 'TMDB', 1, () => { void scrapeMediaMetadata(item, ignoreSavedMatch) })
    setScrapingMetadataItemId(item.id)
    setPlayIntent('正在从 TMDB 刮削元数据...')
    debugLibraryPlayback(`tmdb scrape start id=${item.id} title=${item.title}`)
    try {
      const updatedItem = await scrapeTmdbItem(item, tmdbSettings, { ignoreSavedMatch })
      if (await waitWhileTaskPaused(taskId)) return
      await applyUpdatedMediaItem(updatedItem)
      updateTask(taskId, { status: 'completed', completed: 1, detail: `TMDB ${updatedItem.externalIds?.tmdb ?? ''}` })
      await cacheArtworkRows([updatedItem], `artwork:${item.id}`, `${language === 'zh' ? '图片下载' : 'Artwork'} · ${updatedItem.title}`)
      setPlayIntent(`已用 TMDB 更新 ${updatedItem.title}`)
      debugLibraryPlayback(`tmdb scrape ok id=${item.id} tmdb=${updatedItem.externalIds?.tmdb ?? 'unknown'}`)
    } catch (error) {
      const message = error instanceof Error ? error.message : 'TMDB 刮削失败'
      setPlayIntent(message)
      updateTask(taskId, { status: 'failed', error: message })
      debugLibraryPlayback(`tmdb scrape failed id=${item.id} error=${message}`)
    } finally {
      setScrapingMetadataItemId((current) => current === item.id ? '' : current)
    }
  }

  async function clearMediaMetadata(item: MediaItem): Promise<void> {
    const source = sourceFor(item.sourceId)
    if (source.kind === 'Emby') return
    const clearedItem = clearLocalMetadata(item)
    await applyUpdatedMediaItem(clearedItem)
    setPlayIntent(`已删除 ${clearedItem.title} 的元数据`)
  }

  async function resolveMediaTrailer(item: MediaItem): Promise<string[]> {
    try {
      debugLibraryPlayback(`trailer lookup start item=${item.id} source=${trailerSettings.source}`)
      const urls = trailerSettings.source === 'bilibili'
        ? await searchBilibiliTrailerUrls(item, trailerSettings)
        : await fetchTmdbTrailerUrls(item, tmdbSettings)
      debugLibraryPlayback(`trailer lookup result item=${item.id} source=${trailerSettings.source} count=${urls.length}`)
      if (!urls.length) {
        const provider = trailerSettings.source === 'bilibili' ? (language === 'zh' ? 'B 站' : 'Bilibili') : 'TMDB'
        setPlayIntent(language === 'zh' ? `${provider}没有找到可信的预告片` : `${provider} did not return a reliable trailer`)
        return []
      }
      const updatedItem = { ...item, trailerUrl: urls[0], trailerUrls: urls }
      if (sourceFor(item.sourceId).kind === 'Emby') {
        setDetailItemsById((current) => new Map(current).set(item.id, updatedItem))
      } else {
        await applyUpdatedMediaItem(updatedItem)
      }
      return urls
    } catch (error) {
      const message = error instanceof Error ? error.message : (language === 'zh' ? '预告片查询失败' : 'Trailer lookup failed')
      debugLibraryPlayback(`trailer lookup failed item=${item.id} source=${trailerSettings.source} error=${message}`)
      setPlayIntent(message)
      return []
    }
  }

  async function dissolveAndClearMediaCollection(item: MediaItem): Promise<void> {
    const source = sourceFor(item.sourceId)
    if (source.kind === 'Emby') return
    const members = dissolveMediaCollection(item)
    members.forEach((member) => {
      const path = member.path || member.playbackPath
      if (path) dissolvedCollectionPathsRef.current.add(locationKey(path))
    })
    saveDissolvedCollectionPaths(dissolvedCollectionPathsRef.current)
    const sourceItems = [
      ...allItems.filter((candidate) => candidate.sourceId === item.sourceId && candidate.id !== item.id),
      ...members
    ]
    await client.upsertSourceItems({ ...source, itemCount: sourceItems.length }, sourceItems)
    const visibleRows = await refreshLibraryRowsFor({
      navKey: activeNav,
      view: activeView,
      search: debouncedQuery,
      filterKey: mediaFilter,
      sortKey,
      sortOrder,
      libraryViewId: activeLibraryViewId
    })
    setDetailItemsById((current) => {
      const next = new Map(current)
      next.delete(item.id)
      members.forEach((member) => next.set(member.id, member))
      return next
    })
    setSelectedId(visibleRows.find((row) => members.some((member) => member.id === row.id))?.id ?? '')
    setIsDetailOpen(false)
    setPlayIntent(language === 'zh'
      ? `已删除元数据并解散为 ${members.length} 个独立视频`
      : `Metadata removed and dissolved into ${members.length} individual videos`)
  }

  async function removeMediaFromLibrary(item: MediaItem): Promise<void> {
    const message = language === 'zh'
      ? `从媒体库中移除“${item.title}”？\n\n只会删除媒体库展示和缓存，不会删除真实文件。`
      : `Remove “${item.title}” from the library?\n\nThis only removes the library entry and cache. The real file will not be deleted.`
    if (!window.confirm(message)) return
    await client.hideItem(item)
    const visibleRows = await refreshLibraryRowsFor({
      navKey: activeNav,
      view: activeView,
      search: debouncedQuery,
      filterKey: mediaFilter,
      sortKey,
      sortOrder,
      libraryViewId: activeLibraryViewId
    })
    setDetailItemsById((current) => {
      const next = new Map(current)
      next.delete(item.id)
      return next
    })
    setSelectedId(visibleRows[0]?.id ?? '')
    setIsDetailOpen(false)
    setConnectTone('success')
    setConnectMessage(language === 'zh'
      ? `已从媒体库移除“${item.title}”，真实文件未删除。`
      : `Removed “${item.title}” from the library. The real file was not deleted.`)
  }

  async function clearSourceMetadata(sourceId: string): Promise<void> {
    const source = sourceMap.get(sourceId)
    if (!source || source.kind === 'Emby') return
    if (!window.confirm(`清空 ${source.name} 的全部 TMDB 元数据？`)) return
    const nextAllItems = allItems.map((item) => item.sourceId === sourceId ? clearLocalMetadata(item) : item)
    const nextSourceItems = nextAllItems.filter((item) => item.sourceId === sourceId)
    await client.upsertSourceItems({ ...source, itemCount: nextSourceItems.length }, nextSourceItems)
    const [sourceRows, allRows, continueRows, visibleRows, homeRows] = await Promise.all([
      client.listSources(),
      client.listAllItems(),
      client.getContinueWatching(),
      client.listItems({
        navKey: activeNav,
        view: activeView,
        search: debouncedQuery,
        sortKey,
        sortOrder,
        filterKey: mediaFilter
      }),
      client.listHomeSections()
    ])
    setSources(sourceRows)
    setAllItems(allRows)
    setContinueItems(continueRows)
    setVisibleItems(visibleRows)
    setHomeSections(homeRows)
    setDetailItemsById((current) => {
      const next = new Map(current)
      nextSourceItems.forEach((item) => next.set(item.id, item))
      return next
    })
    setConnectTone('success')
    setConnectMessage(`已清空 ${source.name} 的全部元数据。`)
  }

  async function deleteSource(sourceId: string): Promise<void> {
    const source = sourceMap.get(sourceId)
    if (!source) return
    if (!window.confirm(`删除媒体源 ${source.name}？这会移除该源的缓存条目。`)) return
    if (source.kind !== 'Emby' && scanningLocalFolderSourceIds.has(sourceId)) {
      ignoredLocalFolderScanSourceIdsRef.current.add(sourceId)
    }
    pendingFolderScanCountsRef.current.delete(sourceId)
    await client.removeSource(sourceId)
    setScanningLocalFolderSourceIds((current) => {
      const next = new Set(current)
      next.delete(sourceId)
      return next
    })
    setScanProgressBySourceId((current) => {
      const next = new Map(current)
      next.delete(sourceId)
      return next
    })
    if (source.kind === 'Emby') {
      removeSavedEmbyConnection(sourceId)
      setEmbySessionsBySourceId((current) => {
        const next = new Map(current)
        next.delete(sourceId)
        return next
      })
    } else if (source.kind === 'SMB') {
      removeSmbCredentials(sourceId)
    } else if (source.kind === 'WebDAV') {
      removeWebDavCredentials(sourceId)
    }
    scanLocationsForSource(source).forEach((location) => {
      fileSystemSourceByLocationRef.current.delete(locationKey(location))
    })
    const [sourceRows, allRows, continueRows, homeRows] = await Promise.all([
      client.listSources(),
      client.listAllItems(),
      client.getContinueWatching(),
      client.listHomeSections()
    ])
    const nextNav = defaultLibraryNav(sourceRows
      .filter((candidate) => candidate.kind === 'Emby')
      .map((candidate) => ({
        sourceId: candidate.id,
        name: candidate.name,
        serverUrl: candidate.location,
        username: '',
        ignoreCertificateErrors: false,
        savedAt: Date.now()
      })))
    const fallbackNav = sourceRows[0] ? `source:${sourceRows[0].id}` as NavKey : nextNav
    const visibleRows = await client.listItems({ navKey: fallbackNav, view: 'home', search: '', sortKey: 'recent' })
    setSources(sourceRows)
    setAllItems(allRows)
    setContinueItems(continueRows)
    setHomeSections(homeRows)
    setVisibleItems(visibleRows)
    setSourceSetupMode(sourceRows.length ? 'hidden' : 'select')
    setEditingSourceId('')
    setActiveNav(fallbackNav)
    setActiveView('home')
    setActiveLibraryViewId('')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setDebouncedQuery('')
    setSelectedId(visibleRows[0]?.id ?? '')
    setIsDetailOpen(false)
    setPlayIntent('')
    setConnectTone('success')
    setConnectMessage(`已删除 ${source.name}`)
  }

  async function scrapeSourceMetadata(sourceId: string): Promise<void> {
    const source = sourceMap.get(sourceId)
    if (!source || source.kind === 'Emby') return

    const sourceItems = allItems.filter((item) =>
      item.sourceId === sourceId &&
      item.type !== 'folder' &&
      item.availability !== 'missing' &&
      !item.metadataLocked
    )
    if (!sourceItems.length) {
      setConnectTone('error')
      setConnectMessage('这个文件系统源里没有可刮削的媒体文件。')
      return
    }
    if (!tmdbSettings.credential.trim()) {
      setConnectTone('error')
      setConnectMessage('请先在设置中填写 TMDB API Key 或 Read Access Token。')
      return
    }

    setScrapingSourceMetadataId(sourceId)
    const taskId = `metadata-source:${sourceId}`
    startTask(taskId, 'metadata', `${language === 'zh' ? '批量刮削' : 'Batch metadata'} · ${source.name}`, 'TMDB', sourceItems.length, () => { void scrapeSourceMetadata(sourceId) })
    setConnectTone('idle')
    setConnectMessage(`正在刮削 ${source.name}：0 / ${sourceItems.length}`)
    debugLibraryPlayback(`tmdb source scrape start source=${sourceId} count=${sourceItems.length}`)

    const updatedItems = new Map<string, MediaItem>()
    let failedCount = 0

    try {
      for (let index = 0; index < sourceItems.length; index += 1) {
        if (await waitWhileTaskPaused(taskId)) break
        const item = sourceItems[index]
        updateTask(taskId, { detail: item.title })
        setConnectMessage(`正在刮削 ${source.name}：${index + 1} / ${sourceItems.length} · ${item.title}`)
        try {
          const updatedItem = await scrapeTmdbItem(updatedItems.get(item.id) ?? item, tmdbSettings)
          updatedItems.set(item.id, updatedItem)
          debugLibraryPlayback(`tmdb source scrape item ok source=${sourceId} item=${item.id} tmdb=${updatedItem.externalIds?.tmdb ?? 'unknown'}`)
        } catch (error) {
          failedCount += 1
          const message = error instanceof Error ? error.message : 'TMDB 刮削失败'
          debugLibraryPlayback(`tmdb source scrape item failed source=${sourceId} item=${item.id} error=${message}`)
        }
        advanceTask(taskId, {})
      }

      if (taskStatusRef.current.get(taskId) === 'cancelled') return

      const nextAllItems = allItems.map((item) => updatedItems.get(item.id) ?? item)
      const rawNextSourceItems = nextAllItems.filter((item) => item.sourceId === sourceId)
      const {
        items: nextSourceItems,
        mergedCount,
        representativeIdByMergedId
      } = mergeDuplicateMetadataItems(rawNextSourceItems)
      await client.upsertSourceItems({ ...source, itemCount: nextSourceItems.length }, nextSourceItems)

      const activeSourceId = activeNav.startsWith('source:') ? activeNav.slice('source:'.length) : ''
      const activeSourceKind = activeSourceId ? sourceMap.get(activeSourceId)?.kind : undefined
      const [sourceRows, allRows, continueRows, visibleRows, homeRows] = await Promise.all([
        client.listSources(),
        client.listAllItems(),
        client.getContinueWatching(),
        client.listItems({
          navKey: activeNav,
          view: activeView,
          search: debouncedQuery,
          sortKey,
          sortOrder,
          filterKey: mediaFilter,
          libraryViewId: activeSourceKind === 'Emby' ? activeLibraryViewId : undefined
        }),
        client.listHomeSections()
      ])

      setSources(sourceRows)
      setAllItems(allRows)
      setContinueItems(continueRows)
      setVisibleItems(visibleRows)
      setHomeSections(homeRows)
      setSelectedId((current) => representativeIdByMergedId.get(current) ?? current)
      setDetailItemsById((current) => {
        const next = new Map(current)
        representativeIdByMergedId.forEach((representativeId, mergedId) => {
          if (representativeId !== mergedId) next.delete(mergedId)
        })
        nextSourceItems.forEach((item) => next.set(item.id, item))
        return next
      })

      const successCount = updatedItems.size
      updateTask(taskId, failedCount
        ? { status: 'failed', error: `${failedCount} ${language === 'zh' ? '项失败' : 'failed'}` }
        : { status: 'completed', completed: sourceItems.length })
      await cacheArtworkRows([...updatedItems.values()], `artwork-source:${sourceId}`, `${language === 'zh' ? '批量图片下载' : 'Batch artwork'} · ${source.name}`)
      setConnectTone(successCount > 0 ? 'success' : 'error')
      setConnectMessage(`刮削完成：成功 ${successCount} 个，失败 ${failedCount} 个，合并 ${mergedCount} 个重复项。`)
      debugLibraryPlayback(`tmdb source scrape done source=${sourceId} ok=${successCount} failed=${failedCount} merged=${mergedCount}`)
    } catch (error) {
      const message = error instanceof Error ? error.message : '文件系统源刮削失败'
      setConnectTone('error')
      setConnectMessage(message)
      updateTask(taskId, { status: 'failed', error: message })
      debugLibraryPlayback(`tmdb source scrape failed source=${sourceId} error=${message}`)
    } finally {
      setScrapingSourceMetadataId((current) => current === sourceId ? '' : current)
    }
  }

  async function playMediaItem(item: MediaItem, audioTrackIndex = -2, subtitleTrackIndex = -2): Promise<void> {
    const source = sourceFor(item.sourceId)
    debugLibraryPlayback(`play click id=${item.id} title=${item.title} source=${source.name} kind=${source.kind}`)
    if (source.kind !== 'Emby') {
      const localPlayableItem = item.path
        ? item
        : item.seasons?.flatMap((season) => season.episodes).find((episode) => episode.item?.path)?.item
          ?? item.episodes?.find((episode) => episode.item?.path)?.item
          ?? item
      let playablePath = localPlayableItem.playbackPath || localPlayableItem.path
      if (source.kind === 'WebDAV' && localPlayableItem.path) {
        const credentials = loadWebDavCredentials(source.id)
        playablePath = credentials
          ? credentialedWebDavUrl(localPlayableItem.path, credentials.username, credentials.password)
          : playablePath
      }
      if (!playablePath) {
        setPlayIntent('本地媒体缺少文件路径。')
        debugLibraryPlayback(`play local blocked missing path id=${item.id}`)
        return
      }
      // The library lives in its own window now; ask the host to open the
      // standalone player window (or switch its current media). The library
      // window stays on the library route.
      window.setTimeout(() => {
        if (source.kind === 'SMB') {
          const credentials = loadSmbCredentials(source.id)
          postNativeCommand({
            type: 'command',
            command: 'connectSmbShare',
            path: playablePath,
            username: credentials?.username ?? '',
            password: credentials?.password ?? ''
          })
        }
        postNativeCommand({
          type: 'command',
          command: 'requestPlayback',
          path: playablePath,
          startPositionRatio: item.progress > 0 && item.progress < 1 ? item.progress : undefined,
          audioTrackIndex,
          subtitleTrackIndex
        })
        debugLibraryPlayback(`play local requestPlayback posted id=${localPlayableItem.id} path=${localPlayableItem.path ?? ''}`)
      }, 0)
      setPlayIntent(`Opening ${localPlayableItem.title}`)
      return
    }

    const embySession = embySessionsBySourceId.get(item.sourceId)
    if (!embySession) {
      setPlayIntent('Emby session is not ready. Reconnect this source first.')
      debugLibraryPlayback(`play blocked missing emby session id=${item.id}`)
      return
    }

    setResolvingPlayItemId(item.id)
    setPlayIntent('Resolving Emby playback URL...')
    debugLibraryPlayback(`play resolve start id=${item.id}`)
    try {
      const target = await resolveEmbyPlaybackTarget(embySession, item)
      debugLibraryPlayback(`play resolve ok sourceId=${item.id} targetId=${target.itemId} url=${redactPlaybackUrl(target.url)}`)
      // savePendingEmbyPlaybackReport stores the report in this window's
      // storage AND relays it through native to the player window (which has
      // its own WebView2 storage) so progress reporting works across windows.
      const report = savePendingEmbyPlaybackReport(embySession, target)
      debugLibraryPlayback(`play emby report pending targetId=${target.itemId} reportId=${report?.id ?? 'none'}`)
      window.setTimeout(() => {
        notifyPendingEmbyPlaybackReport(report?.id)
        postNativeCommand({
          type: 'command',
          command: 'requestPlayback',
          path: target.url,
          startPositionRatio: item.progress > 0 && item.progress < 1 ? item.progress : undefined,
          audioTrackIndex,
          subtitleTrackIndex
        })
        debugLibraryPlayback(`play requestPlayback posted targetId=${target.itemId}`)
      }, 0)
      setPlayIntent(`Opening ${target.title}`)
    } catch (error) {
      const message = errorText(error)
      setPlayIntent(message)
      debugLibraryPlayback(`play resolve failed id=${item.id} error=${message}`)
    } finally {
      setResolvingPlayItemId((current) => current === item.id ? '' : current)
    }
  }

  function openSourcePicker(): void {
    setSourceSetupMode('select')
    setEditingSourceId('')
    setActiveLibraryViewId('')
    setPersonSearch(undefined)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setIsPickingLocalFolder(false)
    setFileServiceName('')
    setSmbHost('')
    setSmbUsername('')
    setSmbPassword('')
    setSmbCurrentPath('')
    setSmbDirectories([])
    setSmbSelectedPaths(new Set())
    setIsBrowsingSmb(false)
    setWebDavUrl('')
    setWebDavUsername('')
    setWebDavPassword('')
    setWebDavCurrentPath('')
    setWebDavDirectories([])
    setWebDavSelectedPaths(new Set())
    setIsBrowsingWebDav(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openSettings(): void {
    setSourceSetupMode('settings')
    setEditingSourceId('')
    setActiveLibraryViewId('')
    setPersonSearch(undefined)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setIsPickingLocalFolder(false)
    setFileServiceName('')
    setSmbHost('')
    setSmbUsername('')
    setSmbPassword('')
    setSmbCurrentPath('')
    setSmbDirectories([])
    setSmbSelectedPaths(new Set())
    setIsBrowsingSmb(false)
    setWebDavUrl('')
    setWebDavUsername('')
    setWebDavPassword('')
    setWebDavCurrentPath('')
    setWebDavDirectories([])
    setWebDavSelectedPaths(new Set())
    setIsBrowsingWebDav(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openEmbySetup(): void {
    setSourceSetupMode('emby')
    setEditingSourceId('')
    setActiveLibraryViewId('')
    setPersonSearch(undefined)
    setSelectedId('')
    setIsDetailOpen(false)
    setConnectionName('Emby')
    setServerProtocol('http')
    setServerHost('')
    setServerPort('')
    setServerPath('')
    setUsername('')
    setPassword('')
    setIsPickingLocalFolder(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openLocalFolderSetup(): void {
    setSourceSetupMode('localFolder')
    setEditingSourceId('')
    setActiveLibraryViewId('')
    setPersonSearch(undefined)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setIsPickingLocalFolder(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openSmbSetup(source?: LibrarySource): void {
    const credentials = source ? loadSmbCredentials(source.id) : undefined
    const host = credentials?.host || normalizeSmbHost(source?.location ?? '')
    setSourceSetupMode('smb')
    setEditingSourceId(source?.id ?? '')
    setActiveLibraryViewId('')
    setPersonSearch(undefined)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setFileServiceName(source?.name ?? 'SMB')
    setSmbHost(host)
    setSmbUsername(credentials?.username ?? '')
    setSmbPassword(credentials?.password ?? '')
    setSmbCurrentPath(source?.location ?? (host ? normalizeSmbPath(host) : ''))
    setSmbDirectories([])
    setSmbSelectedPaths(source ? new Set([source.location]) : new Set())
    setIsBrowsingSmb(false)
    setIsPickingLocalFolder(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openWebDavSetup(source?: LibrarySource): void {
    const credentials = source ? loadWebDavCredentials(source.id) : undefined
    const initialUrl = source ? webDavBaseUrlForSource(source) : ''
    const selectedPaths = source ? webDavSelectedPathsForSource(source) : []
    setSourceSetupMode('webdav')
    setEditingSourceId(source?.id ?? '')
    setActiveLibraryViewId('')
    setPersonSearch(undefined)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setFileServiceName(source?.name ?? 'WebDAV')
    setWebDavUrl(initialUrl)
    setWebDavUsername(credentials?.username ?? '')
    setWebDavPassword(credentials?.password ?? '')
    setWebDavCurrentPath(initialUrl)
    setWebDavDirectories([])
    setWebDavSelectedPaths(new Set(selectedPaths))
    setIsBrowsingWebDav(false)
    setIsPickingLocalFolder(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function closeLocalFolderSetup(): void {
    if (editingSourceId) {
      selectNavigation(`source:${editingSourceId}` as NavKey)
      return
    }
    setSourceSetupMode('select')
    setIsPickingLocalFolder(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function pickLocalFolder(): void {
    if (isPickingLocalFolder) return
    setIsPickingLocalFolder(true)
    setConnectTone('idle')
    setConnectMessage('正在打开文件夹选择器...')

    if (!window.chrome?.webview) {
      setIsPickingLocalFolder(false)
      setConnectTone('error')
      setConnectMessage('请在 Anvil Player 桌面应用中选择本地文件夹。')
      return
    }

    postNativeCommand({ type: 'command', command: 'pickLocalFolder' })
  }

  function rescanFileSystemSource(source: LibrarySource): void {
    if (source.kind === 'Emby') return
    if (source.kind !== 'WebDAV') {
      rescanLocalFolder(source)
      return
    }
    const credentials = loadWebDavCredentials(source.id)
    const paths = credentials?.selectedPaths?.length ? credentials.selectedPaths : scanLocationsForSource(source)
    if (!credentials || !paths.length) {
      setConnectTone('error')
      setConnectMessage(language === 'zh' ? 'WebDAV 凭据或扫描目录不可用。' : 'WebDAV credentials or scan paths are unavailable.')
      return
    }
    pendingFolderScanCountsRef.current.set(source.id, paths.length)
    beginSourceScan(source, paths.length, source.itemCount, paths[0])
    setScanningLocalFolderSourceIds((current) => new Set(current).add(source.id))
    paths.forEach((url) => postNativeCommand({
      type: 'command',
      command: 'scanWebDavFolder',
      url,
      username: credentials.username,
      password: credentials.password
    }))
  }

  async function searchMetadataMatches(item: MediaItem, query?: string): Promise<void> {
    setMetadataMatchItem(item)
    setIsSearchingMetadata(true)
    setMetadataCandidates([])
    try {
      setMetadataCandidates(await searchTmdbCandidates(tmdbSettings, item, query))
    } catch (error) {
      setPlayIntent(error instanceof Error ? error.message : 'TMDB search failed')
    } finally {
      setIsSearchingMetadata(false)
    }
  }

  async function applyMetadataCandidate(candidate: TmdbMatchCandidate): Promise<void> {
    if (!metadataMatchItem) return
    const candidateKey = `${candidate.type}:${candidate.id}`
    const taskId = `metadata:${metadataMatchItem.id}`
    setApplyingMetadataCandidateId(candidateKey)
    startTask(taskId, 'metadata', `${language === 'zh' ? '确认匹配' : 'Confirm match'} · ${metadataMatchItem.title}`, candidate.title, 1)
    try {
      const updatedItem = await scrapeTmdbCandidate(metadataMatchItem, tmdbSettings, candidate)
      if (await waitWhileTaskPaused(taskId)) return
      await applyUpdatedMediaItem(updatedItem)
      updateTask(taskId, { status: 'completed', completed: 1, detail: `TMDB ${candidate.id} · ${candidate.title}` })
      setMetadataMatchItem(undefined)
      setMetadataCandidates([])
      setPlayIntent(`${language === 'zh' ? '已识别为' : 'Matched as'}：${candidate.title}`)
      await cacheArtworkRows([updatedItem], `artwork:${updatedItem.id}`, `${language === 'zh' ? '图片下载' : 'Artwork'} · ${candidate.title}`)
    } catch (error) {
      const message = error instanceof Error ? error.message : 'TMDB match failed'
      updateTask(taskId, { status: 'failed', error: message })
      setPlayIntent(message)
    } finally {
      setApplyingMetadataCandidateId('')
    }
  }

  async function toggleMetadataLock(item: MediaItem): Promise<void> {
    const updatedItem = { ...item, metadataLocked: !item.metadataLocked }
    await applyUpdatedMediaItem(updatedItem)
    setPlayIntent(updatedItem.metadataLocked
      ? (language === 'zh' ? '元数据已锁定，扫描和自动刮削不会覆盖。' : 'Metadata locked and protected from automatic updates.')
      : (language === 'zh' ? '元数据已解锁。' : 'Metadata unlocked.'))
  }

  async function removeMissingMediaItem(item: MediaItem): Promise<void> {
    if (item.availability !== 'missing') return
    const source = sourceFor(item.sourceId)
    const remainingItems = allItems.filter((candidate) => candidate.sourceId === item.sourceId && candidate.id !== item.id)
    await client.upsertSourceItems({ ...source, itemCount: remainingItems.length }, remainingItems)
    setAllItems((current) => current.filter((candidate) => candidate.id !== item.id))
    setVisibleItems((current) => current.filter((candidate) => candidate.id !== item.id))
    setContinueItems((current) => current.filter((candidate) => candidate.id !== item.id))
    setDetailItemsById((current) => {
      const next = new Map(current)
      next.delete(item.id)
      return next
    })
    if (selectedId === item.id) {
      setSelectedId('')
      setIsDetailOpen(false)
    }
  }

  function rescanLocalFolder(source: LibrarySource): void {
    if (source.kind === 'Emby' || source.kind === 'WebDAV') return
    const folderPath = source.location.trim()
    if (!folderPath) {
      setConnectTone('error')
      setConnectMessage('当前文件系统源没有可扫描的文件夹路径。')
      return
    }
    if (!window.chrome?.webview) {
      setConnectTone('error')
      setConnectMessage('请在 Anvil Player 桌面应用中重新扫描本地文件夹。')
      return
    }

    ignoredLocalFolderScanSourceIdsRef.current.delete(source.id)
    setScanningLocalFolderSourceIds((current) => new Set(current).add(source.id))
    beginSourceScan(source, 1, source.itemCount)
    setConnectTone('idle')
    setConnectMessage(`正在后台重新扫描 ${source.name}...`)
    const credentials = source.kind === 'SMB' ? loadSmbCredentials(source.id) : undefined
    postNativeCommand({
      type: 'command',
      command: 'scanLocalFolder',
      path: folderPath,
      username: credentials?.username ?? '',
      password: credentials?.password ?? ''
    })
  }

  async function saveAndScanSmbSource(): Promise<void> {
    const selectedPaths = selectedPathRows(smbSelectedPaths)
    if (!selectedPaths.length) {
      setConnectTone('error')
      setConnectMessage('请先在 SMB 文件夹列表中勾选至少一个文件夹。')
      return
    }
    if (!window.chrome?.webview) {
      setConnectTone('error')
      setConnectMessage('请在 Anvil Player 桌面应用中扫描 SMB。')
      return
    }

    const host = normalizeSmbHost(smbHost || selectedPaths[0])
    if (!host) {
      setConnectTone('error')
      setConnectMessage('请填写 SMB 主机或 IP。')
      return
    }

    setIsConnecting(true)
    try {
      const existingRows = await client.listAllItems()
      const sourcesToScan = selectedPaths.map((path, index) => {
        const sourceName = selectedPaths.length === 1 && fileServiceName.trim()
          ? fileServiceName.trim()
          : directoryDisplayName(path, 'SMB')
        const source = buildLocalFolderSource({ name: sourceName, path: normalizeSmbPath(path) }, 0, 'SMB')
        return editingSource?.kind === 'SMB' && index === 0 ? { ...source, id: editingSource.id } : source
      })

      for (const source of sourcesToScan) {
        const sourceItems = existingRows.filter((item) => item.sourceId === source.id)
        await client.upsertSourceItems({ ...source, itemCount: sourceItems.length }, sourceItems)
        saveSmbCredentials({ sourceId: source.id, host, username: smbUsername, password: smbPassword })
        fileSystemSourceByLocationRef.current.set(locationKey(source.location), source)
        ignoredLocalFolderScanSourceIdsRef.current.delete(source.id)
        beginSourceScan(source, 1, sourceItems.length)
      }

      setScanningLocalFolderSourceIds((current) => {
        const next = new Set(current)
        sourcesToScan.forEach((source) => next.add(source.id))
        return next
      })

      await activatePendingFileServiceSource(sourcesToScan[0])
      setConnectTone('idle')
      setConnectMessage(sourcesToScan.length > 1
        ? `已添加 ${sourcesToScan.length} 个 SMB 文件夹，正在后台扫描...`
        : `已保存 ${sourcesToScan[0].name}，正在后台扫描 SMB...`)

      sourcesToScan.forEach((source) => {
        postNativeCommand({
          type: 'command',
          command: 'scanLocalFolder',
          path: source.location,
          username: smbUsername,
          password: smbPassword
        })
      })
    } catch (error) {
      setConnectTone('error')
      setConnectMessage(error instanceof Error ? error.message : '保存 SMB 源失败')
    } finally {
      setIsConnecting(false)
    }
  }

  async function saveAndScanWebDavSource(): Promise<void> {
    let rootUrl = ''
    try {
      rootUrl = normalizeWebDavSourceUrl(webDavUrl)
    } catch {
      setConnectTone('error')
      setConnectMessage('请填写有效的 WebDAV 地址。')
      return
    }

    const selectedPaths = selectedPathRows(webDavSelectedPaths)
    if (!selectedPaths.length) {
      setConnectTone('error')
      setConnectMessage('请先在 WebDAV 文件夹列表中勾选至少一个文件夹。')
      return
    }

    setIsConnecting(true)
    try {
      const existingRows = await client.listAllItems()
      const normalizedSelectedPaths = uniqueLocationRows(selectedPaths.map((path) => normalizeWebDavSourceUrl(path || rootUrl)))
      const sourceName = fileServiceName.trim() || directoryDisplayName(rootUrl, 'WebDAV')
      const baseSource = buildLocalFolderSource({ name: sourceName, path: rootUrl }, 0, 'WebDAV')
      const source: LibrarySource = {
        ...baseSource,
        ...(editingSource?.kind === 'WebDAV' ? { id: editingSource.id } : {}),
        name: sourceName,
        location: rootUrl,
        rootLocation: rootUrl,
        folders: normalizedSelectedPaths
      }

      const sourceItems = existingRows.filter((item) => item.sourceId === source.id)
      await client.upsertSourceItems({ ...source, itemCount: sourceItems.length }, sourceItems)
      saveWebDavCredentials({
        sourceId: source.id,
        username: webDavUsername,
        password: webDavPassword,
        baseUrl: rootUrl,
        selectedPaths: normalizedSelectedPaths
      })
      scanLocationsForSource(source).forEach((location) => {
        fileSystemSourceByLocationRef.current.set(locationKey(location), source)
      })
      ignoredLocalFolderScanSourceIdsRef.current.delete(source.id)
      pendingFolderScanCountsRef.current.set(source.id, normalizedSelectedPaths.length)
      beginSourceScan(source, normalizedSelectedPaths.length, sourceItems.length, normalizedSelectedPaths[0] ?? source.location)
      debugLibraryPlayback(`webdav scan queued source=${source.id} folders=${normalizedSelectedPaths.length}`)

      setScanningLocalFolderSourceIds((current) => {
        const next = new Set(current)
        next.add(source.id)
        return next
      })
      setConnectTone('idle')
      setConnectMessage(normalizedSelectedPaths.length > 1
        ? `正在扫描 ${normalizedSelectedPaths.length} 个 WebDAV 文件夹...`
        : `正在扫描 ${source.name}...`)

      await activatePendingFileServiceSource(source)

      normalizedSelectedPaths.forEach((path) => {
        debugLibraryPlayback(`webdav scan request source=${source.id} url=${path}`)
        postNativeCommand({
          type: 'command',
          command: 'scanWebDavFolder',
          url: path,
          name: directoryDisplayName(path, source.name),
          username: webDavUsername,
          password: webDavPassword
        })
      })
    } catch (error) {
      setConnectTone('error')
      setConnectMessage(error instanceof Error ? error.message : '扫描 WebDAV 失败')
    } finally {
      setIsConnecting(false)
    }
  }

  function openSourceEditor(sourceId: string): void {
    const source = sourceMap.get(sourceId)
    if (!source) return

    if (source.kind !== 'Emby') {
      if (source.kind === 'SMB') {
        openSmbSetup(source)
        setActiveNav(`source:${sourceId}` as NavKey)
        setActiveView('home')
        setMediaFilter('all')
        setSortKey('recent')
        setSortOrder('descending')
        setQuery('')
        setDebouncedQuery('')
        return
      }
      if (source.kind === 'WebDAV') {
        openWebDavSetup(source)
        setActiveNav(`source:${sourceId}` as NavKey)
        setActiveView('home')
        setMediaFilter('all')
        setSortKey('recent')
        setSortOrder('descending')
        setQuery('')
        setDebouncedQuery('')
        return
      }
      setSourceSetupMode('localFolder')
      setEditingSourceId(sourceId)
      setActiveNav(`source:${sourceId}` as NavKey)
      setActiveView('home')
      setActiveLibraryViewId('')
      setMediaFilter('all')
      setSortKey('recent')
      setSortOrder('descending')
      setPersonSearch(undefined)
      setQuery('')
      setDebouncedQuery('')
      setSelectedId('')
      setIsDetailOpen(false)
      setPlayIntent('')
      setIsPickingLocalFolder(false)
      setConnectTone('idle')
      setConnectMessage('')
      return
    }

    const saved = savedConnections.find((connection) => connection.sourceId === sourceId)
    const session = embySessionsBySourceId.get(sourceId) ?? saved?.session
    const parsed = parseServerAddress(saved?.serverUrl || source.location, 'http')

    setSourceSetupMode('emby')
    setEditingSourceId(sourceId)
    setActiveNav(`source:${sourceId}` as NavKey)
    setActiveView('home')
    setActiveLibraryViewId('')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setDebouncedQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setConnectionName(saved?.name || source.name)
    setServerProtocol(parsed?.protocol ?? 'http')
    setServerHost(parsed?.host ?? '')
    setServerPort(parsed?.port ?? '')
    setServerPath(parsed?.path ?? '')
    setUsername(saved?.username || session?.userName || '')
    setPassword('')
    setIgnoreCertificateErrors(saved?.ignoreCertificateErrors ?? ignoreCertificateErrors)
    setIsPickingLocalFolder(false)
    setConnectTone('idle')
    setConnectMessage('')
  }

  function closeEmbySetup(): void {
    if (editingSourceId) {
      selectNavigation(`source:${editingSourceId}` as NavKey)
      return
    }
    setSourceSetupMode('select')
  }

  function selectNavigation(navKey: NavKey): void {
    setSourceSetupMode('hidden')
    setEditingSourceId('')
    setActiveNav(navKey)
    setActiveView('home')
    setActiveLibraryViewId('')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setDebouncedQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function selectMediaItem(itemId: string): void {
    setSelectedId(itemId)
    setIsDetailOpen(true)
    setPlayIntent('')
  }

  function selectRelatedMediaItem(item: MediaItem): void {
    setDetailItemsById((current) => {
      const next = new Map(current)
      next.set(item.id, item)
      return next
    })
    setSelectedId(item.id)
    setIsDetailOpen(true)
    setPlayIntent('')
  }

  function searchCastPerson(person: PersonCredit, sourceId: string): void {
    const name = person.name.trim()
    if (!name) return
    const source = sourceMap.get(sourceId)
    setSourceSetupMode('hidden')
    setEditingSourceId('')
    if (source) {
      setActiveNav(`source:${source.id}` as NavKey)
    }
    setActiveView('home')
    setActiveLibraryViewId('')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(source?.kind === 'Emby' ? { sourceId, personId: person.id, name } : undefined)
    setQuery(name)
    setDebouncedQuery(name)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function openLibraryView(viewId: string): void {
    setActiveLibraryViewId(viewId)
    setActiveView('home')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function openContinueListing(): void {
    setActiveView('home')
    setMediaFilter('inProgress')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function resetCurrentListing(): void {
    if (activeLibraryViewId) {
      setActiveLibraryViewId('')
    }
    setActiveView('home')
    setMediaFilter('all')
    setSortKey('recent')
    setSortOrder('descending')
    setPersonSearch(undefined)
    setQuery('')
    setDebouncedQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function changeActiveView(view: LibraryView): void {
    setActiveView(view)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function changeListingPage(page: number): void {
    const nextPage = Math.max(1, Math.min(page, listingPageCount))
    if (nextPage === effectiveListingPage) return
    setListingPage(nextPage)
    window.requestAnimationFrame(() => {
      listingSectionRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' })
    })
  }

  function changeSortKey(sort: SortKey): void {
    if (sort === sortKey) {
      setSortOrder((current) => current === 'descending' ? 'ascending' : 'descending')
    } else {
      setSortKey(sort)
      setSortOrder('descending')
    }
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function applyServerAddress(value: string): void {
    const parsed = parseServerAddress(value, serverProtocol)
    if (!parsed) return
    setServerProtocol(parsed.protocol)
    setServerHost(parsed.host)
    setServerPort(parsed.port)
    setServerPath(parsed.path)
  }

  function updateServerHost(value: string): void {
    if (/^https?:\/\//i.test(value.trim()) || value.includes('/')) {
      const parsed = parseServerAddress(value, serverProtocol)
      if (parsed) {
        setServerProtocol(parsed.protocol)
        setServerHost(parsed.host)
        setServerPort(parsed.port)
        setServerPath(parsed.path)
        return
      }
    }
    setServerHost(value)
  }

  async function connectEmbySource(): Promise<void> {
    if (!canConnectEmby) return

    setIsConnecting(true)
    setConnectTone('idle')
    setConnectMessage('正在连接 Emby...')

    try {
      postNativeCommand({
        type: 'command',
        command: 'setAllowInsecureCertificates',
        enabled: ignoreCertificateErrors
      })
      const snapshot = password
        ? await loadEmbyLibrary({
            serverUrl,
            username,
            password,
            displayName: connectionName
          })
        : await refreshEmbyLibrary({
            ...(editingSession as EmbySession),
            apiBaseUrl: serverUrl,
            userName: username.trim() || editingSession?.userName || ''
          }, connectionName)
      await client.upsertSourceItems(snapshot.source, snapshot.items, snapshot.homeSections)
      saveSavedEmbyConnection({
        sourceId: snapshot.source.id,
        name: snapshot.source.name,
        serverUrl: snapshot.session.apiBaseUrl,
        username: snapshot.session.userName || username.trim(),
        ignoreCertificateErrors,
        session: snapshot.session,
        savedAt: Date.now()
      })

      const sourceNav = `source:${snapshot.source.id}` as NavKey
      const [sourceRows, allRows, continueRows, visibleRows, homeRows] = await Promise.all([
        client.listSources(),
        client.listAllItems(),
        client.getContinueWatching(),
        client.listItems({ navKey: sourceNav, view: 'home', search: '', sortKey: 'recent' }),
        client.listHomeSections()
      ])

      setSources(sourceRows)
      setAllItems(allRows)
      setContinueItems(continueRows)
      setVisibleItems(visibleRows)
      setHomeSections(homeRows)
      setActiveNav(sourceNav)
      setActiveView('home')
      setActiveLibraryViewId('')
      setMediaFilter('all')
      setSortKey('recent')
      setSortOrder('descending')
      setPersonSearch(undefined)
      setQuery('')
      setSelectedId(visibleRows[0]?.id ?? '')
      setIsDetailOpen(false)
      setConnectionName(snapshot.source.name)
      setEmbySessionsBySourceId((current) => {
        const next = new Map(current)
        next.set(snapshot.source.id, snapshot.session)
        return next
      })
      applyServerAddress(snapshot.session.apiBaseUrl)
      setPassword('')
      setSourceSetupMode('hidden')
      setEditingSourceId('')
      setConnectTone('success')
      setConnectMessage(`已连接 ${snapshot.source.name}，加载 ${visibleRows.length} / ${snapshot.totalRecordCount} 条`)
    } catch (error) {
      setConnectTone('error')
      setConnectMessage(errorText(error))
    } finally {
      setIsConnecting(false)
    }
  }

  function renderSourceRows(sourceRows: LibrarySource[]): JSX.Element[] {
    return sourceRows.map((source) => {
      const key = `source:${source.id}` as NavKey
      const scanProgress = source.kind !== 'Emby' ? scanProgressBySourceId.get(source.id) : undefined
      const scanning = Boolean(scanProgress?.active || (source.kind !== 'Emby' && scanningLocalFolderSourceIds.has(source.id)))
      const sourceLabel = scanProgress
        ? `${source.name} · ${scanProgress.active ? '扫描中' : '已扫描'} · ${scanProgress.foundItems}`
        : source.name
      return (
        <div className="library-source-nav-row" key={source.id}>
          <SidebarButton
            item={{ key, label: scanning || scanProgress ? sourceLabel : source.name, icon: sourceIcon(source.kind) }}
            activeNav={activeNav}
            suppressActive={isSourceSetup && editingSourceId !== source.id}
            onSelect={selectNavigation}
          />
          <button
            className={`library-source-edit-button ${editingSourceId === source.id ? 'is-active' : ''}`}
            type="button"
            title="编辑媒体源"
            aria-label={`编辑 ${source.name}`}
            onPointerDown={(event) => event.stopPropagation()}
            onClick={(event) => {
              event.stopPropagation()
              openSourceEditor(source.id)
            }}
          >
            <MoreHorizontal size={15} />
          </button>
        </div>
      )
    })
  }

  return (
    <div className="library-window">
      {customTitleBarEnabled ? <header
        className="library-window-titlebar"
        onPointerDown={(event) => {
          if (event.button === 0 && event.target === event.currentTarget) {
            postNativeCommand({ type: 'command', command: 'beginWindowDrag' })
          }
        }}
        onDoubleClick={(event) => {
          if (event.target === event.currentTarget) {
            postNativeCommand({ type: 'command', command: 'toggleMaximizeWindow' })
          }
        }}
      >
        <span>Anvil Player</span>
        <div className="library-window-controls">
          <button type="button" aria-label="最小化" onClick={() => { postNativeCommand({ type: 'command', command: 'minimizeWindow' }) }}>
            <i className="is-minimize" />
          </button>
          <button type="button" aria-label="最大化或还原" onClick={() => { postNativeCommand({ type: 'command', command: 'toggleMaximizeWindow' }) }}>
            <i className="is-maximize" />
          </button>
          <button className="is-close" type="button" aria-label="关闭" onClick={() => { postNativeCommand({ type: 'command', command: 'closeWindow' }) }}>
            <X size={15} />
          </button>
        </div>
      </header> : null}

      <div className={`library-shell ${useMainOnlyLayout ? 'is-library-home' : ''}`}>
      <aside className="library-sidebar">
        <div className="library-brand">
          <img src="./app-icon.png" alt="" aria-hidden="true" draggable={false} />
          <div>
            <strong>Anvil Library</strong>
            <span>Media Manager</span>
          </div>
        </div>

        <nav className="library-nav">
          <div className="library-nav-group">
            <div className="library-nav-heading">
              <span>{language === 'zh' ? '媒体库' : 'Library'}</span>
            </div>
            {localizedPrimaryNav.map((item) => (
              <SidebarButton
                key={item.key}
                item={item}
                activeNav={activeNav}
                suppressActive={isSourceSetup}
                onSelect={selectNavigation}
              />
            ))}
          </div>

          <div className="library-nav-group">
            <div className="library-nav-heading">
              <span>{language === 'zh' ? '智能分类' : 'Smart collections'}</span>
            </div>
            {localizedSmartNav.map((item) => (
              <SidebarButton
                key={item.key}
                item={item}
                activeNav={activeNav}
                suppressActive={isSourceSetup}
                onSelect={selectNavigation}
              />
            ))}
          </div>

          <div className="library-nav-group">
            <div className="library-nav-heading">
              <span>{language === 'zh' ? '媒体源' : 'Sources'}</span>
              <button type="button" title={language === 'zh' ? '添加媒体源' : 'Add source'} onClick={openSourcePicker}>
                <Plus size={13} />
              </button>
            </div>
            <div className="library-nav-subheading">{language === 'zh' ? '文件系统' : 'File system'}</div>
            {fileSystemSources.length ? renderSourceRows(fileSystemSources) : (
              <span className="library-nav-empty">{language === 'zh' ? '尚未添加本地文件夹' : 'No local folders'}</span>
            )}
            <div className="library-nav-subheading">{language === 'zh' ? '媒体库服务' : 'Media services'}</div>
            {mediaServiceSources.length ? renderSourceRows(mediaServiceSources) : (
              <span className="library-nav-empty">{language === 'zh' ? '尚未连接 Emby' : 'Emby not connected'}</span>
            )}
          </div>
        </nav>

        <div className="library-sidebar-footer">
          <button
            className={`library-settings-button ${sourceSetupMode === 'settings' ? 'is-active' : ''}`}
            type="button"
            onClick={openSettings}
          >
            <Settings2 size={15} />
            <span>{settingsLabels.settings}</span>
          </button>
          <button className="library-back-button" type="button" onClick={() => { postNativeCommand({ type: 'command', command: 'focusPlayer' }) }}>
          <Play size={15} />
          <span>{language === 'zh' ? '播放器模块' : 'Player'}</span>
          </button>
        </div>
      </aside>

      <main className="library-main">
        <section className={`library-toolbar ${!isSourceSetup && !isActiveEmby ? 'has-management-tools' : ''}`}>
          <div className="library-title-block">
            <span>{pageEyebrow}</span>
            <h1>{pageTitle}</h1>
          </div>
          {!isSourceSetup ? (
            <>
              <div className="library-toolbar-primary-actions">
                <label className="library-search">
                  <Search size={16} />
                  <input
                    value={query}
                    onChange={(event) => {
                      setPersonSearch(undefined)
                      setQuery(event.target.value)
                      setSelectedId('')
                      setIsDetailOpen(false)
                      setPlayIntent('')
                    }}
                    placeholder={language === 'zh' ? '搜索关键字' : 'Search library'}
                  />
                </label>
                {showSortControl ? (
                  <ToolbarSelect
                    value={sortKey}
                    options={sortOptions}
                    ariaLabel={language === 'zh' ? '排序' : 'Sort'}
                    statusIcon={sortOrder === 'descending'
                      ? <ArrowDownWideNarrow size={15} />
                      : <ArrowUpNarrowWide size={15} />}
                    onChange={changeSortKey}
                  />
                ) : null}
              </div>
              {!isActiveEmby ? (
                <div className="library-toolbar-secondary-actions">
                  <div className={`library-toolbar-popover-anchor ${mediaManagementOpen ? 'is-open' : ''}`}>
                    <button className="library-task-button" type="button" aria-expanded={mediaManagementOpen} onClick={() => { setMediaManagementOpen((open) => !open); setTaskCenterOpen(false) }}>
                      <Database size={16} />
                      <span>{language === 'zh' ? '媒体管理' : 'Manage'}</span>
                      {allItems.filter((item) => item.availability === 'missing' || (item.versions?.length ?? 0) > 0).length ? <b>{allItems.filter((item) => item.availability === 'missing' || (item.versions?.length ?? 0) > 0).length}</b> : null}
                    </button>
                    {mediaManagementOpen ? (
                      <MediaManagementDialog
                        language={language}
                        items={allItems}
                        sources={sources}
                        onClose={() => setMediaManagementOpen(false)}
                        onSelect={(item) => { setMediaManagementOpen(false); selectMediaItem(item.id) }}
                        onRescan={(source) => { setMediaManagementOpen(false); rescanFileSystemSource(source) }}
                        onRemoveMissing={(item) => { void removeMissingMediaItem(item) }}
                      />
                    ) : null}
                  </div>
                  <div className={`library-toolbar-popover-anchor ${taskCenterOpen ? 'is-open' : ''}`}>
                    <button className={`library-task-button ${backgroundTasks.some((task) => task.status === 'running' || task.status === 'paused') ? 'is-active' : ''}`} type="button" aria-expanded={taskCenterOpen} onClick={() => { setTaskCenterOpen((open) => !open); setMediaManagementOpen(false) }}>
                      <Activity size={16} />
                      <span>{language === 'zh' ? '后台任务' : 'Tasks'}</span>
                      {backgroundTasks.filter((task) => task.status === 'running' || task.status === 'paused').length ? <b>{backgroundTasks.filter((task) => task.status === 'running' || task.status === 'paused').length}</b> : null}
                    </button>
                    <TaskCenter
                      open={taskCenterOpen}
                      language={language}
                      tasks={backgroundTasks}
                      onClose={() => setTaskCenterOpen(false)}
                      onPause={pauseTask}
                      onResume={resumeTask}
                      onRetry={retryTask}
                      onCancel={cancelTask}
                      onClearFinished={() => setBackgroundTasks((current) => current.filter((task) => !['completed', 'failed', 'cancelled'].includes(task.status)))}
                    />
                  </div>
                </div>
              ) : null}
            </>
          ) : null}
        </section>

        {!isSourceSetup && activeSourceScanProgress ? (
          <section className={`library-scan-status ${activeSourceScanProgress.active ? 'is-active' : ''}`}>
            <span className="library-scan-status-icon">
              {sourceIcon(activeSourceScanProgress.kind, 18)}
            </span>
            <div>
              <strong>{activeSourceScanProgress.active ? '正在扫描媒体源' : '媒体源扫描完成'}</strong>
              <p>{scanProgressSummary(activeSourceScanProgress, activeSource?.itemCount ?? 0)}</p>
            </div>
          </section>
        ) : null}

        {sourceSetupMode === 'settings' ? (
          <LibrarySettingsPage
            language={language}
            displayMetadataPassthrough={displayMetadataPassthrough}
            displayPeakBrightnessNits={displayPeakBrightnessNits}
            autoDisplayFormat={autoDisplayFormat}
            dolbyVisionSystemPipelineExperimental={dolbyVisionSystemPipelineExperimental}
            windowsHdrEnabled={windowsHdrEnabled}
            refreshRateSyncEnabled={refreshRateSyncEnabled}
            refreshRateMaximumMultiple={refreshRateMaximumMultiple}
            tmdbSettings={tmdbSettings}
            tmdbStatus={tmdbStatus}
            isTestingTmdb={isTestingTmdb}
            trailerSettings={trailerSettings}
            onLanguageChange={changeInterfaceLanguage}
            onDisplayMetadataPassthroughChange={setDisplayMetadataPassthrough}
            onDisplayPeakBrightnessChange={setDisplayPeakBrightnessNits}
            onAutoDisplayFormatChange={setAutoDisplayFormat}
            onDolbyVisionSystemPipelineExperimentalChange={setDolbyVisionSystemPipelineExperimental}
            onRefreshRateSyncChange={setRefreshRateSyncEnabled}
            onRefreshRateMaximumMultipleChange={setRefreshRateMaximumMultiple}
            onTmdbSettingsChange={updateTmdbSettings}
            onTestTmdb={() => { void testTmdbSettingsConnection() }}
            onTrailerSettingsChange={setTrailerSettings}
          />
        ) : sourceSetupMode === 'select' ? (
          <SourceTypePicker
            onSelectEmby={openEmbySetup}
            onSelectLocalFolder={openLocalFolderSetup}
            onSelectSmb={() => openSmbSetup()}
            onSelectWebDav={() => openWebDavSetup()}
          />
        ) : sourceSetupMode === 'emby' ? (
          <section className="library-source-setup">
            <div className="library-source-setup-heading">
              <div>
                <span>Emby</span>
                <h2>连接服务器</h2>
              </div>
              <button className="library-secondary-action" type="button" onClick={closeEmbySetup}>
                <ArrowLeft size={15} />
                <span>返回</span>
              </button>
            </div>

            <section className="library-source-editor">
              <div className="library-source-fields">
                <label>
                  <span>名称</span>
                  <input value={connectionName} onChange={(event) => setConnectionName(event.target.value)} />
                </label>
                <label className="library-source-protocol-field">
                  <span>协议</span>
                  <select value={serverProtocol} onChange={(event) => setServerProtocol(event.target.value as ServerProtocol)}>
                    <option value="http">http</option>
                    <option value="https">https</option>
                  </select>
                </label>
                <label className="library-source-host-field">
                  <span>主机</span>
                  <input
                    value={serverHost}
                    onChange={(event) => updateServerHost(event.target.value)}
                    onBlur={(event) => applyServerAddress(event.target.value)}
                    placeholder="192.168.1.10"
                    autoComplete="url"
                  />
                </label>
                <label className="library-source-port-field">
                  <span>端口</span>
                  <input
                    value={serverPort}
                    onChange={(event) => setServerPort(event.target.value.replace(/\D+/g, ''))}
                    inputMode="numeric"
                    placeholder="8096"
                  />
                </label>
                <label className="library-source-path-field">
                  <span>路径</span>
                  <input value={serverPath} onChange={(event) => setServerPath(event.target.value)} placeholder="emby（可选）" />
                </label>
                <label>
                  <span>账号</span>
                  <input value={username} onChange={(event) => setUsername(event.target.value)} autoComplete="username" />
                </label>
                <label>
                  <span>密码</span>
                  <input
                    value={password}
                    type="password"
                    onChange={(event) => setPassword(event.target.value)}
                    autoComplete="current-password"
                  />
                </label>
              </div>
              <div className="library-source-editor-actions">
                <div className="library-source-editor-action-group">
                  <button
                    className={`library-certificate-toggle ${ignoreCertificateErrors ? 'is-active' : ''}`}
                    type="button"
                    aria-pressed={ignoreCertificateErrors}
                    onClick={() => setIgnoreCertificateErrors((enabled) => !enabled)}
                  >
                    <ShieldOff size={15} />
                    <span>跳过 HTTPS 证书校验</span>
                  </button>
                  {editingSourceId ? (
                    <button
                      className="library-danger-action"
                      type="button"
                      disabled={isConnecting}
                      onClick={() => { void deleteSource(editingSourceId) }}
                    >
                      <Trash2 size={15} />
                      <span>删除当前源</span>
                    </button>
                  ) : null}
                </div>
                <button
                  className="library-connect-action"
                  type="button"
                  disabled={isConnecting || !canConnectEmby}
                  onClick={() => void connectEmbySource()}
                >
                  <Server size={15} />
                  <span>{isConnecting ? '连接中' : '连接'}</span>
                </button>
              </div>
              {connectMessage ? (
                <div className={`library-source-status ${connectTone === 'success' ? 'is-success' : ''} ${connectTone === 'error' ? 'is-error' : ''}`}>
                  {connectMessage}
                </div>
              ) : null}
            </section>
          </section>
        ) : sourceSetupMode === 'smb' ? (
          <section className="library-source-setup">
            <div className="library-source-setup-heading">
              <div>
                <span>文件系统 · SMB</span>
                <h2>{editingSource?.kind === 'SMB' ? editingSource.name : '添加 SMB'}</h2>
              </div>
              <button className="library-secondary-action" type="button" onClick={closeLocalFolderSetup}>
                <ArrowLeft size={15} />
                <span>返回</span>
              </button>
            </div>

            <section className="library-source-editor">
              <div className="library-source-fields">
                <label>
                  <span>名称</span>
                  <input value={fileServiceName} onChange={(event) => setFileServiceName(event.target.value)} placeholder="NAS 影视" />
                </label>
                <label className="library-source-host-field">
                  <span>主机 / IP</span>
                  <input
                    value={smbHost}
                    onChange={(event) => {
                      setSmbHost(event.target.value)
                      setSmbCurrentPath('')
                      setSmbDirectories([])
                      setSmbSelectedPaths(new Set())
                    }}
                    placeholder="192.168.1.10"
                  />
                </label>
                <label>
                  <span>账号</span>
                  <input value={smbUsername} onChange={(event) => setSmbUsername(event.target.value)} autoComplete="username" />
                </label>
                <label>
                  <span>密码</span>
                  <input
                    value={smbPassword}
                    type="password"
                    onChange={(event) => setSmbPassword(event.target.value)}
                    autoComplete="current-password"
                  />
                </label>
              </div>
              <FileServiceDirectoryBrowser
                currentPath={smbCurrentPath}
                directories={smbDirectories}
                selectedPaths={smbSelectedRows}
                loading={isBrowsingSmb}
                emptyLabel="连接后会显示可选文件夹"
                parentPath={smbParent}
                onBrowse={(path) => browseSmbDirectory(path)}
                onToggle={(path) => toggleSelectedPath(setSmbSelectedPaths, path)}
              />
              <div className="library-source-editor-actions">
                <div className="library-source-editor-action-group">
                  <button
                    className="library-secondary-action"
                    type="button"
                    disabled={isBrowsingSmb || isConnecting || !smbHost.trim()}
                    onClick={() => browseSmbDirectory()}
                  >
                    <Search size={15} />
                    <span>{isBrowsingSmb ? '读取中' : '连接并浏览'}</span>
                  </button>
                  {editingSource?.kind === 'SMB' ? (
                    <>
                      <button
                        className="library-connect-action"
                        type="button"
                        disabled={editingFileServiceScanning || scrapingSourceMetadataId === editingSource.id || !editingFileServiceItems.length}
                        onClick={() => { void scrapeSourceMetadata(editingSource.id) }}
                      >
                        <Wand2 size={15} />
                        <span>{scrapingSourceMetadataId === editingSource.id ? '刮削中' : '一键刮削全部元数据'}</span>
                      </button>
                      <button
                        className="library-secondary-action"
                        type="button"
                        disabled={editingFileServiceScanning || Boolean(scrapingSourceMetadataId) || !editingFileServiceItems.length}
                        onClick={() => { void clearSourceMetadata(editingSource.id) }}
                      >
                        <Eraser size={15} />
                        <span>清空全部元数据</span>
                      </button>
                      <button
                        className="library-danger-action"
                        type="button"
                        disabled={isConnecting || editingFileServiceScanning}
                        onClick={() => { void deleteSource(editingSource.id) }}
                      >
                        <Trash2 size={15} />
                        <span>删除当前源</span>
                      </button>
                    </>
                  ) : null}
                </div>
                <button
                  className="library-connect-action"
                  type="button"
                  disabled={isConnecting || !smbSelectedRows.length || Boolean(editingSource && scanningLocalFolderSourceIds.has(editingSource.id))}
                  onClick={() => { void saveAndScanSmbSource() }}
                >
                  <Server size={15} />
                  <span>{isConnecting ? '保存中' : editingSource?.kind === 'SMB' ? '保存并扫描所选' : '添加所选文件夹'}</span>
                </button>
              </div>
              {connectMessage || editingFileServiceScanProgress ? (
                <div className={`library-source-status ${connectTone === 'success' ? 'is-success' : ''} ${connectTone === 'error' ? 'is-error' : ''}`}>
                  {connectMessage || scanProgressSummary(editingFileServiceScanProgress, editingFileServiceItems.length)}
                </div>
              ) : null}
            </section>
          </section>
        ) : sourceSetupMode === 'webdav' ? (
          <section className="library-source-setup">
            <div className="library-source-setup-heading">
              <div>
                <span>文件系统 · WebDAV</span>
                <h2>{editingSource?.kind === 'WebDAV' ? editingSource.name : '添加 WebDAV'}</h2>
              </div>
              <button className="library-secondary-action" type="button" onClick={closeLocalFolderSetup}>
                <ArrowLeft size={15} />
                <span>返回</span>
              </button>
            </div>

            <section className="library-source-editor">
              <div className="library-source-fields">
                <label>
                  <span>名称</span>
                  <input value={fileServiceName} onChange={(event) => setFileServiceName(event.target.value)} placeholder="WebDAV 影视" />
                </label>
                <label className="library-source-path-field">
                  <span>WebDAV 地址</span>
                  <input
                    value={webDavUrl}
                    onChange={(event) => {
                      setWebDavUrl(event.target.value)
                      setWebDavCurrentPath('')
                      setWebDavDirectories([])
                      setWebDavSelectedPaths(new Set())
                    }}
                    placeholder="https://example.com/dav/movies/"
                  />
                </label>
                <label>
                  <span>账号</span>
                  <input value={webDavUsername} onChange={(event) => setWebDavUsername(event.target.value)} autoComplete="username" />
                </label>
                <label>
                  <span>密码</span>
                  <input
                    value={webDavPassword}
                    type="password"
                    onChange={(event) => setWebDavPassword(event.target.value)}
                    autoComplete="current-password"
                  />
                </label>
              </div>
              <FileServiceDirectoryBrowser
                currentPath={webDavCurrentPath}
                directories={webDavDirectories}
                selectedPaths={webDavSelectedRows}
                loading={isBrowsingWebDav}
                emptyLabel="连接后会显示可选文件夹"
                parentPath={webDavParent}
                onBrowse={(path) => browseWebDavDirectory(path)}
                onToggle={(path) => toggleSelectedPath(setWebDavSelectedPaths, path)}
              />
              <div className="library-source-editor-actions">
                <div className="library-source-editor-action-group">
                  <button
                    className="library-secondary-action"
                    type="button"
                    disabled={isBrowsingWebDav || isConnecting || !webDavUrl.trim()}
                    onClick={() => browseWebDavDirectory(webDavUrl)}
                  >
                    <Search size={15} />
                    <span>{isBrowsingWebDav ? '读取中' : '连接并浏览'}</span>
                  </button>
                  {editingSource?.kind === 'WebDAV' ? (
                    <>
                      <button
                        className="library-connect-action"
                        type="button"
                        disabled={editingFileServiceScanning || scrapingSourceMetadataId === editingSource.id || !editingFileServiceItems.length}
                        onClick={() => { void scrapeSourceMetadata(editingSource.id) }}
                      >
                        <Wand2 size={15} />
                        <span>{scrapingSourceMetadataId === editingSource.id ? '刮削中' : '一键刮削全部元数据'}</span>
                      </button>
                      <button
                        className="library-secondary-action"
                        type="button"
                        disabled={editingFileServiceScanning || Boolean(scrapingSourceMetadataId) || !editingFileServiceItems.length}
                        onClick={() => { void clearSourceMetadata(editingSource.id) }}
                      >
                        <Eraser size={15} />
                        <span>清空全部元数据</span>
                      </button>
                      <button
                        className="library-danger-action"
                        type="button"
                        disabled={isConnecting || editingFileServiceScanning}
                        onClick={() => { void deleteSource(editingSource.id) }}
                      >
                        <Trash2 size={15} />
                        <span>删除当前源</span>
                      </button>
                    </>
                  ) : null}
                </div>
                <button
                  className="library-connect-action"
                  type="button"
                  disabled={isConnecting || !webDavSelectedRows.length || Boolean(editingSource && scanningLocalFolderSourceIds.has(editingSource.id))}
                  onClick={() => { void saveAndScanWebDavSource() }}
                >
                  <Database size={15} />
                  <span>{isConnecting ? '扫描中' : editingSource?.kind === 'WebDAV' ? '保存并扫描所选' : '添加所选文件夹'}</span>
                </button>
              </div>
              {connectMessage || editingFileServiceScanProgress ? (
                <div className={`library-source-status ${connectTone === 'success' ? 'is-success' : ''} ${connectTone === 'error' ? 'is-error' : ''}`}>
                  {connectMessage || scanProgressSummary(editingFileServiceScanProgress, editingFileServiceItems.length)}
                </div>
              ) : null}
            </section>
          </section>
        ) : sourceSetupMode === 'localFolder' ? (
          <section className="library-source-setup">
            <div className="library-source-setup-heading">
              <div>
                <span>文件系统</span>
                <h2>{editingFileSystemSource ? editingFileSystemSource.name : '添加本地文件夹'}</h2>
              </div>
              <button className="library-secondary-action" type="button" onClick={closeLocalFolderSetup}>
                <ArrowLeft size={15} />
                <span>返回</span>
              </button>
            </div>

            <section className="library-source-editor">
              <div className="library-local-folder-panel">
                <span className="library-local-folder-icon">
                  <FolderOpen size={24} />
                </span>
                <div>
                  <strong>{editingFileSystemSource ? editingFileSystemSource.name : '选择一个包含媒体文件的文件夹'}</strong>
                  <p>
                    {editingFileSystemSource
                      ? `位置：${editingFileSystemSource.location || '未配置'} · ${scanProgressSummary(editingFileServiceScanProgress, editingFileSystemItems.length)}`
                      : '将递归扫描常见视频格式，并作为本地文件系统源加入媒体库。'}
                  </p>
                </div>
              </div>
              {editingFileSystemSource ? (
                <div className="library-local-folder-actions">
                  <button
                    className="library-connect-action"
                    type="button"
                    disabled={editingFileSystemScanning || scrapingSourceMetadataId === editingFileSystemSource.id || !editingFileSystemItems.length}
                    onClick={() => { void scrapeSourceMetadata(editingFileSystemSource.id) }}
                  >
                    <Wand2 size={15} />
                    <span>{scrapingSourceMetadataId === editingFileSystemSource.id ? '刮削中' : '一键刮削全部元数据'}</span>
                  </button>
                  <button
                    className="library-secondary-action"
                    type="button"
                    disabled={isPickingLocalFolder || editingFileSystemScanning || Boolean(scrapingSourceMetadataId)}
                    onClick={() => rescanLocalFolder(editingFileSystemSource)}
                  >
                    <FolderOpen size={15} />
                    <span>{isPickingLocalFolder ? '选择中' : editingFileSystemScanning ? '扫描中' : '重新扫描文件夹'}</span>
                  </button>
                  <button
                    className="library-secondary-action"
                    type="button"
                    disabled={editingFileSystemScanning || Boolean(scrapingSourceMetadataId) || !editingFileSystemItems.length}
                    onClick={() => { void clearSourceMetadata(editingFileSystemSource.id) }}
                  >
                    <Eraser size={15} />
                    <span>清空全部元数据</span>
                  </button>
                  <button
                    className="library-danger-action"
                    type="button"
                    disabled={isPickingLocalFolder || editingFileSystemScanning || Boolean(scrapingSourceMetadataId)}
                    onClick={() => { void deleteSource(editingFileSystemSource.id) }}
                  >
                    <Trash2 size={15} />
                    <span>删除当前源</span>
                  </button>
                </div>
              ) : (
                <button
                  className="library-connect-action"
                  type="button"
                  disabled={isPickingLocalFolder}
                  onClick={pickLocalFolder}
                >
                  <FolderOpen size={15} />
                  <span>{isPickingLocalFolder ? '选择中' : '选择文件夹'}</span>
                </button>
              )}
              {connectMessage ? (
                <div className={`library-source-status ${connectTone === 'success' ? 'is-success' : ''} ${connectTone === 'error' ? 'is-error' : ''}`}>
                  {connectMessage}
                </div>
              ) : null}
            </section>
          </section>
        ) : (
          <>
            {showLibraryViewTabs ? (
            <section className="library-view-tabs" aria-label={language === 'zh' ? '媒体类型' : 'Media type'}>
              {viewTabs.map((tab) => (
                <button
                  key={tab.key}
                  className={activeView === tab.key ? 'is-selected' : ''}
                  type="button"
                  onClick={() => changeActiveView(tab.key)}
                >
                  {tab.label}
                </button>
              ))}
            </section>
            ) : null}

            {showContinueSection ? (
            <section className="library-continue">
              <div className="library-section-heading">
                <span>{activeSource ? `${activeSource.name} · ${language === 'zh' ? '继续观看' : 'Continue watching'}` : (language === 'zh' ? '继续观看' : 'Continue watching')}</span>
                <button type="button" onClick={isActiveEmby ? openContinueListing : () => selectNavigation('continue')}>
                  <Clock3 size={13} />
                  <span>全部</span>
                </button>
              </div>
              {scopedContinueItems.length ? (
                <HorizontalScroller className="library-continue-row">
                  {scopedContinueItems.map((item) => (
                    <ContinueCard
                      key={item.id}
                      item={item}
                      selected={selectedItem?.id === item.id}
                      onSelect={selectMediaItem}
                    />
                  ))}
                </HorizontalScroller>
              ) : (
                <EmptyState icon={<Clock3 size={24} />} title="暂无继续观看" caption="当前没有来自真实媒体源的播放进度" />
              )}
            </section>
            ) : null}

            {showHomeSections ? (
              <LibraryHomeSections
                sections={scopedHomeSections}
                selectedId={selectedItem?.id ?? ''}
                selectedViewId={activeLibraryViewId}
                onSelectItem={selectMediaItem}
                onSelectView={openLibraryView}
              />
            ) : (
            <section className="library-grid-section" ref={listingSectionRef}>
              <div className="library-section-heading">
                <span>{listingTitle} · {visibleItems.length}</span>
                <button type="button" onClick={resetCurrentListing}>
                  <ListFilter size={13} />
                  <span>{activeLibraryViewId ? '全部媒体库' : '重置'}</span>
                </button>
              </div>
              {visibleItems.length ? (
                <>
                  {classificationGroups.length ? classificationGroups.map((group) => (
                    <section className="library-classification-group" key={group.label}>
                      <div className="library-section-heading"><span>{group.label}</span><small>{group.items.length}</small></div>
                      <div className="library-poster-grid">
                        {group.items.map((item) => (
                          <MediaPoster key={`${group.label}:${item.id}`} item={item} source={sourceFor(item.sourceId)} selected={selectedItem?.id === item.id} onSelect={selectMediaItem} />
                        ))}
                      </div>
                    </section>
                  )) : (
                    <div className="library-poster-grid">
                      {pagedVisibleItems.map((item) => (
                        <MediaPoster
                          key={item.id}
                          item={item}
                          source={sourceFor(item.sourceId)}
                          selected={selectedItem?.id === item.id}
                          onSelect={selectMediaItem}
                        />
                      ))}
                    </div>
                  )}
                  {!classificationGroups.length && listingPageCount > 1 ? (
                    <nav className="library-pagination" aria-label="媒体库分页">
                      <button
                        type="button"
                        disabled={effectiveListingPage === 1}
                        aria-label="上一页"
                        onClick={() => changeListingPage(effectiveListingPage - 1)}
                      >
                        <ChevronLeft size={15} />
                        <span>上一页</span>
                      </button>
                      <span className="library-pagination-status">
                        {listingRangeStart}-{listingRangeEnd} / {visibleItems.length}
                        <small>第 {effectiveListingPage} / {listingPageCount} 页</small>
                      </span>
                      <button
                        type="button"
                        disabled={effectiveListingPage === listingPageCount}
                        aria-label="下一页"
                        onClick={() => changeListingPage(effectiveListingPage + 1)}
                      >
                        <span>下一页</span>
                        <ChevronRight size={15} />
                      </button>
                    </nav>
                  ) : null}
                </>
              ) : (
                <EmptyState
                  icon={<Film size={24} />}
                  title="暂无媒体"
                  caption={activeSourceScanProgress
                    ? scanProgressSummary(activeSourceScanProgress, activeSource?.itemCount ?? 0)
                    : isActiveFileSystemScanning ? '正在后台扫描媒体文件，扫描完成后会自动出现。' : '当前没有来自真实媒体源的条目'}
                />
              )}
            </section>
            )}
          </>
        )}
      </main>

      {showDetailPanel && selectedDetailItem ? (
        <DetailPanel
          key={`${selectedDetailItem.id}:${trailerSettings.source}`}
          item={selectedDetailItem}
          source={sourceFor(selectedDetailItem.sourceId)}
          language={language}
          trailerSource={trailerSettings.source}
          trailerSoundEnabled={trailerSettings.soundEnabled}
          trailerAutoPlay={trailerSettings.autoPlayOnDetails}
          trailerAutoPlayReady={sourceFor(selectedDetailItem.sourceId).kind !== 'Emby' || selectedDetailLoaded}
          playIntent={playIntent}
          isResolvingPlayback={Boolean(resolvingPlayItemId)}
          canScrapeMetadata={sourceFor(selectedDetailItem.sourceId).kind !== 'Emby'}
          isScrapingMetadata={scrapingMetadataItemId === selectedDetailItem.id}
          canEditMetadata={sourceFor(selectedDetailItem.sourceId).kind !== 'Emby'}
          onSelectItem={selectRelatedMediaItem}
          onSearchPerson={(person) => searchCastPerson(person, selectedDetailItem.sourceId)}
          onClose={() => {
            setIsDetailOpen(false)
            setSelectedId('')
            setPlayIntent('')
          }}
          onPlayIntent={(item, audioTrackIndex, subtitleTrackIndex) => { void playMediaItem(item, audioTrackIndex, subtitleTrackIndex) }}
          onTrailerIntent={resolveMediaTrailer}
          onRemoveFromLibrary={(item) => { void removeMediaFromLibrary(item) }}
          onToggleFavorite={(item) => { void updateLibraryFlags(item, { favorite: !item.favorite }) }}
          playlists={playlists}
          onTogglePlaylist={(item, playlistId) => { void toggleItemPlaylist(item, playlistId) }}
          onCreatePlaylist={createPlaylistForItem}
          onScrapeIntent={(item) => { void scrapeMediaMetadata(item) }}
          onRematchIntent={(item) => { void scrapeMediaMetadata(item, true) }}
          onSearchMatch={(item) => { void searchMetadataMatches(item) }}
          onToggleMetadataLock={(item) => { void toggleMetadataLock(item) }}
          onEditMetadata={openMetadataEditor}
          onClearMetadata={setMetadataDeleteItem}
        />
      ) : !useMainOnlyLayout && !isSourceSetup ? (
        <EmptyDetail />
      ) : null}

      {newPlaylistItem ? (
        <div className="library-playlist-dialog-backdrop" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget) setNewPlaylistItem(undefined)
        }}>
          <form className="library-playlist-dialog" onSubmit={(event) => { event.preventDefault(); confirmCreatePlaylist() }}>
            <div className="library-playlist-dialog-icon"><ListPlus size={19} /></div>
            <div className="library-playlist-dialog-copy">
              <h2>{language === 'zh' ? '新建片单' : 'New playlist'}</h2>
              <p>{language === 'zh' ? `将《${newPlaylistItem.title}》加入新片单` : `Add ${newPlaylistItem.title} to a new playlist.`}</p>
            </div>
            <label>
              <span>{language === 'zh' ? '名称' : 'Name'}</span>
              <input autoFocus value={newPlaylistName} maxLength={40} onChange={(event) => setNewPlaylistName(event.target.value)} placeholder={language === 'zh' ? '周末电影' : 'Weekend movies'} />
            </label>
            <div className="library-playlist-dialog-actions">
              <button type="button" onClick={() => setNewPlaylistItem(undefined)}>{language === 'zh' ? '取消' : 'Cancel'}</button>
              <button className="is-primary" type="submit" disabled={!newPlaylistName.trim()}>{language === 'zh' ? '创建' : 'Create'}</button>
            </div>
          </form>
        </div>
      ) : null}

      {activeMetadataEditorItem ? (
        <MetadataEditorDialog
          item={activeMetadataEditorItem}
          saving={isSavingManualMetadata}
          onClose={closeMetadataEditor}
          onSave={(item) => { void saveManualMetadata(item) }}
        />
      ) : null}

      {metadataMatchItem ? (
        <MetadataMatchDialog
          item={metadataMatchItem}
          language={language}
          candidates={metadataCandidates}
          searching={isSearchingMetadata}
          applyingId={applyingMetadataCandidateId}
          onSearch={(searchQuery) => { void searchMetadataMatches(metadataMatchItem, searchQuery) }}
          onApply={(candidate) => { void applyMetadataCandidate(candidate) }}
          onClose={() => { setMetadataMatchItem(undefined); setMetadataCandidates([]) }}
        />
      ) : null}

      {metadataDeleteItem ? (
        <MetadataDeleteDialog
          item={metadataDeleteItem}
          language={language}
          onClose={() => setMetadataDeleteItem(undefined)}
          onDeleteOnly={() => {
            const item = metadataDeleteItem
            setMetadataDeleteItem(undefined)
            void clearMediaMetadata(item)
          }}
          onDeleteAndDissolve={() => {
            const item = metadataDeleteItem
            setMetadataDeleteItem(undefined)
            void dissolveAndClearMediaCollection(item)
          }}
        />
      ) : null}

      </div>
    </div>
  )
}
