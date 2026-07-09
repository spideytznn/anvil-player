import type { LocalFolderPickItem } from '../nativeBridge'

export interface WebDavCredentials {
  sourceId: string
  username: string
  password: string
  baseUrl?: string
  selectedPaths?: string[]
}

export interface WebDavScanResult {
  folder: {
    name: string
    path: string
  }
  items: LocalFolderPickItem[]
  truncated: boolean
}

export interface WebDavDirectoryEntry {
  name: string
  path: string
}

const WEB_DAV_CREDENTIALS_KEY = 'anvil-player.webdav.credentials.v1'
const MAX_WEBDAV_SCAN_ITEMS = 2000
const MAX_WEBDAV_SCAN_DEPTH = 8

const MEDIA_EXTENSIONS = new Set([
  '.mp4',
  '.mkv',
  '.mov',
  '.m2ts',
  '.ts',
  '.webm',
  '.avi',
  '.wmv',
  '.flv',
  '.mpg',
  '.mpeg',
  '.m4v'
])

interface WebDavEntry {
  href: string
  isDirectory: boolean
  modifiedAt?: number
  sizeBytes?: number
}

function loadCredentialRows(): WebDavCredentials[] {
  try {
    const raw = window.localStorage.getItem(WEB_DAV_CREDENTIALS_KEY)
    if (!raw) return []
    const parsed: unknown = JSON.parse(raw)
    return Array.isArray(parsed) ? parsed as WebDavCredentials[] : []
  } catch {
    return []
  }
}

function saveCredentialRows(rows: WebDavCredentials[]): void {
  window.localStorage.setItem(WEB_DAV_CREDENTIALS_KEY, JSON.stringify(rows))
}

export function loadWebDavCredentials(sourceId: string): WebDavCredentials | undefined {
  return loadCredentialRows().find((row) => row.sourceId === sourceId)
}

export function saveWebDavCredentials(credentials: WebDavCredentials): void {
  const rows = loadCredentialRows()
  saveCredentialRows([credentials, ...rows.filter((row) => row.sourceId !== credentials.sourceId)])
}

export function removeWebDavCredentials(sourceId: string): void {
  saveCredentialRows(loadCredentialRows().filter((row) => row.sourceId !== sourceId))
}

function normalizeWebDavUrl(value: string): string {
  const url = new URL(value.trim())
  url.hash = ''
  url.search = ''
  url.username = ''
  url.password = ''
  return url.toString().replace(/\/+$/, '/')
}

function webDavName(url: string): string {
  const parsed = new URL(url)
  const parts = parsed.pathname.split('/').filter(Boolean)
  return decodeURIComponent(parts.at(-1) || parsed.hostname || 'WebDAV')
}

function basicAuthHeader(username: string, password: string): string | undefined {
  if (!username && !password) return undefined
  return `Basic ${window.btoa(unescape(encodeURIComponent(`${username}:${password}`)))}`
}

function credentialedUrl(url: string, username: string, password: string): string {
  if (!username && !password) return url
  const parsed = new URL(url)
  parsed.username = username
  parsed.password = password
  return parsed.toString()
}

function absoluteHref(baseUrl: string, href: string): string {
  return new URL(href, baseUrl).toString()
}

function sameResource(left: string, right: string): boolean {
  const normalize = (value: string) => new URL(value).toString().replace(/\/+$/g, '')
  return normalize(left) === normalize(right)
}

function extensionOf(url: string): string {
  const pathname = new URL(url).pathname
  const name = decodeURIComponent(pathname.split('/').pop() ?? '')
  const index = name.lastIndexOf('.')
  return index >= 0 ? name.slice(index).toLowerCase() : ''
}

function mediaName(url: string): string {
  const pathname = new URL(url).pathname
  return decodeURIComponent(pathname.split('/').filter(Boolean).pop() ?? url)
}

function directoryName(url: string): string {
  const parsed = new URL(url)
  const parts = parsed.pathname.split('/').filter(Boolean)
  return decodeURIComponent(parts.at(-1) || parsed.hostname || 'WebDAV')
}

function propText(element: Element, localName: string): string {
  const node = [...element.getElementsByTagName('*')].find((candidate) => candidate.localName === localName)
  return node?.textContent?.trim() ?? ''
}

function parseWebDavEntries(baseUrl: string, xmlText: string): WebDavEntry[] {
  const document = new window.DOMParser().parseFromString(xmlText, 'application/xml')
  const responses = [...document.getElementsByTagName('*')].filter((node) => node.localName === 'response')
  return responses.map((response) => {
    const href = absoluteHref(baseUrl, propText(response, 'href'))
    const resourceType = [...response.getElementsByTagName('*')].find((node) => node.localName === 'resourcetype')
    const isDirectory = Boolean(resourceType && [...resourceType.getElementsByTagName('*')].some((node) => node.localName === 'collection'))
    const modifiedAt = Date.parse(propText(response, 'getlastmodified'))
    const sizeBytes = Number(propText(response, 'getcontentlength'))
    return {
      href,
      isDirectory,
      modifiedAt: Number.isFinite(modifiedAt) ? modifiedAt : undefined,
      sizeBytes: Number.isFinite(sizeBytes) ? sizeBytes : undefined
    }
  })
}

async function propFind(url: string, username: string, password: string): Promise<WebDavEntry[]> {
  const headers: Record<string, string> = {
    Depth: '1',
    'Content-Type': 'application/xml; charset=utf-8'
  }
  const authorization = basicAuthHeader(username, password)
  if (authorization) headers.Authorization = authorization

  const response = await window.fetch(url, {
    method: 'PROPFIND',
    headers,
    body: '<?xml version="1.0" encoding="utf-8"?><propfind xmlns="DAV:"><prop><resourcetype/><getcontentlength/><getlastmodified/></prop></propfind>'
  })
  if (!response.ok && response.status !== 207) {
    throw new Error(`WebDAV 扫描失败：HTTP ${response.status}`)
  }
  return parseWebDavEntries(url, await response.text()).filter((entry) => !sameResource(entry.href, url))
}

export async function listWebDavDirectories(input: {
  url: string
  username: string
  password: string
}): Promise<{ path: string; directories: WebDavDirectoryEntry[] }> {
  const currentUrl = normalizeWebDavUrl(input.url)
  const entries = await propFind(currentUrl, input.username, input.password)
  const directories = entries
    .filter((entry) => entry.isDirectory)
    .map((entry) => {
      const path = entry.href.replace(/\/+$/, '/')
      return {
        name: directoryName(path),
        path
      }
    })
    .sort((left, right) => left.name.localeCompare(right.name, 'zh-Hans-CN'))
  return { path: currentUrl, directories }
}

export async function scanWebDavLibrary(input: {
  url: string
  name?: string
  username: string
  password: string
}): Promise<WebDavScanResult> {
  const rootUrl = normalizeWebDavUrl(input.url)
  const queue: Array<{ url: string; depth: number }> = [{ url: rootUrl, depth: 0 }]
  const seenDirectories = new Set<string>()
  const seenFiles = new Set<string>()
  const items: LocalFolderPickItem[] = []
  let truncated = false

  while (queue.length && items.length < MAX_WEBDAV_SCAN_ITEMS) {
    const current = queue.shift()
    if (!current) break
    if (seenDirectories.has(current.url)) continue
    seenDirectories.add(current.url)

    const entries = await propFind(current.url, input.username, input.password)
    for (const entry of entries) {
      if (entry.isDirectory) {
        if (current.depth < MAX_WEBDAV_SCAN_DEPTH) {
          queue.push({ url: entry.href.replace(/\/+$/, '/'), depth: current.depth + 1 })
        }
        continue
      }

      if (!MEDIA_EXTENSIONS.has(extensionOf(entry.href)) || seenFiles.has(entry.href)) continue
      seenFiles.add(entry.href)
      items.push({
        name: mediaName(entry.href),
        path: entry.href,
        playbackPath: credentialedUrl(entry.href, input.username, input.password),
        modifiedAt: entry.modifiedAt,
        sizeBytes: entry.sizeBytes
      })
      if (items.length >= MAX_WEBDAV_SCAN_ITEMS) {
        truncated = true
        break
      }
    }
  }

  return {
    folder: {
      name: input.name?.trim() || webDavName(rootUrl),
      path: rootUrl
    },
    items,
    truncated
  }
}
