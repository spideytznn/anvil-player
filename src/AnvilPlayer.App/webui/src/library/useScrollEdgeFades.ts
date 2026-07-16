import { useLayoutEffect, type RefObject } from 'react'

const scrollFadeSelector = '[data-scroll-fade]'
const scrollFadeTopAttribute = 'data-scroll-fade-top'
const scrollFadeBottomAttribute = 'data-scroll-fade-bottom'
const scrollEdgeTolerance = 1

function updateScrollFade(element: HTMLElement): void {
  const maximumScrollTop = Math.max(0, element.scrollHeight - element.clientHeight)
  const canScroll = maximumScrollTop > scrollEdgeTolerance

  element.toggleAttribute(
    scrollFadeTopAttribute,
    canScroll && element.scrollTop > scrollEdgeTolerance
  )
  element.toggleAttribute(
    scrollFadeBottomAttribute,
    canScroll && element.scrollTop < maximumScrollTop - scrollEdgeTolerance
  )
}

export function useScrollEdgeFades<T extends HTMLElement>(rootRef: RefObject<T | null>): void {
  useLayoutEffect(() => {
    const root = rootRef.current
    if (!root) return undefined

    const scrollElements = new Set<HTMLElement>()
    let updateFrame = 0

    const resizeObserver = new ResizeObserver((entries) => {
      entries.forEach((entry) => {
        if (entry.target instanceof HTMLElement) updateScrollFade(entry.target)
      })
    })

    const registerElement = (element: HTMLElement): void => {
      if (scrollElements.has(element)) return
      scrollElements.add(element)
      resizeObserver.observe(element)
      updateScrollFade(element)
    }

    const scanNode = (node: Node): void => {
      if (!(node instanceof Element)) return
      if (node.matches(scrollFadeSelector)) registerElement(node as HTMLElement)
      node.querySelectorAll<HTMLElement>(scrollFadeSelector).forEach(registerElement)
    }

    const unregisterNode = (node: Node): void => {
      if (!(node instanceof Element)) return
      const candidates = [
        ...(node.matches(scrollFadeSelector) ? [node as HTMLElement] : []),
        ...node.querySelectorAll<HTMLElement>(scrollFadeSelector)
      ]
      candidates.forEach((element) => {
        if (!scrollElements.delete(element)) return
        resizeObserver.unobserve(element)
      })
    }

    const updateAll = (): void => {
      updateFrame = 0
      scrollElements.forEach(updateScrollFade)
    }

    const scheduleUpdate = (): void => {
      if (updateFrame) return
      updateFrame = window.requestAnimationFrame(updateAll)
    }

    const mutationObserver = new MutationObserver((records) => {
      records.forEach((record) => {
        record.removedNodes.forEach(unregisterNode)
        record.addedNodes.forEach(scanNode)
      })
      scheduleUpdate()
    })

    const handleScroll = (event: Event): void => {
      const target = event.target
      if (target instanceof HTMLElement && scrollElements.has(target)) updateScrollFade(target)
    }

    scanNode(root)
    mutationObserver.observe(root, {
      attributes: true,
      attributeFilter: ['class', 'style', 'hidden', 'aria-hidden'],
      childList: true,
      subtree: true
    })
    root.addEventListener('scroll', handleScroll, true)
    root.addEventListener('load', scheduleUpdate, true)
    window.addEventListener('resize', scheduleUpdate)

    return () => {
      if (updateFrame) window.cancelAnimationFrame(updateFrame)
      mutationObserver.disconnect()
      resizeObserver.disconnect()
      root.removeEventListener('scroll', handleScroll, true)
      root.removeEventListener('load', scheduleUpdate, true)
      window.removeEventListener('resize', scheduleUpdate)
      scrollElements.forEach((element) => {
        element.removeAttribute(scrollFadeTopAttribute)
        element.removeAttribute(scrollFadeBottomAttribute)
      })
    }
  }, [rootRef])
}
