// Shared defensive decoders for untyped JSON read out of localStorage / network.
// Extracted from embyClient.ts, connectionStorage.ts and mediaLibraryClient.ts
// where each carried its own copy of these narrowing helpers.

export function isObject(value: unknown): value is Record<string, unknown> {
  return Boolean(value) && typeof value === 'object'
}

export function stringValue(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

export function numberValue(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined
}

export function objectValue(value: unknown): Record<string, unknown> | undefined {
  return value && typeof value === 'object' ? value as Record<string, unknown> : undefined
}
