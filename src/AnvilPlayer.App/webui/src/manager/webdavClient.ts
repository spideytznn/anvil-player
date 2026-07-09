import { createCredentialStore } from './credentialStore'

export interface WebDavCredentials {
  sourceId: string
  username: string
  password: string
  baseUrl?: string
  selectedPaths?: string[]
}

const webDavCredentialStore = createCredentialStore<WebDavCredentials>('anvil-player.webdav.credentials.v1')

export const loadWebDavCredentials = (sourceId: string): WebDavCredentials | undefined =>
  webDavCredentialStore.load(sourceId)

export const saveWebDavCredentials = (credentials: WebDavCredentials): void => {
  webDavCredentialStore.save(credentials)
}

export const removeWebDavCredentials = (sourceId: string): void => {
  webDavCredentialStore.remove(sourceId)
}

export function credentialedWebDavUrl(url: string, username: string, password: string): string {
  if (!username && !password) return url
  const parsed = new URL(url)
  parsed.username = username
  parsed.password = password
  return parsed.toString()
}
