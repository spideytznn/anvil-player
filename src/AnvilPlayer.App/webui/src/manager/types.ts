export type LibraryView = 'home' | 'movies' | 'series' | 'folders'
export type SortKey = 'recent' | 'title' | 'rating' | 'year'
export type SortOrder = 'ascending' | 'descending'
export type MediaFilterKey = 'all' | 'unwatched' | 'watched' | 'favorites' | 'inProgress'
export type SourceKind = 'Emby' | 'Local' | 'SMB' | 'WebDAV'
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
  rootLocation?: string
  folders?: string[]
}

export interface EpisodeItem {
  id: string
  title: string
  index: string
  duration: string
  poster: string
  item?: MediaItem
}

export interface SeasonItem {
  id: string
  title: string
  index: string
  episodeCount: number
  poster: string
  episodes: EpisodeItem[]
}

export interface PersonCredit {
  id: string
  name: string
  role: string
  image: string
}

export interface MediaStreamSpec {
  id: string
  type: 'video' | 'audio' | 'subtitle'
  title: string
  subtitle: string
  details: Array<{ label: string; value: string }>
  isDefault?: boolean
  isForced?: boolean
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
  videoSpec?: string
  audioSpec?: string
  progress: number
  continueWatching?: boolean
  continueWatchingRank?: number
  lastPlayedAt?: number
  watched: boolean
  favorite: boolean
  inPlaylist?: boolean
  playlistIds?: string[]
  addedDaysAgo: number
  poster: string
  backdrop: string
  tagline: string
  overview: string
  episodes?: EpisodeItem[]
  seasons?: SeasonItem[]
  versions?: MediaItem[]
  cast?: PersonCredit[]
  similarItems?: MediaItem[]
  streamSpecs?: MediaStreamSpec[]
  studios?: string[]
  tags?: string[]
  path?: string
  playbackPath?: string
  externalIds?: {
    tmdb?: string
    imdb?: string
    tvdb?: string
  }
  metadataProvider?: string
  metadataMatchedAt?: number
  metadataLocked?: boolean
  metadataMatchTitle?: string
  fileSizeBytes?: number
  fileModifiedAt?: number
  fileFingerprint?: string
  availability?: 'available' | 'missing'
  missingSince?: number
  mediaSourceId?: string
  providerItemId?: string
  versionLabel?: string
  collectionDissolved?: boolean
  trailerUrl?: string
  trailerUrls?: string[]
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
  sortOrder?: SortOrder
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
