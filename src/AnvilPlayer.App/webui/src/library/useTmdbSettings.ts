// TMDB settings hook.
// Owns the settings state, its persistence effect, and the connection-test
// handler. Extracted from LibraryApp so the settings page reads from a single
// self-contained hook instead of three loose state cells.

import { useEffect, useState } from 'react'
import {
  loadTmdbSettings,
  saveTmdbSettings,
  testTmdbConnection,
  type TmdbSettings
} from '../manager/tmdbClient'

export interface TmdbSettingsState {
  settings: TmdbSettings
  status: string
  isTesting: boolean
  updateSettings: (settings: TmdbSettings) => void
  testConnection: () => Promise<void>
}

export function useTmdbSettings(): TmdbSettingsState {
  const [settings, setSettings] = useState<TmdbSettings>(() => loadTmdbSettings())
  const [status, setStatus] = useState('')
  const [isTesting, setIsTesting] = useState(false)

  useEffect(() => {
    saveTmdbSettings(settings)
  }, [settings])

  function updateSettings(next: TmdbSettings): void {
    setSettings(next)
    setStatus('')
  }

  async function testConnection(): Promise<void> {
    setIsTesting(true)
    setStatus('正在测试 TMDB 连接...')
    try {
      const message = await testTmdbConnection(settings)
      setStatus(message)
    } catch (error) {
      setStatus(error instanceof Error ? `TMDB 连接失败：${error.message}` : 'TMDB 连接失败')
    } finally {
      setIsTesting(false)
    }
  }

  return { settings, status, isTesting, updateSettings, testConnection }
}
