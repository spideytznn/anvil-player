import type {
  EpisodeItem,
  LibraryHomeSection,
  LibrarySource,
  LibraryView,
  MediaFilterKey,
  MediaItem,
  MediaStreamSpec,
  PersonCredit,
  SeasonItem,
  SortKey,
  SortOrder
} from './types'

const CLIENT_NAME = 'Anvil Player'
const CLIENT_VERSION = '0.1.0'
const DEVICE_NAME = 'Anvil Library'
const DEVICE_ID_KEY = 'anvil-player.emby.device-id'
const PLAYBACK_REPORT_STORAGE_KEY = 'anvil-player.emby.playback-report.v1'
const DEFAULT_LIMIT = 120
const TICKS_PER_SECOND = 10_000_000
const TICKS_PER_MINUTE = TICKS_PER_SECOND * 60
const TICKS_PER_MILLISECOND = 10_000
const EMBY_ITEM_FIELDS = [
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
  'Path'
].join(',')

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

export interface EmbySearchOptions {
  libraryViewId?: string
  view?: LibraryView
  filterKey?: MediaFilterKey
  sortKey?: SortKey
  sortOrder?: SortOrder
  limit?: number
}

export interface EmbyPlaybackTarget {
  itemId: string
  title: string
  url: string
  mediaSourceId?: string
  playSessionId?: string
  runTimeTicks?: number
}

export interface EmbyPlaybackReportRecord {
  id: string
  createdAt: number
  session: EmbySession
  target: EmbyPlaybackTarget
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
  LastPlayedDate?: string
}

interface EmbyMediaStream {
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

interface EmbyMediaSource {
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
  Path?: string
  ChildCount?: number
  ParentId?: string
  IndexNumber?: number
  ParentIndexNumber?: number
  SeriesName?: string
}

interface EmbyPerson {
  Id?: string
  Name?: string
  Role?: string
  Type?: string
  PrimaryImageTag?: string
}

interface EmbyPlaybackInfoResponse {
  MediaSources?: EmbyMediaSource[]
  PlaySessionId?: string
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

async function fetchEmpty(url: string, init: RequestInit, label: string): Promise<void> {
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
    case 'BoxSet': return 'folder'
    case 'Movie': return 'movie'
    case 'Episode': return 'movie'
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

function cleanParts(parts: Array<string | undefined>): string {
  const rows = parts
    .map((part) => part?.trim() ?? '')
    .filter(Boolean)
  return rows.filter((part, index) => rows.indexOf(part) === index).join(' · ')
}

function codecLabel(codec?: string): string {
  const normalized = codec?.trim().toLowerCase() ?? ''
  switch (normalized) {
    case 'hevc':
    case 'h265':
      return 'H.265'
    case 'h264':
    case 'avc':
      return 'H.264'
    case 'av1':
      return 'AV1'
    case 'mpeg2video':
      return 'MPEG-2'
    case 'eac3':
      return 'E-AC-3'
    case 'ac3':
      return 'AC-3'
    case 'truehd':
      return 'TrueHD'
    case 'dca':
    case 'dts':
      return 'DTS'
    case 'dtshd_ma':
      return 'DTS-HD MA'
    case 'aac':
      return 'AAC'
    case 'flac':
      return 'FLAC'
    case 'opus':
      return 'Opus'
    case 'mp3':
      return 'MP3'
    default:
      return codec?.trim() ?? ''
  }
}

function numberLabel(value: number, fractionDigits = 1): string {
  return Number(value.toFixed(fractionDigits)).toString()
}

function bitRateLabel(bitRate?: number): string {
  if (!bitRate || bitRate <= 0) return ''
  if (bitRate >= 1_000_000) return `${numberLabel(bitRate / 1_000_000, 1)} Mbps`
  return `${Math.round(bitRate / 1000)} kbps`
}

function frameRateLabel(frameRate?: number): string {
  if (!frameRate || frameRate <= 0) return ''
  return `${numberLabel(frameRate, frameRate % 1 === 0 ? 0 : 3)} fps`
}

function sampleRateLabel(sampleRate?: number): string {
  if (!sampleRate || sampleRate <= 0) return ''
  return `${numberLabel(sampleRate / 1000, 1)} kHz`
}

function channelLabel(stream: EmbyMediaStream): string {
  if (stream.ChannelLayout) return stream.ChannelLayout.replace(/\s+/g, ' ').trim()
  switch (stream.Channels) {
    case 8: return '7.1'
    case 7: return '6.1'
    case 6: return '5.1'
    case 2: return '2.0'
    case 1: return 'Mono'
    default: return stream.Channels ? `${stream.Channels}ch` : ''
  }
}

function streamProfileLabel(stream: EmbyMediaStream, codec: string): string {
  const profile = stream.Profile?.trim() ?? ''
  if (!profile) return ''
  return codec.toLowerCase().includes(profile.toLowerCase()) ? '' : profile
}

function videoSpecLabel(item: EmbyItem): string {
  const source = item.MediaSources?.[0]
  const stream = source?.MediaStreams?.find((row) => row.Type === 'Video')
  if (!source || !stream) return ''

  const codec = codecLabel(stream.Codec)
  const resolution = stream.Width && stream.Height
    ? `${stream.Width}x${stream.Height}`
    : qualityLabel(item)
  const range = stream.VideoRange && stream.VideoRange !== 'SDR' ? stream.VideoRange : ''
  const bitDepth = stream.BitDepth ? `${stream.BitDepth}-bit` : ''
  const container = source.Container?.trim() ? source.Container.trim().toUpperCase() : ''

  return cleanParts([
    codec,
    streamProfileLabel(stream, codec),
    resolution,
    bitDepth,
    range,
    frameRateLabel(stream.RealFrameRate ?? stream.AverageFrameRate),
    bitRateLabel(stream.BitRate ?? source.Bitrate),
    container
  ])
}

function audioSpecLabel(item: EmbyItem): string {
  const audioStreams = item.MediaSources?.[0]?.MediaStreams?.filter((row) => row.Type === 'Audio') ?? []
  const stream = audioStreams.find((row) => row.IsDefault) ?? audioStreams[0]
  if (!stream) return ''

  const codec = codecLabel(stream.Codec)
  const language = stream.DisplayLanguage?.trim() || stream.Language?.trim().toUpperCase() || ''
  const extraTracks = audioStreams.length > 1 ? `+${audioStreams.length - 1} 音轨` : ''

  return cleanParts([
    codec,
    streamProfileLabel(stream, codec),
    channelLabel(stream),
    sampleRateLabel(stream.SampleRate),
    bitRateLabel(stream.BitRate),
    language,
    extraTracks
  ])
}

function personImageBackground(session: EmbySession, person: EmbyPerson): string {
  if (!person.Id || !person.PrimaryImageTag) {
    return 'linear-gradient(145deg, rgba(128, 139, 155, 0.2), rgba(17, 21, 28, 0.96))'
  }
  const imageUrl = apiUrl(session.apiBaseUrl, `/Items/${person.Id}/Images/Primary`, {
    tag: person.PrimaryImageTag,
    quality: 88,
    maxWidth: 280
  })
  return `url("${imageUrl}") center / cover`
}

function mapCast(session: EmbySession, item: EmbyItem): PersonCredit[] {
  return (item.People ?? [])
    .filter((person) => person.Name && (person.Type === 'Actor' || person.Role))
    .slice(0, 24)
    .map((person) => ({
      id: person.Id ?? `${item.Id}:${person.Name}`,
      name: person.Name ?? '',
      role: person.Role || person.Type || '',
      image: personImageBackground(session, person)
    }))
}

function valueRow(label: string, value: string | undefined): { label: string; value: string } | undefined {
  const trimmed = value?.trim() ?? ''
  return trimmed ? { label, value: trimmed } : undefined
}

function booleanRow(label: string, value: boolean | undefined): { label: string; value: string } | undefined {
  return value === undefined ? undefined : { label, value: value ? '是' : '否' }
}

function streamTitle(stream: EmbyMediaStream, fallback: string): string {
  return stream.DisplayTitle?.trim() || stream.Title?.trim() || fallback
}

function mapVideoStreamSpec(stream: EmbyMediaStream, index: number): MediaStreamSpec {
  const codec = codecLabel(stream.Codec)
  const resolution = stream.Width && stream.Height ? `${stream.Width}x${stream.Height}` : ''
  const color = cleanParts([stream.VideoRange, stream.VideoRangeType, stream.ColorSpace])
  return {
    id: `video-${stream.Index ?? index}`,
    type: 'video',
    title: streamTitle(stream, `视频 ${index + 1}`),
    subtitle: cleanParts([codec, resolution, frameRateLabel(stream.RealFrameRate ?? stream.AverageFrameRate)]),
    isDefault: stream.IsDefault,
    details: [
      valueRow('编码', codec),
      valueRow('配置', stream.Profile),
      valueRow('分辨率', resolution),
      valueRow('宽高比', stream.AspectRatio),
      valueRow('帧率', frameRateLabel(stream.RealFrameRate ?? stream.AverageFrameRate)),
      valueRow('码率', bitRateLabel(stream.BitRate)),
      valueRow('位深', stream.BitDepth ? `${stream.BitDepth}-bit` : ''),
      valueRow('色彩', color),
      valueRow('色彩基准', stream.ColorPrimaries),
      valueRow('传递函数', stream.ColorTransfer),
      booleanRow('默认', stream.IsDefault)
    ].filter((row): row is { label: string; value: string } => Boolean(row))
  }
}

function mapAudioStreamSpec(stream: EmbyMediaStream, index: number): MediaStreamSpec {
  const codec = codecLabel(stream.Codec)
  const language = stream.DisplayLanguage?.trim() || stream.Language?.trim().toUpperCase() || ''
  return {
    id: `audio-${stream.Index ?? index}`,
    type: 'audio',
    title: streamTitle(stream, `音频 ${index + 1}`),
    subtitle: cleanParts([codec, channelLabel(stream), language]),
    isDefault: stream.IsDefault,
    details: [
      valueRow('编码', codec),
      valueRow('配置', stream.Profile),
      valueRow('语言', language),
      valueRow('声道', channelLabel(stream)),
      valueRow('采样率', sampleRateLabel(stream.SampleRate)),
      valueRow('码率', bitRateLabel(stream.BitRate)),
      booleanRow('默认', stream.IsDefault)
    ].filter((row): row is { label: string; value: string } => Boolean(row))
  }
}

function mapSubtitleStreamSpec(stream: EmbyMediaStream, index: number): MediaStreamSpec {
  const codec = codecLabel(stream.Codec)
  const language = stream.DisplayLanguage?.trim() || stream.Language?.trim().toUpperCase() || ''
  return {
    id: `subtitle-${stream.Index ?? index}`,
    type: 'subtitle',
    title: streamTitle(stream, `字幕 ${index + 1}`),
    subtitle: cleanParts([language, codec]),
    isDefault: stream.IsDefault,
    isForced: stream.IsForced,
    details: [
      valueRow('编码', codec),
      valueRow('语言', language),
      booleanRow('默认', stream.IsDefault),
      booleanRow('强制', stream.IsForced),
      booleanRow('外挂', stream.IsExternal)
    ].filter((row): row is { label: string; value: string } => Boolean(row))
  }
}

function mapStreamSpecs(item: EmbyItem): MediaStreamSpec[] {
  const streams = item.MediaSources?.[0]?.MediaStreams ?? []
  return streams.map((stream, index) => {
    switch (stream.Type) {
      case 'Video': return mapVideoStreamSpec(stream, index)
      case 'Audio': return mapAudioStreamSpec(stream, index)
      case 'Subtitle': return mapSubtitleStreamSpec(stream, index)
      default: return undefined
    }
  }).filter((stream): stream is MediaStreamSpec => Boolean(stream))
}

function imageUrlBackground(session: EmbySession, path: string, tag: string, maxWidth: number): string {
  const imageUrl = apiUrl(session.apiBaseUrl, path, {
    tag,
    quality: 90,
    maxWidth
  })
  return `url("${imageUrl}") center / cover`
}

function imageBackground(session: EmbySession, item: EmbyItem, type: 'Primary' | 'Backdrop'): string {
  const tag = type === 'Primary'
    ? item.ImageTags?.Primary
    : item.BackdropImageTags?.[0]
  if (tag) {
    const path = type === 'Primary'
      ? `/Items/${item.Id}/Images/Primary`
      : `/Items/${item.Id}/Images/Backdrop/0`
    return imageUrlBackground(session, path, tag, type === 'Primary' ? 480 : 960)
  }

  if (type === 'Primary' && item.SeriesId && item.SeriesPrimaryImageTag) {
    return imageUrlBackground(session, `/Items/${item.SeriesId}/Images/Primary`, item.SeriesPrimaryImageTag, 480)
  }

  if (type === 'Backdrop') {
    const parentBackdropTag = item.ParentBackdropImageTags?.[0]
    const parentBackdropItemId = item.ParentBackdropItemId || item.SeriesId
    if (parentBackdropTag && parentBackdropItemId) {
      return imageUrlBackground(session, `/Items/${parentBackdropItemId}/Images/Backdrop/0`, parentBackdropTag, 960)
    }
    if (item.ParentThumbImageTag && item.ParentThumbItemId) {
      return imageUrlBackground(session, `/Items/${item.ParentThumbItemId}/Images/Thumb`, item.ParentThumbImageTag, 960)
    }
    if (item.SeriesId && item.SeriesPrimaryImageTag) {
      return imageUrlBackground(session, `/Items/${item.SeriesId}/Images/Primary`, item.SeriesPrimaryImageTag, 720)
    }
  }

  return type === 'Primary'
    ? 'linear-gradient(145deg, rgba(82, 181, 75, 0.12), rgba(15, 18, 24, 0.96))'
    : 'linear-gradient(135deg, rgba(37, 44, 56, 0.82), rgba(7, 10, 15, 0.96))'
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

function itemTypesForLibraryView(view: LibraryView | undefined): string {
  switch (view) {
    case 'movies': return 'BoxSet,Movie,Episode'
    case 'series': return 'Series,Episode'
    case 'folders': return 'Folder'
    default: return 'BoxSet,Movie,Series,Episode'
  }
}

function searchResultPriority(item: EmbyItem): number {
  switch (item.Type) {
    case 'BoxSet': return 0
    case 'Movie': return 1
    case 'Series': return 2
    case 'Episode': return 3
    default: return 4
  }
}

function orderSearchResults(items: EmbyItem[]): EmbyItem[] {
  return items
    .map((item, index) => ({ item, index }))
    .sort((a, b) => {
      const priority = searchResultPriority(a.item) - searchResultPriority(b.item)
      return priority || a.index - b.index
    })
    .map((row) => row.item)
}

function sortParamsForSearch(sortKey: SortKey | undefined, sortOrder: SortOrder | undefined): { SortBy: string; SortOrder: 'Ascending' | 'Descending' } {
  const order = sortOrder === 'ascending' ? 'Ascending' : 'Descending'
  switch (sortKey) {
    case 'title': return { SortBy: 'SortName', SortOrder: order }
    case 'rating': return { SortBy: 'CommunityRating', SortOrder: order }
    case 'year': return { SortBy: 'ProductionYear', SortOrder: order }
    default: return { SortBy: 'DateCreated', SortOrder: order }
  }
}

function filterParamsForSearch(filterKey: MediaFilterKey | undefined): Record<string, string | boolean | undefined> {
  switch (filterKey) {
    case 'unwatched': return { IsPlayed: false }
    case 'watched': return { IsPlayed: true }
    case 'favorites': return { IsFavorite: true }
    case 'inProgress': return { Filters: 'IsResumable' }
    default: return {}
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

function mapItem(
  session: EmbySession,
  sourceId: string,
  item: EmbyItem,
  libraryViewId?: string,
  continueWatching = false
): MediaItem {
  const type = mediaKind(item.Type)
  const episodeTitle = item.Type === 'Episode' && item.SeriesName
    ? [
        item.SeriesName,
        item.ParentIndexNumber && item.IndexNumber ? `S${item.ParentIndexNumber}:E${item.IndexNumber}` : '',
        item.Name ?? ''
      ].filter(Boolean).join(' - ')
    : undefined
  return {
    id: item.Id,
    title: episodeTitle ?? item.Name ?? '未命名',
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
    videoSpec: videoSpecLabel(item),
    audioSpec: audioSpecLabel(item),
    progress: progressRatio(item),
    continueWatching,
    watched: item.UserData?.Played ?? false,
    favorite: item.UserData?.IsFavorite ?? false,
    addedDaysAgo: daysSince(item.UserData?.LastPlayedDate ?? item.DateCreated),
    poster: imageBackground(session, item, 'Primary'),
    backdrop: imageBackground(session, item, 'Backdrop'),
    tagline: item.Taglines?.[0] ?? '',
    overview: item.Overview ?? '',
    cast: mapCast(session, item),
    streamSpecs: mapStreamSpecs(item),
    studios: item.Studios?.map((studio) => studio.Name ?? '').filter(Boolean),
    tags: item.Tags ?? [],
    path: item.Path
  }
}

function authHeadersForSession(session: EmbySession): Record<string, string> {
  return {
    'X-Emby-Authorization': authorizationHeader(session.userId),
    'X-Emby-Token': session.accessToken
  }
}

function millisToTicks(positionMs: number): number {
  return Math.max(0, Math.round(positionMs * TICKS_PER_MILLISECOND))
}

function stringValue(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

function numberValue(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined
}

function objectValue(value: unknown): Record<string, unknown> | undefined {
  return value && typeof value === 'object' ? value as Record<string, unknown> : undefined
}

function loadReportTarget(value: unknown): EmbyPlaybackTarget | undefined {
  const target = objectValue(value)
  if (!target) return undefined
  const itemId = stringValue(target.itemId)
  const title = stringValue(target.title)
  const url = stringValue(target.url)
  if (!itemId || !url) return undefined
  return {
    itemId,
    title,
    url,
    mediaSourceId: stringValue(target.mediaSourceId) || undefined,
    playSessionId: stringValue(target.playSessionId) || undefined,
    runTimeTicks: numberValue(target.runTimeTicks)
  }
}

function loadReportSession(value: unknown): EmbySession | undefined {
  const session = objectValue(value)
  if (!session) return undefined
  const record: EmbySession = {
    apiBaseUrl: stringValue(session.apiBaseUrl),
    serverId: stringValue(session.serverId),
    serverName: stringValue(session.serverName),
    serverVersion: stringValue(session.serverVersion),
    userId: stringValue(session.userId),
    userName: stringValue(session.userName),
    accessToken: stringValue(session.accessToken)
  }
  if (!record.apiBaseUrl || !record.userId || !record.accessToken) return undefined
  return record
}

function playbackReportBody(
  report: EmbyPlaybackReportRecord,
  positionTicks: number,
  options: { paused?: boolean; eventName?: string; failed?: boolean } = {}
): Record<string, unknown> {
  const target = report.target
  return {
    CanSeek: true,
    ItemId: target.itemId,
    MediaSourceId: target.mediaSourceId,
    PlaySessionId: target.playSessionId,
    PositionTicks: positionTicks,
    RunTimeTicks: target.runTimeTicks,
    IsPaused: options.paused ?? false,
    IsMuted: false,
    VolumeLevel: 100,
    PlayMethod: 'DirectStream',
    PlaybackRate: 1,
    EventName: options.eventName,
    Failed: options.failed ?? false
  }
}

async function postPlaybackReport(
  report: EmbyPlaybackReportRecord,
  path: string,
  body: Record<string, unknown>,
  label: string,
  keepalive = false
): Promise<void> {
  const headers = authHeadersForSession(report.session)
  await fetchEmpty(
    apiUrl(report.session.apiBaseUrl, path),
    {
      method: 'POST',
      headers: {
        ...headers,
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(body),
      keepalive
    },
    label
  )
}

export function savePendingEmbyPlaybackReport(
  session: EmbySession,
  target: EmbyPlaybackTarget
): EmbyPlaybackReportRecord | undefined {
  const report: EmbyPlaybackReportRecord = {
    id: window.crypto?.randomUUID?.() ?? `emby-playback-${Date.now()}`,
    createdAt: Date.now(),
    session,
    target
  }
  try {
    window.localStorage.setItem(PLAYBACK_REPORT_STORAGE_KEY, JSON.stringify(report))
    return report
  } catch {
    return undefined
  }
}

export function loadPendingEmbyPlaybackReport(): EmbyPlaybackReportRecord | undefined {
  try {
    const raw = window.localStorage.getItem(PLAYBACK_REPORT_STORAGE_KEY)
    if (!raw) return undefined
    const parsed: unknown = JSON.parse(raw)
    const row = objectValue(parsed)
    if (!row) return undefined
    const session = loadReportSession(row.session)
    const target = loadReportTarget(row.target)
    if (!session || !target) return undefined
    const createdAt = numberValue(row.createdAt) ?? 0
    if (createdAt > 0 && Date.now() - createdAt > 24 * 60 * 60 * 1000) {
      clearPendingEmbyPlaybackReport(stringValue(row.id) || undefined)
      return undefined
    }
    return {
      id: stringValue(row.id) || `emby-playback-${createdAt || Date.now()}`,
      createdAt,
      session,
      target
    }
  } catch {
    return undefined
  }
}

export function clearPendingEmbyPlaybackReport(_reportId?: string): void {
  try {
    window.localStorage.removeItem(PLAYBACK_REPORT_STORAGE_KEY)
  } catch {
    // Best effort cleanup only.
  }
}

export async function reportEmbyPlaybackStarted(
  report: EmbyPlaybackReportRecord,
  positionMs: number
): Promise<void> {
  await postPlaybackReport(
    report,
    '/Sessions/Playing',
    playbackReportBody(report, millisToTicks(positionMs), { eventName: 'TimeUpdate' }),
    'Report Emby playback start'
  )
}

export async function reportEmbyPlaybackProgress(
  report: EmbyPlaybackReportRecord,
  positionMs: number,
  paused: boolean,
  eventName = 'TimeUpdate'
): Promise<void> {
  await postPlaybackReport(
    report,
    '/Sessions/Playing/Progress',
    playbackReportBody(report, millisToTicks(positionMs), { paused, eventName }),
    'Report Emby playback progress'
  )
}

export async function reportEmbyPlaybackStopped(
  report: EmbyPlaybackReportRecord,
  positionMs: number,
  failed = false,
  keepalive = false
): Promise<void> {
  await postPlaybackReport(
    report,
    '/Sessions/Playing/Stopped',
    playbackReportBody(report, millisToTicks(positionMs), { failed }),
    'Report Emby playback stop',
    keepalive
  )
}

function normalizePlayableHttpUrl(value: string): string {
  try {
    return new URL(value).toString()
  } catch {
    return value
  }
}

function absoluteApiUrl(
  session: EmbySession,
  value: string,
  params: Record<string, string | number | boolean | undefined> = {}
): string {
  const url = /^https?:\/\//i.test(value)
    ? new URL(value)
    : new URL(value, `${session.apiBaseUrl.replace(/\/+$/, '')}/`)
  if (!url.searchParams.has('api_key') && !url.searchParams.has('X-Emby-Token')) {
    url.searchParams.set('api_key', session.accessToken)
  }
  Object.entries(params).forEach(([key, paramValue]) => {
    if (paramValue !== undefined && paramValue !== '' && !url.searchParams.has(key)) {
      url.searchParams.set(key, String(paramValue))
    }
  })
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

function playbackStreamUrl(
  session: EmbySession,
  itemId: string,
  source: EmbyMediaSource | undefined,
  playSessionId?: string
): string {
  const mediaSourceId = source?.Id
  const playbackParams = {
    DeviceId: getDeviceId(),
    MediaSourceId: mediaSourceId,
    PlaySessionId: playSessionId
  }

  if (source?.DirectStreamUrl) {
    return absoluteApiUrl(session, source.DirectStreamUrl, playbackParams)
  }

  if (source?.Protocol === 'Http' && source.Path && /^https?:\/\//i.test(source.Path)) {
    return normalizePlayableHttpUrl(source.Path)
  }

  return apiUrl(session.apiBaseUrl, `/Videos/${itemId}/stream${streamExtension(source)}`, {
    Static: true,
    api_key: session.accessToken,
    MediaSourceId: mediaSourceId,
    DeviceId: getDeviceId(),
    PlaySessionId: playSessionId
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

async function fetchPlaybackInfo(session: EmbySession, itemId: string): Promise<EmbyPlaybackInfoResponse> {
  return await fetchJson<EmbyPlaybackInfoResponse>(
    apiUrl(session.apiBaseUrl, `/Items/${itemId}/PlaybackInfo`, {
      UserId: session.userId,
      api_key: session.accessToken
    }),
    { method: 'GET' },
    '读取 Emby 播放信息'
  )
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
    const playbackInfo = await fetchPlaybackInfo(session, detail.Id)
    const source = playbackInfo.MediaSources?.[0]
    const fallbackSource = detail.MediaSources?.[0]
    const url = playbackStreamUrl(session, detail.Id, source ?? fallbackSource, playbackInfo.PlaySessionId)
    await assertPlayableHttpUrl(url)
    return {
      itemId: detail.Id,
      title: detail.Name ?? item.title,
      url,
      mediaSourceId: source?.Id ?? fallbackSource?.Id,
      playSessionId: playbackInfo.PlaySessionId,
      runTimeTicks: detail.RunTimeTicks ?? source?.RunTimeTicks ?? fallbackSource?.RunTimeTicks ?? undefined
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
    const playbackInfo = await fetchPlaybackInfo(session, episode.Id)
    const source = playbackInfo.MediaSources?.[0]
    const fallbackSource = episode.MediaSources?.[0]
    const url = playbackStreamUrl(session, episode.Id, source ?? fallbackSource, playbackInfo.PlaySessionId)
    await assertPlayableHttpUrl(url)
    return {
      itemId: episode.Id,
      title: titleParts.join(' - '),
      url,
      mediaSourceId: source?.Id ?? fallbackSource?.Id,
      playSessionId: playbackInfo.PlaySessionId,
      runTimeTicks: episode.RunTimeTicks ?? source?.RunTimeTicks ?? fallbackSource?.RunTimeTicks ?? undefined
    }
  }

  throw new Error('这个条目暂时不能直接播放')
}

export async function searchEmbyLibrary(
  session: EmbySession,
  query: string,
  options: EmbySearchOptions = {}
): Promise<MediaItem[]> {
  const searchTerm = query.trim()
  if (!searchTerm) return []

  const sourceId = sourceIdForSession(session)
  const sort = sortParamsForSearch(options.sortKey, options.sortOrder)
  const response = await fetchJson<EmbyItemsResponse>(
    apiUrl(session.apiBaseUrl, `/Users/${session.userId}/Items`, {
      Recursive: true,
      ParentId: options.libraryViewId,
      SearchTerm: searchTerm,
      IncludeItemTypes: itemTypesForLibraryView(options.view),
      Fields: EMBY_ITEM_FIELDS,
      Limit: options.limit ?? 80,
      SortBy: sort.SortBy,
      SortOrder: sort.SortOrder,
      ...filterParamsForSearch(options.filterKey)
    }),
    { method: 'GET', headers: authHeadersForSession(session) },
    '鎼滅储 Emby'
  )

  return orderSearchResults(response.Items ?? []).map((item) =>
    mapItem(session, sourceId, item, options.libraryViewId)
  )
}

export async function searchEmbyPersonLibrary(
  session: EmbySession,
  person: { id?: string; name: string },
  options: EmbySearchOptions = {}
): Promise<MediaItem[]> {
  const personName = person.name.trim()
  const personId = person.id?.includes(':') ? '' : person.id?.trim() ?? ''
  if (!personName && !personId) return []

  const sourceId = sourceIdForSession(session)
  const sort = sortParamsForSearch(options.sortKey, options.sortOrder)
  const response = await fetchJson<EmbyItemsResponse>(
    apiUrl(session.apiBaseUrl, `/Users/${session.userId}/Items`, {
      Recursive: true,
      ParentId: options.libraryViewId,
      PersonIds: personId || undefined,
      Person: personId ? undefined : personName,
      IncludeItemTypes: options.view === 'movies'
        ? 'BoxSet,Movie'
        : options.view === 'series'
          ? 'Series'
          : 'BoxSet,Movie,Series',
      Fields: EMBY_ITEM_FIELDS,
      Limit: options.limit ?? 80,
      SortBy: sort.SortBy,
      SortOrder: sort.SortOrder,
      ...filterParamsForSearch(options.filterKey)
    }),
    { method: 'GET', headers: authHeadersForSession(session) },
    '搜索 Emby 演员作品'
  )

  return orderSearchResults(response.Items ?? []).map((item) =>
    mapItem(session, sourceId, item, options.libraryViewId)
  )
}

function episodeIndexLabel(item: EmbyItem): string {
  if (item.ParentIndexNumber && item.IndexNumber) {
    return `S${String(item.ParentIndexNumber).padStart(2, '0')}E${String(item.IndexNumber).padStart(2, '0')}`
  }
  if (item.IndexNumber) return `E${String(item.IndexNumber).padStart(2, '0')}`
  return 'Episode'
}

function mapEpisodeItem(session: EmbySession, sourceId: string, item: EmbyItem, libraryViewId?: string): EpisodeItem {
  return {
    id: item.Id,
    title: item.Name ?? '未命名',
    index: episodeIndexLabel(item),
    duration: ticksToRuntime(item.RunTimeTicks),
    poster: imageBackground(session, item, 'Primary'),
    item: mapItem(session, sourceId, item, libraryViewId)
  }
}

async function loadSimilarItems(session: EmbySession, sourceId: string, itemId: string, libraryViewId?: string): Promise<MediaItem[]> {
  try {
    const response = await fetchJson<EmbyItemsResponse>(
      apiUrl(session.apiBaseUrl, `/Items/${itemId}/Similar`, {
        UserId: session.userId,
        Limit: 24,
        Fields: EMBY_ITEM_FIELDS
      }),
      { method: 'GET', headers: authHeadersForSession(session) },
      '读取相似媒体'
    )
    return (response.Items ?? []).map((row) => mapItem(session, sourceId, row, libraryViewId))
  } catch {
    return []
  }
}

async function loadSeriesSeasons(session: EmbySession, sourceId: string, item: MediaItem): Promise<SeasonItem[]> {
  if (item.type !== 'series') return []
  try {
    const seasons = await fetchJson<EmbyItemsResponse>(
      apiUrl(session.apiBaseUrl, `/Shows/${item.id}/Seasons`, {
        UserId: session.userId,
        Fields: EMBY_ITEM_FIELDS
      }),
      { method: 'GET', headers: authHeadersForSession(session) },
      '读取分季'
    )
    const seasonRows = seasons.Items ?? []
    return await Promise.all(seasonRows.map(async (season, seasonIndex): Promise<SeasonItem> => {
      let episodes: EpisodeItem[] = []
      try {
        const episodeResponse = await fetchJson<EmbyItemsResponse>(
          apiUrl(session.apiBaseUrl, `/Shows/${item.id}/Episodes`, {
            UserId: session.userId,
            SeasonId: season.Id,
            Fields: EMBY_ITEM_FIELDS
          }),
          { method: 'GET', headers: authHeadersForSession(session) },
          '读取分集'
        )
        episodes = (episodeResponse.Items ?? []).map((episode) =>
          mapEpisodeItem(session, sourceId, episode, item.libraryViewId)
        )
      } catch {
        episodes = []
      }

      return {
        id: season.Id,
        title: season.Name ?? `第 ${season.IndexNumber ?? seasonIndex + 1} 季`,
        index: season.IndexNumber ? `S${season.IndexNumber}` : `S${seasonIndex + 1}`,
        episodeCount: episodes.length || season.ChildCount || 0,
        poster: imageBackground(session, season, 'Primary'),
        episodes
      }
    }))
  } catch {
    return []
  }
}

export async function loadEmbyItemDetails(session: EmbySession, item: MediaItem): Promise<MediaItem> {
  const sourceId = sourceIdForSession(session)
  const detail = await fetchJson<EmbyItem>(
    apiUrl(session.apiBaseUrl, `/Users/${session.userId}/Items/${item.id}`, {
      Fields: EMBY_ITEM_FIELDS
    }),
    { method: 'GET', headers: authHeadersForSession(session) },
    '读取媒体详情'
  )

  const mapped = mapItem(session, sourceId, detail, item.libraryViewId, item.continueWatching ?? false)
  const [similarItems, seasons] = await Promise.all([
    loadSimilarItems(session, sourceId, item.id, item.libraryViewId),
    loadSeriesSeasons(session, sourceId, mapped)
  ])

  return {
    ...item,
    ...mapped,
    similarItems,
    seasons,
    episodes: seasons[0]?.episodes ?? mapped.episodes
  }
}

async function loadLibraryForSession(
  session: EmbySession,
  displayName: string | undefined,
  limit: number
): Promise<EmbyLibrarySnapshot> {
  const apiBaseUrl = session.apiBaseUrl
  const authHeaders = authHeadersForSession(session)
  const fields = EMBY_ITEM_FIELDS

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
  const resumeItems = await (async (): Promise<EmbyItem[]> => {
    try {
      const resume = await fetchJson<EmbyItemsResponse>(
        apiUrl(apiBaseUrl, `/Users/${session.userId}/Items/Resume`, {
          Recursive: true,
          MediaTypes: 'Video',
          IncludeItemTypes: 'Movie,Episode',
          Fields: fields,
          Limit: Math.min(Math.max(limit, 24), 80)
        }),
        { method: 'GET', headers: authHeaders },
        '读取 Emby 继续观看'
      )
      return resume.Items ?? []
    } catch {
      return []
    }
  })()
  const itemRows = new Map<string, { item: EmbyItem; libraryViewId?: string; continueWatching?: boolean }>()
  resumeItems.forEach((item) => {
    if (!itemRows.has(item.Id)) itemRows.set(item.Id, { item, continueWatching: true })
  })
  itemsByView.forEach((row) => {
    row.items.forEach((item) => {
      if (!itemRows.has(item.Id)) itemRows.set(item.Id, { item, libraryViewId: row.view.Id })
    })
  })
  mediaItems.forEach((item) => {
    if (!itemRows.has(item.Id)) itemRows.set(item.Id, { item })
  })
  const mappedItems = [...itemRows.values()].map((row) =>
    mapItem(session, sourceId, row.item, row.libraryViewId, row.continueWatching ?? false)
  )
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
