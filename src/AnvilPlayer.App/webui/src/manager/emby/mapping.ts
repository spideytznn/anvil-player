// Emby JSON -> MediaItem mapping: label formatters, image URL builders,
// stream-spec mapping, item/episode/season mapping and home-section assembly.
// Pure transformation logic with no network access; depends only on emby/core.

import {
  apiUrl,
  TICKS_PER_MINUTE,
  type EmbyItem,
  type EmbyMediaStream,
  type EmbyPerson,
  type EmbySession,
  type EmbyView
} from './core'
import type {
  EpisodeItem,
  LibraryHomeSection,
  LibraryView,
  MediaFilterKey,
  MediaItem,
  MediaStreamSpec,
  PersonCredit,
  SortKey,
  SortOrder
} from '../types'

export interface EmbyViewLatest {
  view: EmbyView
  items: EmbyItem[]
}

export interface EmbyViewItems {
  view: EmbyView
  items: EmbyItem[]
  totalRecordCount: number
}

export function mediaKind(type?: string): MediaItem['type'] {
  switch (type) {
    case 'BoxSet': return 'folder'
    case 'Movie': return 'movie'
    case 'Video': return 'movie'
    case 'Episode': return 'movie'
    case 'Series': return 'series'
    default: return 'folder'
  }
}

export function ticksToRuntime(ticks?: number | null, fallback?: string): string {
  if (!ticks || ticks <= 0) return fallback ?? ''
  const totalMinutes = Math.max(1, Math.round(ticks / TICKS_PER_MINUTE))
  const hours = Math.floor(totalMinutes / 60)
  const minutes = totalMinutes % 60
  if (!hours) return `${minutes}m`
  return minutes ? `${hours}h ${minutes}m` : `${hours}h`
}

export function progressRatio(item: EmbyItem): number {
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

export function qualityLabel(item: EmbyItem): string {
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

export function mapCast(session: EmbySession, item: EmbyItem): PersonCredit[] {
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

export function imageBackground(session: EmbySession, item: EmbyItem, type: 'Primary' | 'Backdrop'): string {
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

export function viewImageBackground(session: EmbySession, view: EmbyView): string {
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

export function viewSubtitle(view: EmbyView): string {
  switch (view.CollectionType) {
    case 'movies': return '电影库'
    case 'tvshows': return '剧集库'
    case 'music': return '音乐库'
    default: return '媒体库'
  }
}

export function itemCardSubtitle(item: EmbyItem): string {
  const pieces = [
    item.ProductionYear ? String(item.ProductionYear) : '',
    item.Type === 'Series' ? '剧集' : item.Type === 'Movie' ? '电影' : '媒体'
  ].filter(Boolean)
  return pieces.join(' · ')
}

export function latestItemTypesForView(view: EmbyView): string {
  switch (view.CollectionType) {
    case 'movies': return 'Movie,Video'
    case 'tvshows': return 'Series'
    default: return 'Movie,Series,Video'
  }
}

export function itemTypesForLibraryView(view: LibraryView | undefined): string {
  switch (view) {
    case 'movies': return 'BoxSet,Movie,Video'
    case 'series': return 'Series'
    case 'folders': return 'Folder'
    default: return 'BoxSet,Movie,Series,Video'
  }
}

export function itemTypesForSearch(view: LibraryView | undefined): string {
  switch (view) {
    case 'movies': return 'BoxSet,Movie,Video,Episode'
    case 'series': return 'Series,Episode'
    case 'folders': return 'Folder'
    default: return 'BoxSet,Movie,Series,Video,Episode'
  }
}

export function searchResultPriority(item: EmbyItem): number {
  switch (item.Type) {
    case 'BoxSet': return 0
    case 'Movie': return 1
    case 'Video': return 2
    case 'Series': return 3
    case 'Episode': return 4
    default: return 4
  }
}

export function orderSearchResults(items: EmbyItem[]): EmbyItem[] {
  return items
    .map((item, index) => ({ item, index }))
    .sort((a, b) => {
      const priority = searchResultPriority(a.item) - searchResultPriority(b.item)
      return priority || a.index - b.index
    })
    .map((row) => row.item)
}

export function filterSupportedLatestItems(items: EmbyItem[], includeItemTypes: string): EmbyItem[] {
  const allowedTypes = new Set(includeItemTypes.split(','))
  return items.filter((item) => item.Type && allowedTypes.has(item.Type))
}

export function buildHomeSections(
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

export function mapItem(
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
  const mapped: MediaItem = {
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
    trailerUrl: item.RemoteTrailers?.find((trailer) => trailer.Url)?.Url,
    trailerUrls: item.RemoteTrailers?.flatMap((trailer) => trailer.Url ? [trailer.Url] : []),
    cast: mapCast(session, item),
    streamSpecs: mapStreamSpecs(item),
    studios: item.Studios?.map((studio) => studio.Name ?? '').filter(Boolean),
    tags: item.Tags ?? [],
    path: item.Path,
    mediaSourceId: item.MediaSources?.[0]?.Id,
    metadataProvider: 'emby',
    metadataMatchTitle: episodeTitle ?? item.Name ?? ''
  }
  const additionalSources = item.MediaSources?.slice(1) ?? []
  if (additionalSources.length) {
    mapped.versions = additionalSources.map((mediaSource, index) => {
      const versionItem = { ...item, MediaSources: [mediaSource] }
      const label = mediaSource.Name?.trim() || qualityLabel(versionItem) || `${mediaSource.Container?.toUpperCase() || 'Version'} ${index + 2}`
      return {
        ...mapped,
        id: `${item.Id}:version:${mediaSource.Id || index + 1}`,
        path: mediaSource.Path || item.Path,
        mediaSourceId: mediaSource.Id,
        providerItemId: item.Id,
        versionLabel: label,
        quality: qualityLabel(versionItem),
        videoSpec: videoSpecLabel(versionItem),
        audioSpec: audioSpecLabel(versionItem),
        streamSpecs: mapStreamSpecs(versionItem),
        versions: undefined
      }
    })
  }
  return mapped
}

export function episodeIndexLabel(item: EmbyItem): string {
  if (item.ParentIndexNumber && item.IndexNumber) {
    return `S${String(item.ParentIndexNumber).padStart(2, '0')}E${String(item.IndexNumber).padStart(2, '0')}`
  }
  if (item.IndexNumber) return `E${String(item.IndexNumber).padStart(2, '0')}`
  return 'Episode'
}

export function mapEpisodeItem(session: EmbySession, sourceId: string, item: EmbyItem, libraryViewId?: string): EpisodeItem {
  return {
    id: item.Id,
    title: item.Name ?? '未命名',
    index: episodeIndexLabel(item),
    duration: ticksToRuntime(item.RunTimeTicks),
    poster: imageBackground(session, item, 'Primary'),
    item: mapItem(session, sourceId, item, libraryViewId)
  }
}

export function sortParamsForSearch(sortKey: SortKey | undefined, sortOrder: SortOrder | undefined): { SortBy: string; SortOrder: 'Ascending' | 'Descending' } {
  const order = sortOrder === 'ascending' ? 'Ascending' : 'Descending'
  switch (sortKey) {
    case 'title': return { SortBy: 'SortName', SortOrder: order }
    case 'rating': return { SortBy: 'CommunityRating', SortOrder: order }
    case 'year': return { SortBy: 'ProductionYear', SortOrder: order }
    default: return { SortBy: 'DateCreated', SortOrder: order }
  }
}

export function filterParamsForSearch(filterKey: MediaFilterKey | undefined): Record<string, string | boolean | undefined> {
  switch (filterKey) {
    case 'unwatched': return { IsPlayed: false }
    case 'watched': return { IsPlayed: true }
    case 'favorites': return { IsFavorite: true }
    case 'inProgress': return { Filters: 'IsResumable' }
    default: return {}
  }
}
