// Generic credential-store helper. Unifies the identical load/save/remove-by-id
// pattern that was duplicated verbatim across webdavClient.ts and smbCredentials.ts.
//
// Each store persists an array of credential rows under a single localStorage
// key, keyed by `sourceId`. createCredentialStore returns the three accessors
// (load/save/remove) so callers keep their original public signatures while
// sharing one implementation.

interface CredentialRow {
  sourceId: string
}

export interface CredentialStore<T extends CredentialRow> {
  load(sourceId: string): T | undefined
  save(row: T): void
  remove(sourceId: string): void
}

export function createCredentialStore<T extends CredentialRow>(storageKey: string): CredentialStore<T> {
  function loadRows(): T[] {
    try {
      const raw = window.localStorage.getItem(storageKey)
      if (!raw) return []
      const parsed: unknown = JSON.parse(raw)
      return Array.isArray(parsed) ? parsed as T[] : []
    } catch {
      return []
    }
  }

  function saveRows(rows: T[]): void {
    window.localStorage.setItem(storageKey, JSON.stringify(rows))
  }

  return {
    load(sourceId) {
      return loadRows().find((row) => row.sourceId === sourceId)
    },
    save(row) {
      saveRows([row, ...loadRows().filter((existing) => existing.sourceId !== row.sourceId)])
    },
    remove(sourceId) {
      saveRows(loadRows().filter((row) => row.sourceId !== sourceId))
    }
  }
}
