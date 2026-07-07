export interface AppearanceSettings {
  motionSpeed: number
  glassGlow: boolean
}

export const DEFAULT_APPEARANCE_SETTINGS: AppearanceSettings = {
  motionSpeed: 50,
  glassGlow: false
}

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value))
}

function cssNumber(value: number): string {
  return value.toFixed(3)
}

function normalizeSettings(settings: Partial<AppearanceSettings> | null | undefined): AppearanceSettings {
  const speed = Number(settings?.motionSpeed ?? DEFAULT_APPEARANCE_SETTINGS.motionSpeed)
  return {
    motionSpeed: clamp(Number.isFinite(speed) ? speed : DEFAULT_APPEARANCE_SETTINGS.motionSpeed, 25, 200),
    glassGlow: settings?.glassGlow ?? DEFAULT_APPEARANCE_SETTINGS.glassGlow
  }
}

export function applyAppearanceSettings(settings: Partial<AppearanceSettings> = DEFAULT_APPEARANCE_SETTINGS): void {
  const root = document.documentElement
  const normalized = normalizeSettings(settings)
  const durationFactor = 50 / normalized.motionSpeed

  root.dataset.glassGlow = normalized.glassGlow ? 'on' : 'off'
  root.style.setProperty('--motion-collapse-open', `${Math.round(550 * durationFactor)}ms`)
  root.style.setProperty('--motion-collapse-close', `${Math.round(480 * durationFactor)}ms`)
  root.style.setProperty('--motion-sidebar', `${Math.round(500 * durationFactor)}ms`)
  root.style.setProperty('--motion-sidebar-content-open', `${Math.round(410 * durationFactor)}ms`)
  root.style.setProperty('--motion-sidebar-content-close', `${Math.round(320 * durationFactor)}ms`)
  root.style.setProperty('--motion-sidebar-content-delay', `${Math.round(50 * durationFactor)}ms`)

  root.style.setProperty('--glass-shell-alpha', cssNumber(1))
  root.style.setProperty('--glass-sidebar-alpha', cssNumber(0.988))
  root.style.setProperty('--glass-main-alpha', cssNumber(0.992))
  root.style.setProperty('--glass-panel-alpha', cssNumber(0.94))
  root.style.setProperty('--glass-soft-alpha', cssNumber(0.885))
  root.style.setProperty('--glass-control-alpha', cssNumber(0.84))
  root.style.setProperty('--glass-active-alpha', cssNumber(0.88))
  root.style.setProperty('--glass-frost-strong-alpha', cssNumber(0.992))
  root.style.setProperty('--glass-frost-panel-alpha', cssNumber(0.972))
  root.style.setProperty('--glass-frost-soft-alpha', cssNumber(0.928))
  root.style.setProperty('--glass-frost-control-alpha', cssNumber(0.872))
  root.style.setProperty('--glass-lens-strong', cssNumber(0.997))
  root.style.setProperty('--glass-lens-panel', cssNumber(0.982))
  root.style.setProperty('--glass-lens-soft', cssNumber(0.948))
  root.style.setProperty('--glass-lens-control', cssNumber(0.91))
  root.style.setProperty('--glass-window-blur', '0px')
  root.style.setProperty('--glass-ambient-opacity', normalized.glassGlow ? '0.48' : '0.22')
}
