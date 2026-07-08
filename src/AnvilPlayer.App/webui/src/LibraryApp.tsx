import { useEffect, useMemo, useState, type CSSProperties, type ReactNode } from 'react'
import {
  ArrowLeft,
  Check,
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
import { loadSavedEmbyConnection, saveSavedEmbyConnection } from './manager/connectionStorage'
import { createEmptyLibraryClient } from './manager/mediaLibraryClient'
import { loadEmbyLibrary, refreshEmbyLibrary, resolveEmbyPlaybackTarget, type EmbySession } from './manager/embyClient'
import { requestPlayerLaunch } from './manager/playerBridge'
import type { LibraryHomeCard, LibraryHomeSection, LibrarySource, LibraryView, MediaFilterKey, MediaItem, NavKey, SortKey, SourceKind } from './manager/types'
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
  { key: 'series', label: '剧集' },
  { key: 'folders', label: '文件夹' }
]

const mediaFilters: Array<{ key: MediaFilterKey; label: string }> = [
  { key: 'all', label: '全部' },
  { key: 'unwatched', label: '未看' },
  { key: 'watched', label: '已看' },
  { key: 'favorites', label: '收藏' },
  { key: 'inProgress', label: '观看中' }
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

function EmptyState(props: { icon: ReactNode; title: string; caption: string }): JSX.Element {
  return (
    <div className="library-empty-state">
      {props.icon}
      <strong>{props.title}</strong>
      <span>{props.caption}</span>
    </div>
  )
}

function ContinueCard(props: {
  item: MediaItem
  selected: boolean
  onSelect: (id: string) => void
}): JSX.Element {
  return (
    <button
      className={`library-continue-card ${props.selected ? 'is-selected' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.id)}
    >
      <div className="library-continue-art" style={{ background: props.item.backdrop }}>
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
          <div className={`library-home-grid is-${section.layout}`}>
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
          </div>
        </section>
      ))}
    </>
  )
}

function DetailPanel(props: {
  item: MediaItem
  source: LibrarySource
  playIntent: string
  onPlayIntent: (item: MediaItem) => void
  onClose: () => void
}): JSX.Element {
  const backdropStyle: CSSProperties = { background: props.item.backdrop }

  return (
    <aside className="library-detail">
      <div className="library-detail-backdrop" style={backdropStyle}>
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
          <button className="library-primary-action" type="button" onClick={() => props.onPlayIntent(props.item)}>
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
          <span>{props.item.quality}</span>
        </div>

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

        {props.item.episodes?.length ? (
          <section className="library-episode-section">
            <div className="library-section-heading">
              <span>剧集</span>
              <button type="button">
                <ListFilter size={13} />
                <span>排序</span>
              </button>
            </div>
            <div className="library-episode-row">
              {props.item.episodes.map((episode) => (
                <button className="library-episode-card" type="button" key={episode.id}>
                  <div className="library-episode-thumb" style={{ background: episode.poster }}>
                    <Play size={18} />
                  </div>
                  <span>{episode.index}</span>
                  <strong>{episode.title}</strong>
                  <small>{episode.duration}</small>
                </button>
              ))}
            </div>
          </section>
        ) : null}
      </div>
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
  const savedConnection = useMemo(() => loadSavedEmbyConnection(), [])
  const savedAddress = useMemo(
    () => savedConnection ? parseServerAddress(savedConnection.serverUrl, 'http') : undefined,
    [savedConnection]
  )
  const client = useMemo(() => createEmptyLibraryClient(), [])
  const [language, setLanguage] = useState<UiLanguage>(() => getInitialLanguage())
  const [sources, setSources] = useState<LibrarySource[]>([])
  const [allItems, setAllItems] = useState<MediaItem[]>([])
  const [visibleItems, setVisibleItems] = useState<MediaItem[]>([])
  const [continueItems, setContinueItems] = useState<MediaItem[]>([])
  const [homeSections, setHomeSections] = useState<LibraryHomeSection[]>([])
  const [activeNav, setActiveNav] = useState<NavKey>(() => savedConnection?.sourceId ? `source:${savedConnection.sourceId}` as NavKey : 'recent')
  const [activeView, setActiveView] = useState<LibraryView>('home')
  const [sortKey, setSortKey] = useState<SortKey>('recent')
  const [mediaFilter, setMediaFilter] = useState<MediaFilterKey>('all')
  const [query, setQuery] = useState('')
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
  const [embySession, setEmbySession] = useState<EmbySession | undefined>(() => savedConnection?.session)
  const [playIntent, setPlayIntent] = useState('')
  const [isConnecting, setIsConnecting] = useState(false)
  const [connectTone, setConnectTone] = useState<'idle' | 'success' | 'error'>('idle')
  const [connectMessage, setConnectMessage] = useState('')

  useEffect(() => {
    applyAppearanceSettings()
  }, [])

  useEffect(() => {
    applyDocumentLanguage(language)
    saveUiLanguage(language)
  }, [language])

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
      const initialNav = savedConnection?.sourceId ? `source:${savedConnection.sourceId}` as NavKey : 'recent'
      const [sourceRows, allRows, visibleRows, continueRows, homeRows] = await Promise.all([
        client.listSources(),
        client.listAllItems(),
        client.listItems({ navKey: initialNav, view: 'home', search: '', sortKey: 'recent' }),
        client.getContinueWatching(),
        client.listHomeSections(savedConnection?.sourceId)
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
  }, [client])

  useEffect(() => {
    const connection = savedConnection
    const session = connection?.session
    if (!connection || !session) return
    const savedSession = session
    const savedConnectionName = connection.name
    const savedIgnoreCertificateErrors = connection.ignoreCertificateErrors
    const savedUsername = connection.username
    let cancelled = false

    async function refreshSavedConnection(): Promise<void> {
      try {
        postNativeCommand({
          type: 'command',
          command: 'setAllowInsecureCertificates',
          enabled: savedIgnoreCertificateErrors
        })
        const snapshot = await refreshEmbyLibrary(savedSession, savedConnectionName)
        await client.upsertSourceItems(snapshot.source, snapshot.items, snapshot.homeSections)

        saveSavedEmbyConnection({
          sourceId: snapshot.source.id,
          name: snapshot.source.name,
          serverUrl: snapshot.session.apiBaseUrl,
          username: snapshot.session.userName || savedUsername,
          ignoreCertificateErrors: savedIgnoreCertificateErrors,
          session: snapshot.session,
          savedAt: Date.now()
        })

        const sourceNav = `source:${snapshot.source.id}` as NavKey
        const [sourceRows, allRows, continueRows, visibleRows, homeRows] = await Promise.all([
          client.listSources(),
          client.listAllItems(),
          client.getContinueWatching(),
          client.listItems({ navKey: sourceNav, view: 'home', search: '', sortKey: 'recent' }),
          client.listHomeSections(snapshot.source.id)
        ])

        if (cancelled) return
        setSources(sourceRows)
        setAllItems(allRows)
        setContinueItems(continueRows)
        setVisibleItems(visibleRows)
        setHomeSections(homeRows)
        setActiveNav(sourceNav)
        setActiveView('home')
        setActiveLibraryViewId('')
        setMediaFilter('all')
        setQuery('')
        setSelectedId(visibleRows[0]?.id ?? '')
        setIsDetailOpen(false)
        setConnectionName(snapshot.source.name)
        setUsername(snapshot.session.userName || savedUsername)
        setEmbySession(snapshot.session)
        applyServerAddress(snapshot.session.apiBaseUrl)
        setSourceSetupMode('hidden')
      } catch (error) {
        if (cancelled) return
        setConnectTone('error')
        setConnectMessage(`已加载本地缓存，刷新 Emby 失败：${errorText(error)}`)
      }
    }

    void refreshSavedConnection()
    return () => {
      cancelled = true
    }
  }, [client, savedConnection])

  useEffect(() => {
    let cancelled = false
    const activeSourceId = activeNav.startsWith('source:') ? activeNav.slice('source:'.length) : ''
    const activeSourceKind = activeSourceId ? sources.find((source) => source.id === activeSourceId)?.kind : undefined

    async function loadVisibleItems(): Promise<void> {
      const rows = await client.listItems({
        navKey: activeNav,
        view: activeView,
        search: query,
        sortKey,
        filterKey: mediaFilter,
        libraryViewId: activeSourceKind === 'Emby' ? activeLibraryViewId : undefined
      })
      if (!cancelled) setVisibleItems(rows)
    }

    void loadVisibleItems()
    return () => {
      cancelled = true
    }
  }, [activeLibraryViewId, activeNav, activeView, client, mediaFilter, query, sortKey, sources])

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
      ?? (isActiveEmby ? undefined : visibleItems[0])
  const embySource = sources.find((source) => source.kind === 'Emby')
  const readyEmby = sources.find((source) => source.kind === 'Emby' && source.status === 'online')
  const sourceScopedItems = activeSource
    ? allItems.filter((item) => item.sourceId === activeSource.id)
    : allItems.filter((item) => !embySourceIds.has(item.sourceId))
  const sourceScopedContinueItems = activeSource
    ? continueItems.filter((item) => item.sourceId === activeSource.id)
    : continueItems.filter((item) => !embySourceIds.has(item.sourceId))
  const scopedItems = activeLibraryViewId && sourceScopedItems.some((item) => item.libraryViewId)
    ? sourceScopedItems.filter((item) => item.libraryViewId === activeLibraryViewId)
    : sourceScopedItems
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
    && scopedHomeSections.length
  )
  const showDetailPanel = Boolean(!isSourceSetup && selectedItem && (!isActiveEmby || isDetailOpen))
  const useMainOnlyLayout = Boolean(isSourceSetup || (isActiveEmby && !showDetailPanel))
  const showContinueSection = Boolean(!isActiveEmby || mediaFilter === 'all')
  const activeMediaFilter = mediaFilters.find((filter) => filter.key === mediaFilter)
  const listingBaseTitle = activeLibraryCard?.title ?? (activeSource ? `${activeSource.name} 主页` : navLabel(activeNav, sourceMap))
  const listingTitle = mediaFilter === 'all'
    ? listingBaseTitle
    : `${listingBaseTitle} · ${mediaFilter === 'inProgress' ? '继续观看' : activeMediaFilter?.label ?? '筛选'}`
  const serverUrl = buildServerAddress(serverProtocol, serverHost, serverPort, serverPath)
  const canConnectEmby = Boolean(serverHost.trim() && username.trim() && password)
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

    if (!embySession) {
      setPlayIntent('Emby session is not ready. Reconnect this source first.')
      debugLibraryPlayback(`play blocked missing emby session id=${item.id}`)
      return
    }

    setPlayIntent('Resolving Emby playback URL...')
    debugLibraryPlayback(`play resolve start id=${item.id}`)
    try {
      const target = await resolveEmbyPlaybackTarget(embySession, item)
      debugLibraryPlayback(`play resolve ok sourceId=${item.id} targetId=${target.itemId} url=${redactPlaybackUrl(target.url)}`)
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
    }
  }

  function openSourcePicker(): void {
    setSourceSetupMode('select')
    setActiveLibraryViewId('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openSettings(): void {
    setSourceSetupMode('settings')
    setActiveLibraryViewId('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openEmbySetup(): void {
    setSourceSetupMode('emby')
    setActiveLibraryViewId('')
    setSelectedId('')
    setIsDetailOpen(false)
    setConnectionName((current) => current || 'Emby')
    setConnectTone('idle')
    setConnectMessage('')
  }

  function selectNavigation(navKey: NavKey): void {
    setSourceSetupMode('hidden')
    setActiveNav(navKey)
    setActiveView('home')
    setActiveLibraryViewId('')
    setMediaFilter('all')
    setQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function selectMediaItem(itemId: string): void {
    setSelectedId(itemId)
    setIsDetailOpen(true)
    setPlayIntent('')
  }

  function openLibraryView(viewId: string): void {
    setActiveLibraryViewId(viewId)
    setActiveView('home')
    setMediaFilter('all')
    setSortKey('recent')
    setQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function openContinueListing(): void {
    setActiveView('home')
    setMediaFilter('inProgress')
    setSortKey('recent')
    setQuery('')
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function resetCurrentListing(): void {
    if (activeLibraryViewId) {
      setActiveLibraryViewId('')
      setActiveView('home')
    }
    setMediaFilter('all')
    setSortKey('recent')
    setQuery('')
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

  function changeMediaFilter(filter: MediaFilterKey): void {
    setMediaFilter(filter)
    setSelectedId('')
    setIsDetailOpen(false)
    setPlayIntent('')
  }

  function changeSortKey(sort: SortKey): void {
    setSortKey(sort)
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
      const snapshot = await loadEmbyLibrary({
        serverUrl,
        username,
        password,
        displayName: connectionName
      })
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
        client.listHomeSections(snapshot.source.id)
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
      setQuery('')
      setSelectedId(visibleRows[0]?.id ?? '')
      setIsDetailOpen(false)
      setConnectionName(snapshot.source.name)
      setEmbySession(snapshot.session)
      applyServerAddress(snapshot.session.apiBaseUrl)
      setPassword('')
      setSourceSetupMode('hidden')
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
            <button
              className={`library-nav-button ${sourceSetupMode === 'select' || sourceSetupMode === 'emby' ? 'is-active' : ''}`}
              type="button"
              onClick={openSourcePicker}
            >
              <Plus size={16} />
              <span>添加媒体源</span>
            </button>
            {sources.map((source) => {
              const key = `source:${source.id}` as NavKey
              return (
                <SidebarButton
                  key={source.id}
                  item={{ key, label: source.name, icon: sourceIcon(source.kind) }}
                  activeNav={activeNav}
                  suppressActive={isSourceSetup}
                  onSelect={selectNavigation}
                />
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
                    setQuery(event.target.value)
                    setSelectedId('')
                    setIsDetailOpen(false)
                    setPlayIntent('')
                  }}
                  placeholder="搜索标题 / 类型 / 标签"
                />
              </label>
              <div className="library-toolbar-actions">
                <button
                  type="button"
                  title={activeLibraryViewId ? '返回 Emby 媒体库' : '媒体库首页'}
                  onClick={resetCurrentListing}
                >
                  <Grid3X3 size={16} />
                </button>
                <select value={mediaFilter} onChange={(event) => changeMediaFilter(event.target.value as MediaFilterKey)} aria-label="筛选">
                  {mediaFilters.map((filter) => (
                    <option value={filter.key} key={filter.key}>{filter.label}</option>
                  ))}
                </select>
                <select value={sortKey} onChange={(event) => changeSortKey(event.target.value as SortKey)} aria-label="排序">
                  <option value="recent">最近添加</option>
                  <option value="title">标题</option>
                  <option value="rating">评分</option>
                  <option value="year">发行年份</option>
                </select>
              </div>
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
              <button className="library-secondary-action" type="button" onClick={() => setSourceSetupMode('select')}>
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
            <section className="library-stats-row">
              <div className="library-stat">
                <span>片源</span>
                <strong>{scopedItems.length}</strong>
              </div>
              <div className="library-stat">
                <span>{activeSource ? '当前源' : '媒体源'}</span>
                <strong>{activeSource ? sourceKindLabel(activeSource.kind) : sources.length}</strong>
              </div>
              <div className="library-stat">
                <span>未看</span>
                <strong>{scopedItems.filter((item) => !item.watched).length}</strong>
              </div>
              <div className={`library-connect-card ${activeSource?.status === 'online' || readyEmby ? 'is-ready' : ''}`}>
                {sourceIcon(activeSource?.kind ?? 'Emby', 22)}
                <div>
                  <span>{activeSource ? sourceKindLabel(activeSource.kind) : 'Emby'}</span>
                  <strong>
                    {activeSource
                      ? `${activeSource.name} · ${activeSource.status === 'online' ? '已接入' : '待连接'}`
                      : readyEmby
                        ? `${readyEmby.name} · 已接入`
                        : embySource
                          ? `${embySource.name} · 待连接`
                          : '未配置'}
                  </strong>
                </div>
              </div>
            </section>

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
                <div className="library-continue-row">
                  {scopedContinueItems.map((item) => (
                    <ContinueCard
                      key={item.id}
                      item={item}
                      selected={selectedItem?.id === item.id}
                      onSelect={selectMediaItem}
                    />
                  ))}
                </div>
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

      {showDetailPanel && selectedItem ? (
        <DetailPanel
          item={selectedItem}
          source={sourceFor(selectedItem.sourceId)}
          playIntent={playIntent}
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
