const SCROLLBAR_ACTIVE_CLASS = 'is-scrollbar-active'
const SCROLLBAR_HIDE_DELAY_MS = 850

function scrollElementFromTarget(target: EventTarget | null): Element | null {
  if (target instanceof Document) {
    return document.scrollingElement
  }
  if (target instanceof Element) {
    return target
  }
  return document.scrollingElement
}

export function installAutoHidingScrollbars(): () => void {
  const timers = new Map<Element, number>()

  const markActive = (target: EventTarget | null): void => {
    const element = scrollElementFromTarget(target)
    if (!element) return

    element.classList.add(SCROLLBAR_ACTIVE_CLASS)
    const existing = timers.get(element)
    if (existing !== undefined) window.clearTimeout(existing)

    const timer = window.setTimeout(() => {
      element.classList.remove(SCROLLBAR_ACTIVE_CLASS)
      timers.delete(element)
    }, SCROLLBAR_HIDE_DELAY_MS)
    timers.set(element, timer)
  }

  const onScroll = (event: Event): void => markActive(event.target)
  document.addEventListener('scroll', onScroll, true)

  return () => {
    document.removeEventListener('scroll', onScroll, true)
    for (const [element, timer] of timers) {
      window.clearTimeout(timer)
      element.classList.remove(SCROLLBAR_ACTIVE_CLASS)
    }
    timers.clear()
  }
}
