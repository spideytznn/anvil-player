import type { LibraryHomeSection, LibrarySource, MediaItem } from './types'

const CLIENT_NAME = 'Anvil Player'
const CLIENT_VERSION = '0.1.0'
const DEVICE_NAME = 'Anvil Library'
const DEVICE_ID_KEY = 'anvil-player.emby.device-id'
const DEFAULT_LIMIT = 120
const TICKS_PER_SECOND = 10_000_000
const TICKS_PER_MINUTE = TICKS_PER_SECOND * 60

export interface EmbyConnectionInput {
  serverUrl: string
  username: string
  password: string
  displayName?: string
}

export interface EmbySession {
  apiBaseUrl: string
  serverId: string
  serverName: string
  serverVersion: string
  userId: string
  userName: string
  accessToken: string
}

export interface EmbyLibrarySnapshot {
  session: EmbySession
  source: LibrarySource
  items: MediaItem[]
  homeSections: LibraryHomeSection[]
  viewCount: number
  totalRecordCount: number
}

export interface EmbyPlaybackTarget {
  itemId: string
  title: string
  url: string
}

interface EmbyPublicInfo {
  Id?: string
  ServerName?: string
  Version?: string
}

interface EmbyAuthResult {
  AccessToken: string
  ServerId?: string
  User: {
    Id: string
    Name: string
  }
}

interface EmbyItemsResponse {
  Items?: EmbyItem[]
  TotalRecordCount?: number
}

interface EmbyView {
  Id: string
  Name?: string
  Type?: string
  CollectionType?: string | null
  ChildCount?: number
  ImageTags?: Record<string, string>
  BackdropImageTags?: string[]
}

interface EmbyViewsResponse {
  Items?: EmbyView[]
  TotalRecordCount?: number
}

interface EmbyUserData {
  IsFavorite?: boolean
  Played?: boolean
  PlaybackPositionTicks?: number
  PlayedPercentage?: number
}

interface EmbyMediaStream {
  Type?: string
  Height?: number
  VideoRange?: string
}

interface EmbyMediaSource {
  Id?: string
  Name?: string
  Container?: string
  Path?: string
  DirectStreamUrl?: string
  TranscodingUrl?: string
  Protocol?: string
  Height?: number
  MediaStreams?: EmbyMediaStream[]
}

interface EmbyItem {
  Id: string
  Name?: string
  OriginalTitle?: string
  SortName?: string
  Type?: string
  DateCreated?: string
  ProductionYear?: number
  RunTimeTicks?: number | null
  CommunityRating?: number
  OfficialRating?: string
  Overview?: string
  Taglines?: string[]
  Genres?: string[]
  ProductionLocations?: string[]
  UserData?: EmbyUserData
  ImageTags?: Record<string, string>
  BackdropImageTags?: string[]
  MediaSources?: EmbyMediaSource[]
  ChildCount?: number
  ParentId?: string
  IndexNumber?: number
  ParentIndexNumber?: number
  SeriesName?: string
}

interface EmbyPlaybackInfoResponse {
  MediaSources?: EmbyMediaSource[]
}

function getDeviceId(): string {
  try {
    const existing = window.localStorage.getItem(DEVICE_ID_KEY)
    if (existing) return existing
    const next = window.crypto?.randomUUID?.() ?? `anvil-${Date.now()}`
    window.localStorage.setItem(DEVICE_ID_KEY, next)
    return next
  } catch {
    return 'anvil-player-web'
  }
}

function authorizationHeader(userId?: string): string {
  const parts = [
    `Client="${CLIENT_NAME}"`,
    `Device="${DEVICE_NAME}"`,
    `DeviceId="${getDeviceId()}"`,
    `Version="${CLIENT_VERSION}"`
  ]
  if (userId) parts.unshift(`UserId="${userId}"`)
  return `MediaBrowser ${parts.join(', ')}`
}

function normalizeServerUrl(serverUrl: string): string {
  const trimmed = serverUrl.trim().replace(/\/+$/, '')
  if (!trimmed) throw new Error('请输入 Emby 服务器地址')
  return /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`
}

function unique(values: string[]): string[] {
  return values.filter((value, index) => values.indexOf(value) === index)
}

function apiUrl(apiBaseUrl: string, path: string, params?: Record<string, string | number | boolean | undefined>): string {
  const url = new URL(`${apiBaseUrl.replace(/\/+$/, '')}/${path.replace(/^\/+/, '')}`)
  Object.entries(params ?? {}).forEach(([key, value]) => {
    if (value !== undefined && value !== '') url.searchParams.set(key, String(value))
  })
  return url.toString()
}

async function fetchJson<T>(url: string, init: RequestInit, label: string): Promise<T> {
  let response: Response
  try {
    response = await fetch(url, {
      ...init,
      headers: {
        Accept: 'application/json',
        ...init.headers
      }
    })
  } catch (error) {
    const networkMessage = error instanceof Error ? error.message : 'network error'
    if (/failed to fetch/i.test(networkMessage)) {
      throw new Error(`${label}失败：无法访问 Emby 服务器。请确认地址能在本机浏览器打开，并重启 Anvil Player 后再试`)
    }
    throw new Error(`${label}失败：${networkMessage}`)
  }
  if (!response.ok) {
    let detail = ''
    try {
      const text = await response.text()
      detail = text ? `: ${text.slice(0, 140)}` : ''
    } catch {
      detail = ''
    }
    throw new Error(`${label}失败 (${response.status})${detail}`)
  }
  return await response.json() as T
}

async function resolveApiBase(serverUrl: string): Promise<{ apiBaseUrl: string; publicInfo: EmbyPublicInfo }> {
  const normalized = normalizeServerUrl(serverUrl)
  const candidates = normalized.toLowerCase().endsWith('/emby')
    ? [normalized]
    : [`${normalized}/emby`, normalized]
  let lastError: unknown

  for (const candidate of unique(candidates)) {
    try {
      const publicInfo = await fetchJson<EmbyPublicInfo>(
        apiUrl(candidate, '/System/Info/Public'),
        { method: 'GET' },
        '读取服务器信息'
      )
      return { apiBaseUrl: candidate, publicInfo }
    } catch (error) {
      lastError = error
    }
  }

  if (lastError instanceof Error) throw lastError
  throw new Error('无法连接 Emby 服务器')
}

function sourceIdForSession(session: EmbySession): string {
  const normalized = session.serverId.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-')
  return `emby-${normalized || 'server'}`
}

function mediaKind(type?: string): MediaItem['type'] {
  switch (type) {
    case 'Movie': return 'movie'
    case 'Series': return 'series'
    default: return 'folder'
  }
}

function ticksToRuntime(ticks?: number | null, fallback?: string): string {
  if (!ticks || ticks <= 0) return fallback ?? ''
  const totalMinutes = Math.max(1, Math.round(ticks / TICKS_PER_MINUTE))
  const hours = Math.floor(totalMinutes / 60)
  const minutes = totalMinutes % 60
  if (!hours) return `${minutes}m`
  return minutes ? `${hours}h ${minutes}m` : `${hours}h`
}

function progressRatio(item: EmbyItem): number {
  const position = item.UserData?.PlaybackPositionTicks ?? 0
  const runtime = item.RunTimeTicks ?? 0
  if (position > 0 && runtime > 0) return Math.min(0.99, Math.max(0, position / runtime))
  const percentage = item.UserData?.PlayedPercentage
  if (percentage && percentage > 0) return Math.min(0.99, Math.max(0, percentage / 100))
  return 0
}

function daysSince(dateValue?: string): number {
  if (!dateValue) return 9999
  const parsed = new Date(dateValue.endsWith('Z') ? dateValue : `${dateValue}Z`)
  const time = parsed.getTime()
  if (!Number.isFinite(time)) return 9999
  return Math.max(0, Math.floor((Date.now() - time) / 86_400_000))
}

function qualityLabel(item: EmbyItem): string {
  const source = item.MediaSources?.[0]
  const videoStream = source?.MediaStreams?.find((stream) => stream.Type === 'Video')
  const height = videoStream?.Height ?? source?.Height ?? 0
  const range = videoStream?.VideoRange && videoStream.VideoRange !== 'SDR' ? ' HDR' : ''

  if (height >= 2160) return `4K${range}`
  if (height >= 1440) return `1440p${range}`
  if (height >= 1080) return `1080p${range}`
  if (height >= 720) return `720p${range}`
  return source?.Container?.toUpperCase() ?? source?.Name ?? '媒体'
}

function imageBackground(session: EmbySession, item: EmbyItem, type: 'Primary' | 'Backdrop'): string {
  const tag = type === 'Primary'
    ? item.ImageTags?.Primary
    : item.BackdropImageTags?.[0]
  if (!tag) {
    return type === 'Primary'
      ? 'linear-gradient(145deg, rgba(82, 181, 75, 0.12), rgba(15, 18, 24, 0.96))'
      : 'linear-gradient(135deg, rgba(37, 44, 56, 0.82), rgba(7, 10, 15, 0.96))'
  }

  const path = type === 'Primary'
    ? `/Items/${item.Id}/Images/Primary`
    : `/Items/${item.Id}/Images/Backdrop/0`
  const imageUrl = apiUrl(session.apiBaseUrl, path, {
    tag,
    quality: 90,
    maxWidth: type === 'Primary' ? 480 : 960
  })
  return `url("${imageUrl}") center / cover`
}

function viewImageBackground(session: EmbySession, view: EmbyView): string {
  const imageType = view.BackdropImageTags?.[0] ? 'Backdrop' : 'Primary'
  const tag = imageType === 'Backdrop' ? view.BackdropImageTags?.[0] : view.ImageTags?.Primary
  if (!tag) return 'linear-gradient(135deg, rgba(82, 181, 75, 0.2), rgba(15, 18, 24, 0.94))'

  const path = imageType === 'Backdrop'
    ? `/Items/${view.Id}/Images/Backdrop/0`
    : `/Items/${view.Id}/Images/Primary`
  const imageUrl = apiUrl(session.apiBaseUrl, path, {
    tag,
    quality: 90,
    maxWidth: 720
  })
  return `url("${imageUrl}") center / cover`
}

function viewSubtitle(view: EmbyView): string {
  switch (view.CollectionType) {
    case 'movies': return '电影库'
    case 'tvshows': return '剧集库'
    case 'music': return '音乐库'
    default: return '媒体库'
  }
}

function itemCardSubtitle(item: EmbyItem): string {
  const pieces = [
    item.ProductionYear ? String(item.ProductionYear) : '',
    item.Type === 'Series' ? '剧集' : item.Type === 'Movie' ? '电影' : '媒体'
  ].filter(Boolean)
  return pieces.join(' · ')
}

interface EmbyViewLatest {
  view: EmbyView
  items: EmbyItem[]
}

interface EmbyViewItems {
  view: EmbyView
  items: EmbyItem[]
  totalRecordCount: number
}

function latestItemTypesForView(view: EmbyView): string {
  switch (view.CollectionType) {
    case 'movies': return 'Movie'
    case 'tvshows': return 'Series'
    default: return 'Movie,Series'
  }
}

function filterSupportedLatestItems(items: EmbyItem[], includeItemTypes: string): EmbyItem[] {
  const allowedTypes = new Set(includeItemTypes.split(','))
  return items.filter((item) => item.Type && allowedTypes.has(item.Type))
}

function buildHomeSections(
  session: EmbySession,
  sourceId: string,
  views: EmbyView[],
  latestByView: EmbyViewLatest[],
  fallbackItems: EmbyItem[]
): LibraryHomeSection[] {
  const sections: LibraryHomeSection[] = []

  if (views.length) {
    sections.push({
      id: `${sourceId}:views`,
      sourceId,
      title: '媒体库',
      layout: 'landscape',
      cards: views.map((view) => ({
        id: `view:${view.Id}`,
        sourceId,
        title: view.Name ?? '媒体库',
        subtitle: viewSubtitle(view),
        image: viewImageBackground(session, view),
        kind: 'view',
        viewId: view.Id
      }))
    })
  }

  latestByView
    .filter((row) => row.items.length > 0)
    .forEach((row) => {
      sections.push({
        id: `${sourceId}:latest:${row.view.Id}`,
        sourceId,
        title: `最新 ${row.view.Name ?? '媒体'}`,
        layout: 'poster',
        cards: row.items.map((item) => ({
          id: `latest:${row.view.Id}:${item.Id}`,
          sourceId,
          title: item.Name ?? '未命名',
          subtitle: itemCardSubtitle(item),
          image: imageBackground(session, item, 'Primary'),
          kind: 'item',
          itemId: item.Id,
          mediaType: mediaKind(item.Type)
        }))
      })
    })

  if (sections.length === (views.length ? 1 : 0) && fallbackItems.length) {
    sections.push({
      id: `${sourceId}:latest`,
      sourceId,
      title: '最新媒体',
      layout: 'poster',
      cards: fallbackItems.map((item) => ({
        id: `latest:${item.Id}`,
        sourceId,
        title: item.Name ?? '未命名',
        subtitle: itemCardSubtitle(item),
        image: imageBackground(session, item, 'Primary'),
        kind: 'item',
        itemId: item.Id,
        mediaType: mediaKind(item.Type)
      }))
    })
  }

  return sections
}

function fallbackRuntime(item: EmbyItem): string {
  if (item.Type === 'Series') return item.ChildCount ? `${item.ChildCount} episodes` : 'Series'
  if (item.Type === 'Folder') return item.ChildCount ? `${item.ChildCount} items` : 'Folder'
  return ''
}

function mapItem(session: EmbySession, sourceId: string, item: EmbyItem, libraryViewId?: string): MediaItem {
  const type = mediaKind(item.Type)
  return {
    id: item.Id,
    title: item.Name ?? '未命名',
    originalTitle: item.OriginalTitle ?? item.SortName ?? item.Name ?? '',
    type,
    year: item.ProductionYear ?? 0,
    rating: item.CommunityRating ?? 0,
    runtime: ticksToRuntime(item.RunTimeTicks, fallbackRuntime(item)),
    sourceId,
    libraryViewId,
    genres: item.Genres?.length ? item.Genres : ['未分类'],
    country: item.ProductionLocations?.[0] ?? '',
    quality: qualityLabel(item),
    progress: progressRatio(item),
    watched: item.UserData?.Played ?? false,
    favorite: item.UserData?.IsFavorite ?? false,
    addedDaysAgo: daysSince(item.DateCreated),
    poster: imageBackground(session, item, 'Primary'),
    backdrop: imageBackground(session, item, 'Backdrop'),
    tagline: item.Taglines?.[0] ?? '',
    overview: item.Overview ?? ''
  }
}

function authHeadersForSession(session: EmbySession): Record<string, string> {
  return {
    'X-Emby-Authorization': authorizationHeader(session.userId),
    'X-Emby-Token': session.accessToken
  }
}

function normalizePlayableHttpUrl(value: string): string {
  try {
    return new URL(value).toString()
  } catch {
    return value
  }
}

function absoluteApiUrl(session: EmbySession, value: string): string {
  const url = /^https?:\/\//i.test(value)
    ? new URL(value)
    : new URL(value, `${session.apiBaseUrl.replace(/\/+$/, '')}/`)
  if (!url.searchParams.has('api_key') && !url.searchParams.has('X-Emby-Token')) {
    url.searchParams.set('api_key', session.accessToken)
  }
  return url.toString()
}

function streamExtension(source: EmbyMediaSource | undefined): string {
  const container = source?.Container
    ?.split(',')
    .map((value) => value.trim())
    .find(Boolean)
  const clean = container?.replace(/[^a-z0-9]/gi, '').toLowerCase()
  return clean ? `.${clean}` : ''
}

function playbackStreamUrl(session: EmbySession, itemId: string, source: EmbyMediaSource | undefined): string {
  if (source?.Protocol === 'Http' && source.Path && /^https?:\/\//i.test(source.Path)) {
    return normalizePlayableHttpUrl(source.Path)
  }

  if (source?.DirectStreamUrl) {
    return absoluteApiUrl(session, source.DirectStreamUrl)
  }

  const mediaSourceId = source?.Id
  return apiUrl(session.apiBaseUrl, `/Videos/${itemId}/stream${streamExtension(source)}`, {
    Static: true,
    api_key: session.accessToken,
    MediaSourceId: mediaSourceId,
    DeviceId: getDeviceId()
  })
}

async function assertPlayableHttpUrl(url: string): Promise<void> {
  if (!/^https?:\/\//i.test(url)) return

  let response: Response
  try {
    response = await fetch(url, { method: 'HEAD', redirect: 'follow' })
  } catch {
    return
  }

  if (!response.ok) {
    if (response.status === 405) return
    throw new Error(`播放地址不可用 (${response.status})`)
  }

  const contentType = response.headers.get('content-type')?.toLowerCase() ?? ''
  const looksLikeErrorBody = contentType.includes('application/json') ||
    contentType.includes('text/html') ||
    contentType.includes('text/plain')
  if (!looksLikeErrorBody) return

  let detail = ''
  try {
    const body = await fetch(url, {
      method: 'GET',
      redirect: 'follow',
      headers: { Range: 'bytes=0-2047' }
    })
    const text = (await body.text()).trim()
    if (text) {
      try {
        const parsed = JSON.parse(text) as { message?: unknown; error?: unknown }
        detail = String(parsed.message ?? parsed.error ?? text)
      } catch {
        detail = text.slice(0, 160)
      }
    }
  } catch {
    detail = contentType
  }

  throw new Error(`播放地址返回的不是视频${detail ? `：${detail}` : ''}`)
}

async function fetchPlaybackMediaSource(session: EmbySession, itemId: string): Promise<EmbyMediaSource | undefined> {
  const response = await fetchJson<EmbyPlaybackInfoResponse>(
    apiUrl(session.apiBaseUrl, `/Items/${itemId}/PlaybackInfo`, { api_key: session.accessToken }),
    { method: 'GET' },
    '读取 Emby 播放信息'
  )
  return response.MediaSources?.[0]
}

function episodeOrderValue(item: EmbyItem): number {
  const season = item.ParentIndexNumber ?? 0
  const episode = item.IndexNumber ?? 0
  return season * 10000 + episode
}

function choosePlayableEpisode(items: EmbyItem[]): EmbyItem | undefined {
  const sorted = [...items].sort((left, right) => episodeOrderValue(left) - episodeOrderValue(right))
  return sorted.find((item) => progressRatio(item) > 0 && progressRatio(item) < 1)
    ?? sorted.find((item) => !item.UserData?.Played)
    ?? sorted[0]
}

export async function resolveEmbyPlaybackTarget(
  session: EmbySession,
  item: MediaItem
): Promise<EmbyPlaybackTarget> {
  const fields = [
    'MediaSources',
    'UserData',
    'RunTimeTicks',
    'IndexNumber',
    'ParentIndexNumber',
    'SeriesName'
  ].join(',')

  if (item.type === 'movie') {
    const detail = await fetchJson<EmbyItem>(
      apiUrl(session.apiBaseUrl, `/Users/${session.userId}/Items/${item.id}`, {
        Fields: fields,
        api_key: session.accessToken
      }),
      { method: 'GET' },
      '读取 Emby 播放信息'
    )
    const source = await fetchPlaybackMediaSource(session, detail.Id)
    const url = playbackStreamUrl(session, detail.Id, source ?? detail.MediaSources?.[0])
    return {
      itemId: detail.Id,
      title: detail.Name ?? item.title,
      url
    }
  }

  if (item.type === 'series') {
    const episodes = await fetchJson<EmbyItemsResponse>(
      apiUrl(session.apiBaseUrl, `/Shows/${item.id}/Episodes`, {
        UserId: session.userId,
        Fields: fields,
        SortBy: 'ParentIndexNumber,IndexNumber',
        SortOrder: 'Ascending',
        api_key: session.accessToken
      }),
      { method: 'GET' },
      '读取 Emby 剧集'
    )
    const episode = choosePlayableEpisode(episodes.Items ?? [])
    if (!episode) {
      throw new Error('这个剧集没有可播放的分集')
    }
    const titleParts = [
      episode.SeriesName ?? item.title,
      episode.ParentIndexNumber && episode.IndexNumber
        ? `S${episode.ParentIndexNumber}:E${episode.IndexNumber}`
        : '',
      episode.Name ?? ''
    ].filter(Boolean)
    const source = await fetchPlaybackMediaSource(session, episode.Id)
    const url = playbackStreamUrl(session, episode.Id, source ?? episode.MediaSources?.[0])
    return {
      itemId: episode.Id,
      title: titleParts.join(' - '),
      url
    }
  }

  throw new Error('这个条目暂时不能直接播放')
}

async function loadLibraryForSession(
  session: EmbySession,
  displayName: string | undefined,
  limit: number
): Promise<EmbyLibrarySnapshot> {
  const apiBaseUrl = session.apiBaseUrl
  const authHeaders = authHeadersForSession(session)
  const fields = [
    'PrimaryImageAspectRatio',
    'Overview',
    'Genres',
    'ProductionYear',
    'RunTimeTicks',
    'DateCreated',
    'UserData',
    'CommunityRating',
    'OfficialRating',
    'MediaSources',
    'OriginalTitle',
    'Taglines',
    'ProductionLocations',
    'BackdropImageTags'
  ].join(',')

  const [views, items] = await Promise.all([
    fetchJson<EmbyViewsResponse>(
      apiUrl(apiBaseUrl, `/Users/${session.userId}/Views`),
      { method: 'GET', headers: authHeaders },
      '读取媒体库'
    ),
    fetchJson<EmbyItemsResponse>(
      apiUrl(apiBaseUrl, `/Users/${session.userId}/Items`, {
        Recursive: true,
        IncludeItemTypes: 'Movie,Series',
        Fields: fields,
        SortBy: 'DateCreated',
        SortOrder: 'Descending',
        Limit: limit
      }),
      { method: 'GET', headers: authHeaders },
      '读取媒体条目'
    )
  ])

  const sourceId = sourceIdForSession(session)
  const viewRows = views.Items ?? []
  const mediaItems = items.Items ?? []
  const itemsByView = await Promise.all(viewRows.map(async (view): Promise<EmbyViewItems> => {
    const includeItemTypes = latestItemTypesForView(view)
    try {
      const response = await fetchJson<EmbyItemsResponse>(
        apiUrl(apiBaseUrl, `/Users/${session.userId}/Items`, {
          ParentId: view.Id,
          Recursive: true,
          IncludeItemTypes: includeItemTypes,
          Fields: fields,
          SortBy: 'DateCreated',
          SortOrder: 'Descending',
          Limit: limit
        }),
        { method: 'GET', headers: authHeaders },
        '读取媒体库条目'
      )
      return {
        view,
        items: response.Items ?? [],
        totalRecordCount: response.TotalRecordCount ?? response.Items?.length ?? 0
      }
    } catch {
      return { view, items: [], totalRecordCount: 0 }
    }
  }))
  const latestByView = await Promise.all(viewRows.map(async (view, index): Promise<EmbyViewLatest> => {
    const includeItemTypes = latestItemTypesForView(view)
    try {
      const latest = await fetchJson<EmbyItem[]>(
        apiUrl(apiBaseUrl, `/Users/${session.userId}/Items/Latest`, {
          ParentId: view.Id,
          Limit: 16,
          Fields: fields,
          IncludeItemTypes: includeItemTypes,
          GroupItems: false
        }),
        { method: 'GET', headers: authHeaders },
        '读取最新媒体'
      )
      const supportedLatest = filterSupportedLatestItems(latest, includeItemTypes)
      if (supportedLatest.length) return { view, items: supportedLatest }
    } catch {
      // Some Emby libraries do not expose Series rows through Items/Latest.
    }

    return { view, items: itemsByView[index]?.items.slice(0, 16) ?? [] }
  }))
  const itemRows = new Map<string, { item: EmbyItem; libraryViewId?: string }>()
  itemsByView.forEach((row) => {
    row.items.forEach((item) => {
      if (!itemRows.has(item.Id)) itemRows.set(item.Id, { item, libraryViewId: row.view.Id })
    })
  })
  mediaItems.forEach((item) => {
    if (!itemRows.has(item.Id)) itemRows.set(item.Id, { item })
  })
  const mappedItems = [...itemRows.values()].map((row) => mapItem(session, sourceId, row.item, row.libraryViewId))
  const homeSections = buildHomeSections(
    session,
    sourceId,
    viewRows,
    latestByView,
    mediaItems.slice(0, Math.min(36, mediaItems.length))
  )
  const viewItemCount = itemsByView.reduce((sum, row) => sum + row.totalRecordCount, 0)
  const totalRecordCount = items.TotalRecordCount ?? (viewItemCount > 0 ? viewItemCount : mediaItems.length)
  const source: LibrarySource = {
    id: sourceId,
    name: displayName?.trim() || session.serverName,
    kind: 'Emby',
    status: 'online',
    itemCount: totalRecordCount,
    location: apiBaseUrl
  }

  return {
    session,
    source,
    items: mappedItems,
    homeSections,
    viewCount: views.TotalRecordCount ?? views.Items?.length ?? 0,
    totalRecordCount
  }
}

export async function loadEmbyLibrary(input: EmbyConnectionInput, limit = DEFAULT_LIMIT): Promise<EmbyLibrarySnapshot> {
  const { apiBaseUrl, publicInfo } = await resolveApiBase(input.serverUrl)
  const auth = await fetchJson<EmbyAuthResult>(
    apiUrl(apiBaseUrl, '/Users/AuthenticateByName'),
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-Emby-Authorization': authorizationHeader()
      },
      body: JSON.stringify({ Username: input.username.trim(), Pw: input.password })
    },
    '登录 Emby'
  )

  const session: EmbySession = {
    apiBaseUrl,
    serverId: auth.ServerId ?? publicInfo.Id ?? apiBaseUrl,
    serverName: publicInfo.ServerName ?? input.displayName?.trim() ?? 'Emby',
    serverVersion: publicInfo.Version ?? '',
    userId: auth.User.Id,
    userName: auth.User.Name,
    accessToken: auth.AccessToken
  }

  return await loadLibraryForSession(session, input.displayName, limit)
}

export async function refreshEmbyLibrary(
  session: EmbySession,
  displayName?: string,
  limit = DEFAULT_LIMIT
): Promise<EmbyLibrarySnapshot> {
  return await loadLibraryForSession(session, displayName, limit)
}
