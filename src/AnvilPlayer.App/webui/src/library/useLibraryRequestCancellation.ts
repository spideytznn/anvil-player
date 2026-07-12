import { useEffect, useRef, type MutableRefObject } from 'react'

export interface LibraryRequestCancellation {
  connection: MutableRefObject<AbortController | undefined>
  tasks: MutableRefObject<Map<string, AbortController>>
}

export function useLibraryRequestCancellation(): LibraryRequestCancellation {
  const connection = useRef<AbortController>()
  const tasks = useRef<Map<string, AbortController>>(new Map())

  useEffect(() => () => {
    connection.current?.abort()
    tasks.current.forEach((controller) => controller.abort())
    tasks.current.clear()
  }, [])

  return { connection, tasks }
}
