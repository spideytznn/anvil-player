import { useEffect, useId, useLayoutEffect, useMemo, useRef, useState, type CSSProperties, type ReactNode } from 'react'
import {
  ArrowDownWideNarrow,
  ArrowLeft,
  ArrowUpNarrowWide,
  Check,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  Clock3,
  Database,
  Film,
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
  Tv,
  Wand2,
  X
} from 'lucide-react'
import { applyAppearanceSettings } from './appearance'
import { loadSavedEmbyConnections, saveSavedEmbyConnection, type SavedEmbyConnection } from './manager/connectionStorage'
import { createEmptyLibraryClient } from './manager/mediaLibraryClient'
import {
  loadEmbyItemDetails,
  loadEmbyLibrary,
  refreshEmbyLibrary,
  resolveEmbyPlaybackTarget,
  savePendingEmbyPlaybackReport,
  searchEmbyLibrary,
  searchEmbyPersonLibrary,
  type EmbySession
} from './manager/embyClient'
import { requestPlayerLaunch } from './manager/playerBridge'
import type { LibraryHomeCard, LibraryHomeSection, LibrarySource, LibraryView, MediaFilterKey, MediaItem, NavKey, PersonCredit, SortKey, SortOrder, SourceKind } from './manager/types'
import { postNativeCommand } from './nativeBridge'
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

type SourceSetupMode = 'hidden' | 'select' | 'emby' | 'settings'
type ServerProtocol = 'http' | 'https'

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
    default: return '本机'
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

function hasImageBackground(background: string): boolean {
  return background.trim().startsWith('url(')
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
      <div className="library-continue-art" style={{ background: art }}>
        <span style={{ width: `${Math.round(props.item.progress * 100)}%` }} />
      </div>
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
  const hasPosterImage = hasImageBackground(props.item.poster)

  return (
    <button
      className={`library-poster-card ${props.selected ? 'is-selected' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.id)}
    >
      <div className="library-poster-art" style={{ background: props.item.poster }}>
        <div className="library-poster-shine" />
        <span className="library-poster-type">{mediaTypeLabel(props.item.type)}</span>
        {!hasPosterImage ? (
          <span className="library-poster-empty">
            <Film size={18} />
          </span>
        ) : null}
      </div>
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
      <div className="library-home-card-art" style={{ background: props.card.image }}>
        <div className="library-poster-shine" />
        {props.card.kind === 'view' ? (
          <span className="library-home-view-icon">{sourceIcon('Emby', 16)}</span>
        ) : props.card.mediaType ? (
          <span className="library-poster-type">{mediaTypeLabel(props.card.mediaType)}</span>
        ) : null}
      </div>
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
  onPlayIntent: (item: MediaItem) => void
  onSelectItem: (item: MediaItem) => void
  onSearchPerson: (person: PersonCredit) => void
  onClose: () => void
}): JSX.Element {
  const backdropStyle: CSSProperties = { background: props.item.backdrop }
  const seasons = props.item.seasons ?? []
  const [activeSeasonId, setActiveSeasonId] = useState('')
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
        <button className="library-detail-close" type="button" title="关闭详情" onClick={props.onClose}>
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
          <button className="library-icon-action" type="button" title="更多">
            <MoreHorizontal size={16} />
          </button>
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
                  <div className="library-episode-thumb" style={{ background: episode.poster }}>
                    <Play size={18} />
                  </div>
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
                  <div style={{ background: item.poster }} />
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

function SourceTypePicker(props: { onSelectEmby: () => void }): JSX.Element {
  return (
    <section className="library-source-setup">
      <div className="library-source-setup-heading">
        <span>媒体源</span>
        <h2>添加媒体源</h2>
      </div>

      <div className="library-source-type-grid">
        <button className="library-source-type-button" type="button" onClick={props.onSelectEmby}>
          {sourceIcon('Emby', 22)}
          <strong>Emby</strong>
          <span>服务器媒体库</span>
        </button>
        <button className="library-source-type-button" type="button" disabled>
          {sourceIcon('Local', 22)}
          <strong>本机</strong>
          <span>本地文件夹</span>
        </button>
        <button className="library-source-type-button" type="button" disabled>
          {sourceIcon('WebDAV', 22)}
          <strong>WebDAV</strong>
          <span>网络存储</span>
        </button>
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
    chinese: '中文',
    english: 'English'
  }
}

function LibrarySettingsPage(props: {
  language: UiLanguage
  onLanguageChange: (language: UiLanguage) => void
}): JSX.Element {
  const t = settingsCopy[props.language]
  const languageOptions: Array<{ value: UiLanguage; label: string }> = [
    { value: 'zh', label: t.chinese },
    { value: 'en', label: t.english }
  ]

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
  const [ignoreCertificateErrors, setIgnoreCertificateErrors] = useState(() => savedConnection?.ignoreCertificateErrors ?? false)
  const [embySessionsBySourceId, setEmbySessionsBySourceId] = useState<Map<string, EmbySession>>(() => new Map(
    savedConnections
      .filter((connection) => connection.session)
      .map((connection) => [connection.sourceId, connection.session as EmbySession])
  ))
  const [playIntent, setPlayIntent] = useState('')
  const [resolvingPlayItemId, setResolvingPlayItemId] = useState('')
  const [isConnecting, setIsConnecting] = useState(false)
  const [connectTone, setConnectTone] = useState<'idle' | 'success' | 'error'>('idle')
  const [connectMessage, setConnectMessage] = useState('')
  const [editingSourceId, setEditingSourceId] = useState('')
  const homeSectionRefreshSourceIdsRef = useRef<Set<string>>(new Set())
  const activeNavRef = useRef<NavKey>(initialActiveNav)

  useEffect(() => {
    applyAppearanceSettings()
  }, [])

  useEffect(() => {
    applyDocumentLanguage(language)
    saveUiLanguage(language)
  }, [language])

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
    }

    void loadInitialLibrary()
    return () => {
      cancelled = true
    }
  }, [client, initialActiveNav])

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
    const activeSourceId = activeNav.startsWith('source:') ? activeNav.slice('source:'.length) : ''
    const activeSourceKind = activeSourceId ? sources.find((source) => source.id === activeSourceId)?.kind : undefined
    if (activeSourceKind === 'Emby' && !isDetailOpen) {
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
  const activeSource = activeNav.startsWith('source:') ? sourceMap.get(activeNav.slice('source:'.length)) : undefined
  const isSourceSetup = sourceSetupMode !== 'hidden'
  const isActiveEmby = activeSource?.kind === 'Emby'
  const selectedItem = isSourceSetup
    ? undefined
    : visibleItems.find((item) => item.id === selectedId)
      ?? allItems.find((item) => item.id === selectedId && (!activeSource || item.sourceId === activeSource.id))
      ?? detailItemsById.get(selectedId)
      ?? (isActiveEmby ? undefined : visibleItems[0])
  const selectedDetailItem = selectedItem ? detailItemsById.get(selectedItem.id) ?? selectedItem : undefined
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
  const showDetailPanel = Boolean(!isSourceSetup && selectedItem && (!isActiveEmby || isDetailOpen))
  const useMainOnlyLayout = Boolean(isSourceSetup || (isActiveEmby && !showDetailPanel))
  const isInsideEmbyLibraryView = Boolean(isActiveEmby && activeLibraryViewId)
  const showLibraryViewTabs = !isInsideEmbyLibraryView
  const showContinueSection = Boolean(!isInsideEmbyLibraryView && (!isActiveEmby || mediaFilter === 'all'))
  const showSortControl = Boolean(activeLibraryViewId || (!isActiveEmby && activeView !== 'home'))
  const listingBaseTitle = activeLibraryCard?.title ?? (activeSource ? `${activeSource.name} 主页` : navLabel(activeNav, sourceMap))
  const listingTitle = mediaFilter === 'all'
    ? listingBaseTitle
    : `${listingBaseTitle} · ${mediaFilter === 'inProgress' ? '继续观看' : '筛选'}`
  const serverUrl = buildServerAddress(serverProtocol, serverHost, serverPort, serverPath)
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

  async function playMediaItem(item: MediaItem): Promise<void> {
    const source = sourceFor(item.sourceId)
    debugLibraryPlayback(`play click id=${item.id} title=${item.title} source=${source.name} kind=${source.kind}`)
    if (source.kind !== 'Emby') {
      setPlayIntent(requestPlayerLaunch(item))
      debugLibraryPlayback(`play local placeholder id=${item.id}`)
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
      window.location.hash = '#/player'
      window.setTimeout(() => {
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
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openSourceEditor(sourceId: string): void {
    const source = sourceMap.get(sourceId)
    if (!source || source.kind !== 'Emby') return

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
            {sources.map((source) => {
              const key = `source:${source.id}` as NavKey
              return (
                <div className="library-source-nav-row" key={source.id}>
                  <SidebarButton
                    item={{ key, label: source.name, icon: sourceIcon(source.kind) }}
                    activeNav={activeNav}
                    suppressActive={isSourceSetup && editingSourceId !== source.id}
                    onSelect={selectNavigation}
                  />
                  <button
                    className={`library-source-edit-button ${editingSourceId === source.id ? 'is-active' : ''}`}
                    type="button"
                    title="编辑媒体源"
                    aria-label={`编辑 ${source.name}`}
                    disabled={source.kind !== 'Emby'}
                    onClick={() => openSourceEditor(source.id)}
                  >
                    <MoreHorizontal size={15} />
                  </button>
                </div>
              )
            })}
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
                  placeholder="搜索标题 / 类型 / 标签"
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
          <LibrarySettingsPage language={language} onLanguageChange={setLanguage} />
        ) : sourceSetupMode === 'select' ? (
          <SourceTypePicker onSelectEmby={openEmbySetup} />
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
              <button
                className={`library-certificate-toggle ${ignoreCertificateErrors ? 'is-active' : ''}`}
                type="button"
                aria-pressed={ignoreCertificateErrors}
                onClick={() => setIgnoreCertificateErrors((enabled) => !enabled)}
              >
                <ShieldOff size={15} />
                <span>跳过 HTTPS 证书校验</span>
              </button>
              <button
                className="library-connect-action"
                type="button"
                disabled={isConnecting || !canConnectEmby}
                onClick={() => void connectEmbySource()}
              >
                <Server size={15} />
                <span>{isConnecting ? '连接中' : '连接'}</span>
              </button>
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
                <EmptyState icon={<Film size={24} />} title="暂无媒体" caption="当前没有来自真实媒体源的条目" />
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
          onSelectItem={selectRelatedMediaItem}
          onSearchPerson={(person) => searchCastPerson(person, selectedDetailItem.sourceId)}
          onClose={() => {
            setIsDetailOpen(false)
            setSelectedId('')
            setPlayIntent('')
          }}
          onPlayIntent={(item) => { void playMediaItem(item) }}
        />
      ) : !useMainOnlyLayout && !isSourceSetup ? (
        <EmptyDetail />
      ) : null}
    </div>
  )
}
