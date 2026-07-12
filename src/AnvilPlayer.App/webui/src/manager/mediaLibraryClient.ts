import { isObject } from './storageCodec'
import type { EpisodeItem, LibraryHomeSection, LibraryQuery, LibrarySource, LibraryView, MediaFilterKey, MediaItem, NavKey, SeasonItem, SortKey, SortOrder, SourceDraft } from './types'

const LIBRARY_CACHE_KEY = 'anvil-player.library.cache.v1'
const HIDDEN_MEDIA_KEY = 'anvil-player.library.hidden-media.v1'
const LIBRARY_CACHE_WRITE_DELAY_MS = 250

export interface MediaLibraryClient {
  listSources: () => Promise<LibrarySource[]>
  listAllItems: () => Promise<MediaItem[]>
  listItems: (query: LibraryQuery) => Promise<MediaItem[]>
  listHomeSections: (sourceId?: string) => Promise<LibraryHomeSection[]>
  getContinueWatching: () => Promise<MediaItem[]>
  saveSourceDraft: (draft: SourceDraft) => Promise<LibrarySource>
  updateItem: (item: MediaItem) => Promise<void>
  removeItem: (itemId: string) => Promise<void>
  hideItem: (item: MediaItem) => Promise<void>
  removeSource: (sourceId: string) => Promise<void>
  upsertSourceItems: (source: LibrarySource, sourceItems: MediaItem[], sourceHomeSections?: LibraryHomeSection[]) => Promise<void>
}

function filterByNav(items: MediaItem[], navKey: NavKey): MediaItem[] {
  if (navKey.startsWith('source:')) {
    const sourceId = navKey.slice('source:'.length)
    return items.filter((item) => item.sourceId === sourceId)
  }
  switch (navKey) {
    case 'continue': return items.filter((item) => item.continueWatching || (item.progress > 0 && item.progress < 1))
    case 'recent': return items
    case 'movies': return items.filter((item) => item.type === 'movie')
    case 'series': return items.filter((item) => item.type === 'series')
    case 'unwatched': return items.filter((item) => !item.watched)
    case 'watched': return items.filter((item) => item.watched)
    case 'favorites': return items.filter((item) => item.favorite)
    case 'playlist': return items.filter((item) => item.inPlaylist || Boolean(item.playlistIds?.length))
    case 'genre': return [...items].sort((a, b) => a.genres[0].localeCompare(b.genres[0], 'zh-Hans-CN'))
    case 'rating': return [...items].sort((a, b) => b.rating - a.rating)
    case 'release': return [...items].sort((a, b) => b.year - a.year)
    default: return items
  }
}

function filterByView(items: MediaItem[], view: LibraryView): MediaItem[] {
  switch (view) {
    case 'movies': return items.filter((item) => item.type === 'movie')
    case 'series': return items.filter((item) => item.type === 'series')
    case 'folders': return items.filter((item) => item.type === 'folder')
    default: return items
  }
}

function filterByLibraryView(items: MediaItem[], libraryViewId?: string): MediaItem[] {
  if (!libraryViewId) return items
  if (!items.some((item) => item.libraryViewId)) return items
  return items.filter((item) => item.libraryViewId === libraryViewId)
}

function filterByMediaFilter(items: MediaItem[], filterKey: MediaFilterKey = 'all'): MediaItem[] {
  switch (filterKey) {
    case 'unwatched': return items.filter((item) => !item.watched)
    case 'watched': return items.filter((item) => item.watched)
    case 'favorites': return items.filter((item) => item.favorite)
    case 'inProgress': return items.filter((item) => item.continueWatching || (item.progress > 0 && item.progress < 1))
    default: return items
  }
}

function sortDirection(order: SortOrder | undefined): number {
  return order === 'ascending' ? 1 : -1
}

function sortItems(items: MediaItem[], sortKey: SortKey, sortOrder?: SortOrder): MediaItem[] {
  const sorted = [...items]
  const direction = sortDirection(sortOrder)
  switch (sortKey) {
    case 'title':
      return sorted.sort((a, b) => direction * a.title.localeCompare(b.title, 'zh-Hans-CN'))
    case 'rating':
      return sorted.sort((a, b) => direction * (a.rating - b.rating))
    case 'year':
      return sorted.sort((a, b) => direction * (a.year - b.year))
    default:
      return sorted.sort((a, b) => direction * (b.addedDaysAgo - a.addedDaysAgo))
  }
}

function matchesSearch(item: MediaItem, search: string): boolean {
  const normalized = search.trim().toLowerCase()
  if (!normalized) return true
  return `${item.title} ${item.originalTitle} ${item.genres.join(' ')}`
    .toLowerCase()
    .includes(normalized)
}

function sourceIdFromName(name: string): string {
  const normalized = name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/(^-|-$)/g, '')
  return `source-${normalized || Date.now()}`
}

function localLibraryItems(items: MediaItem[], sources: LibrarySource[]): MediaItem[] {
  const embySourceIds = new Set(sources.filter((source) => source.kind === 'Emby').map((source) => source.id))
  return items.filter((item) => !embySourceIds.has(item.sourceId))
}

interface LibraryCache {
  version: 1
  sources: LibrarySource[]
  items: MediaItem[]
  homeSections: LibraryHomeSection[]
}

function loadCache(): LibraryCache {
  try {
    const raw = window.localStorage.getItem(LIBRARY_CACHE_KEY)
    if (!raw) throw new Error('empty cache')
    const parsed: unknown = JSON.parse(raw)
    if (!isObject(parsed)) throw new Error('invalid cache')
    return {
      version: 1,
      sources: Array.isArray(parsed.sources) ? parsed.sources as LibrarySource[] : [],
      items: Array.isArray(parsed.items) ? parsed.items as MediaItem[] : [],
      homeSections: Array.isArray(parsed.homeSections) ? parsed.homeSections as LibraryHomeSection[] : []
    }
  } catch {
    return { version: 1, sources: [], items: [], homeSections: [] }
  }
}

function compactNestedItemForCache(item: MediaItem): MediaItem {
  const {
    cast,
    episodes,
    seasons,
    versions,
    similarItems,
    streamSpecs,
    studios,
    tags,
    playbackPath,
    ...cacheItem
  } = item
  return {
    ...cacheItem,
    genres: item.genres.slice(0, 8),
    tagline: item.tagline.slice(0, 240),
    overview: item.overview.slice(0, 2400)
  }
}

function compactEpisodeForCache(episode: EpisodeItem): EpisodeItem {
  return {
    ...episode,
    item: episode.item ? compactNestedItemForCache(episode.item) : undefined
  }
}

function compactSeasonForCache(season: SeasonItem): SeasonItem {
  return {
    ...season,
    episodes: season.episodes.map(compactEpisodeForCache)
  }
}

function compactItemForCache(item: MediaItem): MediaItem {
  const compactItem = compactNestedItemForCache(item)
  if (item.cast?.length) {
    compactItem.cast = item.cast.slice(0, 12)
  }
  if (item.streamSpecs?.length) {
    compactItem.streamSpecs = item.streamSpecs
  }
  if (item.episodes?.length) {
    compactItem.episodes = item.episodes.map(compactEpisodeForCache)
  }
  if (item.seasons?.length) {
    compactItem.seasons = item.seasons.map(compactSeasonForCache)
  }
  if (item.versions?.length) {
    compactItem.versions = item.versions.map(compactNestedItemForCache)
  }
  return compactItem
}

function minimalItemForCache(item: MediaItem): MediaItem {
  const minimalItem = minimalNestedItemForCache(item)
  if (item.cast?.length) {
    minimalItem.cast = item.cast.slice(0, 12)
  }
  if (item.streamSpecs?.length) {
    minimalItem.streamSpecs = item.streamSpecs
  }
  if (item.episodes?.length) {
    minimalItem.episodes = item.episodes.map(minimalEpisodeForCache)
  }
  if (item.seasons?.length) {
    minimalItem.seasons = item.seasons.map(minimalSeasonForCache)
  }
  if (item.versions?.length) {
    minimalItem.versions = item.versions.map(minimalNestedItemForCache)
  }
  return minimalItem
}

function minimalNestedItemForCache(item: MediaItem): MediaItem {
  const compactItem = compactNestedItemForCache(item)
  return {
    ...compactItem,
    originalTitle: '',
    country: '',
    genres: item.genres.slice(0, 3),
    videoSpec: '',
    audioSpec: '',
    tagline: item.tagline.slice(0, 120),
    overview: item.overview.slice(0, 480)
  }
}

function minimalEpisodeForCache(episode: EpisodeItem): EpisodeItem {
  return {
    ...episode,
    item: episode.item ? minimalNestedItemForCache(episode.item) : undefined
  }
}

function minimalSeasonForCache(season: SeasonItem): SeasonItem {
  return {
    ...season,
    episodes: season.episodes.map(minimalEpisodeForCache)
  }
}

function tinyNestedItemForCache(item: MediaItem): MediaItem {
  return {
    id: item.id,
    title: item.title,
    originalTitle: item.originalTitle,
    type: item.type,
    year: item.year,
    rating: item.rating,
    runtime: item.runtime,
    sourceId: item.sourceId,
    libraryViewId: item.libraryViewId,
    genres: item.genres.slice(0, 2),
    country: item.country.slice(0, 80),
    quality: item.quality,
    videoSpec: '',
    audioSpec: '',
    progress: item.progress,
    continueWatching: item.continueWatching,
    watched: item.watched,
    favorite: item.favorite,
    inPlaylist: item.inPlaylist,
    playlistIds: item.playlistIds,
    addedDaysAgo: item.addedDaysAgo,
    poster: item.poster,
    backdrop: '',
    tagline: '',
    overview: '',
    path: item.path,
    externalIds: item.externalIds,
    metadataProvider: item.metadataProvider,
    metadataMatchedAt: item.metadataMatchedAt,
    metadataLocked: item.metadataLocked,
    metadataMatchTitle: item.metadataMatchTitle,
    fileSizeBytes: item.fileSizeBytes,
    fileModifiedAt: item.fileModifiedAt,
    fileFingerprint: item.fileFingerprint,
    availability: item.availability,
    missingSince: item.missingSince,
    mediaSourceId: item.mediaSourceId,
    providerItemId: item.providerItemId,
    versionLabel: item.versionLabel,
    collectionDissolved: item.collectionDissolved,
    trailerUrl: item.trailerUrl,
    trailerUrls: item.trailerUrls
  }
}

function tinyEpisodeForCache(episode: EpisodeItem): EpisodeItem {
  return {
    id: episode.id,
    title: episode.title,
    index: episode.index,
    duration: episode.duration,
    poster: episode.poster,
    item: episode.item ? tinyNestedItemForCache(episode.item) : undefined
  }
}

function tinySeasonForCache(season: SeasonItem): SeasonItem {
  return {
    id: season.id,
    title: season.title,
    index: season.index,
    episodeCount: season.episodeCount,
    poster: season.poster,
    episodes: season.episodes.map(tinyEpisodeForCache)
  }
}

function tinyItemForCache(item: MediaItem): MediaItem {
  const tinyItem = tinyNestedItemForCache(item)
  if (item.episodes?.length) {
    tinyItem.episodes = item.episodes.map(tinyEpisodeForCache)
  }
  if (item.seasons?.length) {
    tinyItem.seasons = item.seasons.map(tinySeasonForCache)
  }
  if (item.versions?.length) {
    tinyItem.versions = item.versions.map(tinyNestedItemForCache)
  }
  return tinyItem
}

function flatTinyItemForCache(item: MediaItem): MediaItem {
  const tinyItem = tinyNestedItemForCache(item)
  if (item.versions?.length) {
    tinyItem.versions = item.versions.map(tinyNestedItemForCache)
  }
  return tinyItem
}

interface CacheWriteAttempt {
  compactItem: (item: MediaItem) => MediaItem
  includeHomeSections: boolean
}

function normalizedIdentity(value: string): string {
  return value.trim().replace(/\\/g, '/').toLowerCase()
}

function hiddenIdentityKeys(item: MediaItem): string[] {
  const prefix = `${item.sourceId}|`
  return [
    item.id ? `${prefix}id:${item.id}` : '',
    item.providerItemId ? `${prefix}id:${item.providerItemId}` : '',
    item.path ? `${prefix}path:${normalizedIdentity(item.path)}` : '',
    item.playbackPath ? `${prefix}path:${normalizedIdentity(item.playbackPath)}` : '',
    item.externalIds?.tmdb ? `${prefix}tmdb:${item.type}:${item.externalIds.tmdb}` : ''
  ].filter(Boolean)
}

function loadHiddenMediaKeys(): Set<string> {
  try {
    const parsed: unknown = JSON.parse(window.localStorage.getItem(HIDDEN_MEDIA_KEY) || '[]')
    return new Set(Array.isArray(parsed) ? parsed.filter((value): value is string => typeof value === 'string') : [])
  } catch {
    return new Set()
  }
}

function saveHiddenMediaKeys(keys: Set<string>): void {
  window.localStorage.setItem(HIDDEN_MEDIA_KEY, JSON.stringify([...keys]))
}

const CACHE_WRITE_ATTEMPTS: CacheWriteAttempt[] = [
  { compactItem: compactItemForCache, includeHomeSections: true },
  { compactItem: minimalItemForCache, includeHomeSections: true },
  { compactItem: tinyItemForCache, includeHomeSections: true },
  { compactItem: flatTinyItemForCache, includeHomeSections: false }
]

function saveCache(cache: LibraryCache): void {
  // Construct fallbacks only after the previous payload is rejected. Building
  // all four variants up front briefly retained four deep item trees.
  for (const attempt of CACHE_WRITE_ATTEMPTS) {
    const payload: LibraryCache = {
      version: 1,
      sources: cache.sources,
      items: cache.items.map(attempt.compactItem),
      homeSections: attempt.includeHomeSections ? cache.homeSections : []
    }
    try {
      window.localStorage.setItem(LIBRARY_CACHE_KEY, JSON.stringify(payload))
      return
    } catch {
      // Retry with a smaller payload below; keep in-memory data usable if storage is full.
    }
  }
}

function createCacheWriter(readCache: () => LibraryCache): () => void {
  let dirty = false
  let writeTimer: number | undefined

  const flush = (): void => {
    if (!dirty) return
    dirty = false
    writeTimer = undefined
    saveCache(readCache())
  }

  const schedule = (): void => {
    dirty = true
    if (writeTimer !== undefined) return
    writeTimer = window.setTimeout(flush, LIBRARY_CACHE_WRITE_DELAY_MS)
  }

  window.addEventListener('pagehide', () => {
    if (writeTimer !== undefined) {
      window.clearTimeout(writeTimer)
      writeTimer = undefined
    }
    flush()
  })

  return schedule
}

export function createEmptyLibraryClient(): MediaLibraryClient {
  const cached = loadCache()
  let sources: LibrarySource[] = cached.sources
  let items: MediaItem[] = cached.items
  let homeSections: LibraryHomeSection[] = cached.homeSections
  const hiddenMediaKeys = loadHiddenMediaKeys()
  const isHidden = (item: MediaItem): boolean => hiddenIdentityKeys(item).some((key) => hiddenMediaKeys.has(key))
  items = items.filter((item) => !isHidden(item))
  const scheduleCacheWrite = createCacheWriter(() => ({ version: 1, sources, items, homeSections }))

  return {
    async listSources() {
      return [...sources]
    },

    async listAllItems() {
      return items.filter((item) => !isHidden(item))
    },

    async listItems(query) {
      const includesUserCollections = query.navKey === 'favorites' || query.navKey === 'playlist'
      const queryItems = query.navKey.startsWith('source:') || includesUserCollections
        ? items.filter((item) => !isHidden(item))
        : localLibraryItems(items.filter((item) => !isHidden(item)), sources)
      return sortItems(
        filterByMediaFilter(
          filterByView(
            filterByLibraryView(filterByNav(queryItems, query.navKey), query.libraryViewId),
            query.view
          ).filter((item) => matchesSearch(item, query.search)),
          query.filterKey
        ),
        query.sortKey,
        query.sortOrder
      )
    },

    async listHomeSections(sourceId) {
      const rows = sourceId
        ? homeSections.filter((section) => section.sourceId === sourceId)
        : homeSections
      return rows.map((section) => ({
        ...section,
        cards: section.cards.filter((card) => !card.itemId || !hiddenMediaKeys.has(`${section.sourceId}|id:${card.itemId}`))
      }))
    },

    async getContinueWatching() {
      return items.filter((item) => !isHidden(item) && (item.continueWatching || (item.progress > 0 && item.progress < 1)))
    },

    async saveSourceDraft(draft) {
      const existingIndex = sources.findIndex((source) => source.kind === draft.kind && source.name === draft.name)
      const source: LibrarySource = {
        id: existingIndex >= 0 ? sources[existingIndex].id : sourceIdFromName(draft.name),
        name: draft.name.trim() || 'Emby',
        kind: draft.kind,
        status: 'draft',
        itemCount: 0,
        location: draft.location.trim()
      }
      if (existingIndex >= 0) {
        sources = sources.map((candidate, index) => index === existingIndex ? source : candidate)
      } else {
        sources = [source, ...sources]
      }
      scheduleCacheWrite()
      return source
    },

    async updateItem(item) {
      items = items.map((candidate) => candidate.id === item.id ? item : candidate)
      scheduleCacheWrite()
    },

    async removeItem(itemId) {
      items = items.filter((candidate) => candidate.id !== itemId)
      scheduleCacheWrite()
    },

    async hideItem(item) {
      hiddenIdentityKeys(item).forEach((key) => hiddenMediaKeys.add(key))
      for (const version of item.versions ?? []) {
        hiddenIdentityKeys(version).forEach((key) => hiddenMediaKeys.add(key))
      }
      saveHiddenMediaKeys(hiddenMediaKeys)
      items = items.filter((candidate) => !isHidden(candidate))
      scheduleCacheWrite()
    },

    async removeSource(sourceId) {
      sources = sources.filter((source) => source.id !== sourceId)
      items = items.filter((item) => item.sourceId !== sourceId)
      homeSections = homeSections.filter((section) => section.sourceId !== sourceId)
      scheduleCacheWrite()
    },

    async upsertSourceItems(source, sourceItems, sourceHomeSections = []) {
      const existingIndex = sources.findIndex((candidate) => candidate.id === source.id)
      sources = existingIndex >= 0
        ? sources.map((candidate, index) => index === existingIndex ? source : candidate)
        : [source, ...sources]
      items = [
        ...items.filter((item) => item.sourceId !== source.id),
        ...sourceItems.map((item) => ({ ...item, sourceId: source.id })).filter((item) => !isHidden(item))
      ]
      homeSections = [
        ...homeSections.filter((section) => section.sourceId !== source.id),
        ...sourceHomeSections
      ]
      scheduleCacheWrite()
    }
  }
}
