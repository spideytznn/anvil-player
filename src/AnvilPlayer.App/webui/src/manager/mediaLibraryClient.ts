import type { LibraryHomeSection, LibraryQuery, LibrarySource, LibraryView, MediaFilterKey, MediaItem, NavKey, SortKey, SortOrder, SourceDraft } from './types'

const LIBRARY_CACHE_KEY = 'anvil-player.library.cache.v1'

export interface MediaLibraryClient {
  listSources: () => Promise<LibrarySource[]>
  listAllItems: () => Promise<MediaItem[]>
  listItems: (query: LibraryQuery) => Promise<MediaItem[]>
  listHomeSections: (sourceId?: string) => Promise<LibraryHomeSection[]>
  getContinueWatching: () => Promise<MediaItem[]>
  saveSourceDraft: (draft: SourceDraft) => Promise<LibrarySource>
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
    case 'playlist': return items.filter((item) => item.favorite || item.progress > 0)
    case 'genre': return [...items].sort((a, b) => a.genres[0].localeCompare(b.genres[0], 'zh-Hans-CN'))
    case 'rating': return items.filter((item) => item.rating >= 8)
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

function isObject(value: unknown): value is Record<string, unknown> {
  return Boolean(value) && typeof value === 'object'
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

function compactItemForCache(item: MediaItem): MediaItem {
  const {
    cast,
    episodes,
    seasons,
    similarItems,
    streamSpecs,
    studios,
    tags,
    ...cacheItem
  } = item
  return {
    ...cacheItem,
    genres: item.genres.slice(0, 8),
    tagline: '',
    overview: ''
  }
}

function minimalItemForCache(item: MediaItem): MediaItem {
  return {
    ...compactItemForCache(item),
    originalTitle: '',
    country: '',
    genres: item.genres.slice(0, 3),
    videoSpec: '',
    audioSpec: '',
    tagline: '',
    overview: '',
    path: undefined
  }
}

function saveCache(sources: LibrarySource[], items: MediaItem[], homeSections: LibraryHomeSection[]): void {
  const attempts: LibraryCache[] = [
    {
      version: 1,
      sources,
      items: items.map(compactItemForCache),
      homeSections
    },
    {
      version: 1,
      sources,
      items: items.map(minimalItemForCache),
      homeSections
    }
  ]

  for (const payload of attempts) {
    try {
      window.localStorage.setItem(LIBRARY_CACHE_KEY, JSON.stringify(payload))
      return
    } catch {
      // Retry with a smaller payload below; keep in-memory data usable if storage is full.
    }
  }
}

export function createEmptyLibraryClient(): MediaLibraryClient {
  const cached = loadCache()
  let sources: LibrarySource[] = cached.sources
  let items: MediaItem[] = cached.items
  let homeSections: LibraryHomeSection[] = cached.homeSections

  return {
    async listSources() {
      return [...sources]
    },

    async listAllItems() {
      return [...items]
    },

    async listItems(query) {
      const queryItems = query.navKey.startsWith('source:')
        ? items
        : localLibraryItems(items, sources)
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
        cards: [...section.cards]
      }))
    },

    async getContinueWatching() {
      return items.filter((item) => item.continueWatching || (item.progress > 0 && item.progress < 1))
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
      saveCache(sources, items, homeSections)
      return source
    },

    async upsertSourceItems(source, sourceItems, sourceHomeSections = []) {
      sources = [source, ...sources.filter((candidate) => candidate.id !== source.id)]
      items = [
        ...items.filter((item) => item.sourceId !== source.id),
        ...sourceItems.map((item) => ({ ...item, sourceId: source.id }))
      ]
      homeSections = [
        ...homeSections.filter((section) => section.sourceId !== source.id),
        ...sourceHomeSections
      ]
      saveCache(sources, items, homeSections)
    }
  }
}
