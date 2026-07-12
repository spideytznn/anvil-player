import { ChevronLeft, ChevronRight, Film, ListFilter } from 'lucide-react'
import type { Ref } from 'react'
import type { LibrarySource, MediaItem } from '../manager/types'
import type { UiLanguage } from '../uiSettings'
import { EmptyState, MediaPoster } from './LibraryHome'

export interface LibraryClassificationGroup {
  label: string
  items: MediaItem[]
}

export function LibraryList(props: {
  title: string
  language: UiLanguage
  items: MediaItem[]
  pagedItems: MediaItem[]
  groups: LibraryClassificationGroup[]
  selectedId: string
  page: number
  pageCount: number
  rangeStart: number
  rangeEnd: number
  emptyCaption: string
  sectionRef?: Ref<HTMLElement>
  sourceFor: (sourceId: string) => LibrarySource
  onSelect: (id: string) => void
  onReset: () => void
  onPageChange: (page: number) => void
}): JSX.Element {
  return (
    <section className="library-grid-section" ref={props.sectionRef}>
      <div className="library-section-heading">
        <span>{props.title} · {props.items.length}</span>
        <button type="button" onClick={props.onReset}>
          <ListFilter size={13} />
          <span>{props.language === 'zh' ? '重置' : 'Reset'}</span>
        </button>
      </div>
      {props.items.length ? (
        <>
          {props.groups.length ? props.groups.map((group) => (
            <section className="library-classification-group" key={group.label}>
              <div className="library-section-heading"><span>{group.label}</span><small>{group.items.length}</small></div>
              <div className="library-poster-grid">
                {group.items.map((item) => (
                  <MediaPoster key={`${group.label}:${item.id}`} item={item} source={props.sourceFor(item.sourceId)} selected={props.selectedId === item.id} language={props.language} onSelect={props.onSelect} />
                ))}
              </div>
            </section>
          )) : (
            <div className="library-poster-grid">
              {props.pagedItems.map((item) => (
                <MediaPoster key={item.id} item={item} source={props.sourceFor(item.sourceId)} selected={props.selectedId === item.id} language={props.language} onSelect={props.onSelect} />
              ))}
            </div>
          )}
          {!props.groups.length && props.pageCount > 1 ? (
            <nav className="library-pagination" aria-label={props.language === 'zh' ? '媒体库分页' : 'Library pages'}>
              <button type="button" disabled={props.page === 1} aria-label={props.language === 'zh' ? '上一页' : 'Previous page'} onClick={() => props.onPageChange(props.page - 1)}>
                <ChevronLeft size={15} />
                <span>{props.language === 'zh' ? '上一页' : 'Previous'}</span>
              </button>
              <span className="library-pagination-status">
                {props.rangeStart}-{props.rangeEnd} / {props.items.length}
                <small>{props.page} / {props.pageCount}</small>
              </span>
              <button type="button" disabled={props.page === props.pageCount} aria-label={props.language === 'zh' ? '下一页' : 'Next page'} onClick={() => props.onPageChange(props.page + 1)}>
                <span>{props.language === 'zh' ? '下一页' : 'Next'}</span>
                <ChevronRight size={15} />
              </button>
            </nav>
          ) : null}
        </>
      ) : (
        <EmptyState icon={<Film size={24} />} title={props.language === 'zh' ? '暂无媒体' : 'No media'} caption={props.emptyCaption} />
      )}
    </section>
  )
}
