// Library source / address path helpers.
// Pure functions extracted from LibraryApp.tsx: server-address parsing, SMB /
// WebDAV path normalization, scan-root membership checks and scan-progress
// summary formatting. None of these depend on React state.

import { loadWebDavCredentials } from '../manager/webdavClient'
import type { LibrarySource, MediaItem, SourceKind } from '../manager/types'

export type ServerProtocol = 'http' | 'https'

export interface ServerAddressParts {
  protocol: ServerProtocol
  host: string
  port: string
  path: string
}

export interface SourceScanProgress {
  sourceId: string
  sourceName: string
  kind: Exclude<SourceKind, 'Emby'>
  totalFolders: number
  completedFolders: number
  foundItems: number
  failedFolders: number
  active: boolean
  truncated: boolean
  lastPath: string
}

export function parseServerAddress(value: string, fallbackProtocol: ServerProtocol): ServerAddressParts | undefined {
  const trimmed = value.trim()
  if (!trimmed) return undefined

  try {
    const url = new URL(/^https?:\/\//i.test(trimmed) ? trimmed : `${fallbackProtocol}://${trimmed}`)
    const protocol = url.protocol.toLowerCase() === 'https:' ? 'https' : 'http'
    return {
      protocol,
      host: url.hostname,
      port: url.port,
      path: url.pathname === '/' ? '' : url.pathname.replace(/^\/+|\/+$/g, '')
    }
  } catch {
    return undefined
  }
}

export function buildServerAddress(protocol: ServerProtocol, host: string, port: string, path: string): string {
  const cleanHost = host.trim()
  const cleanPort = port.trim()
  const cleanPath = path.trim().replace(/^\/+|\/+$/g, '')
  return `${protocol}://${cleanHost}${cleanPort ? `:${cleanPort}` : ''}${cleanPath ? `/${cleanPath}` : ''}`
}

export function normalizeSmbPath(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  if (/^smb:\/\//i.test(trimmed)) {
    const url = new URL(trimmed.replace(/^smb:/i, 'file:'))
    return `\\\\${url.hostname}${decodeURIComponent(url.pathname).replace(/\//g, '\\')}`
  }
  const slashNormalized = trimmed.replace(/\//g, '\\')
  if (slashNormalized.startsWith('\\\\')) return slashNormalized
  return `\\\\${slashNormalized.replace(/^\\+/g, '')}`
}

export function normalizeSmbHost(value: string): string {
  const normalized = normalizeSmbPath(value)
  return normalized.split(/[\\/]/g).filter(Boolean)[0] ?? ''
}

export function smbParentPath(path: string): string {
  const parts = normalizeSmbPath(path).split(/[\\/]/g).filter(Boolean)
  if (parts.length <= 1) return ''
  return `\\\\${parts.slice(0, -1).join('\\')}`
}

export function normalizeWebDavSourceUrl(value: string): string {
  const url = new URL(value.trim())
  url.username = ''
  url.password = ''
  url.hash = ''
  url.search = ''
  const parts = url.pathname
    .split('/')
    .filter(Boolean)
    .map((part) => decodeURIComponent(part))
  url.pathname = parts.length ? `/${parts.join('/')}/` : '/'
  return url.toString().replace(/\/+$/, '/')
}

export function tryNormalizeWebDavSourceUrl(value: string): string {
  try {
    return normalizeWebDavSourceUrl(value)
  } catch {
    return value.trim()
  }
}

export function webDavParentUrl(value: string): string {
  const url = new URL(normalizeWebDavSourceUrl(value))
  const parts = url.pathname.split('/').filter(Boolean)
  if (!parts.length) return ''
  parts.pop()
  url.pathname = parts.length ? `/${parts.map((part) => encodeURIComponent(decodeURIComponent(part))).join('/')}/` : '/'
  return normalizeWebDavSourceUrl(url.toString())
}

export function locationKey(value: string): string {
  return value.trim().replace(/[\\/]+$/g, '').toLowerCase()
}

export function fileServiceNameFromLocation(location: string, fallback: string): string {
  const trimmed = location.trim().replace(/[\\/]+$/g, '')
  if (!trimmed) return fallback
  try {
    const url = new URL(trimmed)
    const parts = url.pathname.split('/').filter(Boolean)
    return decodeURIComponent(parts.at(-1) || url.hostname || fallback)
  } catch {
    return trimmed.split(/[\\/]/g).filter(Boolean).pop() || fallback
  }
}

export function directoryDisplayName(path: string, fallback: string): string {
  return fileServiceNameFromLocation(path, fallback)
}

export function uniqueLocationRows(rows: string[]): string[] {
  const result: string[] = []
  rows.forEach((row) => {
    const trimmed = row.trim()
    if (!trimmed) return
    if (!result.some((candidate) => locationKey(candidate) === locationKey(trimmed))) {
      result.push(trimmed)
    }
  })
  return result
}

export function webDavBaseUrlForSource(source?: LibrarySource): string {
  if (!source) return ''
  const credentials = loadWebDavCredentials(source.id)
  return tryNormalizeWebDavSourceUrl(credentials?.baseUrl || source.rootLocation || source.location)
}

export function webDavSelectedPathsForSource(source?: LibrarySource): string[] {
  if (!source) return []
  const credentials = loadWebDavCredentials(source.id)
  const rows = credentials?.selectedPaths?.length
    ? credentials.selectedPaths
    : source.folders?.length
      ? source.folders
      : source.location
        ? [source.location]
        : []
  return uniqueLocationRows(rows.map(tryNormalizeWebDavSourceUrl))
}

export function scanLocationsForSource(source: LibrarySource): string[] {
  if (source.kind === 'WebDAV') {
    return uniqueLocationRows([
      source.location,
      ...(source.folders ?? []),
      ...(loadWebDavCredentials(source.id)?.selectedPaths ?? [])
    ])
  }
  return uniqueLocationRows([source.location])
}

function normalizedPrefixLocation(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  try {
    const url = new URL(trimmed)
    if (url.protocol === 'http:' || url.protocol === 'https:') {
      return normalizeWebDavSourceUrl(trimmed).toLowerCase()
    }
  } catch {
    // Fall through to path-style normalization.
  }
  return trimmed.replace(/[\\/]+$/g, '').toLowerCase()
}

export function mediaItemBelongsToScanRoot(item: MediaItem, rootPath: string): boolean {
  const root = normalizedPrefixLocation(rootPath)
  if (!root) return false
  return [item.path, item.playbackPath].some((candidate) => {
    if (!candidate) return false
    const value = normalizedPrefixLocation(candidate)
    return value === root ||
      value.startsWith(root.endsWith('/') || root.endsWith('\\') ? root : `${root}/`) ||
      value.startsWith(root.endsWith('/') || root.endsWith('\\') ? root : `${root}\\`)
  })
}

export function sourceScanKind(source: LibrarySource): Exclude<SourceKind, 'Emby'> {
  return source.kind === 'Emby' ? 'Local' : source.kind
}

function clampCompletedFolders(progress: SourceScanProgress): number {
  return Math.min(progress.completedFolders, progress.totalFolders)
}

export function scanProgressSummary(progress: SourceScanProgress | undefined, fallbackCount = 0): string {
  if (!progress) return `${fallbackCount} 个媒体文件`
  const completedFolders = clampCompletedFolders(progress)
  const folderPart = progress.totalFolders > 1
    ? `${completedFolders}/${progress.totalFolders} 个文件夹`
    : completedFolders > 0
      ? '1 个文件夹'
      : '当前文件夹'
  const foundPart = `已找到 ${progress.foundItems} 个媒体`
  const failedPart = progress.failedFolders > 0 ? ` · ${progress.failedFolders} 个失败` : ''
  const truncatedPart = progress.truncated ? ' · 已达到扫描上限' : ''
  return progress.active
    ? `扫描中 · ${folderPart} · ${foundPart}${failedPart}${truncatedPart}`
    : `扫描完成 · ${folderPart} · ${foundPart}${failedPart}${truncatedPart}`
}
