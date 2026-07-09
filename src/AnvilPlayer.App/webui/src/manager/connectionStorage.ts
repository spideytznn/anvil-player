import type { EmbySession } from './embyClient'

const EMBY_CONNECTION_STORAGE_KEY = 'anvil-player.library.emby.connection.v1'
const EMBY_CONNECTIONS_STORAGE_KEY = 'anvil-player.library.emby.connections.v1'

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
  return loadSavedEmbyConnections()[0]
}

export function loadSavedEmbyConnections(): SavedEmbyConnection[] {
  try {
    const raw = window.localStorage.getItem(EMBY_CONNECTIONS_STORAGE_KEY)
    if (raw) {
      const parsed: unknown = JSON.parse(raw)
      if (Array.isArray(parsed)) {
        return parsed
          .map(loadConnection)
          .filter((connection): connection is SavedEmbyConnection => Boolean(connection))
      }
    }
  } catch {
    // Fall through to the legacy single-connection key.
  }

  try {
    const raw = window.localStorage.getItem(EMBY_CONNECTION_STORAGE_KEY)
    if (!raw) return []
    const parsed: unknown = JSON.parse(raw)
    const legacy = loadConnection(parsed)
    return legacy ? [legacy] : []
  } catch {
    return []
  }
}

function loadConnection(value: unknown): SavedEmbyConnection | undefined {
  if (!isObject(value)) return undefined

  const serverUrl = stringValue(value.serverUrl)
  if (!serverUrl) return undefined

  return {
    sourceId: stringValue(value.sourceId),
    name: stringValue(value.name) || 'Emby',
    serverUrl,
    username: stringValue(value.username),
    ignoreCertificateErrors: value.ignoreCertificateErrors === true,
    session: loadSession(value.session),
    savedAt: typeof value.savedAt === 'number' ? value.savedAt : 0
  }
}

export function saveSavedEmbyConnection(connection: SavedEmbyConnection): void {
  try {
    const existing = loadSavedEmbyConnections()
    const next = [
      connection,
      ...existing.filter((candidate) => candidate.sourceId !== connection.sourceId)
    ]
    window.localStorage.setItem(EMBY_CONNECTIONS_STORAGE_KEY, JSON.stringify(next))
    window.localStorage.setItem(EMBY_CONNECTION_STORAGE_KEY, JSON.stringify(connection))
  } catch {
    // Connection restore is a convenience; a full storage quota should not block playback.
  }
}

export function removeSavedEmbyConnection(sourceId: string): void {
  try {
    const next = loadSavedEmbyConnections().filter((candidate) => candidate.sourceId !== sourceId)
    if (next.length) {
      window.localStorage.setItem(EMBY_CONNECTIONS_STORAGE_KEY, JSON.stringify(next))
      window.localStorage.setItem(EMBY_CONNECTION_STORAGE_KEY, JSON.stringify(next[0]))
    } else {
      window.localStorage.removeItem(EMBY_CONNECTIONS_STORAGE_KEY)
      window.localStorage.removeItem(EMBY_CONNECTION_STORAGE_KEY)
    }
  } catch {
    // Connection restore is a convenience; deletion should not block the UI.
  }
}
