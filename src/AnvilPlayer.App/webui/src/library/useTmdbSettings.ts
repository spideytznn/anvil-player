// TMDB settings hook.
// Owns the settings state, its persistence effect, and the connection-test
// handler. Extracted from LibraryApp so the settings page reads from a single
// self-contained hook instead of three loose state cells.

import { useEffect, useRef, useState } from 'react'
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
  const testAbortControllerRef = useRef<AbortController>()

  useEffect(() => {
    saveTmdbSettings(settings)
  }, [settings])

  useEffect(() => () => testAbortControllerRef.current?.abort(), [])

  function updateSettings(next: TmdbSettings): void {
    setSettings(next)
    setStatus('')
  }

  async function testConnection(): Promise<void> {
    testAbortControllerRef.current?.abort()
    const abortController = new AbortController()
    testAbortControllerRef.current = abortController
    setIsTesting(true)
    setStatus('正在测试 TMDB 连接...')
    try {
      const message = await testTmdbConnection(settings, abortController.signal)
      setStatus(message)
    } catch (error) {
      if (abortController.signal.aborted) return
      setStatus(error instanceof Error ? `TMDB 连接失败：${error.message}` : 'TMDB 连接失败')
    } finally {
      if (testAbortControllerRef.current === abortController) {
        testAbortControllerRef.current = undefined
        setIsTesting(false)
      }
    }
  }

  return { settings, status, isTesting, updateSettings, testConnection }
}
