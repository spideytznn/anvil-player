import type { LibrarySource, MediaItem } from './types'

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
  viewCount: number
  totalRecordCount: number
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

interface EmbyViewsResponse {
  Items?: Array<{ Id: string; Name: string; Type?: string }>
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
  Name?: string
  Container?: string
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
  if (!tag) return type === 'Primary' ? '#181815' : '#0d0d0c'

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

function fallbackRuntime(item: EmbyItem): string {
  if (item.Type === 'Series') return item.ChildCount ? `${item.ChildCount} episodes` : 'Series'
  if (item.Type === 'Folder') return item.ChildCount ? `${item.ChildCount} items` : 'Folder'
  return ''
}

function mapItem(session: EmbySession, sourceId: string, item: EmbyItem): MediaItem {
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

  const authHeaders = {
    'X-Emby-Authorization': authorizationHeader(session.userId),
    'X-Emby-Token': session.accessToken
  }
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
        IncludeItemTypes: 'Movie,Series,Folder',
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
  const totalRecordCount = items.TotalRecordCount ?? items.Items?.length ?? 0
  const source: LibrarySource = {
    id: sourceId,
    name: input.displayName?.trim() || session.serverName,
    kind: 'Emby',
    status: 'online',
    itemCount: totalRecordCount,
    location: apiBaseUrl
  }

  return {
    session,
    source,
    items: (items.Items ?? []).map((item) => mapItem(session, sourceId, item)),
    viewCount: views.TotalRecordCount ?? views.Items?.length ?? 0,
    totalRecordCount
  }
}
