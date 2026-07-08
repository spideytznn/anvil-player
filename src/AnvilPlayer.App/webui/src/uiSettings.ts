export type UiLanguage = 'en' | 'zh'

export const UI_LANGUAGE_STORAGE_KEY = 'anvil-player.uiLanguage'

export function getInitialLanguage(): UiLanguage {
  try {
    const saved = window.localStorage.getItem(UI_LANGUAGE_STORAGE_KEY)
    if (saved === 'en' || saved === 'zh') return saved
  } catch {
    // Ignore storage failures inside constrained WebView profiles.
  }
  return navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en'
}

export function saveUiLanguage(language: UiLanguage): void {
  try {
    window.localStorage.setItem(UI_LANGUAGE_STORAGE_KEY, language)
  } catch {
    // Ignore storage failures inside constrained WebView profiles.
  }
}

export function applyDocumentLanguage(language: UiLanguage): void {
  document.documentElement.lang = language === 'zh' ? 'zh-CN' : 'en'
}

export function uiLanguageLabel(language: UiLanguage): string {
  return language === 'zh' ? '中文' : 'English'
}
