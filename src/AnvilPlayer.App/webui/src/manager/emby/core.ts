// Shared Emby foundation: protocol types, connection primitives and auth.
// Extracted from embyClient.ts so the mapping, search, library and playback
// layers can depend on a single transport layer instead of each reaching back
// into the monolith.

export const CLIENT_NAME = 'Anvil Player'
export const CLIENT_VERSION = '1.0.0'
export const DEVICE_NAME = 'Anvil Library'
export const DEVICE_ID_KEY = 'anvil-player.emby.device-id'
export const PLAYBACK_REPORT_STORAGE_KEY = 'anvil-player.emby.playback-report.v1'
export const EMBY_PLAYBACK_REPORT_PENDING_EVENT = 'anvil-emby-playback-report-pending'
export const DEFAULT_LIMIT = 120
export const TICKS_PER_SECOND = 10_000_000
export const TICKS_PER_MINUTE = TICKS_PER_SECOND * 60
export const TICKS_PER_MILLISECOND = 10_000
export const EMBY_ITEM_FIELDS = [
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
  'BackdropImageTags',
  'ParentBackdropImageTags',
  'ParentBackdropItemId',
  'ParentThumbImageTag',
  'ParentThumbItemId',
  'SeriesId',
  'SeriesPrimaryImageTag',
  'IndexNumber',
  'ParentIndexNumber',
  'SeriesName',
  'People',
  'Studios',
  'Tags',
  'RemoteTrailers',
  'Path'
].join(',')

export interface EmbySession {
  apiBaseUrl: string
  serverId: string
  serverName: string
  serverVersion: string
  userId: string
  userName: string
  accessToken: string
}

export interface EmbyPlaybackTarget {
  itemId: string
  title: string
  url: string
  mediaSourceId?: string
  playSessionId?: string
  runTimeTicks?: number
}

export interface EmbyPublicInfo {
  Id?: string
  ServerName?: string
  Version?: string
}

export interface EmbyAuthResult {
  AccessToken: string
  ServerId?: string
  User: {
    Id: string
    Name: string
  }
}

export interface EmbyMediaStream {
  Index?: number
  Type?: string
  Codec?: string
  Profile?: string
  Level?: number
  AspectRatio?: string
  DisplayTitle?: string
  Title?: string
  Language?: string
  DisplayLanguage?: string
  Width?: number
  Height?: number
  BitRate?: number
  BitDepth?: number
  RealFrameRate?: number
  AverageFrameRate?: number
  VideoRange?: string
  VideoRangeType?: string
  ColorPrimaries?: string
  ColorSpace?: string
  ColorTransfer?: string
  Channels?: number
  ChannelLayout?: string
  SampleRate?: number
  IsDefault?: boolean
  IsForced?: boolean
  IsExternal?: boolean
}

export interface EmbyMediaSource {
  Id?: string
  Name?: string
  Container?: string
  Bitrate?: number
  Path?: string
  DirectStreamUrl?: string
  TranscodingUrl?: string
  Protocol?: string
  Height?: number
  RunTimeTicks?: number | null
  MediaStreams?: EmbyMediaStream[]
}

export interface EmbyItem {
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
  ParentBackdropImageTags?: string[]
  ParentBackdropItemId?: string
  ParentThumbImageTag?: string
  ParentThumbItemId?: string
  SeriesId?: string
  SeriesPrimaryImageTag?: string
  MediaSources?: EmbyMediaSource[]
  People?: EmbyPerson[]
  Studios?: Array<{ Name?: string }>
  Tags?: string[]
  RemoteTrailers?: Array<{ Name?: string; Url?: string }>
  Path?: string
  ChildCount?: number
  ParentId?: string
  IndexNumber?: number
  ParentIndexNumber?: number
  SeriesName?: string
}

export interface EmbyPerson {
  Id?: string
  Name?: string
  Role?: string
  Type?: string
  PrimaryImageTag?: string
}

export interface EmbyView {
  Id: string
  Name?: string
  Type?: string
  CollectionType?: string | null
  ChildCount?: number
  ImageTags?: Record<string, string>
  BackdropImageTags?: string[]
}

export interface EmbyUserData {
  IsFavorite?: boolean
  Played?: boolean
  PlaybackPositionTicks?: number
  PlayedPercentage?: number
  LastPlayedDate?: string
}

export interface EmbyItemsResponse {
  Items?: EmbyItem[]
  TotalRecordCount?: number
}

export interface EmbyViewsResponse {
  Items?: EmbyView[]
  TotalRecordCount?: number
}

export interface EmbyPlaybackInfoResponse {
  MediaSources?: EmbyMediaSource[]
  PlaySessionId?: string
}

export function getDeviceId(): string {
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

export function authorizationHeader(userId?: string): string {
  const parts = [
    `Client="${CLIENT_NAME}"`,
    `Device="${DEVICE_NAME}"`,
    `DeviceId="${getDeviceId()}"`,
    `Version="${CLIENT_VERSION}"`
  ]
  if (userId) parts.unshift(`UserId="${userId}"`)
  return `MediaBrowser ${parts.join(', ')}`
}

export function normalizeServerUrl(serverUrl: string): string {
  const trimmed = serverUrl.trim().replace(/\/+$/, '')
  if (!trimmed) throw new Error('请输入 Emby 服务器地址')
  return /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`
}

export function unique(values: string[]): string[] {
  return values.filter((value, index) => values.indexOf(value) === index)
}

export function apiUrl(apiBaseUrl: string, path: string, params?: Record<string, string | number | boolean | undefined>): string {
  const url = new URL(`${apiBaseUrl.replace(/\/+$/, '')}/${path.replace(/^\/+/, '')}`)
  Object.entries(params ?? {}).forEach(([key, value]) => {
    if (value !== undefined && value !== '') url.searchParams.set(key, String(value))
  })
  return url.toString()
}

export async function fetchJson<T>(url: string, init: RequestInit, label: string): Promise<T> {
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

export async function fetchEmpty(url: string, init: RequestInit, label: string): Promise<void> {
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
    throw new Error(`${label} failed: ${networkMessage}`)
  }
  if (!response.ok) {
    let detail = ''
    try {
      const text = await response.text()
      detail = text ? `: ${text.slice(0, 140)}` : ''
    } catch {
      detail = ''
    }
    throw new Error(`${label} failed (${response.status})${detail}`)
  }
}

export async function resolveApiBase(serverUrl: string): Promise<{ apiBaseUrl: string; publicInfo: EmbyPublicInfo }> {
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

export function sourceIdForSession(session: EmbySession): string {
  const normalized = session.serverId.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-')
  return `emby-${normalized || 'server'}`
}

export function authHeadersForSession(session: EmbySession): Record<string, string> {
  return {
    'X-Emby-Authorization': authorizationHeader(session.userId),
    'X-Emby-Token': session.accessToken
  }
}

export function millisToTicks(positionMs: number): number {
  return Math.max(0, Math.round(positionMs * TICKS_PER_MILLISECOND))
}
