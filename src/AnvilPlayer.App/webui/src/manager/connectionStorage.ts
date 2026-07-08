import type { EmbySession } from './embyClient'

const EMBY_CONNECTION_STORAGE_KEY = 'anvil-player.library.emby.connection.v1'

export interface SavedEmbyConnection {
  sourceId: string
  name: string
  serverUrl: string
  username: string
  ignoreCertificateErrors: boolean
  session?: EmbySession
  savedAt: number
}

function isObject(value: unknown): value is Record<string, unknown> {
  return Boolean(value) && typeof value === 'object'
}

function stringValue(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

function loadSession(value: unknown): EmbySession | undefined {
  if (!isObject(value)) return undefined
  const session: EmbySession = {
    apiBaseUrl: stringValue(value.apiBaseUrl),
    serverId: stringValue(value.serverId),
    serverName: stringValue(value.serverName),
    serverVersion: stringValue(value.serverVersion),
    userId: stringValue(value.userId),
    userName: stringValue(value.userName),
    accessToken: stringValue(value.accessToken)
  }
  if (!session.apiBaseUrl || !session.userId || !session.accessToken) {
    return undefined
  }
  return session
}

export function loadSavedEmbyConnection(): SavedEmbyConnection | undefined {
  try {
    const raw = window.localStorage.getItem(EMBY_CONNECTION_STORAGE_KEY)
    if (!raw) return undefined
    const parsed: unknown = JSON.parse(raw)
    if (!isObject(parsed)) return undefined

    const serverUrl = stringValue(parsed.serverUrl)
    if (!serverUrl) return undefined

    return {
      sourceId: stringValue(parsed.sourceId),
      name: stringValue(parsed.name) || 'Emby',
      serverUrl,
      username: stringValue(parsed.username),
      ignoreCertificateErrors: parsed.ignoreCertificateErrors === true,
      session: loadSession(parsed.session),
      savedAt: typeof parsed.savedAt === 'number' ? parsed.savedAt : 0
    }
  } catch {
    return undefined
  }
}

export function saveSavedEmbyConnection(connection: SavedEmbyConnection): void {
  try {
    window.localStorage.setItem(EMBY_CONNECTION_STORAGE_KEY, JSON.stringify(connection))
  } catch {
    // Connection restore is a convenience; a full storage quota should not block playback.
  }
}
