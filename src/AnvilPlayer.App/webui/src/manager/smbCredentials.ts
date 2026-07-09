export interface SmbCredentials {
  sourceId: string
  host: string
  username: string
  password: string
}

const SMB_CREDENTIALS_KEY = 'anvil-player.smb.credentials.v1'

function loadCredentialRows(): SmbCredentials[] {
  try {
    const raw = window.localStorage.getItem(SMB_CREDENTIALS_KEY)
    if (!raw) return []
    const parsed: unknown = JSON.parse(raw)
    return Array.isArray(parsed) ? parsed as SmbCredentials[] : []
  } catch {
    return []
  }
}

function saveCredentialRows(rows: SmbCredentials[]): void {
  window.localStorage.setItem(SMB_CREDENTIALS_KEY, JSON.stringify(rows))
}

export function loadSmbCredentials(sourceId: string): SmbCredentials | undefined {
  return loadCredentialRows().find((row) => row.sourceId === sourceId)
}

export function saveSmbCredentials(credentials: SmbCredentials): void {
  const rows = loadCredentialRows()
  saveCredentialRows([credentials, ...rows.filter((row) => row.sourceId !== credentials.sourceId)])
}

export function removeSmbCredentials(sourceId: string): void {
  saveCredentialRows(loadCredentialRows().filter((row) => row.sourceId !== sourceId))
}
