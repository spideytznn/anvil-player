import type { LibraryQuery, LibrarySource, LibraryView, MediaItem, NavKey, SortKey, SourceDraft } from './types'

export interface MediaLibraryClient {
  listSources: () => Promise<LibrarySource[]>
  listItems: (query: LibraryQuery) => Promise<MediaItem[]>
  getContinueWatching: () => Promise<MediaItem[]>
  saveSourceDraft: (draft: SourceDraft) => Promise<LibrarySource>
  upsertSourceItems: (source: LibrarySource, sourceItems: MediaItem[]) => Promise<void>
}

function filterByNav(items: MediaItem[], navKey: NavKey): MediaItem[] {
  if (navKey.startsWith('source:')) {
    const sourceId = navKey.slice('source:'.length)
    return items.filter((item) => item.sourceId === sourceId)
  }
  switch (navKey) {
    case 'continue': return items.filter((item) => item.progress > 0 && item.progress < 1)
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

function sortItems(items: MediaItem[], sortKey: SortKey): MediaItem[] {
  const sorted = [...items]
  switch (sortKey) {
    case 'title':
      return sorted.sort((a, b) => a.title.localeCompare(b.title, 'zh-Hans-CN'))
    case 'rating':
      return sorted.sort((a, b) => b.rating - a.rating)
    case 'year':
      return sorted.sort((a, b) => b.year - a.year)
    default:
      return sorted.sort((a, b) => a.addedDaysAgo - b.addedDaysAgo)
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

export function createEmptyLibraryClient(): MediaLibraryClient {
  let sources: LibrarySource[] = []
  let items: MediaItem[] = []

  return {
    async listSources() {
      return [...sources]
    },

    async listItems(query) {
      return sortItems(
        filterByView(filterByNav(items, query.navKey), query.view).filter((item) => matchesSearch(item, query.search)),
        query.sortKey
      )
    },

    async getContinueWatching() {
      return items.filter((item) => item.progress > 0 && item.progress < 1)
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
      return source
    },

    async upsertSourceItems(source, sourceItems) {
      sources = [source, ...sources.filter((candidate) => candidate.id !== source.id)]
      items = [
        ...items.filter((item) => item.sourceId !== source.id),
        ...sourceItems.map((item) => ({ ...item, sourceId: source.id }))
      ]
    }
  }
}
