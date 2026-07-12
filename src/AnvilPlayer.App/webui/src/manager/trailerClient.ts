import { postNativeCommand, subscribeNativeMessages } from '../nativeBridge'
import type { MediaItem } from './types'

const TRAILER_SETTINGS_KEY = 'anvil-player.trailer.settings.v1'

export type TrailerSource = 'youtube' | 'bilibili'

export interface TrailerSettings {
  source: TrailerSource
  bilibiliKeywordTemplate: string
  soundEnabled: boolean
  autoPlayOnDetails: boolean
}

const DEFAULT_TRAILER_SETTINGS: TrailerSettings = {
  source: 'youtube',
  bilibiliKeywordTemplate: '{title} {year} 预告',
  soundEnabled: true,
  autoPlayOnDetails: false
}

interface BilibiliSearchItem {
  bvid?: string
  title?: string
  description?: string
  author?: string
  typename?: string
  tag?: string
  duration?: string
  play?: number | string
  rank_index?: number
}

interface BilibiliSearchPayload {
  code?: number
  message?: string
  data?: { result?: BilibiliSearchItem[] }
}

function cleanText(value: string | undefined): string {
  if (!value) return ''
  const element = document.createElement('div')
  element.innerHTML = value
  return (element.textContent ?? '').replace(/\s+/g, ' ').trim()
}

function normalized(value: string): string {
  return value.toLocaleLowerCase().replace(/[\s·:：—_\-《》【】\[\]（）()'"“”‘’]/g, '')
}

function durationSeconds(value: string | undefined): number {
  if (!value) return 0
  return value.split(':').reduce((total, part) => total * 60 + Number(part || 0), 0)
}

function candidateScore(candidate: BilibiliSearchItem, item: MediaItem): number {
  const title = cleanText(candidate.title)
  const text = normalized(`${title} ${candidate.description ?? ''} ${candidate.tag ?? ''}`)
  const mediaTitle = normalized(item.title)
  const originalTitle = normalized(item.originalTitle || '')
  let score = 0

  if (mediaTitle && text.includes(mediaTitle)) score += 90
  if (originalTitle && originalTitle !== mediaTitle && text.includes(originalTitle)) score += 45
  if (/预告片|正式预告|终极预告|官方预告|trailer|teaser/i.test(title)) score += 55
  else if (/预告|先导/i.test(title)) score += 28
  if (/官方|电影官方|放映员|预告·资讯/i.test(`${candidate.author ?? ''} ${candidate.typename ?? ''}`)) score += 26
  if (item.year > 0 && text.includes(String(item.year))) score += 8
  if (/reaction|解说|混剪|片段|cut|幕后|花絮|采访|主题曲|reaction/i.test(title)) score -= 55

  const seconds = durationSeconds(candidate.duration)
  if (seconds >= 45 && seconds <= 240) score += 18
  else if (seconds > 600 || seconds < 20) score -= 25

  const plays = Number(candidate.play ?? 0)
  if (Number.isFinite(plays) && plays > 0) score += Math.min(12, Math.log10(plays + 1) * 2)
  score -= Math.min(10, Math.max(0, Number(candidate.rank_index ?? 1) - 1) * 0.5)
  return score
}

function searchKeyword(item: MediaItem, template: string): string {
  const resolved = template
    .replaceAll('{title}', item.title)
    .replaceAll('{originalTitle}', item.originalTitle || '')
    .replaceAll('{year}', item.year > 0 ? String(item.year) : '')
    .replace(/\s+/g, ' ')
    .trim()
  return resolved || `${item.title} 预告片`
}

export function loadTrailerSettings(): TrailerSettings {
  try {
    const parsed = JSON.parse(localStorage.getItem(TRAILER_SETTINGS_KEY) || '{}') as Partial<TrailerSettings>
    const savedKeywordTemplate = parsed.bilibiliKeywordTemplate === '{title} {year} 官方预告片'
      ? DEFAULT_TRAILER_SETTINGS.bilibiliKeywordTemplate
      : parsed.bilibiliKeywordTemplate
    return {
      source: parsed.source === 'bilibili' ? 'bilibili' : 'youtube',
      bilibiliKeywordTemplate: typeof savedKeywordTemplate === 'string' && savedKeywordTemplate.trim()
        ? savedKeywordTemplate
        : DEFAULT_TRAILER_SETTINGS.bilibiliKeywordTemplate,
      soundEnabled: typeof parsed.soundEnabled === 'boolean'
        ? parsed.soundEnabled
        : (parsed as Partial<TrailerSettings> & { bilibiliSoundEnabled?: boolean }).bilibiliSoundEnabled !== false,
      autoPlayOnDetails: parsed.autoPlayOnDetails === true
    }
  } catch {
    return DEFAULT_TRAILER_SETTINGS
  }
}

export function saveTrailerSettings(settings: TrailerSettings): void {
  localStorage.setItem(TRAILER_SETTINGS_KEY, JSON.stringify(settings))
}

export function trailerUrlMatchesSource(url: string, source: TrailerSource): boolean {
  try {
    const host = new URL(url).hostname.toLocaleLowerCase()
    return source === 'bilibili'
      ? host === 'bilibili.com' || host.endsWith('.bilibili.com') || host === 'b23.tv'
      : host === 'youtu.be' || host === 'youtube.com' || host.endsWith('.youtube.com')
  } catch {
    return false
  }
}

export async function searchBilibiliTrailerUrls(
  item: MediaItem,
  settings: TrailerSettings,
  signal?: AbortSignal
): Promise<string[]> {
  const requestId = `bilibili-trailer-${Date.now()}-${Math.random().toString(36).slice(2)}`
  const keyword = searchKeyword(item, settings.bilibiliKeywordTemplate)
  const payload = await new Promise<BilibiliSearchPayload>((resolve, reject) => {
    let unsubscribe = (): void => {}
    const abort = (): void => {
      window.clearTimeout(timeout)
      unsubscribe()
      reject(new DOMException('The trailer request was cancelled', 'AbortError'))
    }
    const timeout = window.setTimeout(() => {
      signal?.removeEventListener('abort', abort)
      unsubscribe()
      reject(new Error('B 站搜索超时'))
    }, 18000)
    unsubscribe = subscribeNativeMessages((message) => {
      if (message.type === 'bilibiliTrailerSearchCompleted' && message.requestId === requestId) {
        window.clearTimeout(timeout)
        signal?.removeEventListener('abort', abort)
        unsubscribe()
        resolve(message.response as BilibiliSearchPayload)
      } else if (message.type === 'bilibiliTrailerSearchFailed' && message.requestId === requestId) {
        window.clearTimeout(timeout)
        signal?.removeEventListener('abort', abort)
        unsubscribe()
        reject(new Error(message.message))
      }
    })
    signal?.addEventListener('abort', abort, { once: true })
    if (signal?.aborted) {
      abort()
      return
    }
    postNativeCommand({ type: 'command', command: 'searchBilibiliTrailers', requestId, keyword })
  })

  if (payload.code !== 0) throw new Error(payload.message || `B 站搜索失败（${payload.code ?? 'unknown'}）`)
  return (payload.data?.result ?? [])
    .filter((candidate): candidate is BilibiliSearchItem & { bvid: string } => Boolean(candidate.bvid))
    .map((candidate) => ({ candidate, score: candidateScore(candidate, item) }))
    .filter((entry) => entry.score >= 65)
    .sort((left, right) => right.score - left.score)
    .slice(0, 8)
    .map((entry) => `https://www.bilibili.com/video/${entry.candidate.bvid}`)
}
