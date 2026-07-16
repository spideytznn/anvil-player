import { Copy, FileWarning, RefreshCw, Trash2, X } from 'lucide-react'
import { useMemo, useState, type CSSProperties } from 'react'
import type { LibrarySource, MediaItem } from '../manager/types'
import type { UiLanguage } from '../uiSettings'

interface MediaManagementDialogProps {
  language: UiLanguage
  items: MediaItem[]
  sources: LibrarySource[]
  onClose: () => void
  onSelect: (item: MediaItem) => void
  onRescan: (source: LibrarySource) => void
  onRemoveMissing: (item: MediaItem) => void
}

export function MediaManagementDialog(props: MediaManagementDialogProps): JSX.Element {
  const zh = props.language === 'zh'
  const [tab, setTab] = useState<'missing' | 'duplicates'>('missing')
  const sourceMap = useMemo(() => new Map(props.sources.map((source) => [source.id, source])), [props.sources])
  const missingItems = props.items.filter((item) => item.availability === 'missing')
  const duplicateItems = props.items.filter((item) => (item.versions?.length ?? 0) > 0)
  const rows = tab === 'missing' ? missingItems : duplicateItems
  const panelHeight = rows.length ? Math.min(336, 112 + rows.length * 56) : 156

  return (
      <section
        className={`library-management-dialog ${rows.length ? 'has-items' : 'is-empty'}`}
        role="dialog"
        aria-modal="false"
        style={{ '--management-panel-height': `${panelHeight}px` } as CSSProperties}
      >
        <header>
          <div><span>{zh ? '媒体管理' : 'Media management'}</span><h2>{zh ? '缺失文件与重复版本' : 'Missing files and duplicate versions'}</h2></div>
          <button type="button" aria-label={zh ? '关闭' : 'Close'} onClick={props.onClose}><X size={18} /></button>
        </header>
        <nav>
          <button className={tab === 'missing' ? 'is-active' : ''} type="button" onClick={() => setTab('missing')}><FileWarning size={15} />{zh ? '缺失文件' : 'Missing'}<b>{missingItems.length}</b></button>
          <button className={tab === 'duplicates' ? 'is-active' : ''} type="button" onClick={() => setTab('duplicates')}><Copy size={15} />{zh ? '重复与多版本' : 'Duplicates & versions'}<b>{duplicateItems.length}</b></button>
        </nav>
        <div className="library-management-list" data-scroll-fade>
          {rows.map((item) => {
            const source = sourceMap.get(item.sourceId)
            return (
              <article key={item.id}>
                <button className="library-management-main" type="button" onClick={() => props.onSelect(item)}>
                  <div style={{ background: item.poster }} />
                  <span><strong>{item.title}</strong><small>{source?.name || item.sourceId} · {item.path || item.mediaSourceId || '—'}</small></span>
                </button>
                {tab === 'missing' ? (
                  <div>
                    {source && source.kind !== 'Emby' ? <button type="button" onClick={() => props.onRescan(source)}><RefreshCw size={14} />{zh ? '重新扫描' : 'Rescan'}</button> : null}
                    <button type="button" onClick={() => props.onRemoveMissing(item)}><Trash2 size={14} />{zh ? '移除记录' : 'Remove record'}</button>
                  </div>
                ) : (
                  <span className="library-management-version-count">{(item.versions?.length ?? 0) + 1} {zh ? '个版本' : 'versions'}</span>
                )}
              </article>
            )
          })}
          {!rows.length ? <div className="library-management-empty">{tab === 'missing' ? (zh ? '没有缺失文件' : 'No missing files') : (zh ? '没有重复或多版本条目' : 'No duplicate or multi-version items')}</div> : null}
        </div>
      </section>
  )
}
