export type LibraryView = 'home' | 'movies' | 'series' | 'folders'
export type SortKey = 'recent' | 'title' | 'rating' | 'year'
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

export interface LibraryQuery {
  navKey: NavKey
  view: LibraryView
  search: string
  sortKey: SortKey
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
