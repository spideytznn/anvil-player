// Player-side formatting and HDR tone-curve math helpers.
// Extracted from App.tsx: pure functions with no React or Copy (translation)
// dependency, so the player UI components can share them without pulling the
// full App module.

import type { CSSProperties } from 'react'

export function formatTime(ms: number): string {
  const totalSeconds = Math.max(0, Math.floor(ms / 1000))
  const hours = Math.floor(totalSeconds / 3600)
  const minutes = Math.floor((totalSeconds % 3600) / 60)
  const seconds = totalSeconds % 60
  if (hours > 0) {
    return `${hours}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`
  }
  return `${minutes}:${String(seconds).padStart(2, '0')}`
}

export function compactPath(path: string, emptyLabel: string): string {
  if (!path) return emptyLabel
  const parts = path.split(/[\\/]/).filter(Boolean)
  if (parts.length <= 3) return path
  return `${parts[0]} / ... / ${parts.slice(-2).join(' / ')}`
}

export function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value))
}

export function formatSignedPixels(value: number): string {
  return `${value > 0 ? '+' : ''}${value}px`
}

export function formatDelay(ms: number): string {
  const seconds = ms / 1000
  return `${seconds > 0 ? '+' : ''}${seconds.toFixed(1)}s`
}

export function formatScale(scale: number): string {
  return `${Math.round(scale * 100)}%`
}

export function formatPercent(value: number): string {
  return `${Math.round(value)}%`
}

export function rangeStyle(value: number, min: number, max: number): CSSProperties {
  const span = Math.max(1, max - min)
  const percent = clamp(((value - min) / span) * 100, 0, 100)
  return { '--range-value': `${percent}%` } as CSSProperties
}

export const HDR_CURVE_MAX_NITS = 4000
export const HDR_CURVE_FOCUS_NITS = 1000
export const HDR_CURVE_FOCUS_UNIT = 0.72

export function toneCurveNitsToUnit(nits: number): number {
  const clamped = clamp(nits, 0, HDR_CURVE_MAX_NITS)
  if (clamped <= HDR_CURVE_FOCUS_NITS) {
    return (clamped / HDR_CURVE_FOCUS_NITS) * HDR_CURVE_FOCUS_UNIT
  }
  return HDR_CURVE_FOCUS_UNIT + ((clamped - HDR_CURVE_FOCUS_NITS) / (HDR_CURVE_MAX_NITS - HDR_CURVE_FOCUS_NITS)) * (1 - HDR_CURVE_FOCUS_UNIT)
}

export function toneCurveUnitToNits(unit: number): number {
  const clamped = clamp(unit, 0, 1)
  if (clamped <= HDR_CURVE_FOCUS_UNIT) {
    return (clamped / HDR_CURVE_FOCUS_UNIT) * HDR_CURVE_FOCUS_NITS
  }
  return HDR_CURVE_FOCUS_NITS + ((clamped - HDR_CURVE_FOCUS_UNIT) / (1 - HDR_CURVE_FOCUS_UNIT)) * (HDR_CURVE_MAX_NITS - HDR_CURVE_FOCUS_NITS)
}
