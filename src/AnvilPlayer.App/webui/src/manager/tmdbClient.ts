import type { MediaItem, PersonCredit } from './types'

const TMDB_SETTINGS_KEY = 'anvil-player.tmdb.settings.v1'

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

interface TmdbCreditCast {
  id: number
  name: string
  character?: string
  profile_path?: string
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
}

export const DEFAULT_TMDB_SETTINGS: TmdbSettings = {
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

async function tmdbFetch<T>(settings: TmdbSettings, path: string, params: Record<string, string | number | undefined> = {}): Promise<T> {
  const credential = requireCredential(settings)
  const url = new URL(`${tmdbApiBaseUrl(settings)}${path}`)
  Object.entries(params).forEach(([key, value]) => {
    if (value !== undefined && String(value).trim()) url.searchParams.set(key, String(value))
  })
  if (settings.authMode === 'apiKey') {
    url.searchParams.set('api_key', credential)
  }

  const response = await fetch(url.toString(), {
    headers: settings.authMode === 'readToken'
      ? { Authorization: `Bearer ${credential}` }
      : undefined
  })
  if (!response.ok) {
    throw new Error(`TMDB 请求失败：${response.status} ${response.statusText}`)
  }
  return await response.json() as T
}

function itemYear(item: MediaItem): number | undefined {
  if (item.year > 0) return item.year
  const match = `${item.title} ${item.originalTitle} ${item.path ?? ''}`.match(/\b((?:19|20)\d{2})\b/)
  return match ? Number(match[1]) : undefined
}

function cleanSearchTitle(value: string): string {
  return value
    .replace(/\bS\d{1,2}E\d{1,3}\b/gi, ' ')
    .replace(/\b(?:19|20)\d{2}\b/g, ' ')
    .replace(/\b(2160p|1080p|720p|480p|4k|uhd|bluray|web-dl|webrip|hdr|dv|x265|x264)\b/gi, ' ')
    .replace(/[._-]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

function releaseYear(result: TmdbSearchResult): number {
  const date = result.release_date || result.first_air_date || ''
  return Number(date.slice(0, 4)) || 0
}

function candidateTitle(result: TmdbSearchResult): string {
  return result.title || result.name || result.original_title || result.original_name || ''
}

function scoreCandidate(result: TmdbSearchResult, query: string, year?: number): number {
  const title = candidateTitle(result).toLowerCase()
  const normalizedQuery = query.toLowerCase()
  const candidateYear = releaseYear(result)
  let score = result.popularity ?? 0
  if (title === normalizedQuery) score += 120
  else if (title.includes(normalizedQuery) || normalizedQuery.includes(title)) score += 42
  if (year && candidateYear) score += Math.max(0, 34 - Math.abs(year - candidateYear) * 14)
  if (result.poster_path) score += 6
  return score
}

async function searchBestCandidate(settings: TmdbSettings, item: MediaItem): Promise<TmdbSearchResult> {
  const mediaPath = item.type === 'series' ? '/search/tv' : '/search/movie'
  const year = itemYear(item)
  const queries = Array.from(new Set([
    cleanSearchTitle(item.title),
    cleanSearchTitle(item.originalTitle),
    cleanSearchTitle(item.path?.split(/[\\/]/g).pop() ?? '')
  ].filter(Boolean)))

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
      return [...results].sort((a, b) => scoreCandidate(b, query, year) - scoreCandidate(a, query, year))[0]
    }
  }

  throw new Error('没有找到匹配的 TMDB 条目。')
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
    poster: tmdbImageUrl(settings, details.poster_path, 'w500'),
    backdrop: tmdbImageUrl(settings, details.backdrop_path, 'w780'),
    tagline: details.tagline || 'TMDB',
    overview: details.overview || item.overview,
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
    poster: tmdbImageUrl(settings, details.poster_path, 'w500'),
    backdrop: tmdbImageUrl(settings, details.backdrop_path, 'w780'),
    tagline: details.tagline || 'TMDB',
    overview: details.overview || item.overview,
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

export async function testTmdbConnection(settings: TmdbSettings): Promise<string> {
  const configuration = await tmdbFetch<TmdbConfigurationResponse>(settings, '/configuration')
  const posterSizes = configuration.images?.poster_sizes?.join(', ') || 'unknown'
  return `TMDB 连接成功，海报尺寸：${posterSizes}`
}

export async function scrapeTmdbItem(item: MediaItem, settings: TmdbSettings): Promise<MediaItem> {
  const candidate = await searchBestCandidate(settings, item)
  if (item.type === 'series') {
    const details = await tmdbFetch<TmdbTvDetails>(settings, `/tv/${candidate.id}`, {
      language: settings.language,
      append_to_response: 'credits,external_ids'
    })
    return tvToItem(settings, item, details)
  }

  const details = await tmdbFetch<TmdbMovieDetails>(settings, `/movie/${candidate.id}`, {
    language: settings.language,
    append_to_response: 'credits,external_ids'
  })
  return movieToItem(settings, item, details)
}
