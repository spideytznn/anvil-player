import { useEffect, useId, useLayoutEffect, useMemo, useRef, useState, type CSSProperties, type Dispatch, type FormEvent, type ReactNode, type SetStateAction } from 'react'
import {
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
  ListFilter,
  MoreHorizontal,
  Play,
  Plus,
  Search,
  Server,
  Settings2,
  ShieldOff,
  Star,
  Trash2,
  Tv,
  Wand2,
  X
} from 'lucide-react'
import { applyAppearanceSettings } from './appearance'
import { loadSavedEmbyConnections, removeSavedEmbyConnection, saveSavedEmbyConnection, type SavedEmbyConnection } from './manager/connectionStorage'
import { createEmptyLibraryClient } from './manager/mediaLibraryClient'
import {
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
  loadTmdbSettings,
  saveTmdbSettings,
  scrapeTmdbItem,
  testTmdbConnection,
  tmdbApiBaseUrl,
  type TmdbAuthMode,
  type TmdbNetworkMode,
  type TmdbSettings
} from './manager/tmdbClient'
import {
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
  type UiLanguage
} from './uiSettings'
import './library.css'

interface NavItem {
  key: NavKey
  label: string
  icon: ReactNode
}

type SourceSetupMode = 'hidden' | 'select' | 'emby' | 'localFolder' | 'smb' | 'webdav' | 'settings'
type ServerProtocol = 'http' | 'https'

interface MetadataEditForm {
  title: string
  originalTitle: string
  type: MediaItem['type']
  year: string
  rating: string
  runtime: string
  genres: string
  country: string
  quality: string
  poster: string
  backdrop: string
  tagline: string
  overview: string
  tmdbId: string
  imdbId: string
  tvdbId: string
}

interface FileServiceDirectory {
  name: string
  path: string
}

interface ServerAddressParts {
  protocol: ServerProtocol
  host: string
  port: string
  path: string
}

function parseServerAddress(value: string, fallbackProtocol: ServerProtocol): ServerAddressParts | undefined {
  const trimmed = value.trim()
  if (!trimmed) return undefined

  try {
    const url = new URL(/^https?:\/\//i.test(trimmed) ? trimmed : `${fallbackProtocol}://${trimmed}`)
    const protocol = url.protocol.toLowerCase() === 'https:' ? 'https' : 'http'
    return {
      protocol,
      host: url.hostname,
      port: url.port,
      path: url.pathname === '/' ? '' : url.pathname.replace(/^\/+|\/+$/g, '')
    }
  } catch {
    return undefined
  }
}

function buildServerAddress(protocol: ServerProtocol, host: string, port: string, path: string): string {
  const cleanHost = host.trim()
  const cleanPort = port.trim()
  const cleanPath = path.trim().replace(/^\/+|\/+$/g, '')
  return `${protocol}://${cleanHost}${cleanPort ? `:${cleanPort}` : ''}${cleanPath ? `/${cleanPath}` : ''}`
}

const primaryNav: NavItem[] = [
  { key: 'continue', label: '继续观看', icon: <Clock3 size={16} /> },
  { key: 'recent', label: '最近添加', icon: <Grid3X3 size={16} /> },
  { key: 'movies', label: '电影', icon: <Film size={16} /> },
  { key: 'series', label: '剧集', icon: <Tv size={16} /> }
]

const smartNav: NavItem[] = [
  { key: 'unwatched', label: '未看', icon: <Clock3 size={16} /> },
  { key: 'watched', label: '已看', icon: <Check size={16} /> },
  { key: 'favorites', label: '收藏', icon: <Heart size={16} /> },
  { key: 'playlist', label: '片单', icon: <Wand2 size={16} /> },
  { key: 'genre', label: '类型', icon: <ListFilter size={16} /> },
  { key: 'rating', label: '评分', icon: <Star size={16} /> },
  { key: 'release', label: '发行年份', icon: <Database size={16} /> }
]

const viewTabs: Array<{ key: LibraryView; label: string }> = [
  { key: 'home', label: '全部' },
  { key: 'movies', label: '电影' },
  { key: 'series', label: '剧集' }
]

const sortOptions: Array<{ key: SortKey; label: string }> = [
  { key: 'recent', label: '最近添加' },
  { key: 'title', label: '标题' },
  { key: 'rating', label: '评分' },
  { key: 'year', label: '发行年份' }
]

function mediaTypeLabel(type: MediaItem['type']): string {
  switch (type) {
    case 'movie': return '电影'
    case 'series': return '剧集'
    default: return '文件夹'
  }
}

function sourceKindLabel(kind: SourceKind): string {
  switch (kind) {
    case 'Emby': return 'Emby'
    case 'WebDAV': return 'WebDAV'
    case 'SMB': return 'SMB'
    default: return '文件系统'
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

function navLabel(navKey: NavKey, sourceMap: Map<string, LibrarySource>): string {
  if (navKey.startsWith('source:')) {
    return sourceMap.get(navKey.slice('source:'.length))?.name ?? '媒体源'
  }
  return [...primaryNav, ...smartNav].find((item) => item.key === navKey)?.label ?? '媒体库'
}

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : '连接 Emby 失败'
}

function posterMark(item: MediaItem): string {
  if (item.type === 'folder') return '合集'
  return item.title.slice(0, 2)
}

function localFileName(path: string | undefined, fallback: string): string {
  if (!path) return fallback
  return path.split(/[\\/]/g).pop()?.replace(/\.[^.\\/]+$/g, '') || fallback
}

function clearLocalMetadata(item: MediaItem): MediaItem {
  const title = localFileName(item.path, item.originalTitle || item.title)
  return {
    ...item,
    title,
    originalTitle: title,
    year: 0,
    rating: 0,
    runtime: '未知',
    genres: ['未分类'],
    country: '本地',
    quality: item.quality || '本地文件',
    poster: '',
    backdrop: '',
    tagline: '等待刮削',
    overview: item.path ? `本地媒体文件：${item.path}` : '',
    cast: undefined,
    similarItems: undefined,
    studios: undefined,
    tags: undefined,
    externalIds: undefined,
    metadataProvider: undefined,
    metadataMatchedAt: undefined
  }
}

function imageBackgroundUrl(background: string): string | undefined {
  const match = background.trim().match(/^url\((["']?)(.*?)\1\)/)
  return match?.[2]
}

function metadataFormFromItem(item: MediaItem): MetadataEditForm {
  return {
    title: item.title,
    originalTitle: item.originalTitle,
    type: item.type,
    year: item.year ? String(item.year) : '',
    rating: item.rating ? String(item.rating) : '',
    runtime: item.runtime === '未知' ? '' : item.runtime,
    genres: item.genres.join(', '),
    country: item.country === '本地' ? '' : item.country,
    quality: item.quality,
    poster: imageBackgroundUrl(item.poster) ?? item.poster,
    backdrop: imageBackgroundUrl(item.backdrop) ?? item.backdrop,
    tagline: item.tagline === '等待刮削' ? '' : item.tagline,
    overview: item.overview,
    tmdbId: item.externalIds?.tmdb ?? '',
    imdbId: item.externalIds?.imdb ?? '',
    tvdbId: item.externalIds?.tvdb ?? ''
  }
}

function splitMetadataList(value: string): string[] {
  return value
    .split(/[,，/、]+/g)
    .map((entry) => entry.trim())
    .filter(Boolean)
}

function normalizeArtworkValue(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  if (/^url\(/i.test(trimmed)) return trimmed
  return `url("${trimmed.replace(/"/g, '%22')}")`
}

function normalizeSmbPath(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  if (/^smb:\/\//i.test(trimmed)) {
    const url = new URL(trimmed.replace(/^smb:/i, 'file:'))
    return `\\\\${url.hostname}${decodeURIComponent(url.pathname).replace(/\//g, '\\')}`
  }
  const slashNormalized = trimmed.replace(/\//g, '\\')
  if (slashNormalized.startsWith('\\\\')) return slashNormalized
  return `\\\\${slashNormalized.replace(/^\\+/g, '')}`
}

function normalizeSmbHost(value: string): string {
  const normalized = normalizeSmbPath(value)
  return normalized.split(/[\\/]/g).filter(Boolean)[0] ?? ''
}

function smbParentPath(path: string): string {
  const parts = normalizeSmbPath(path).split(/[\\/]/g).filter(Boolean)
  if (parts.length <= 1) return ''
  return `\\\\${parts.slice(0, -1).join('\\')}`
}

function webDavParentUrl(value: string): string {
  const url = new URL(normalizeWebDavSourceUrl(value))
  const parts = url.pathname.split('/').filter(Boolean)
  if (!parts.length) return ''
  parts.pop()
  url.pathname = parts.length ? `/${parts.map((part) => encodeURIComponent(decodeURIComponent(part))).join('/')}/` : '/'
  return normalizeWebDavSourceUrl(url.toString())
}

function directoryDisplayName(path: string, fallback: string): string {
  return fileServiceNameFromLocation(path, fallback)
}

function locationKey(value: string): string {
  return value.trim().replace(/[\\/]+$/g, '').toLowerCase()
}

function fileServiceNameFromLocation(location: string, fallback: string): string {
  const trimmed = location.trim().replace(/[\\/]+$/g, '')
  if (!trimmed) return fallback
  try {
    const url = new URL(trimmed)
    const parts = url.pathname.split('/').filter(Boolean)
    return decodeURIComponent(parts.at(-1) || url.hostname || fallback)
  } catch {
    return trimmed.split(/[\\/]/g).filter(Boolean).pop() || fallback
  }
}

function normalizeWebDavSourceUrl(value: string): string {
  const url = new URL(value.trim())
  url.username = ''
  url.password = ''
  url.hash = ''
  url.search = ''
  const parts = url.pathname
    .split('/')
    .filter(Boolean)
    .map((part) => decodeURIComponent(part))
  url.pathname = parts.length ? `/${parts.join('/')}/` : '/'
  return url.toString().replace(/\/+$/, '/')
}

function tryNormalizeWebDavSourceUrl(value: string): string {
  try {
    return normalizeWebDavSourceUrl(value)
  } catch {
    return value.trim()
  }
}

function uniqueLocationRows(rows: string[]): string[] {
  const result: string[] = []
  rows.forEach((row) => {
    const trimmed = row.trim()
    if (!trimmed) return
    if (!result.some((candidate) => locationKey(candidate) === locationKey(trimmed))) {
      result.push(trimmed)
    }
  })
  return result
}

function webDavBaseUrlForSource(source?: LibrarySource): string {
  if (!source) return ''
  const credentials = loadWebDavCredentials(source.id)
  return tryNormalizeWebDavSourceUrl(credentials?.baseUrl || source.rootLocation || source.location)
}

function webDavSelectedPathsForSource(source?: LibrarySource): string[] {
  if (!source) return []
  const credentials = loadWebDavCredentials(source.id)
  const rows = credentials?.selectedPaths?.length
    ? credentials.selectedPaths
    : source.folders?.length
      ? source.folders
      : source.location
        ? [source.location]
        : []
  return uniqueLocationRows(rows.map(tryNormalizeWebDavSourceUrl))
}

function scanLocationsForSource(source: LibrarySource): string[] {
  if (source.kind === 'WebDAV') {
    return uniqueLocationRows([
      source.location,
      ...(source.folders ?? []),
      ...(loadWebDavCredentials(source.id)?.selectedPaths ?? [])
    ])
  }
  return uniqueLocationRows([source.location])
}

function normalizedPrefixLocation(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  try {
    const url = new URL(trimmed)
    if (url.protocol === 'http:' || url.protocol === 'https:') {
      return normalizeWebDavSourceUrl(trimmed).toLowerCase()
    }
  } catch {
    // Fall through to path-style normalization.
  }
  return trimmed.replace(/[\\/]+$/g, '').toLowerCase()
}

function mediaItemBelongsToScanRoot(item: MediaItem, rootPath: string): boolean {
  const root = normalizedPrefixLocation(rootPath)
  if (!root) return false
  return [item.path, item.playbackPath].some((candidate) => {
    if (!candidate) return false
    const value = normalizedPrefixLocation(candidate)
    return value === root ||
      value.startsWith(root.endsWith('/') || root.endsWith('\\') ? root : `${root}/`) ||
      value.startsWith(root.endsWith('/') || root.endsWith('\\') ? root : `${root}\\`)
  })
}

function parseMetadataYear(value: string): number {
  const year = Number(value.trim())
  return Number.isFinite(year) && year > 0 ? Math.round(year) : 0
}

function parseMetadataRating(value: string): number {
  const rating = Number(value.trim())
  if (!Number.isFinite(rating)) return 0
  return Math.max(0, Math.min(10, Math.round(rating * 10) / 10))
}

function buildManualMetadataItem(item: MediaItem, form: MetadataEditForm): MediaItem {
  const genres = splitMetadataList(form.genres)
  const externalIds = {
    tmdb: form.tmdbId.trim() || undefined,
    imdb: form.imdbId.trim() || undefined,
    tvdb: form.tvdbId.trim() || undefined
  }
  const hasExternalIds = Boolean(externalIds.tmdb || externalIds.imdb || externalIds.tvdb)

  return {
    ...item,
    title: form.title.trim() || item.title,
    originalTitle: form.originalTitle.trim() || form.title.trim() || item.originalTitle,
    type: form.type,
    year: parseMetadataYear(form.year),
    rating: parseMetadataRating(form.rating),
    runtime: form.runtime.trim() || '未知',
    genres: genres.length ? genres : ['未分类'],
    country: form.country.trim() || '本地',
    quality: form.quality.trim() || '本地文件',
    poster: normalizeArtworkValue(form.poster),
    backdrop: normalizeArtworkValue(form.backdrop),
    tagline: form.tagline.trim() || '手动编辑',
    overview: form.overview.trim(),
    externalIds: hasExternalIds ? externalIds : undefined,
    metadataProvider: 'manual',
    metadataMatchedAt: Date.now()
  }
}

function mergeLocalScannedItem(existingItem: MediaItem | undefined, scannedItem: MediaItem): MediaItem {
  if (!existingItem) return scannedItem

  const preservedState: Pick<MediaItem, 'progress' | 'continueWatching' | 'watched' | 'favorite'> = {
    progress: existingItem.progress,
    continueWatching: existingItem.continueWatching,
    watched: existingItem.watched,
    favorite: existingItem.favorite
  }

  if (!existingItem.metadataProvider) {
    return {
      ...scannedItem,
      ...preservedState
    }
  }

  return {
    ...scannedItem,
    ...preservedState,
    title: existingItem.title,
    originalTitle: existingItem.originalTitle,
    type: existingItem.type,
    year: existingItem.year,
    rating: existingItem.rating,
    runtime: existingItem.runtime,
    genres: existingItem.genres,
    country: existingItem.country,
    quality: existingItem.quality,
    poster: existingItem.poster,
    backdrop: existingItem.backdrop,
    tagline: existingItem.tagline,
    overview: existingItem.overview,
    cast: existingItem.cast,
    similarItems: existingItem.similarItems,
    studios: existingItem.studios,
    tags: existingItem.tags,
    externalIds: existingItem.externalIds,
    metadataProvider: existingItem.metadataProvider,
    metadataMatchedAt: existingItem.metadataMatchedAt
  }
}

function hasImageBackground(background: string): boolean {
  return Boolean(imageBackgroundUrl(background))
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
      className={`library-poster-card ${props.selected ? 'is-selected' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.id)}
    >
      <MediaArtwork background={props.item.poster} className="library-poster-art">
        <div className="library-poster-shine" />
        <span className="library-poster-type">{mediaTypeLabel(props.item.type)}</span>
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

function DetailPanel(props: {
  item: MediaItem
  source: LibrarySource
  playIntent: string
  isResolvingPlayback: boolean
  canScrapeMetadata: boolean
  isScrapingMetadata: boolean
  canEditMetadata: boolean
  onPlayIntent: (item: MediaItem) => void
  onScrapeIntent: (item: MediaItem) => void
  onEditMetadata: (item: MediaItem) => void
  onClearMetadata: (item: MediaItem) => void
  onSelectItem: (item: MediaItem) => void
  onSearchPerson: (person: PersonCredit) => void
  onClose: () => void
}): JSX.Element {
  const backdropStyle: CSSProperties = { background: props.item.backdrop }
  const seasons = props.item.seasons ?? []
  const [activeSeasonId, setActiveSeasonId] = useState('')
  const [metadataMenuOpen, setMetadataMenuOpen] = useState(false)
  const activeSeason = seasons.find((season) => season.id === activeSeasonId) ?? seasons[0]
  const seasonOptions = seasons.map((season) => ({ key: season.id, label: season.index }))
  const activeSeasonSelectId = activeSeason?.id ?? seasonOptions[0]?.key ?? ''
  const episodeRows = activeSeason?.episodes ?? props.item.episodes ?? []
  const streamDetailValue = (type: 'video' | 'audio', label: string): string => {
    const stream = props.item.streamSpecs?.find((row) => row.type === type)
    return stream?.details.find((detail) => detail.label === label)?.value ?? ''
  }
  const compactCodec = (value: string): string => value.replace(/\./g, '').replace(/-/g, '')
  const compactResolution = (value: string): string => {
    const match = value.match(/x(\d{3,4})$/i)
    return match ? `${match[1]}p` : value
  }
  const videoBrief = cleanJoin([
    compactResolution(streamDetailValue('video', '分辨率') || props.item.quality),
    compactCodec(streamDetailValue('video', '编码'))
  ], ' ')
  const audioStream = props.item.streamSpecs?.find((row) => row.type === 'audio')
  const audioDefault = audioStream?.details.some((detail) => detail.label === '默认' && detail.value === '是')
  const audioBrief = cleanJoin([
    streamDetailValue('audio', '语言'),
    compactCodec(streamDetailValue('audio', '编码')),
    streamDetailValue('audio', '声道'),
    audioDefault ? '(默认)' : ''
  ], ' ')

  useEffect(() => {
    if (!seasons.length) {
      if (activeSeasonId) setActiveSeasonId('')
      return
    }
    if (!activeSeasonId || !seasons.some((season) => season.id === activeSeasonId)) {
      setActiveSeasonId(seasons[0].id)
    }
  }, [activeSeasonId, seasons])

  return (
    <aside className="library-detail">
      <div className="library-detail-backdrop">
        <div className="library-detail-backdrop-image" style={backdropStyle} />
        <button
          className="library-detail-close"
          type="button"
          title="关闭详情"
          onPointerDown={(event) => event.stopPropagation()}
          onClick={(event) => {
            event.stopPropagation()
            props.onClose()
          }}
        >
          <X size={15} />
        </button>
        <div className="library-detail-gradient" />
        <div className="library-detail-copy">
          <span className="library-source-pill">
            {sourceIcon(props.source.kind, 13)}
            {props.source.name}
          </span>
          <h2>{props.item.title}</h2>
          <p>{props.item.originalTitle}</p>
        </div>
      </div>

      <div className="library-detail-body">
        <div className="library-detail-actions">
          <button
            className="library-primary-action"
            type="button"
            disabled={props.isResolvingPlayback}
            onClick={() => props.onPlayIntent(props.item)}
          >
            <Play size={16} />
            <span>{props.item.progress > 0 && props.item.progress < 1 ? '继续播放' : '播放'}</span>
          </button>
          <button className={`library-icon-action ${props.item.favorite ? 'is-active' : ''}`} type="button" title="收藏">
            <Heart size={16} />
          </button>
          {props.canScrapeMetadata ? (
            <button
              className="library-icon-action"
              type="button"
              title="TMDB 刮削"
              disabled={props.isScrapingMetadata}
              onClick={() => props.onScrapeIntent(props.item)}
            >
              <Wand2 size={16} />
            </button>
          ) : null}
          {props.canEditMetadata ? (
            <div className="library-detail-menu">
              <button
                className={`library-icon-action ${metadataMenuOpen ? 'is-active' : ''}`}
                type="button"
                title="更多"
                aria-expanded={metadataMenuOpen}
                onClick={() => setMetadataMenuOpen((open) => !open)}
              >
                <MoreHorizontal size={16} />
              </button>
              {metadataMenuOpen ? (
                <div className="library-detail-menu-panel">
                  <button
                    type="button"
                    onClick={() => {
                      setMetadataMenuOpen(false)
                      props.onEditMetadata(props.item)
                    }}
                  >
                    修改元数据
                  </button>
                  <button
                    type="button"
                    onClick={() => {
                      setMetadataMenuOpen(false)
                      props.onClearMetadata(props.item)
                    }}
                  >
                    删除元数据
                  </button>
                </div>
              ) : null}
            </div>
          ) : (
            <button className="library-icon-action" type="button" title="更多">
              <MoreHorizontal size={16} />
            </button>
          )}
        </div>

        {props.playIntent ? <div className="library-intent">{props.playIntent}</div> : null}

        <div className="library-metadata-line">
          <span>
            <Star size={13} />
            {props.item.rating.toFixed(1)}
          </span>
          <span>{props.item.year}</span>
          <span>{props.item.runtime}</span>
          {!videoBrief && props.item.quality ? <span>{props.item.quality}</span> : null}
        </div>

        {(videoBrief || audioBrief) ? (
          <div className="library-media-brief">
            {videoBrief ? (
              <span><strong>视频</strong>{videoBrief}</span>
            ) : null}
            {audioBrief ? (
              <span><strong>音频</strong>{audioBrief}</span>
            ) : null}
          </div>
        ) : null}

        <div className="library-genre-row">
          {props.item.genres.map((genre) => <span key={genre}>{genre}</span>)}
        </div>

        <p className="library-tagline">{props.item.tagline}</p>
        <p className="library-overview">{props.item.overview}</p>

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
                  <span className="library-season-badge">{activeSeason?.index ?? 'S1'}</span>
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
                    props.onPlayIntent(episode.item)
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
}): JSX.Element {
  const t = settingsCopy[props.language]
  const languageOptions: Array<{ value: UiLanguage; label: string }> = [
    { value: 'zh', label: t.chinese },
    { value: 'en', label: t.english }
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

      <section className="library-settings-panel">
        <div className="library-settings-panel-title">
          <Languages size={16} />
          <span>{t.interfaceLanguage}</span>
        </div>
        <p>{t.languageCaption}</p>
        <div className="library-setting-options" role="group" aria-label={t.interfaceLanguage}>
          {languageOptions.map((option) => (
            <button
              key={option.value}
              className={option.value === props.language ? 'is-selected' : ''}
              type="button"
              onClick={() => props.onLanguageChange(option.value)}
            >
              <span>{option.label}</span>
            </button>
          ))}
        </div>
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
            <input
              value={props.tmdbSettings.language}
              onChange={(event) => updateTmdbSettings({ language: event.target.value })}
              placeholder="zh-CN"
            />
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
  const [tmdbSettings, setTmdbSettings] = useState<TmdbSettings>(() => loadTmdbSettings())
  const [tmdbStatus, setTmdbStatus] = useState('')
  const [isTestingTmdb, setIsTestingTmdb] = useState(false)
  const [sources, setSources] = useState<LibrarySource[]>([])
  const [allItems, setAllItems] = useState<MediaItem[]>([])
  const [visibleItems, setVisibleItems] = useState<MediaItem[]>([])
  const [detailItemsById, setDetailItemsById] = useState<Map<string, MediaItem>>(() => new Map())
  const [loadedDetailIds, setLoadedDetailIds] = useState<Set<string>>(() => new Set())
  const [continueItems, setContinueItems] = useState<MediaItem[]>([])
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
  const [isSavingManualMetadata, setIsSavingManualMetadata] = useState(false)
  const [isConnecting, setIsConnecting] = useState(false)
  const [isPickingLocalFolder, setIsPickingLocalFolder] = useState(false)
  const [connectTone, setConnectTone] = useState<'idle' | 'success' | 'error'>('idle')
  const [connectMessage, setConnectMessage] = useState('')
  const [editingSourceId, setEditingSourceId] = useState('')
  const ignoredLocalFolderScanSourceIdsRef = useRef<Set<string>>(new Set())
  const fileSystemSourceByLocationRef = useRef<Map<string, LibrarySource>>(new Map())
  const homeSectionRefreshSourceIdsRef = useRef<Set<string>>(new Set())
  const activeNavRef = useRef<NavKey>(initialActiveNav)
  const smbBrowseRequestIdRef = useRef('')
  const webDavBrowseRequestIdRef = useRef('')
  const localScanUpdateChainRef = useRef<Promise<void>>(Promise.resolve())
  const pendingFolderScanCountsRef = useRef<Map<string, number>>(new Map())

  useEffect(() => {
    applyAppearanceSettings()
  }, [])

  useEffect(() => {
    applyDocumentLanguage(language)
    saveUiLanguage(language)
  }, [language])

  useEffect(() => {
    saveTmdbSettings(tmdbSettings)
  }, [tmdbSettings])

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
      const [sourceRows, allRows, visibleRows, continueRows, homeRows] = await Promise.all([
        client.listSources(),
        client.listAllItems(),
        client.listItems({ navKey: initialActiveNav, view: 'home', search: '', sortKey: 'recent' }),
        client.getContinueWatching(),
        client.listHomeSections()
      ])
      if (cancelled) return
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
      const snapshot = buildLocalFolderLibrary(result, knownSource ? { source: knownSource } : undefined)
      if (!activate && ignoredLocalFolderScanSourceIdsRef.current.has(snapshot.source.id)) {
        return
      }
      const existingRows = await client.listAllItems()
      const existingSourceItems = existingRows.filter((item) => item.sourceId === snapshot.source.id)
      const existingById = new Map(existingSourceItems.map((item) => [item.id, item]))
      const scannedItems = snapshot.items.map((item) => mergeLocalScannedItem(existingById.get(item.id), item))
      const mergesPartialWebDavFolder = Boolean(
        knownSource?.kind === 'WebDAV' &&
        ((knownSource.folders?.length ?? 0) > 1 || (loadWebDavCredentials(knownSource.id)?.selectedPaths?.length ?? 0) > 1)
      )
      const mergedItems = mergesPartialWebDavFolder
        ? [
            ...existingSourceItems.filter((item) => !mediaItemBelongsToScanRoot(item, result.folder.path)),
            ...scannedItems
          ]
        : scannedItems

      await client.upsertSourceItems({ ...snapshot.source, itemCount: mergedItems.length }, mergedItems)
      const pendingScanCount = pendingFolderScanCountsRef.current.get(snapshot.source.id) ?? 0
      const stillScanningSource = pendingScanCount > 1
      if (stillScanningSource) {
        pendingFolderScanCountsRef.current.set(snapshot.source.id, pendingScanCount - 1)
      } else {
        pendingFolderScanCountsRef.current.delete(snapshot.source.id)
      }
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

    return subscribeNativeMessages((message) => {
      if (message.type === 'localFolderPicked') {
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
      }
    })
  }, [activeLibraryViewId, activeNav, activeView, client, debouncedQuery, mediaFilter, selectedId, sortKey, sortOrder, sources])

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
  }, [client, savedConnections])

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
  const selectedItem = isSourceSetup
    ? undefined
    : visibleItems.find((item) => item.id === selectedId)
      ?? allItems.find((item) => item.id === selectedId && (!activeSource || item.sourceId === activeSource.id))
      ?? detailItemsById.get(selectedId)
  const selectedDetailItem = selectedItem ? detailItemsById.get(selectedItem.id) ?? selectedItem : undefined
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
  const listingBaseTitle = activeLibraryCard?.title ?? (activeSource ? `${activeSource.name} 主页` : navLabel(activeNav, sourceMap))
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
  const pageEyebrow = sourceSetupMode === 'settings'
    ? settingsLabels.globalSettings
    : sourceSetupMode === 'select'
    ? '媒体源'
    : sourceSetupMode === 'emby'
      ? '媒体源 · Emby'
      : sourceSetupMode === 'localFolder'
      ? editingFileSystemSource ? `文件系统 · ${editingFileSystemSource.location}` : '媒体源 · 文件系统'
      : sourceSetupMode === 'smb'
      ? editingSource ? `文件系统 · ${editingSource.location}` : '媒体源 · SMB'
      : sourceSetupMode === 'webdav'
      ? editingSource ? `文件系统 · ${editingSource.location}` : '媒体源 · WebDAV'
      : activeSource
        ? activeLibraryCard
          ? `${sourceKindLabel(activeSource.kind)} · ${activeSource.name}`
          : `${sourceKindLabel(activeSource.kind)} · ${activeSource.location}`
        : `独立管理器 · ${navLabel(activeNav, sourceMap)}`
  const pageTitle = sourceSetupMode === 'settings'
    ? settingsLabels.settings
    : sourceSetupMode === 'select'
    ? '添加媒体源'
    : sourceSetupMode === 'emby'
      ? '添加 Emby 源'
      : sourceSetupMode === 'localFolder'
      ? editingFileSystemSource?.name ?? '添加本地文件夹'
      : sourceSetupMode === 'smb'
      ? editingSource?.name ?? '添加 SMB'
      : sourceSetupMode === 'webdav'
      ? editingSource?.name ?? '添加 WebDAV'
      : activeLibraryCard?.title ?? activeSource?.name ?? '媒体库'

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

  function updateTmdbSettings(settings: TmdbSettings): void {
    setTmdbSettings(settings)
    setTmdbStatus('')
  }

  async function testTmdbSettingsConnection(): Promise<void> {
    setIsTestingTmdb(true)
    setTmdbStatus('正在测试 TMDB 连接...')
    try {
      const message = await testTmdbConnection(tmdbSettings)
      setTmdbStatus(message)
    } catch (error) {
      setTmdbStatus(error instanceof Error ? `TMDB 连接失败：${error.message}` : 'TMDB 连接失败')
    } finally {
      setIsTestingTmdb(false)
    }
  }

  async function applyUpdatedMediaItem(updatedItem: MediaItem): Promise<void> {
    const source = sourceFor(updatedItem.sourceId)
    const nextAllItems = allItems.some((item) => item.id === updatedItem.id)
      ? allItems.map((item) => item.id === updatedItem.id ? updatedItem : item)
      : [...allItems, updatedItem]
    const sourceItems = nextAllItems.filter((item) => item.sourceId === updatedItem.sourceId)
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
    setSelectedId(updatedItem.id)
    setDetailItemsById((current) => {
      const next = new Map(current)
      next.set(updatedItem.id, updatedItem)
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

  async function scrapeMediaMetadata(item: MediaItem): Promise<void> {
    const source = sourceFor(item.sourceId)
    if (source.kind === 'Emby') return

    setScrapingMetadataItemId(item.id)
    setPlayIntent('正在从 TMDB 刮削元数据...')
    debugLibraryPlayback(`tmdb scrape start id=${item.id} title=${item.title}`)
    try {
      const updatedItem = await scrapeTmdbItem(item, tmdbSettings)
      await applyUpdatedMediaItem(updatedItem)
      setPlayIntent(`已用 TMDB 更新 ${updatedItem.title}`)
      debugLibraryPlayback(`tmdb scrape ok id=${item.id} tmdb=${updatedItem.externalIds?.tmdb ?? 'unknown'}`)
    } catch (error) {
      const message = error instanceof Error ? error.message : 'TMDB 刮削失败'
      setPlayIntent(message)
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

    const sourceItems = allItems.filter((item) => item.sourceId === sourceId && item.type !== 'folder')
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
    setConnectTone('idle')
    setConnectMessage(`正在刮削 ${source.name}：0 / ${sourceItems.length}`)
    debugLibraryPlayback(`tmdb source scrape start source=${sourceId} count=${sourceItems.length}`)

    const updatedItems = new Map<string, MediaItem>()
    let failedCount = 0

    try {
      for (let index = 0; index < sourceItems.length; index += 1) {
        const item = sourceItems[index]
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
      }

      const nextAllItems = allItems.map((item) => updatedItems.get(item.id) ?? item)
      const nextSourceItems = nextAllItems.filter((item) => item.sourceId === sourceId)
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
      setDetailItemsById((current) => {
        const next = new Map(current)
        updatedItems.forEach((item) => next.set(item.id, item))
        return next
      })

      const successCount = updatedItems.size
      setConnectTone(successCount > 0 ? 'success' : 'error')
      setConnectMessage(`刮削完成：成功 ${successCount} 个，失败 ${failedCount} 个。`)
      debugLibraryPlayback(`tmdb source scrape done source=${sourceId} ok=${successCount} failed=${failedCount}`)
    } catch (error) {
      const message = error instanceof Error ? error.message : '文件系统源刮削失败'
      setConnectTone('error')
      setConnectMessage(message)
      debugLibraryPlayback(`tmdb source scrape failed source=${sourceId} error=${message}`)
    } finally {
      setScrapingSourceMetadataId((current) => current === sourceId ? '' : current)
    }
  }

  async function playMediaItem(item: MediaItem): Promise<void> {
    const source = sourceFor(item.sourceId)
    debugLibraryPlayback(`play click id=${item.id} title=${item.title} source=${source.name} kind=${source.kind}`)
    if (source.kind !== 'Emby') {
      const localPlayableItem = item.path
        ? item
        : item.seasons?.flatMap((season) => season.episodes).find((episode) => episode.item?.path)?.item
          ?? item.episodes?.find((episode) => episode.item?.path)?.item
          ?? item
      const playablePath = localPlayableItem.playbackPath || localPlayableItem.path
      if (!playablePath) {
        setPlayIntent('本地媒体缺少文件路径。')
        debugLibraryPlayback(`play local blocked missing path id=${item.id}`)
        return
      }
      postNativeCommand({ type: 'command', command: 'setWebUiRoute', route: 'player' })
      postNativeCommand({ type: 'command', command: 'inspectorMedia' })
      window.location.hash = '#/player'
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
          command: 'openPath',
          path: playablePath
        })
        debugLibraryPlayback(`play local openPath posted id=${localPlayableItem.id} path=${localPlayableItem.path ?? ''}`)
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
      const report = savePendingEmbyPlaybackReport(embySession, target)
      debugLibraryPlayback(`play emby report pending targetId=${target.itemId} reportId=${report?.id ?? 'none'}`)
      postNativeCommand({ type: 'command', command: 'setWebUiRoute', route: 'player' })
      postNativeCommand({ type: 'command', command: 'inspectorMedia' })
      window.location.hash = '#/player'
      window.setTimeout(() => {
        notifyPendingEmbyPlaybackReport(report?.id)
        postNativeCommand({
          type: 'command',
          command: 'openPath',
          path: target.url
        })
        debugLibraryPlayback(`play openPath posted targetId=${target.itemId}`)
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
      const scanning = source.kind !== 'Emby' && scanningLocalFolderSourceIds.has(source.id)
      return (
        <div className="library-source-nav-row" key={source.id}>
          <SidebarButton
            item={{ key, label: scanning ? `${source.name} · 扫描中` : source.name, icon: sourceIcon(source.kind) }}
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
    <div className={`library-shell ${useMainOnlyLayout ? 'is-library-home' : ''}`}>
      <aside className="library-sidebar">
        <div className="library-brand">
          <Film size={30} />
          <div>
            <strong>Anvil Library</strong>
            <span>Media Manager</span>
          </div>
        </div>

        <nav className="library-nav">
          <div className="library-nav-group">
            <div className="library-nav-heading">
              <span>媒体库</span>
            </div>
            {primaryNav.map((item) => (
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
              <span>智能分类</span>
            </div>
            {smartNav.map((item) => (
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
              <span>媒体源</span>
              <button type="button" title="添加媒体源" onClick={openSourcePicker}>
                <Plus size={13} />
              </button>
            </div>
            <div className="library-nav-subheading">文件系统</div>
            {fileSystemSources.length ? renderSourceRows(fileSystemSources) : (
              <span className="library-nav-empty">尚未添加本地文件夹</span>
            )}
            <div className="library-nav-subheading">媒体库服务</div>
            {mediaServiceSources.length ? renderSourceRows(mediaServiceSources) : (
              <span className="library-nav-empty">尚未连接 Emby</span>
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
          <button className="library-back-button" type="button" onClick={() => { window.location.hash = '#/player' }}>
          <Play size={15} />
          <span>播放器模块</span>
          </button>
        </div>
      </aside>

      <main className="library-main">
        <section className="library-toolbar">
          <div className="library-title-block">
            <span>{pageEyebrow}</span>
            <h1>{pageTitle}</h1>
          </div>
          {!isSourceSetup ? (
            <>
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
                  placeholder="搜索关键字"
                />
              </label>
              {showSortControl ? (
                <div className="library-toolbar-actions">
                  <ToolbarSelect
                    value={sortKey}
                    options={sortOptions}
                    ariaLabel="排序"
                    statusIcon={sortOrder === 'descending'
                      ? <ArrowDownWideNarrow size={15} />
                      : <ArrowUpNarrowWide size={15} />}
                    onChange={changeSortKey}
                  />
                </div>
              ) : null}
            </>
          ) : null}
        </section>

        {sourceSetupMode === 'settings' ? (
          <LibrarySettingsPage
            language={language}
            tmdbSettings={tmdbSettings}
            tmdbStatus={tmdbStatus}
            isTestingTmdb={isTestingTmdb}
            onLanguageChange={setLanguage}
            onTmdbSettingsChange={updateTmdbSettings}
            onTestTmdb={() => { void testTmdbSettingsConnection() }}
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
              {connectMessage ? (
                <div className={`library-source-status ${connectTone === 'success' ? 'is-success' : ''} ${connectTone === 'error' ? 'is-error' : ''}`}>
                  {connectMessage}
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
              {connectMessage ? (
                <div className={`library-source-status ${connectTone === 'success' ? 'is-success' : ''} ${connectTone === 'error' ? 'is-error' : ''}`}>
                  {connectMessage}
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
                      ? `位置：${editingFileSystemSource.location || '未配置'} · ${editingFileSystemScanning ? '正在后台扫描' : `${editingFileSystemItems.length} 个媒体文件`}`
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
            <section className="library-view-tabs" aria-label="媒体类型">
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
                <span>{activeSource ? `${activeSource.name} · 继续观看` : '继续观看'}</span>
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
            <section className="library-grid-section">
              <div className="library-section-heading">
                <span>{listingTitle} · {visibleItems.length}</span>
                <button type="button" onClick={resetCurrentListing}>
                  <ListFilter size={13} />
                  <span>{activeLibraryViewId ? '全部媒体库' : '重置'}</span>
                </button>
              </div>
              {visibleItems.length ? (
                <div className="library-poster-grid">
                  {visibleItems.map((item) => (
                    <MediaPoster
                      key={item.id}
                      item={item}
                      source={sourceFor(item.sourceId)}
                      selected={selectedItem?.id === item.id}
                      onSelect={selectMediaItem}
                    />
                  ))}
                </div>
              ) : (
                <EmptyState
                  icon={<Film size={24} />}
                  title="暂无媒体"
                  caption={isActiveFileSystemScanning ? '正在后台扫描媒体文件，扫描完成后会自动出现。' : '当前没有来自真实媒体源的条目'}
                />
              )}
            </section>
            )}
          </>
        )}
      </main>

      {showDetailPanel && selectedDetailItem ? (
        <DetailPanel
          item={selectedDetailItem}
          source={sourceFor(selectedDetailItem.sourceId)}
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
          onPlayIntent={(item) => { void playMediaItem(item) }}
          onScrapeIntent={(item) => { void scrapeMediaMetadata(item) }}
          onEditMetadata={openMetadataEditor}
          onClearMetadata={(item) => { void clearMediaMetadata(item) }}
        />
      ) : !useMainOnlyLayout && !isSourceSetup ? (
        <EmptyDetail />
      ) : null}

      {activeMetadataEditorItem ? (
        <MetadataEditorDialog
          item={activeMetadataEditorItem}
          saving={isSavingManualMetadata}
          onClose={closeMetadataEditor}
          onSave={(item) => { void saveManualMetadata(item) }}
        />
      ) : null}
    </div>
  )
}
