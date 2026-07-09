// Shared path/URL helpers used across the media library managers.
// Extracted from localFolderLibrary.ts and tmdbClient.ts to eliminate
// duplicated implementations of basename / dir / decode / year-inference.

export function safeDecodeURIComponent(value: string): string {
  try {
    return decodeURIComponent(value)
  } catch {
    return value
  }
}

export function trimTrailingSeparators(path: string): string {
  return path.trim().replace(/[\\/]+$/g, '')
}

export function trimTrailingSlashes(value: string): string {
  return value.replace(/\/+$/g, '')
}

export function stripExtension(name: string): string {
  return name.replace(/\.[^.\\/]+$/g, '')
}

export function pathSegments(path: string): string[] {
  const clean = trimTrailingSeparators(path)
  if (!clean) return []
  if (/^[a-z][a-z\d+.-]*:\/\//i.test(clean)) {
    try {
      return new URL(clean).pathname
        .split('/')
        .filter(Boolean)
        .map(safeDecodeURIComponent)
    } catch {
      // Fall back to plain path splitting below.
    }
  }
  return clean.split(/[\\/]/g).filter(Boolean).map(safeDecodeURIComponent)
}

// Basename without an extension-aware fallback; callers needing a specific
// fallback should pass `fallback`. Returns '' for empty input (mirrors the
// original tmdbClient behaviour; localFolderLibrary used a '本地文件夹' default
// via its own wrapper).
export function pathBaseName(value: string): string {
  const clean = trimTrailingSeparators(value)
  if (!clean) return ''
  if (/^[a-z][a-z\d+.-]*:\/\//i.test(clean)) {
    try {
      const parts = new URL(clean).pathname.split('/').filter(Boolean)
      return safeDecodeURIComponent(parts.at(-1) ?? '')
    } catch {
      // Fall back to plain path splitting below.
    }
  }
  return safeDecodeURIComponent(clean.split(/[\\/]/g).pop() ?? clean)
}

export function pathDirName(path: string): string {
  const clean = trimTrailingSeparators(path)
  const index = Math.max(clean.lastIndexOf('\\'), clean.lastIndexOf('/'))
  return index >= 0 ? clean.slice(0, index) : ''
}

export function normalizePathKey(path: string): string {
  return trimTrailingSeparators(path).toLowerCase()
}

// Extracts the leading 4-digit year (19xx/20xx) from a path/title blob,
// returning 0 when none is found. Matches the historic behaviour of
// localFolderLibrary.inferYear / tmdbClient.itemYear.
export function yearFromText(value: string): number {
  const match = value.match(/\b((?:19|20)\d{2})\b/)
  return match ? Number(match[1]) : 0
}
