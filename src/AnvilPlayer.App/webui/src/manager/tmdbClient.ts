import { pathBaseName, pathDirName, safeDecodeURIComponent } from './pathUtils'
import type { MediaItem, PersonCredit } from './types'

const TMDB_SETTINGS_KEY = 'anvil-player.tmdb.settings.v1'
const TMDB_REQUEST_TIMEOUT_MS = 15_000

export type TmdbNetworkMode = 'official' | 'alternate' | 'custom'
export type TmdbAuthMode = 'apiKey' | 'readToken'

export interface TmdbSettings {
  networkMode: TmdbNetworkMode
  authMode: TmdbAuthMode
  credential: string
  customApiBaseUrl: string
  imageBaseUrl: string
  language: string
}

interface TmdbConfigurationResponse {
  images?: {
    secure_base_url?: string
    poster_sizes?: string[]
    backdrop_sizes?: string[]
    profile_sizes?: string[]
  }
}

interface TmdbGenre {
  id: number
  name: string
}

interface TmdbSearchResult {
  id: number
  media_type?: 'movie' | 'tv' | 'person'
  title?: string
  name?: string
  original_title?: string
  original_name?: string
  release_date?: string
  first_air_date?: string
  overview?: string
  poster_path?: string
  backdrop_path?: string
  vote_average?: number
  popularity?: number
}

interface TmdbSearchResponse {
  results?: TmdbSearchResult[]
}

interface TmdbFindResponse {
  movie_results?: TmdbSearchResult[]
  tv_results?: TmdbSearchResult[]
}

interface TmdbCandidate {
  id: number
  type: 'movie' | 'tv'
}

export interface TmdbMatchCandidate extends TmdbCandidate {
  title: string
  originalTitle: string
  year: number
  overview: string
  poster: string
  score: number
}

interface TmdbCreditCast {
  id: number
  name: string
  character?: string
  profile_path?: string
}

interface TmdbVideo {
  key?: string
  site?: string
  type?: string
  official?: boolean
}

interface TmdbVideosResponse {
  results?: TmdbVideo[]
}

interface TmdbCreditsResponse {
  cast?: TmdbCreditCast[]
}

interface TmdbMovieDetails {
  id: number
  title?: string
  original_title?: string
  release_date?: string
  runtime?: number
  vote_average?: number
  overview?: string
  tagline?: string
  poster_path?: string
  backdrop_path?: string
  genres?: TmdbGenre[]
  production_countries?: Array<{ name: string }>
  credits?: { cast?: TmdbCreditCast[] }
  external_ids?: { imdb_id?: string }
  videos?: { results?: TmdbVideo[] }
}

interface TmdbTvDetails {
  id: number
  name?: string
  original_name?: string
  first_air_date?: string
  episode_run_time?: number[]
  vote_average?: number
  overview?: string
  tagline?: string
  poster_path?: string
  backdrop_path?: string
  genres?: TmdbGenre[]
  origin_country?: string[]
  credits?: { cast?: TmdbCreditCast[] }
  external_ids?: { imdb_id?: string; tvdb_id?: number }
  videos?: { results?: TmdbVideo[] }
}

interface TmdbSeasonEpisode {
  episode_number?: number
  name?: string
  overview?: string
  runtime?: number
  still_path?: string
}

interface TmdbSeasonDetails {
  season_number?: number
  poster_path?: string
  episodes?: TmdbSeasonEpisode[]
}

const DEFAULT_TMDB_SETTINGS: TmdbSettings = {
  networkMode: 'official',
  authMode: 'apiKey',
  credential: '',
  customApiBaseUrl: '',
  imageBaseUrl: 'https://image.tmdb.org/t/p',
  language: 'zh-CN'
}

export function loadTmdbSettings(): TmdbSettings {
  try {
    const raw = window.localStorage.getItem(TMDB_SETTINGS_KEY)
    if (!raw) return DEFAULT_TMDB_SETTINGS
    const parsed: unknown = JSON.parse(raw)
    if (!parsed || typeof parsed !== 'object') return DEFAULT_TMDB_SETTINGS
    const candidate = parsed as Partial<TmdbSettings>
    return {
      networkMode: candidate.networkMode === 'alternate' || candidate.networkMode === 'custom' ? candidate.networkMode : 'official',
      authMode: candidate.authMode === 'readToken' ? 'readToken' : 'apiKey',
      credential: typeof candidate.credential === 'string' ? candidate.credential : '',
      customApiBaseUrl: typeof candidate.customApiBaseUrl === 'string' ? candidate.customApiBaseUrl : '',
      imageBaseUrl: typeof candidate.imageBaseUrl === 'string' && candidate.imageBaseUrl.trim()
        ? candidate.imageBaseUrl
        : DEFAULT_TMDB_SETTINGS.imageBaseUrl,
      language: typeof candidate.language === 'string' && candidate.language.trim()
        ? candidate.language
        : DEFAULT_TMDB_SETTINGS.language
    }
  } catch {
    return DEFAULT_TMDB_SETTINGS
  }
}

export function saveTmdbSettings(settings: TmdbSettings): void {
  try {
    window.localStorage.setItem(TMDB_SETTINGS_KEY, JSON.stringify(settings))
  } catch {
    // Metadata settings are optional; keep the in-memory copy usable.
  }
}

export function tmdbApiBaseUrl(settings: TmdbSettings): string {
  const custom = settings.customApiBaseUrl.trim().replace(/\/+$/g, '')
  if (settings.networkMode === 'alternate') return 'https://api.tmdb.org/3'
  if (settings.networkMode === 'custom' && custom) return custom.endsWith('/3') ? custom : `${custom}/3`
  return 'https://api.themoviedb.org/3'
}

function tmdbImageBaseUrl(settings: TmdbSettings): string {
  return settings.imageBaseUrl.trim().replace(/\/+$/g, '') || DEFAULT_TMDB_SETTINGS.imageBaseUrl
}

function tmdbImageUrl(settings: TmdbSettings, path: string | undefined, size: string): string {
  if (!path) return ''
  const base = tmdbImageBaseUrl(settings)
  const imageUrl = base.includes('{path}') || base.includes('{size}')
    ? base.replace(/\{size\}/g, size).replace(/\{path\}/g, path)
    : `${base}/${size}${path}`
  return `url("${imageUrl.replace(/"/g, '%22')}")`
}

function requireCredential(settings: TmdbSettings): string {
  const credential = settings.credential.trim()
  if (!credential) {
    throw new Error('请先在设置中填写 TMDB API Key 或 Read Access Token。')
  }
  return credential
}

async function tmdbFetch<T>(
  settings: TmdbSettings,
  path: string,
  params: Record<string, string | number | undefined> = {},
  signal?: AbortSignal
): Promise<T> {
  const credential = requireCredential(settings)
  const url = new URL(`${tmdbApiBaseUrl(settings)}${path}`)
  Object.entries(params).forEach(([key, value]) => {
    if (value !== undefined && String(value).trim()) url.searchParams.set(key, String(value))
  })
  if (settings.authMode === 'apiKey') {
    url.searchParams.set('api_key', credential)
  }

  const requestController = new AbortController()
  let timedOut = false
  const abortFromCaller = (): void => requestController.abort(signal?.reason)
  if (signal?.aborted) {
    abortFromCaller()
  } else {
    signal?.addEventListener('abort', abortFromCaller, { once: true })
  }
  const timeout = window.setTimeout(() => {
    timedOut = true
    requestController.abort()
  }, TMDB_REQUEST_TIMEOUT_MS)

  try {
    const response = await fetch(url.toString(), {
      signal: requestController.signal,
      headers: settings.authMode === 'readToken'
        ? { Authorization: `Bearer ${credential}` }
        : undefined
    })
    if (!response.ok) {
      throw new Error(`TMDB 请求失败：${response.status} ${response.statusText}`)
    }
    return await response.json() as T
  } catch (error) {
    if (timedOut) {
      throw new Error(`TMDB 请求超时（${TMDB_REQUEST_TIMEOUT_MS / 1000} 秒）`)
    }
    throw error
  } finally {
    window.clearTimeout(timeout)
    signal?.removeEventListener('abort', abortFromCaller)
  }
}

function itemYear(item: MediaItem): number | undefined {
  if (item.year > 0) return item.year
  const match = `${item.title} ${item.originalTitle} ${item.path ?? ''}`.match(/\b((?:19|20)\d{2})\b/)
  return match ? Number(match[1]) : undefined
}

function cleanSearchTitle(value: string): string {
  return safeDecodeURIComponent(value)
    .replace(/%[0-9a-f]{2}/gi, ' ')
    .replace(/\.(?:mkv|mp4|m4v|avi|mov|wmv|ts|m2ts|webm|flv|iso)$/gi, ' ')
    .replace(/\{(?:tmdb(?:id)?|imdb|tvdb)[-_: ](?:tt)?\d+\}|\[(?:tmdb(?:id)?|imdb|tvdb)[-_: ](?:tt)?\d+\]/gi, ' ')
    .replace(/\bS\d{1,2}[\s._-]*E\d{1,3}\b/gi, ' ')
    .replace(/\b\d{1,2}x\d{1,3}\b/gi, ' ')
    .replace(/\bseason\s*\d{1,2}\b/gi, ' ')
    .replace(/\bs\d{1,2}\b/gi, ' ')
    .replace(/第\s*\d{1,2}\s*[季部].*?第\s*\d{1,3}\s*[集话話]/g, ' ')
    .replace(/第\s*\d{1,3}\s*[集话話]/g, ' ')
    .replace(/\b(?:19|20)\d{2}\b/g, ' ')
    .replace(/\b\d+(?:\.\d+)?\s*(?:gib|gb|mib|mb)\b/gi, ' ')
    .replace(/\b(?:3840x2160|4096x2160|1920x1080|1280x720)\b/gi, ' ')
    .replace(/\b(2160p|1080p|720p|480p|4k|8k|uhd|bluray|blu-ray|bdrip|bdremux|web(?:\s|-)?dl|webrip|hdtv|hdr10\+?|hdr|sdr|dovi|dolby[ ._-]?vision|dv|x265|x264|h\.?265|h\.?264|hevc|avc|av1|10bit|12bit|remux|proper|repack|hdsweb|truehd|dts(?:[ ._-]?hd(?:[ ._-]?ma)?)?|ddp?|eac3|ac3|flac|lpcm|aac|atmos|dual[ ._-]?audio|multi[ ._-]?audio)\b/gi, ' ')
    .replace(/\b(?:extended|director'?s[ ._-]?cut|theatrical|unrated|uncut|imax|remastered|criterion)[ ._-]?(?:cut|edition|version)?\b/gi, ' ')
    .replace(/\b(?:5\.1|7\.1|2\.0)\b/g, ' ')
    .replace(/-[a-z0-9]{2,16}$/i, ' ')
    .replace(/\[[^\]]*(?:2160|1080|720|bluray|remux|web|hevc|avc|truehd|dts|gb)[^\]]*\]/gi, ' ')
    .replace(/[()[\]{}（）]+/g, ' ')
    .replace(/[._-]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

function isWeakSearchTitle(value: string): boolean {
  return !value || /^(?:season|series|episode|ep|s)\s*\d*$/i.test(value.trim())
}

function releaseYear(result: TmdbSearchResult): number {
  const date = result.release_date || result.first_air_date || ''
  return Number(date.slice(0, 4)) || 0
}

function normalizedMatchTitle(value: string): string {
  return value.normalize('NFKC').toLowerCase().replace(/[^\p{L}\p{N}]+/gu, '')
}

function titleSimilarity(left: string, right: string): number {
  const normalizedLeft = normalizedMatchTitle(left)
  const normalizedRight = normalizedMatchTitle(right)
  if (!normalizedLeft || !normalizedRight) return 0
  if (normalizedLeft === normalizedRight) return 1
  const leftTokens = new Set<string>()
  const rightTokens = new Set<string>()
  for (let index = 0; index < Math.max(1, normalizedLeft.length - 1); index += 1) leftTokens.add(normalizedLeft.slice(index, index + 2))
  for (let index = 0; index < Math.max(1, normalizedRight.length - 1); index += 1) rightTokens.add(normalizedRight.slice(index, index + 2))
  let overlap = 0
  leftTokens.forEach((token) => {
    if (rightTokens.has(token)) overlap += 1
  })
  return (2 * overlap) / Math.max(1, leftTokens.size + rightTokens.size)
}

function scoreCandidate(result: TmdbSearchResult, query: string, year: number | undefined, expectedType: 'movie' | 'tv'): number {
  const titles = [result.title, result.name, result.original_title, result.original_name].filter((value): value is string => Boolean(value))
  const similarity = Math.max(0, ...titles.map((title) => titleSimilarity(title, query)))
  const candidateYear = releaseYear(result)
  const resultType = result.media_type === 'tv' ? 'tv' : result.media_type === 'movie' ? 'movie' : expectedType
  let score = similarity === 1 ? 140 : similarity * 75
  if (year && candidateYear) {
    const difference = Math.abs(year - candidateYear)
    if (difference === 0) score += 45
    else if (difference === 1) score += 25
    else if (difference === 2) score += 8
    else score -= Math.min(35, difference * 8)
  }
  if (resultType === expectedType) score += 30
  if (result.poster_path) score += 8
  if (result.overview) score += 4
  score += Math.min(5, Math.max(0, result.popularity ?? 0) / 20)
  return score
}

function publicCandidate(
  settings: TmdbSettings,
  result: TmdbSearchResult,
  query: string,
  year: number | undefined,
  expectedType: 'movie' | 'tv'
): TmdbMatchCandidate {
  const type = result.media_type === 'tv' ? 'tv' : result.media_type === 'movie' ? 'movie' : expectedType
  return {
    id: result.id,
    type,
    title: result.title || result.name || result.original_title || result.original_name || `TMDB ${result.id}`,
    originalTitle: result.original_title || result.original_name || result.title || result.name || '',
    year: releaseYear(result),
    overview: result.overview || '',
    poster: tmdbImageUrl(settings, result.poster_path, 'w300'),
    score: scoreCandidate({ ...result, media_type: type }, query, year, expectedType)
  }
}

export async function searchTmdbCandidates(
  settings: TmdbSettings,
  item: MediaItem,
  manualQuery = '',
  signal?: AbortSignal
): Promise<TmdbMatchCandidate[]> {
  const expectedType: 'movie' | 'tv' = item.type === 'series' ? 'tv' : 'movie'
  const query = manualQuery.trim() || buildTmdbSearchQueries(item)[0]
  if (!query) return []
  const response = await tmdbFetch<TmdbSearchResponse>(settings, '/search/multi', {
    query,
    language: settings.language,
    include_adult: 'false',
    page: 1
  }, signal)
  const candidates = (response.results ?? [])
    .filter((result) => result.media_type === 'movie' || result.media_type === 'tv')
    .map((result) => publicCandidate(settings, result, query, itemYear(item), expectedType))
    .sort((left, right) => right.score - left.score)
  return candidates.filter((candidate, index) =>
    candidates.findIndex((row) => row.id === candidate.id && row.type === candidate.type) === index
  ).slice(0, 20)
}

async function searchBestCandidateLegacy(settings: TmdbSettings, item: MediaItem): Promise<TmdbSearchResult> {
  const mediaPath = item.type === 'series' ? '/search/tv' : '/search/movie'
  const year = itemYear(item)
  const queries = Array.from(new Set([
    cleanSearchTitle(item.title),
    cleanSearchTitle(item.originalTitle),
    cleanSearchTitle(pathBaseName(item.path ?? ''))
  ].filter((query) => !isWeakSearchTitle(query))))

  for (const query of queries) {
    const response = await tmdbFetch<TmdbSearchResponse>(settings, mediaPath, {
      query,
      language: settings.language,
      include_adult: 'false',
      page: 1,
      year: item.type === 'movie' ? year : undefined,
      first_air_date_year: item.type === 'series' ? year : undefined
    })
    const results = response.results ?? []
    if (results.length) {
      const expectedType = item.type === 'series' ? 'tv' : 'movie'
      return [...results].sort((a, b) => scoreCandidate(b, query, year, expectedType) - scoreCandidate(a, query, year, expectedType))[0]
    }
  }

  throw new Error('没有找到匹配的 TMDB 条目。')
}

function explicitProviderIds(item: MediaItem): { tmdb?: number; imdb?: string } {
  const text = [item.title, item.originalTitle, item.path, pathBaseName(pathDirName(item.path ?? ''))].filter(Boolean).join(' ')
  const tmdbMatch = text.match(/[\[{]tmdb(?:id)?[-_: ](\d+)[\]}]/i) ?? text.match(/\btmdb(?:id)?[-_: ](\d+)\b/i)
  const imdbMatch = text.match(/\btt\d{7,10}\b/i)
  const tmdb = tmdbMatch ? Number(tmdbMatch[1]) : undefined
  return { tmdb: tmdb && Number.isFinite(tmdb) ? tmdb : undefined, imdb: imdbMatch?.[0].toLowerCase() }
}

function coreTitleVariants(value: string): string[] {
  const decoded = safeDecodeURIComponent(value).replace(/\.(?:mkv|mp4|m4v|avi|mov|wmv|ts|m2ts|webm|flv|iso)$/gi, ' ')
  const variants = [cleanSearchTitle(decoded)]
  const beforeYear = decoded.match(/^(.+?)[\s._[(（-]+(?:19|20)\d{2}(?:[\])）]|\b)/)?.[1]
  if (beforeYear) variants.push(cleanSearchTitle(beforeYear))
  const beforeTechnical = decoded.split(/\b(?:2160p|1080p|720p|4k|8k|uhd|bluray|remux|web[ ._-]?dl|webrip|hdr|dovi|dv|hevc|avc|x26[45]|truehd|dts|ddp|\d+(?:\.\d+)?\s*(?:gib|gb))\b/i)[0]
  if (beforeTechnical) variants.push(cleanSearchTitle(beforeTechnical))
  return variants.filter((query) => !isWeakSearchTitle(query))
}

export function buildTmdbSearchQueries(item: MediaItem): string[] {
  const parentFolder = item.path ? pathBaseName(pathDirName(item.path)) : ''
  return Array.from(new Set([
    ...coreTitleVariants(item.title),
    ...coreTitleVariants(item.originalTitle),
    ...coreTitleVariants(pathBaseName(item.path ?? '')),
    ...coreTitleVariants(parentFolder)
  ])).slice(0, 8)
}

async function findByImdbId(settings: TmdbSettings, imdbId: string, expectedType: 'movie' | 'tv', signal?: AbortSignal): Promise<TmdbCandidate | undefined> {
  const response = await tmdbFetch<TmdbFindResponse>(settings, `/find/${imdbId}`, {
    external_source: 'imdb_id',
    language: settings.language
  }, signal)
  const preferred = expectedType === 'movie' ? response.movie_results?.[0] : response.tv_results?.[0]
  const alternate = expectedType === 'movie' ? response.tv_results?.[0] : response.movie_results?.[0]
  const result = preferred ?? alternate
  if (!result) return undefined
  return { id: result.id, type: preferred ? expectedType : expectedType === 'movie' ? 'tv' : 'movie' }
}

async function searchBestCandidate(settings: TmdbSettings, item: MediaItem, signal?: AbortSignal): Promise<TmdbCandidate> {
  const expectedType: 'movie' | 'tv' = item.type === 'series' ? 'tv' : 'movie'
  const explicitIds = explicitProviderIds(item)
  if (explicitIds.tmdb) return { id: explicitIds.tmdb, type: expectedType }
  if (explicitIds.imdb) {
    const result = await findByImdbId(settings, explicitIds.imdb, expectedType, signal)
    if (result) return result
  }

  const mediaPath = expectedType === 'movie' ? '/search/movie' : '/search/tv'
  const year = itemYear(item)
  const queries = buildTmdbSearchQueries(item)
  const candidates = new Map<string, { result: TmdbSearchResult; score: number; type: 'movie' | 'tv' }>()
  const addResults = (results: TmdbSearchResult[], query: string, fallbackType: 'movie' | 'tv'): void => {
    results.forEach((result) => {
      if (result.media_type === 'person') return
      const type = result.media_type === 'tv' ? 'tv' : result.media_type === 'movie' ? 'movie' : fallbackType
      const scoredResult = { ...result, media_type: type }
      const score = scoreCandidate(scoredResult, query, year, expectedType)
      const key = `${type}:${result.id}`
      if (score > (candidates.get(key)?.score ?? Number.NEGATIVE_INFINITY)) candidates.set(key, { result: scoredResult, score, type })
    }, signal)
  }
  const bestScore = (): number => Math.max(Number.NEGATIVE_INFINITY, ...[...candidates.values()].map((candidate) => candidate.score))

  for (const query of queries) {
    const response = await tmdbFetch<TmdbSearchResponse>(settings, mediaPath, {
      query,
      language: settings.language,
      include_adult: 'false',
      page: 1,
      primary_release_year: expectedType === 'movie' ? year : undefined,
      first_air_date_year: expectedType === 'tv' ? year : undefined
    }, signal)
    addResults(response.results ?? [], query, expectedType)
    if (bestScore() >= 160) break
  }

  if (bestScore() < 80) {
    for (const query of queries.slice(0, 4)) {
      const response = await tmdbFetch<TmdbSearchResponse>(settings, mediaPath, {
        query,
        language: settings.language,
        include_adult: 'false',
        page: 1
      }, signal)
      addResults(response.results ?? [], query, expectedType)
      if (bestScore() >= 140) break
    }
  }

  if (bestScore() < 80) {
    for (const query of queries.slice(0, 3)) {
      const response = await tmdbFetch<TmdbSearchResponse>(settings, '/search/multi', {
        query,
        language: settings.language,
        include_adult: 'false',
        page: 1
      }, signal)
      addResults(response.results ?? [], query, expectedType)
      if (bestScore() >= 140) break
    }
  }

  const ranked = [...candidates.values()].sort((left, right) => right.score - left.score)
  const best = ranked[0]
  if (!best || best.score < 80) throw new Error('没有找到可信度足够的 TMDB 条目')
  const runnerUp = ranked.find((candidate) => candidate.result.id !== best.result.id || candidate.type !== best.type)
  if (runnerUp && best.score < 140 && best.score - runnerUp.score < 10) {
    throw new Error('找到多个相近的 TMDB 条目，请手动指定 TMDB 或 IMDb ID')
  }
  return { id: best.result.id, type: best.type }
}

function formatRuntime(minutes: number | undefined): string {
  if (!minutes || minutes <= 0) return '未知'
  const hours = Math.floor(minutes / 60)
  const rest = minutes % 60
  return hours > 0 ? `${hours}h ${rest}m` : `${rest}m`
}

function castRows(settings: TmdbSettings, cast: TmdbCreditCast[] | undefined): PersonCredit[] {
  return (cast ?? []).slice(0, 12).map((person) => ({
    id: String(person.id),
    name: person.name,
    role: person.character ?? '',
    image: tmdbImageUrl(settings, person.profile_path, 'w185')
  }))
}

export async function fetchTmdbCast(
  item: MediaItem,
  settings: TmdbSettings,
  signal?: AbortSignal
): Promise<PersonCredit[]> {
  const tmdbId = Number(item.externalIds?.tmdb)
  if (!Number.isFinite(tmdbId) || tmdbId <= 0 || item.type === 'folder') return []
  const mediaPath = item.type === 'series' ? 'tv' : 'movie'
  const response = await tmdbFetch<TmdbCreditsResponse>(settings, `/${mediaPath}/${tmdbId}/credits`, {
    language: settings.language
  }, signal)
  return castRows(settings, response.cast)
}

function trailerUrls(videos: TmdbVideo[] | undefined): string[] {
  return (videos ?? [])
    .filter((video) => video.site === 'YouTube' && video.key)
    .map((video, index) => ({
      video,
      index,
      score: (video.type === 'Trailer' ? 100 : video.type === 'Teaser' ? 40 : 0) + (video.official ? 20 : 0)
    }))
    .sort((left, right) => right.score - left.score || left.index - right.index)
    .map(({ video }) => `https://www.youtube.com/watch?v=${encodeURIComponent(video.key!)}`)
}

function trailerUrl(videos: TmdbVideo[] | undefined): string | undefined {
  return trailerUrls(videos)[0]
}

function hasTrailer(videos: TmdbVideo[]): boolean {
  return videos.some((video) => video.site === 'YouTube' && video.key && video.type === 'Trailer')
}

function mergeVideos(...groups: Array<TmdbVideo[] | undefined>): TmdbVideo[] {
  const seen = new Set<string>()
  return groups.flatMap((group) => group ?? []).filter((video) => {
    const key = `${video.site ?? ''}:${video.key ?? ''}`
    if (!video.key || seen.has(key)) return false
    seen.add(key)
    return true
  })
}

async function loadTmdbVideos(
  settings: TmdbSettings,
  path: string,
  appendedVideos: TmdbVideo[] | undefined,
  signal?: AbortSignal
): Promise<TmdbVideo[]> {
  let preferredVideos = appendedVideos ?? []
  try {
    const response = await tmdbFetch<TmdbVideosResponse>(settings, `${path}/videos`, {
      language: settings.language
    }, signal)
    preferredVideos = mergeVideos(preferredVideos, response.results)
  } catch {
    signal?.throwIfAborted()
    // Trailer lookup is optional; keep the rest of the scraped metadata usable.
  }
  if (hasTrailer(preferredVideos) || settings.language.toLowerCase() === 'en-us') return preferredVideos

  try {
    const fallback = await tmdbFetch<TmdbVideosResponse>(settings, `${path}/videos`, {
      language: 'en-US'
    }, signal)
    return mergeVideos(preferredVideos, fallback.results)
  } catch {
    signal?.throwIfAborted()
    return preferredVideos
  }
}

function movieToItem(settings: TmdbSettings, item: MediaItem, details: TmdbMovieDetails): MediaItem {
  const year = Number((details.release_date ?? '').slice(0, 4)) || item.year
  return {
    ...item,
    title: details.title || item.title,
    originalTitle: details.original_title || item.originalTitle,
    type: 'movie',
    year,
    rating: Number((details.vote_average ?? item.rating).toFixed(1)),
    runtime: formatRuntime(details.runtime),
    genres: details.genres?.map((genre) => genre.name) ?? item.genres,
    country: details.production_countries?.map((country) => country.name).filter(Boolean).join(' / ') || item.country,
    poster: tmdbImageUrl(settings, details.poster_path, 'w500') || item.poster,
    backdrop: tmdbImageUrl(settings, details.backdrop_path, 'w780') || item.backdrop,
    tagline: details.tagline || 'TMDB',
    overview: details.overview || item.overview,
    trailerUrl: trailerUrl(details.videos?.results) || item.trailerUrl,
    trailerUrls: trailerUrls(details.videos?.results).length ? trailerUrls(details.videos?.results) : item.trailerUrls,
    cast: castRows(settings, details.credits?.cast),
    externalIds: {
      ...item.externalIds,
      tmdb: String(details.id),
      imdb: details.external_ids?.imdb_id || item.externalIds?.imdb
    },
    metadataProvider: 'tmdb',
    metadataMatchedAt: Date.now()
  }
}

function tvToItem(settings: TmdbSettings, item: MediaItem, details: TmdbTvDetails): MediaItem {
  const year = Number((details.first_air_date ?? '').slice(0, 4)) || item.year
  return {
    ...item,
    title: details.name || item.title,
    originalTitle: details.original_name || item.originalTitle,
    type: 'series',
    year,
    rating: Number((details.vote_average ?? item.rating).toFixed(1)),
    runtime: formatRuntime(details.episode_run_time?.[0]),
    genres: details.genres?.map((genre) => genre.name) ?? item.genres,
    country: details.origin_country?.join(' / ') || item.country,
    poster: tmdbImageUrl(settings, details.poster_path, 'w500') || item.poster,
    backdrop: tmdbImageUrl(settings, details.backdrop_path, 'w780') || item.backdrop,
    tagline: details.tagline || 'TMDB',
    overview: details.overview || item.overview,
    trailerUrl: trailerUrl(details.videos?.results) || item.trailerUrl,
    trailerUrls: trailerUrls(details.videos?.results).length ? trailerUrls(details.videos?.results) : item.trailerUrls,
    cast: castRows(settings, details.credits?.cast),
    externalIds: {
      ...item.externalIds,
      tmdb: String(details.id),
      imdb: details.external_ids?.imdb_id || item.externalIds?.imdb,
      tvdb: details.external_ids?.tvdb_id ? String(details.external_ids.tvdb_id) : item.externalIds?.tvdb
    },
    metadataProvider: 'tmdb',
    metadataMatchedAt: Date.now()
  }
}

export async function refreshTmdbCoreMetadata(
  item: MediaItem,
  settings: TmdbSettings,
  signal?: AbortSignal
): Promise<MediaItem> {
  const tmdbId = Number(item.externalIds?.tmdb)
  if (!Number.isFinite(tmdbId) || tmdbId <= 0) {
    throw new Error('缺少有效的 TMDB ID')
  }
  const updatedItem = item.type === 'series'
    ? tvToItem(settings, item, await tmdbFetch<TmdbTvDetails>(settings, `/tv/${tmdbId}`, {
        language: settings.language,
        append_to_response: 'credits,external_ids'
      }, signal))
    : movieToItem(settings, item, await tmdbFetch<TmdbMovieDetails>(settings, `/movie/${tmdbId}`, {
        language: settings.language,
        append_to_response: 'credits,external_ids'
      }, signal))
  return {
    ...updatedItem,
    metadataMatchTitle: item.metadataMatchTitle || updatedItem.title
  }
}

async function addLocalEpisodeMetadata(
  settings: TmdbSettings,
  item: MediaItem,
  tmdbId: number,
  signal?: AbortSignal
): Promise<MediaItem> {
  if (!item.seasons?.length) return item
  const seasonNumbers = Array.from(new Set(item.seasons.map((season) => Number(season.index.replace(/^S/i, ''))).filter(Number.isFinite)))
  const seasonDetails = await Promise.all(seasonNumbers.map(async (seasonNumber) => {
    try {
      return await tmdbFetch<TmdbSeasonDetails>(settings, `/tv/${tmdbId}/season/${seasonNumber}`, { language: settings.language }, signal)
    } catch {
      signal?.throwIfAborted()
      return undefined
    }
  }))
  const detailsBySeason = new Map(seasonDetails.filter((season): season is TmdbSeasonDetails => Boolean(season)).map((season) => [season.season_number ?? -1, season]))
  const seasons = item.seasons.map((season) => {
    const seasonNumber = Number(season.index.replace(/^S/i, ''))
    const details = detailsBySeason.get(seasonNumber)
    if (!details) return season
    const episodesByNumber = new Map((details.episodes ?? []).map((episode) => [episode.episode_number ?? -1, episode]))
    return {
      ...season,
      poster: tmdbImageUrl(settings, details.poster_path, 'w300') || season.poster,
      episodes: season.episodes.map((episode) => {
        const match = episode.index.match(/E(\d+)/i)
        const episodeNumber = match ? Number(match[1]) : 0
        const metadata = episodesByNumber.get(episodeNumber)
        if (!metadata) return episode
        const poster = tmdbImageUrl(settings, metadata.still_path, 'w300') || episode.poster
        return {
          ...episode,
          title: metadata.name || episode.title,
          duration: metadata.runtime ? formatRuntime(metadata.runtime) : episode.duration,
          poster,
          item: episode.item ? {
            ...episode.item,
            title: metadata.name || episode.item.title,
            overview: metadata.overview || episode.item.overview,
            runtime: metadata.runtime ? formatRuntime(metadata.runtime) : episode.item.runtime,
            poster,
            backdrop: poster || episode.item.backdrop
          } : episode.item
        }
      })
    }
  })
  return { ...item, seasons, episodes: seasons[0]?.episodes ?? item.episodes }
}

export async function testTmdbConnection(settings: TmdbSettings, signal?: AbortSignal): Promise<string> {
  const configuration = await tmdbFetch<TmdbConfigurationResponse>(settings, '/configuration', {}, signal)
  const posterSizes = configuration.images?.poster_sizes?.join(', ') || 'unknown'
  return `TMDB 连接成功，海报尺寸：${posterSizes}`
}

export async function scrapeTmdbCandidate(
  item: MediaItem,
  settings: TmdbSettings,
  candidate: Pick<TmdbMatchCandidate, 'id' | 'type' | 'title'>,
  signal?: AbortSignal
): Promise<MediaItem> {
  let updatedItem: MediaItem
  if (candidate.type === 'tv') {
    const details = await tmdbFetch<TmdbTvDetails>(settings, `/tv/${candidate.id}`, {
      language: settings.language,
      append_to_response: 'credits,external_ids,videos'
    }, signal)
    const videos = await loadTmdbVideos(settings, `/tv/${candidate.id}`, details.videos?.results, signal)
    updatedItem = await addLocalEpisodeMetadata(settings, tvToItem(settings, item, {
      ...details,
      videos: { results: videos }
    }), candidate.id, signal)
  } else {
    const details = await tmdbFetch<TmdbMovieDetails>(settings, `/movie/${candidate.id}`, {
      language: settings.language,
      append_to_response: 'credits,external_ids,videos'
    }, signal)
    const videos = await loadTmdbVideos(settings, `/movie/${candidate.id}`, details.videos?.results, signal)
    updatedItem = movieToItem(settings, item, {
      ...details,
      videos: { results: videos }
    })
  }
  return {
    ...updatedItem,
    metadataMatchTitle: candidate.title || updatedItem.title
  }
}

export async function scrapeTmdbItem(
  item: MediaItem,
  settings: TmdbSettings,
  options: { ignoreSavedMatch?: boolean; signal?: AbortSignal } = {}
): Promise<MediaItem> {
  if (item.metadataLocked && !options.ignoreSavedMatch) return item
  const savedTmdbId = Number(item.externalIds?.tmdb)
  const candidate: TmdbCandidate = !options.ignoreSavedMatch && Number.isFinite(savedTmdbId) && savedTmdbId > 0
    ? { id: savedTmdbId, type: item.type === 'series' ? 'tv' : 'movie' }
    : await searchBestCandidate(settings, item, options.signal)
  return await scrapeTmdbCandidate(item, settings, {
    ...candidate,
    title: ''
  }, options.signal)
}

export async function fetchTmdbTrailerUrls(item: MediaItem, settings: TmdbSettings, signal?: AbortSignal): Promise<string[]> {
  const savedTmdbId = Number(item.externalIds?.tmdb)
  const candidate: TmdbCandidate = Number.isFinite(savedTmdbId) && savedTmdbId > 0
    ? { id: savedTmdbId, type: item.type === 'series' ? 'tv' : 'movie' }
    : await searchBestCandidate(settings, item, signal)
  const path = candidate.type === 'tv' ? `/tv/${candidate.id}` : `/movie/${candidate.id}`
  return trailerUrls(await loadTmdbVideos(settings, path, undefined, signal))
}
