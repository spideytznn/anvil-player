import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react'
import { ChevronLeft, ChevronRight } from 'lucide-react'
import { hasImageBackground, imageBackgroundUrl } from './merge'
import type { LibraryHomeCard, LibraryHomeSection, LibrarySource, MediaItem } from '../manager/types'
import type { UiLanguage } from '../uiSettings'

export function homeCardViewId(card: LibraryHomeCard): string {
  if (card.viewId) return card.viewId
  return card.kind === 'view' && card.id.startsWith('view:') ? card.id.slice('view:'.length) : ''
}

function mediaTypeLabel(type: MediaItem['type'], language: UiLanguage): string {
  switch (type) {
    case 'movie': return language === 'zh' ? '电影' : 'Movie'
    case 'series': return language === 'zh' ? '剧集' : 'Series'
    default: return language === 'zh' ? '文件夹' : 'Folder'
  }
}

function EmbyIcon(): JSX.Element {
  return (
    <svg className="library-emby-icon" width="16" height="16" viewBox="0 0 512 512" aria-hidden="true">
      <path className="library-emby-icon-mark" d="m97.1 229.4 26.5 26.5L0 379.5l132.4 132.4 26.5-26.5L282.5 609l141.2-141.2-26.5-26.5L512 326.5 379.6 194.1l-26.5 26.5L229.5 97z" transform="translate(0 -97)" />
      <path className="library-emby-icon-play" d="M196.8 351.2v-193L366 254.7 281.4 303z" />
    </svg>
  )
}

export function MediaArtwork(props: {
  background: string
  className: string
  children?: ReactNode
}): JSX.Element {
  const imageUrl = imageBackgroundUrl(props.background)
  const [imageFailed, setImageFailed] = useState(false)
  const showImage = Boolean(imageUrl && !imageFailed)

  useEffect(() => {
    setImageFailed(false)
  }, [imageUrl])

  return (
    <div className={`${props.className} ${showImage ? 'has-media-image' : 'has-default-media-poster'}`}>
      <div className="library-default-film" style={{ backgroundImage: 'url("./default-media-poster.png")' }} aria-hidden="true" />
      {showImage ? (
        <img
          className="library-art-image"
          src={imageUrl}
          alt=""
          loading="lazy"
          decoding="async"
          draggable={false}
          onError={() => setImageFailed(true)}
        />
      ) : null}
      {props.children}
    </div>
  )
}

export function EmptyState(props: { icon: ReactNode; title: string; caption: string }): JSX.Element {
  return (
    <div className="library-empty-state">
      {props.icon}
      <strong>{props.title}</strong>
      <span>{props.caption}</span>
    </div>
  )
}

export function HorizontalScroller(props: {
  className: string
  children: ReactNode
  pageRatio?: number
}): JSX.Element {
  const scrollerRef = useRef<HTMLDivElement | null>(null)
  const [scrollState, setScrollState] = useState({ canLeft: false, canRight: false })

  function updateScrollState(): void {
    const scroller = scrollerRef.current
    if (!scroller) return
    const maxScrollLeft = scroller.scrollWidth - scroller.clientWidth
    setScrollState({
      canLeft: scroller.scrollLeft > 2,
      canRight: scroller.scrollLeft < maxScrollLeft - 2
    })
  }

  function scrollPage(direction: -1 | 1): void {
    const scroller = scrollerRef.current
    if (!scroller) return
    const distance = Math.max(160, scroller.clientWidth * (props.pageRatio ?? 0.72))
    scroller.scrollBy({ left: direction * distance, behavior: 'smooth' })
  }

  useLayoutEffect(() => {
    const scroller = scrollerRef.current
    if (!scroller) return undefined
    let frame = 0
    const scheduleUpdate = (): void => {
      window.cancelAnimationFrame(frame)
      frame = window.requestAnimationFrame(updateScrollState)
    }
    scheduleUpdate()
    scroller.addEventListener('scroll', scheduleUpdate, { passive: true })
    window.addEventListener('resize', scheduleUpdate)
    const observer = typeof ResizeObserver === 'undefined' ? undefined : new ResizeObserver(scheduleUpdate)
    observer?.observe(scroller)
    return () => {
      window.cancelAnimationFrame(frame)
      scroller.removeEventListener('scroll', scheduleUpdate)
      window.removeEventListener('resize', scheduleUpdate)
      observer?.disconnect()
    }
  }, [props.children])

  const shellClassName = [
    'horizontal-scroll-shell',
    props.className.includes('library-home-grid') ? 'is-home-grid' : '',
    props.className.includes('library-continue-row') ? 'is-continue-row' : '',
    props.className.includes('library-episode-row') ? 'is-episode-row' : '',
    props.className.includes('library-cast-row') ? 'is-cast-row' : '',
    props.className.includes('library-similar-row') ? 'is-similar-row' : '',
    props.className.includes('library-season-tabs') ? 'is-season-tabs' : ''
  ].filter(Boolean).join(' ')

  return (
    <div className={shellClassName} data-scrollable={scrollState.canLeft || scrollState.canRight} data-can-left={scrollState.canLeft} data-can-right={scrollState.canRight} onPointerEnter={updateScrollState}>
      <div className={props.className} ref={scrollerRef}>{props.children}</div>
      <div className="horizontal-scroll-edge is-left">
        <button className="horizontal-scroll-button" type="button" aria-label="Previous page" disabled={!scrollState.canLeft} onClick={() => scrollPage(-1)}>
          <ChevronLeft size={18} />
        </button>
      </div>
      <div className="horizontal-scroll-edge is-right">
        <button className="horizontal-scroll-button" type="button" aria-label="Next page" disabled={!scrollState.canRight} onClick={() => scrollPage(1)}>
          <ChevronRight size={18} />
        </button>
      </div>
    </div>
  )
}

export function ContinueCard(props: {
  item: MediaItem
  selected: boolean
  onSelect: (id: string) => void
}): JSX.Element {
  const art = hasImageBackground(props.item.backdrop) ? props.item.backdrop : props.item.poster
  return (
    <button className={`library-continue-card ${props.selected ? 'is-selected' : ''}`} type="button" onClick={() => props.onSelect(props.item.id)}>
      <MediaArtwork background={art} className="library-continue-art">
        <span className="library-continue-progress" style={{ width: `${Math.round(props.item.progress * 100)}%` }} />
      </MediaArtwork>
      <strong>{props.item.title}</strong>
      <small>{Math.round(props.item.progress * 100)}%</small>
    </button>
  )
}

export function MediaPoster(props: {
  item: MediaItem
  source: LibrarySource
  selected: boolean
  language: UiLanguage
  onSelect: (id: string) => void
}): JSX.Element {
  return (
    <button className={`library-poster-card ${props.selected ? 'is-selected' : ''} ${props.item.availability === 'missing' ? 'is-missing' : ''}`} type="button" onClick={() => props.onSelect(props.item.id)}>
      <MediaArtwork background={props.item.poster} className="library-poster-art">
        <div className="library-poster-shine" />
        <span className="library-poster-type">{mediaTypeLabel(props.item.type, props.language)}</span>
        {props.item.availability === 'missing' ? <span className="library-poster-warning">{props.language === 'zh' ? '缺失' : 'Missing'}</span> : null}
        {(props.item.versions?.length ?? 0) > 0 ? <span className="library-poster-versions">{(props.item.versions?.length ?? 0) + 1}×</span> : null}
      </MediaArtwork>
      {props.item.progress > 0 && props.item.progress < 1 ? <span className="library-progress-track"><span style={{ width: `${Math.round(props.item.progress * 100)}%` }} /></span> : null}
      <span className="library-poster-title">{props.item.title}</span>
      <span className="library-poster-meta">{props.item.year} · {props.item.quality} · {props.source.name}</span>
    </button>
  )
}

function HomeCard(props: {
  card: LibraryHomeCard
  layout: LibraryHomeSection['layout']
  selected: boolean
  language: UiLanguage
  onSelectItem: (id: string) => void
  onSelectView: (id: string) => void
}): JSX.Element {
  const viewId = homeCardViewId(props.card)
  return (
    <button className={`library-home-card is-${props.layout} ${props.selected ? 'is-selected' : ''}`} type="button" onClick={() => props.card.itemId ? props.onSelectItem(props.card.itemId) : viewId && props.onSelectView(viewId)} aria-disabled={!props.card.itemId && !viewId}>
      {props.card.kind === 'view' ? (
        <div className="library-home-card-art" style={{ background: props.card.image }}><span className="library-home-view-icon"><EmbyIcon /></span></div>
      ) : (
        <MediaArtwork background={props.card.image} className="library-home-card-art">
          <div className="library-poster-shine" />
          {props.card.mediaType ? <span className="library-poster-type">{mediaTypeLabel(props.card.mediaType, props.language)}</span> : null}
        </MediaArtwork>
      )}
      <span className="library-home-card-title">{props.card.title}</span>
      <span className="library-home-card-meta">{props.card.subtitle}</span>
    </button>
  )
}

export function LibraryHome(props: {
  sections: LibraryHomeSection[]
  selectedId: string
  selectedViewId: string
  language: UiLanguage
  onSelectItem: (id: string) => void
  onSelectView: (id: string) => void
}): JSX.Element {
  return <>{props.sections.map((section) => (
    <section className="library-home-section" key={section.id}>
      <div className="library-section-heading"><span>{section.title}</span><small>{section.cards.length}</small></div>
      <HorizontalScroller className={`library-home-grid is-${section.layout}`}>
        {section.cards.map((card) => (
          <HomeCard key={card.id} card={card} layout={section.layout} language={props.language} selected={card.itemId === props.selectedId || homeCardViewId(card) === props.selectedViewId} onSelectItem={props.onSelectItem} onSelectView={props.onSelectView} />
        ))}
      </HorizontalScroller>
    </section>
  ))}</>
}
