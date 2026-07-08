export type LibraryView = 'home' | 'movies' | 'series' | 'folders'
export type SortKey = 'recent' | 'title' | 'rating' | 'year'
export type MediaFilterKey = 'all' | 'unwatched' | 'watched' | 'favorites' | 'inProgress'
export type SourceKind = 'Emby' | 'Local' | 'WebDAV'
export type SourceStatus = 'online' | 'draft'

export type NavKey =
  | 'continue'
  | 'recent'
  | 'movies'
  | 'series'
  | 'unwatched'
  | 'watched'
  | 'favorites'
  | 'playlist'
  | 'genre'
  | 'rating'
  | 'release'
  | `source:${string}`

export interface LibrarySource {
  id: string
  name: string
  kind: SourceKind
  status: SourceStatus
  itemCount: number
  location: string
}

export interface EpisodeItem {
  id: string
  title: string
  index: string
  duration: string
  poster: string
}

export interface MediaItem {
  id: string
  title: string
  originalTitle: string
  type: 'movie' | 'series' | 'folder'
  year: number
  rating: number
  runtime: string
  sourceId: string
  libraryViewId?: string
  genres: string[]
  country: string
  quality: string
  progress: number
  watched: boolean
  favorite: boolean
  addedDaysAgo: number
  poster: string
  backdrop: string
  tagline: string
  overview: string
  episodes?: EpisodeItem[]
}

export interface LibraryHomeCard {
  id: string
  sourceId: string
  title: string
  subtitle: string
  image: string
  kind: 'view' | 'item'
  viewId?: string
  mediaType?: MediaItem['type']
  itemId?: string
}

export interface LibraryHomeSection {
  id: string
  sourceId: string
  title: string
  layout: 'landscape' | 'poster'
  cards: LibraryHomeCard[]
}

export interface LibraryQuery {
  navKey: NavKey
  view: LibraryView
  search: string
  sortKey: SortKey
  filterKey?: MediaFilterKey
  libraryViewId?: string
}

export interface SourceDraft {
  name: string
  location: string
  kind: SourceKind
}

export interface PlayerLaunchRequest {
  mediaId: string
  title: string
  sourceId: string
  mediaKind: MediaItem['type']
  startPositionRatio: number
  target: 'external-player-window'
}
