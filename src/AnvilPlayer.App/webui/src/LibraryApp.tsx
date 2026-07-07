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
  ListFilter,
  MoreHorizontal,
  Play,
  Plus,
  Search,
  Server,
  Settings2,
  Star,
  Tv,
  Wand2
} from 'lucide-react'
import { createEmptyLibraryClient } from './manager/mediaLibraryClient'
import { loadEmbyLibrary } from './manager/embyClient'
import { requestPlayerLaunch } from './manager/playerBridge'
import type { LibrarySource, LibraryView, MediaItem, NavKey, SortKey, SourceKind } from './manager/types'
import './library.css'

interface NavItem {
  key: NavKey
  label: string
  icon: ReactNode
}

type SourceSetupMode = 'hidden' | 'select' | 'emby'

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

function sourceIcon(kind: SourceKind, size = 16): ReactNode {
  switch (kind) {
    case 'Emby': return <Server size={size} />
    case 'WebDAV': return <Database size={size} />
    default: return <HardDrive size={size} />
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
  return (
    <button
      className={`library-poster-card ${props.selected ? 'is-selected' : ''}`}
      type="button"
      onClick={() => props.onSelect(props.item.id)}
    >
      <div className="library-poster-art" style={{ background: props.item.poster }}>
        <div className="library-poster-shine" />
        <span className="library-poster-type">{mediaTypeLabel(props.item.type)}</span>
        <span className="library-poster-mark">{posterMark(props.item)}</span>
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

function DetailPanel(props: {
  item: MediaItem
  source: LibrarySource
  playIntent: string
  onPlayIntent: (item: MediaItem) => void
}): JSX.Element {
  const backdropStyle: CSSProperties = { background: props.item.backdrop }

  return (
    <aside className="library-detail">
      <div className="library-detail-backdrop" style={backdropStyle}>
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

export default function LibraryApp(): JSX.Element {
  const client = useMemo(() => createEmptyLibraryClient(), [])
  const [sources, setSources] = useState<LibrarySource[]>([])
  const [allItems, setAllItems] = useState<MediaItem[]>([])
  const [visibleItems, setVisibleItems] = useState<MediaItem[]>([])
  const [continueItems, setContinueItems] = useState<MediaItem[]>([])
  const [activeNav, setActiveNav] = useState<NavKey>('recent')
  const [activeView, setActiveView] = useState<LibraryView>('home')
  const [sortKey, setSortKey] = useState<SortKey>('recent')
  const [query, setQuery] = useState('')
  const [selectedId, setSelectedId] = useState('')
  const [sourceSetupMode, setSourceSetupMode] = useState<SourceSetupMode>('select')
  const [connectionName, setConnectionName] = useState('')
  const [serverUrl, setServerUrl] = useState('')
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [playIntent, setPlayIntent] = useState('')
  const [isConnecting, setIsConnecting] = useState(false)
  const [connectTone, setConnectTone] = useState<'idle' | 'success' | 'error'>('idle')
  const [connectMessage, setConnectMessage] = useState('')

  useEffect(() => {
    let cancelled = false

    async function loadInitialLibrary(): Promise<void> {
      const [sourceRows, itemRows, continueRows] = await Promise.all([
        client.listSources(),
        client.listItems({ navKey: 'release', view: 'home', search: '', sortKey: 'recent' }),
        client.getContinueWatching()
      ])
      if (cancelled) return
      setSources(sourceRows)
      setAllItems(itemRows)
      setContinueItems(continueRows)
      setVisibleItems(itemRows)
    }

    void loadInitialLibrary()
    return () => {
      cancelled = true
    }
  }, [client])

  useEffect(() => {
    let cancelled = false

    async function loadVisibleItems(): Promise<void> {
      const rows = await client.listItems({ navKey: activeNav, view: activeView, search: query, sortKey })
      if (!cancelled) setVisibleItems(rows)
    }

    void loadVisibleItems()
    return () => {
      cancelled = true
    }
  }, [activeNav, activeView, client, query, sortKey])

  useEffect(() => {
    if (!visibleItems.length) return
    if (!visibleItems.some((item) => item.id === selectedId)) {
      setSelectedId(visibleItems[0].id)
    }
  }, [selectedId, visibleItems])

  const sourceMap = useMemo(() => new Map(sources.map((source) => [source.id, source])), [sources])
  const activeSource = activeNav.startsWith('source:') ? sourceMap.get(activeNav.slice('source:'.length)) : undefined
  const isSourceSetup = sourceSetupMode !== 'hidden'
  const selectedItem = isSourceSetup ? undefined : allItems.find((item) => item.id === selectedId) ?? visibleItems[0]
  const embySource = sources.find((source) => source.kind === 'Emby')
  const readyEmby = sources.find((source) => source.kind === 'Emby' && source.status === 'online')
  const scopedItems = activeSource ? allItems.filter((item) => item.sourceId === activeSource.id) : allItems
  const scopedContinueItems = activeSource
    ? continueItems.filter((item) => item.sourceId === activeSource.id)
    : continueItems
  const pageEyebrow = sourceSetupMode === 'select'
    ? '媒体源'
    : sourceSetupMode === 'emby'
      ? '媒体源 · Emby'
      : activeSource
        ? `${sourceKindLabel(activeSource.kind)} · ${activeSource.location}`
        : `独立管理器 · ${navLabel(activeNav, sourceMap)}`
  const pageTitle = sourceSetupMode === 'select'
    ? '添加媒体源'
    : sourceSetupMode === 'emby'
      ? '添加 Emby 源'
      : activeSource?.name ?? '媒体库'

  const fallbackSource: LibrarySource = {
    id: 'unknown',
    name: '未归档',
    kind: 'Local',
    status: 'draft',
    itemCount: 0,
    location: ''
  }
  const sourceFor = (sourceId: string): LibrarySource => sourceMap.get(sourceId) ?? fallbackSource

  function openSourcePicker(): void {
    setSourceSetupMode('select')
    setSelectedId('')
    setPlayIntent('')
    setConnectTone('idle')
    setConnectMessage('')
  }

  function openEmbySetup(): void {
    setSourceSetupMode('emby')
    setConnectionName((current) => current || 'Emby')
    setConnectTone('idle')
    setConnectMessage('')
  }

  function selectNavigation(navKey: NavKey): void {
    setSourceSetupMode('hidden')
    setActiveNav(navKey)
    setPlayIntent('')
  }

  async function connectEmbySource(): Promise<void> {
    if (!serverUrl.trim() || !username.trim() || !password) return

    setIsConnecting(true)
    setConnectTone('idle')
    setConnectMessage('正在连接 Emby...')

    try {
      const snapshot = await loadEmbyLibrary({
        serverUrl,
        username,
        password,
        displayName: connectionName
      })
      await client.upsertSourceItems(snapshot.source, snapshot.items)

      const sourceNav = `source:${snapshot.source.id}` as NavKey
      const [sourceRows, allRows, continueRows, visibleRows] = await Promise.all([
        client.listSources(),
        client.listItems({ navKey: 'release', view: 'home', search: '', sortKey: 'recent' }),
        client.getContinueWatching(),
        client.listItems({ navKey: sourceNav, view: 'home', search: '', sortKey: 'recent' })
      ])

      setSources(sourceRows)
      setAllItems(allRows)
      setContinueItems(continueRows)
      setVisibleItems(visibleRows)
      setActiveNav(sourceNav)
      setActiveView('home')
      setQuery('')
      setSelectedId(visibleRows[0]?.id ?? '')
      setConnectionName(snapshot.source.name)
      setServerUrl(snapshot.session.apiBaseUrl)
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
    <div className="library-shell">
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
              className={`library-nav-button ${isSourceSetup ? 'is-active' : ''}`}
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

        <button className="library-back-button" type="button" onClick={() => { window.location.hash = '#/player' }}>
          <Play size={15} />
          <span>播放器模块</span>
        </button>
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
                  onChange={(event) => setQuery(event.target.value)}
                  placeholder="搜索标题 / 类型 / 标签"
                />
              </label>
              <div className="library-toolbar-actions">
                <button type="button" title="视图">
                  <Grid3X3 size={16} />
                </button>
                <button type="button" title="筛选">
                  <Settings2 size={16} />
                </button>
                <select value={sortKey} onChange={(event) => setSortKey(event.target.value as SortKey)} aria-label="排序">
                  <option value="recent">最近</option>
                  <option value="title">标题</option>
                  <option value="rating">评分</option>
                  <option value="year">年份</option>
                </select>
              </div>
            </>
          ) : null}
        </section>

        {sourceSetupMode === 'select' ? (
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
                <label>
                  <span>服务器</span>
                  <input value={serverUrl} onChange={(event) => setServerUrl(event.target.value)} />
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
                type="button"
                disabled={isConnecting || !serverUrl.trim() || !username.trim() || !password}
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
                  onClick={() => setActiveView(tab.key)}
                >
                  {tab.label}
                </button>
              ))}
            </section>

            <section className="library-continue">
              <div className="library-section-heading">
                <span>{activeSource ? `${activeSource.name} · 继续观看` : '继续观看'}</span>
                <button type="button" onClick={() => selectNavigation('continue')}>
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
                      onSelect={setSelectedId}
                    />
                  ))}
                </div>
              ) : (
                <EmptyState icon={<Clock3 size={24} />} title="暂无继续观看" caption="当前没有来自真实媒体源的播放进度" />
              )}
            </section>

            <section className="library-grid-section">
              <div className="library-section-heading">
                <span>{activeSource ? `${activeSource.name} 主页` : navLabel(activeNav, sourceMap)} · {visibleItems.length}</span>
                <button type="button" onClick={() => setQuery('')}>
                  <ListFilter size={13} />
                  <span>重置</span>
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
                      onSelect={setSelectedId}
                    />
                  ))}
                </div>
              ) : (
                <EmptyState icon={<Film size={24} />} title="暂无媒体" caption="当前没有来自真实媒体源的条目" />
              )}
            </section>
          </>
        )}
      </main>

      {selectedItem ? (
        <DetailPanel
          item={selectedItem}
          source={sourceFor(selectedItem.sourceId)}
          playIntent={playIntent}
          onPlayIntent={(item) => setPlayIntent(requestPlayerLaunch(item))}
        />
      ) : (
        <EmptyDetail />
      )}
    </div>
  )
}
