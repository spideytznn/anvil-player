import { createCredentialStore } from './credentialStore'

export interface SmbCredentials {
  sourceId: string
  host: string
  username: string
  password: string
}

const smbCredentialStore = createCredentialStore<SmbCredentials>('anvil-player.smb.credentials.v1')

export const loadSmbCredentials = (sourceId: string): SmbCredentials | undefined =>
  smbCredentialStore.load(sourceId)

export const saveSmbCredentials = (credentials: SmbCredentials): void => {
  smbCredentialStore.save(credentials)
}

export const removeSmbCredentials = (sourceId: string): void => {
  smbCredentialStore.remove(sourceId)
}
