// Emby client public API.
//
// Behaviour-preserving split: the transport/auth foundation lives in
// ./emby/core and the Emby JSON -> MediaItem mapping lives in ./emby/mapping.
// This file keeps the consumer-facing surface (library load/search/details,
// playback reporting, playback-target resolution) and re-exports the public
// types so existing `import { ... } from './embyClient'` paths keep working.

import { numberValue, objectValue, stringValue } from './storageCodec'
import {
  apiUrl,
  authHeadersForSession,
  authorizationHeader,
  DEFAULT_LIMIT,
  EMBY_ITEM_FIELDS,
  EMBY_PLAYBACK_REPORT_PENDING_EVENT,
  fetchEmpty,
  fetchJson,
  getDeviceId,
  millisToTicks,
  PLAYBACK_REPORT_STORAGE_KEY,
  resolveApiBase,
  sourceIdForSession,
  type EmbyAuthResult,
  type EmbyItem,
  type EmbyItemsResponse,
  type EmbyMediaSource,
  type EmbyPlaybackInfoResponse,
  type EmbyPublicInfo,
  type EmbySession,
  type EmbyView
} from './emby/core'
import {
  buildHomeSections,
  filterParamsForSearch,
  filterSupportedLatestItems,
  imageBackground,
  itemTypesForLibraryView,
  latestItemTypesForView,
  mapEpisodeItem,
  mapItem,
  orderSearchResults,
  progressRatio,
  qualityLabel,
  sortParamsForSearch,
  type EmbyViewItems,
  type EmbyViewLatest
} from './emby/mapping'
import type {
  EpisodeItem,
  LibraryHomeSection,
  LibrarySource,
  LibraryView,
  MediaFilterKey,
  MediaItem,
  SeasonItem,
  SortKey,
  SortOrder
} from './types'

export { EMBY_PLAYBACK_REPORT_PENDING_EVENT }

export interface EmbyConnectionInput {
  serverUrl: string
  username: string
  password: string
  displayName?: string
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

// Re-export the session type so connectionStorage / consumers keep importing
// it from './embyClient'.
export type { EmbySession } from './emby/core'

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
    notifyPendingEmbyPlaybackReport(report.id)
    // The player window is a separate WebView2 environment with its own
    // storage, so the pending report stored above is invisible to it. Relay
    // the full report through native so the player window can inject it.
    deliverEmbyPlaybackReportToNative(report)
    return report
  } catch {
    return undefined
  }
}

export function notifyPendingEmbyPlaybackReport(reportId?: string): void {
  window.dispatchEvent(new CustomEvent(EMBY_PLAYBACK_REPORT_PENDING_EVENT, { detail: { reportId } }))
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

export function clearPendingEmbyPlaybackReport(reportId?: string): void {
  try {
    if (reportId) {
      const raw = window.localStorage.getItem(PLAYBACK_REPORT_STORAGE_KEY)
      if (raw) {
        const parsed = objectValue(JSON.parse(raw))
        const currentId = stringValue(parsed?.id)
        if (currentId && currentId !== reportId) return
      }
    }
    window.localStorage.removeItem(PLAYBACK_REPORT_STORAGE_KEY)
  } catch {
    // Best effort cleanup only.
  }
}

// Relay the pending report to native so the host can forward it to the player
// window (which lives in a separate WebView2 environment with its own storage).
function deliverEmbyPlaybackReportToNative(report: EmbyPlaybackReportRecord): void {
  try {
    window.chrome?.webview?.postMessage({
      type: 'command',
      command: 'deliverEmbyPlaybackReport',
      report
    })
  } catch {
    // Non-fatal: the player just won't receive the cross-window relay.
  }
}

// Player-window side: injects a report received from native (relayed from the
// library window) into this environment's storage and fires the pending event
// so useEmbyPlaybackReporting picks it up.
export function receiveEmbyPlaybackReportFromNative(report: unknown): void {
  try {
    const row = objectValue(report)
    if (!row) return
    const session = loadReportSession(row.session)
    const target = loadReportTarget(row.target)
    if (!session || !target) return
    const record: EmbyPlaybackReportRecord = {
      id: stringValue(row.id) || `emby-playback-${numberValue(row.createdAt) ?? Date.now()}`,
      createdAt: numberValue(row.createdAt) ?? Date.now(),
      session,
      target
    }
    window.localStorage.setItem(PLAYBACK_REPORT_STORAGE_KEY, JSON.stringify(record))
    notifyPendingEmbyPlaybackReport(record.id)
  } catch {
    // Malformed relay payload; ignore.
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
  eventName = 'TimeUpdate',
  keepalive = false
): Promise<void> {
  await postPlaybackReport(
    report,
    '/Sessions/Playing/Progress',
    playbackReportBody(report, millisToTicks(positionMs), { paused, eventName }),
    'Report Emby playback progress',
    keepalive
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
    fetchJson<import('./emby/core').EmbyViewsResponse>(
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
