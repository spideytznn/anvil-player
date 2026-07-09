// Media-item metadata merge / transform helpers.
// Pure functions extracted from LibraryApp.tsx: duplicate detection & merge
// across sources, metadata-form parsing/building, and local-scan item merge.

import type { MediaItem } from '../manager/types'

export interface MetadataEditForm {
  title: string
  originalTitle: string
  type: MediaItem['type']
  year: string
  rating: string
  runtime: string
  genres: string
  country: string
  quality: string
  poster: string
  backdrop: string
  tagline: string
  overview: string
  tmdbId: string
  imdbId: string
  tvdbId: string
}

export function localFileName(path: string | undefined, fallback: string): string {
  if (!path) return fallback
  const name = path.split(/[\\/]/g).pop()?.replace(/\.[^.\\/]+$/g, '') || fallback
  try {
    return decodeURIComponent(name)
  } catch {
    return name
  }
}

export function clearLocalMetadata(item: MediaItem): MediaItem {
  const title = localFileName(item.path, item.originalTitle || item.title)
  return {
    ...item,
    title,
    originalTitle: title,
    year: 0,
    rating: 0,
    runtime: '未知',
    genres: ['未分类'],
    country: '本地',
    quality: item.quality || '本地文件',
    poster: '',
    backdrop: '',
    tagline: '等待刮削',
    overview: item.path ? `本地媒体文件：${item.path}` : '',
    cast: undefined,
    similarItems: undefined,
    studios: undefined,
    tags: undefined,
    externalIds: undefined,
    metadataProvider: undefined,
    metadataMatchedAt: undefined
  }
}

export function localVersionLabel(item: MediaItem, index: number): string {
  const fileName = localFileName(item.path, '')
  if (fileName && fileName !== item.title) return fileName
  return item.quality || `版本 ${index + 1}`
}

export function duplicateMetadataKey(item: MediaItem): string {
  if (item.type === 'folder') return ''
  const externalId = item.externalIds?.tmdb
    ? `tmdb:${item.externalIds.tmdb}`
    : item.externalIds?.imdb
      ? `imdb:${item.externalIds.imdb}`
      : item.externalIds?.tvdb
        ? `tvdb:${item.externalIds.tvdb}`
        : ''
  return externalId ? `${item.sourceId}:${item.type}:${externalId}` : ''
}

export function mediaPathKey(item: MediaItem): string {
  return (item.path || item.playbackPath || item.id).trim().toLowerCase()
}

export function itemHasPlaybackPath(item: MediaItem): boolean {
  return Boolean(item.path || item.playbackPath || item.seasons?.some((season) =>
    season.episodes.some((episode) => episode.item?.path || episode.item?.playbackPath)
  ) || item.episodes?.some((episode) => episode.item?.path || episode.item?.playbackPath))
}

function itemWatchWeight(item: MediaItem): number {
  return (item.favorite ? 1000 : 0) +
    (item.continueWatching ? 500 : 0) +
    (item.watched ? 250 : 0) +
    Math.round(item.progress * 100)
}

export function itemMergeRank(item: MediaItem): number {
  return (item.metadataProvider ? 10000 : 0) +
    (itemHasPlaybackPath(item) ? 1000 : 0) +
    itemWatchWeight(item)
}

function stripMergedVersionRelations(item: MediaItem): MediaItem {
  const { versions, similarItems, cast, streamSpecs, studios, tags, ...versionItem } = item
  return versionItem
}

function mergeWatchState(primary: MediaItem, rows: MediaItem[]): Pick<MediaItem, 'progress' | 'continueWatching' | 'watched' | 'favorite' | 'addedDaysAgo'> {
  return {
    progress: Math.max(primary.progress, ...rows.map((item) => item.progress)),
    continueWatching: rows.some((item) => item.continueWatching || (item.progress > 0 && item.progress < 1)),
    watched: rows.some((item) => item.watched),
    favorite: rows.some((item) => item.favorite),
    addedDaysAgo: Math.min(primary.addedDaysAgo, ...rows.map((item) => item.addedDaysAgo))
  }
}

function mergeDuplicateMovieItems(primary: MediaItem, rows: MediaItem[]): MediaItem {
  const seenVersionKeys = new Set([mediaPathKey(primary)])
  const versions: MediaItem[] = []
  const addVersion = (candidate: MediaItem): void => {
    const version = stripMergedVersionRelations(candidate)
    const key = mediaPathKey(version)
    if (seenVersionKeys.has(key)) return
    seenVersionKeys.add(key)
    versions.push(version)
  }

  rows.forEach((item) => {
    item.versions?.forEach(addVersion)
    if (item.id !== primary.id) addVersion(item)
  })

  const watchState = mergeWatchState(primary, rows)
  return {
    ...primary,
    ...watchState,
    versions: versions.length ? versions : undefined
  }
}

function episodeMergeKey(episode: { id: string; index: string; item?: MediaItem }): string {
  return episode.item ? mediaPathKey(episode.item) : `${episode.index}:${episode.id}`
}

function mergeSeriesSeasons(rows: MediaItem[]): MediaItem['seasons'] {
  const seasonsByKey = new Map<string, NonNullable<MediaItem['seasons']>[number]>()
  rows.forEach((item) => {
    item.seasons?.forEach((season) => {
      const key = season.index || season.title || season.id
      const existing = seasonsByKey.get(key)
      if (!existing) {
        seasonsByKey.set(key, {
          ...season,
          episodes: [...season.episodes]
        })
        return
      }
      const seenEpisodes = new Set(existing.episodes.map(episodeMergeKey))
      season.episodes.forEach((episode) => {
        const episodeKey = episodeMergeKey(episode)
        if (seenEpisodes.has(episodeKey)) return
        seenEpisodes.add(episodeKey)
        existing.episodes.push(episode)
      })
      existing.episodes.sort((left, right) => left.index.localeCompare(right.index, 'zh-Hans-CN'))
      existing.episodeCount = existing.episodes.length
    })
  })
  return [...seasonsByKey.values()].sort((left, right) => left.index.localeCompare(right.index, 'zh-Hans-CN'))
}

function mergeDuplicateSeriesItems(primary: MediaItem, rows: MediaItem[]): MediaItem {
  const seasons = mergeSeriesSeasons(rows)
  const episodes = seasons?.[0]?.episodes ?? primary.episodes
  const firstEpisodeItem = seasons
    ?.flatMap((season) => season.episodes)
    .find((episode) => episode.item?.path || episode.item?.playbackPath)
    ?.item
  const watchState = mergeWatchState(primary, rows)
  return {
    ...primary,
    ...watchState,
    runtime: seasons?.length
      ? `${seasons.reduce((total, season) => total + season.episodes.length, 0)} 集`
      : primary.runtime,
    path: primary.path || firstEpisodeItem?.path,
    playbackPath: primary.playbackPath || firstEpisodeItem?.playbackPath,
    seasons,
    episodes
  }
}

export function mergeDuplicateMetadataItems(sourceItems: MediaItem[]): { items: MediaItem[]; mergedCount: number; representativeIdByMergedId: Map<string, string> } {
  const representativeIdByMergedId = new Map<string, string>()
  const groups = new Map<string, MediaItem[]>()
  sourceItems.forEach((item) => {
    const key = duplicateMetadataKey(item)
    if (!key) return
    const rows = groups.get(key) ?? []
    rows.push(item)
    groups.set(key, rows)
  })

  const mergedItemsById = new Map<string, MediaItem>()
  let mergedCount = 0
  groups.forEach((rows) => {
    if (rows.length < 2) return
    const primary = [...rows].sort((left, right) => itemMergeRank(right) - itemMergeRank(left))[0]
    const merged = primary.type === 'series'
      ? mergeDuplicateSeriesItems(primary, rows)
      : mergeDuplicateMovieItems(primary, rows)
    rows.forEach((item) => representativeIdByMergedId.set(item.id, primary.id))
    mergedItemsById.set(primary.id, merged)
    mergedCount += rows.length - 1
  })

  if (!mergedItemsById.size) {
    return { items: sourceItems, mergedCount: 0, representativeIdByMergedId }
  }

  const result: MediaItem[] = []
  sourceItems.forEach((item) => {
    const representativeId = representativeIdByMergedId.get(item.id)
    if (!representativeId) {
      result.push(item)
      return
    }
    if (representativeId !== item.id) return
    result.push(mergedItemsById.get(item.id) ?? item)
  })
  return { items: result, mergedCount, representativeIdByMergedId }
}

export function imageBackgroundUrl(background: string): string | undefined {
  const match = background.trim().match(/^url\((["']?)(.*?)\1\)/)
  return match?.[2]
}

export function hasImageBackground(background: string): boolean {
  return Boolean(imageBackgroundUrl(background))
}

export function metadataFormFromItem(item: MediaItem): MetadataEditForm {
  return {
    title: item.title,
    originalTitle: item.originalTitle,
    type: item.type,
    year: item.year ? String(item.year) : '',
    rating: item.rating ? String(item.rating) : '',
    runtime: item.runtime === '未知' ? '' : item.runtime,
    genres: item.genres.join(', '),
    country: item.country === '本地' ? '' : item.country,
    quality: item.quality,
    poster: imageBackgroundUrl(item.poster) ?? item.poster,
    backdrop: imageBackgroundUrl(item.backdrop) ?? item.backdrop,
    tagline: item.tagline === '等待刮削' ? '' : item.tagline,
    overview: item.overview,
    tmdbId: item.externalIds?.tmdb ?? '',
    imdbId: item.externalIds?.imdb ?? '',
    tvdbId: item.externalIds?.tvdb ?? ''
  }
}

function splitMetadataList(value: string): string[] {
  return value
    .split(/[,，/、]+/g)
    .map((entry) => entry.trim())
    .filter(Boolean)
}

function normalizeArtworkValue(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  if (/^url\(/i.test(trimmed)) return trimmed
  return `url("${trimmed.replace(/"/g, '%22')}")`
}

function parseMetadataYear(value: string): number {
  const year = Number(value.trim())
  return Number.isFinite(year) && year > 0 ? Math.round(year) : 0
}

function parseMetadataRating(value: string): number {
  const rating = Number(value.trim())
  if (!Number.isFinite(rating)) return 0
  return Math.max(0, Math.min(10, Math.round(rating * 10) / 10))
}

export function buildManualMetadataItem(item: MediaItem, form: MetadataEditForm): MediaItem {
  const genres = splitMetadataList(form.genres)
  const externalIds = {
    tmdb: form.tmdbId.trim() || undefined,
    imdb: form.imdbId.trim() || undefined,
    tvdb: form.tvdbId.trim() || undefined
  }
  const hasExternalIds = Boolean(externalIds.tmdb || externalIds.imdb || externalIds.tvdb)

  return {
    ...item,
    title: form.title.trim() || item.title,
    originalTitle: form.originalTitle.trim() || form.title.trim() || item.originalTitle,
    type: form.type,
    year: parseMetadataYear(form.year),
    rating: parseMetadataRating(form.rating),
    runtime: form.runtime.trim() || '未知',
    genres: genres.length ? genres : ['未分类'],
    country: form.country.trim() || '本地',
    quality: form.quality.trim() || '本地文件',
    poster: normalizeArtworkValue(form.poster),
    backdrop: normalizeArtworkValue(form.backdrop),
    tagline: form.tagline.trim() || '手动编辑',
    overview: form.overview.trim(),
    externalIds: hasExternalIds ? externalIds : undefined,
    metadataProvider: 'manual',
    metadataMatchedAt: Date.now()
  }
}

export function mergeLocalScannedItem(existingItem: MediaItem | undefined, scannedItem: MediaItem): MediaItem {
  if (!existingItem) return scannedItem

  const preservedState: Pick<MediaItem, 'progress' | 'continueWatching' | 'watched' | 'favorite'> = {
    progress: existingItem.progress,
    continueWatching: existingItem.continueWatching,
    watched: existingItem.watched,
    favorite: existingItem.favorite
  }

  if (!existingItem.metadataProvider) {
    return {
      ...scannedItem,
      ...preservedState
    }
  }

  return {
    ...scannedItem,
    ...preservedState,
    title: existingItem.title,
    originalTitle: existingItem.originalTitle,
    type: existingItem.type,
    year: existingItem.year,
    rating: existingItem.rating,
    runtime: existingItem.runtime,
    genres: existingItem.genres,
    country: existingItem.country,
    quality: existingItem.quality,
    poster: existingItem.poster,
    backdrop: existingItem.backdrop,
    tagline: existingItem.tagline,
    overview: existingItem.overview,
    cast: existingItem.cast,
    similarItems: existingItem.similarItems,
    studios: existingItem.studios,
    tags: existingItem.tags,
    versions: existingItem.versions,
    externalIds: existingItem.externalIds,
    metadataProvider: existingItem.metadataProvider,
    metadataMatchedAt: existingItem.metadataMatchedAt
  }
}
