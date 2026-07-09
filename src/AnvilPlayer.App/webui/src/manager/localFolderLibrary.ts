import type { LocalFolderPickItem, LocalFolderPickResult, LocalFolderScanCompleted } from '../nativeBridge'
import type { EpisodeItem, LibrarySource, MediaItem, SeasonItem, SourceKind } from './types'

export interface LocalFolderLibrarySnapshot {
  source: LibrarySource
  items: MediaItem[]
}

interface LocalFolderDescriptor {
  name: string
  path: string
}

interface EpisodeRecord {
  file: LocalFolderPickItem
  fileIndex: number
  season: number
  episode: number
  seriesTitle: string
  episodeTitle: string
}

type LocalFolderScanPayload = Pick<LocalFolderPickResult | LocalFolderScanCompleted, 'folder' | 'items'>
interface LocalFolderBuildOptions {
  kind?: Exclude<SourceKind, 'Emby'>
  source?: LibrarySource
}

const DAY_MS = 24 * 60 * 60 * 1000

function stableHash(value: string): string {
  let hash = 2166136261
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index)
    hash = Math.imul(hash, 16777619)
  }
  return (hash >>> 0).toString(36)
}

function pathBaseName(path: string): string {
  const clean = path.trim().replace(/[\\/]+$/g, '')
  return clean.split(/[\\/]/g).pop() || clean || '本地文件夹'
}

function pathDirName(path: string): string {
  const clean = path.trim().replace(/[\\/]+$/g, '')
  const index = Math.max(clean.lastIndexOf('\\'), clean.lastIndexOf('/'))
  return index >= 0 ? clean.slice(0, index) : ''
}

function normalizePathKey(path: string): string {
  return path.trim().replace(/[\\/]+$/g, '').toLowerCase()
}

function stripExtension(name: string): string {
  return name.replace(/\.[^.\\/]+$/g, '')
}

function cleanupTitle(value: string): string {
  return value
    .replace(/\b(?:19|20)\d{2}\b/g, ' ')
    .replace(/\b(2160p|1080p|720p|480p|4k|uhd|bluray|blu-ray|web-dl|webrip|hdr|dv|x265|x264|h\.?265|h\.?264)\b/gi, ' ')
    .replace(/\[[^\]]+\]|\([^\)]*(?:2160p|1080p|720p|x265|x264|web-dl|bluray)[^\)]*\)/gi, ' ')
    .replace(/[._-]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

function displayTitle(file: LocalFolderPickItem): string {
  const rawName = stripExtension(file.name || pathBaseName(file.path))
  const withoutEpisode = rawName
    .replace(/\bS\d{1,2}[\s._-]*E\d{1,3}\b/gi, ' ')
    .replace(/\b\d{1,2}x\d{1,3}\b/gi, ' ')
    .replace(/第\s*\d{1,2}\s*[季部].*?第\s*\d{1,3}\s*[集话話]/g, ' ')
  return cleanupTitle(withoutEpisode) || rawName || file.name || pathBaseName(file.path)
}

function inferYear(file: LocalFolderPickItem): number {
  const match = `${file.name} ${file.path}`.match(/\b((?:19|20)\d{2})\b/)
  return match ? Number(match[1]) : 0
}

function inferQuality(file: LocalFolderPickItem): string {
  const value = `${file.name} ${file.path}`
  const resolution = value.match(/\b(2160p|1080p|720p|480p)\b/i)?.[1]
  if (resolution) return resolution.toUpperCase()
  if (/\b4k\b/i.test(value)) return '4K'
  return '本地文件'
}

function daysSinceModified(file: LocalFolderPickItem, index: number): number {
  if (typeof file.modifiedAt === 'number' && Number.isFinite(file.modifiedAt) && file.modifiedAt > 0) {
    return Math.max(0, Math.floor((Date.now() - file.modifiedAt) / DAY_MS))
  }
  return index
}

function seasonFolderTitle(filePath: string): string {
  const segments = filePath.split(/[\\/]/g).filter(Boolean)
  for (let index = segments.length - 2; index > 0; index -= 1) {
    if (/^(?:season\s*\d+|第\s*\d+\s*季)$/i.test(segments[index])) {
      return cleanupTitle(segments[index - 1]) || segments[index - 1]
    }
  }
  return ''
}

function seasonFromPath(filePath: string): number {
  const segments = filePath.split(/[\\/]/g).filter(Boolean)
  for (let index = segments.length - 2; index >= 0; index -= 1) {
    const match = segments[index].match(/^(?:season\s*(\d{1,2})|第\s*(\d{1,2})\s*季)$/i)
    if (match) return Number(match[1] || match[2])
  }
  return 0
}

function episodeTitleFromSuffix(rawName: string, tokenEnd: number, fallback: string): string {
  const suffix = cleanupTitle(rawName.slice(tokenEnd))
  if (suffix && !/^(?:cd|part|disc)\s*\d+$/i.test(suffix)) return suffix
  return fallback
}

function inferEpisodeRecord(file: LocalFolderPickItem, fileIndex: number, folderPath: string): EpisodeRecord | undefined {
  const rawName = stripExtension(file.name || pathBaseName(file.path))
  const folderTitle = seasonFolderTitle(file.path)
  const pathSeason = seasonFromPath(file.path)
  const fallbackTitle = cleanupTitle(folderTitle || pathBaseName(pathDirName(file.path)) || pathBaseName(folderPath))

  const namedPatterns: Array<{
    regex: RegExp
    season: (match: RegExpMatchArray) => number
    episode: (match: RegExpMatchArray) => number
  }> = [
    {
      regex: /\bS(\d{1,2})[\s._-]*E(\d{1,3})\b/i,
      season: (match) => Number(match[1]),
      episode: (match) => Number(match[2])
    },
    {
      regex: /\b(\d{1,2})x(\d{1,3})\b/i,
      season: (match) => Number(match[1]),
      episode: (match) => Number(match[2])
    },
    {
      regex: /第\s*(\d{1,2})\s*[季部].*?第\s*(\d{1,3})\s*[集话話]/,
      season: (match) => Number(match[1]),
      episode: (match) => Number(match[2])
    }
  ]

  for (const pattern of namedPatterns) {
    const match = rawName.match(pattern.regex)
    if (!match || match.index === undefined) continue
    const prefix = cleanupTitle(rawName.slice(0, match.index))
    const seriesTitle = prefix || fallbackTitle
    return {
      file,
      fileIndex,
      season: pattern.season(match) || 1,
      episode: pattern.episode(match) || fileIndex + 1,
      seriesTitle: seriesTitle || pathBaseName(folderPath),
      episodeTitle: episodeTitleFromSuffix(rawName, match.index + match[0].length, `第 ${pattern.episode(match) || fileIndex + 1} 集`)
    }
  }

  if (pathSeason > 0) {
    const match = rawName.match(/\b(?:E|EP|Episode)[\s._-]*(\d{1,3})\b/i) ?? rawName.match(/^\D*(\d{1,3})(?:\D|$)/)
    if (match) {
      const episode = Number(match[1]) || fileIndex + 1
      return {
        file,
        fileIndex,
        season: pathSeason,
        episode,
        seriesTitle: fallbackTitle || pathBaseName(folderPath),
        episodeTitle: episodeTitleFromSuffix(rawName, (match.index ?? 0) + match[0].length, `第 ${episode} 集`)
      }
    }
  }

  return undefined
}

export function localFolderSourceId(folderPath: string, kind: Exclude<SourceKind, 'Emby'> = 'Local'): string {
  const key = stableHash(folderPath.trim().toLowerCase())
  switch (kind) {
    case 'WebDAV': return `webdav-${key}`
    case 'SMB': return `smb-${key}`
    default: return `local-folder-${key}`
  }
}

export function buildLocalFolderSource(
  folder: LocalFolderDescriptor,
  itemCount = 0,
  kind: Exclude<SourceKind, 'Emby'> = 'Local'
): LibrarySource {
  const folderPath = folder.path.trim()
  const folderName = folder.name.trim() || pathBaseName(folderPath)
  return {
    id: localFolderSourceId(folderPath, kind),
    name: folderName,
    kind,
    status: 'online',
    itemCount,
    location: folderPath
  }
}

function buildFileMediaItem(
  folderPath: string,
  sourceId: string,
  file: LocalFolderPickItem,
  index: number,
  type: MediaItem['type'],
  title = displayTitle(file)
): MediaItem {
  const normalizedFolderPath = folderPath.trim()
  return {
    id: `local:${stableHash(normalizedFolderPath.toLowerCase())}:${stableHash(file.path.toLowerCase())}`,
    title,
    originalTitle: file.name || pathBaseName(file.path),
    type,
    year: inferYear(file),
    rating: 0,
    runtime: '未知',
    sourceId,
    genres: ['未分类'],
    country: '本地',
    quality: inferQuality(file),
    videoSpec: '',
    audioSpec: '',
    progress: 0,
    continueWatching: false,
    watched: false,
    favorite: false,
    addedDaysAgo: daysSinceModified(file, index),
    poster: '',
    backdrop: '',
    tagline: '等待刮削',
    overview: `本地媒体文件：${file.path}`,
    path: file.path,
    playbackPath: file.playbackPath
  }
}

function episodeIndexLabel(season: number, episode: number): string {
  return `S${String(season).padStart(2, '0')}E${String(episode).padStart(2, '0')}`
}

function seriesGroupKey(folderPath: string, title: string): string {
  return `${normalizePathKey(folderPath)}::${cleanupTitle(title).toLowerCase()}`
}

function buildSeriesItem(folderPath: string, sourceId: string, title: string, records: EpisodeRecord[], keySeed = title): MediaItem {
  const seriesTitle = cleanupTitle(title) || pathBaseName(folderPath)
  const sortedRecords = [...records].sort((left, right) =>
    left.season - right.season ||
    left.episode - right.episode ||
    left.file.name.localeCompare(right.file.name, 'zh-Hans-CN')
  )
  const seriesId = `local-series:${stableHash(folderPath.toLowerCase())}:${stableHash(`${normalizePathKey(keySeed)}::${seriesTitle.toLowerCase()}`)}`
  const episodeItems = sortedRecords.map((record, index) => {
    const label = episodeIndexLabel(record.season, record.episode)
    return buildFileMediaItem(
      folderPath,
      sourceId,
      record.file,
      record.fileIndex,
      'series',
      `${seriesTitle} ${label}`
    )
  })
  const seasonsByNumber = new Map<number, EpisodeItem[]>()
  sortedRecords.forEach((record, index) => {
    const episodeItem = episodeItems[index]
    const rows = seasonsByNumber.get(record.season) ?? []
    rows.push({
      id: episodeItem.id,
      title: record.episodeTitle || episodeItem.originalTitle,
      index: episodeIndexLabel(record.season, record.episode),
      duration: episodeItem.quality || '本地文件',
      poster: '',
      item: episodeItem
    })
    seasonsByNumber.set(record.season, rows)
  })
  const seasons: SeasonItem[] = [...seasonsByNumber.entries()]
    .sort(([left], [right]) => left - right)
    .map(([season, episodes]) => ({
      id: `${seriesId}:season:${season}`,
      title: `第 ${season} 季`,
      index: `S${season}`,
      episodeCount: episodes.length,
      poster: '',
      episodes
    }))
  const firstFile = sortedRecords[0]?.file
  const firstIndex = sortedRecords[0]?.fileIndex ?? 0
  const firstEpisode = episodeItems[0]

  return {
    id: seriesId,
    title: seriesTitle,
    originalTitle: seriesTitle,
    type: 'series',
    year: firstFile ? inferYear(firstFile) : 0,
    rating: 0,
    runtime: `${sortedRecords.length} 集`,
    sourceId,
    genres: ['未分类'],
    country: '本地',
    quality: '本地合集',
    videoSpec: '',
    audioSpec: '',
    progress: 0,
    continueWatching: false,
    watched: false,
    favorite: false,
    addedDaysAgo: firstFile ? daysSinceModified(firstFile, firstIndex) : 0,
    poster: '',
    backdrop: '',
    tagline: '等待刮削',
    overview: `本地电视剧合集：${seriesTitle}`,
    path: firstEpisode?.path,
    seasons,
    episodes: seasons[0]?.episodes
  }
}

function looksLikeLooseEpisode(file: LocalFolderPickItem): boolean {
  const rawName = stripExtension(file.name || pathBaseName(file.path))
  return /第\s*\d{1,3}\s*[集话話]/.test(rawName) ||
    /\b(?:E|EP|Episode)[\s._-]*\d{1,3}\b/i.test(rawName) ||
    /^\D{0,8}\d{1,3}(?:\D|$)/.test(rawName)
}

function directoryFallbackGroups(files: LocalFolderPickItem[], folderPath: string): Map<string, LocalFolderPickItem[]> {
  const rootKey = normalizePathKey(folderPath)
  const byDirectory = new Map<string, LocalFolderPickItem[]>()
  files.forEach((file) => {
    const directory = pathDirName(file.path)
    const rows = byDirectory.get(directory) ?? []
    rows.push(file)
    byDirectory.set(directory, rows)
  })

  const groups = new Map<string, LocalFolderPickItem[]>()
  byDirectory.forEach((rows, directory) => {
    const isRoot = normalizePathKey(directory) === rootKey
    if (rows.length < 2) return
    if (!isRoot || rows.length === files.length && rows.length <= 80 && rows.every(looksLikeLooseEpisode)) {
      groups.set(directory, rows)
    }
  })
  return groups
}

export function buildLocalFolderLibrary(result: LocalFolderScanPayload, options: LocalFolderBuildOptions = {}): LocalFolderLibrarySnapshot {
  const folderPath = result.folder.path.trim()
  const kind = options.source?.kind && options.source.kind !== 'Emby' ? options.source.kind : options.kind ?? 'Local'
  const sourceId = options.source?.id ?? localFolderSourceId(folderPath, kind)
  const seenPaths = new Set<string>()
  const files = result.items.filter((item) => {
    const key = item.path.trim().toLowerCase()
    if (!key || seenPaths.has(key)) return false
    seenPaths.add(key)
    return true
  })

  const usedPaths = new Set<string>()
  const explicitSeries = new Map<string, EpisodeRecord[]>()
  files.forEach((file, index) => {
    const record = inferEpisodeRecord(file, index, folderPath)
    if (!record) return
    const key = seriesGroupKey(folderPath, record.seriesTitle)
    const rows = explicitSeries.get(key) ?? []
    rows.push(record)
    explicitSeries.set(key, rows)
    usedPaths.add(normalizePathKey(file.path))
  })

  const items: MediaItem[] = []
  explicitSeries.forEach((records, key) => {
    items.push(buildSeriesItem(folderPath, sourceId, records[0]?.seriesTitle ?? result.folder.name, records, key))
  })

  const ungroupedFiles = files.filter((file) => !usedPaths.has(normalizePathKey(file.path)))
  const fallbackGroups = directoryFallbackGroups(ungroupedFiles, folderPath)
  fallbackGroups.forEach((rows, directory) => {
    rows.forEach((file) => usedPaths.add(normalizePathKey(file.path)))
    const records = rows
      .slice()
      .sort((left, right) => left.name.localeCompare(right.name, 'zh-Hans-CN'))
      .map((file, index): EpisodeRecord => ({
        file,
        fileIndex: files.indexOf(file),
        season: 1,
        episode: index + 1,
        seriesTitle: pathBaseName(directory) || result.folder.name,
        episodeTitle: displayTitle(file) || `第 ${index + 1} 集`
      }))
    items.push(buildSeriesItem(folderPath, sourceId, pathBaseName(directory) || result.folder.name, records, directory))
  })

  files.forEach((file, index) => {
    if (usedPaths.has(normalizePathKey(file.path))) return
    items.push(buildFileMediaItem(folderPath, sourceId, file, index, 'movie'))
  })

  items.sort((left, right) => left.addedDaysAgo - right.addedDaysAgo || left.title.localeCompare(right.title, 'zh-Hans-CN'))

  return {
    source: {
      ...buildLocalFolderSource(result.folder, items.length, kind),
      ...options.source,
      itemCount: items.length
    },
    items
  }
}
