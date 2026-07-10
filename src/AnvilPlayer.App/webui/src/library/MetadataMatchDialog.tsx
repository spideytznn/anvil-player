import { Check, Search, X } from 'lucide-react'
import { useEffect, useState, type FormEvent } from 'react'
import type { TmdbMatchCandidate } from '../manager/tmdbClient'
import type { MediaItem } from '../manager/types'
import type { UiLanguage } from '../uiSettings'

interface MetadataMatchDialogProps {
  item: MediaItem
  language: UiLanguage
  candidates: TmdbMatchCandidate[]
  searching: boolean
  applyingId: string
  onSearch: (query: string) => void
  onApply: (candidate: TmdbMatchCandidate) => void
  onClose: () => void
}

export function MetadataMatchDialog(props: MetadataMatchDialogProps): JSX.Element {
  const [query, setQuery] = useState(props.item.originalTitle || props.item.title)
  const zh = props.language === 'zh'

  useEffect(() => {
    setQuery(props.item.originalTitle || props.item.title)
  }, [props.item.id])

  function submit(event: FormEvent): void {
    event.preventDefault()
    props.onSearch(query)
  }

  return (
    <div className="library-metadata-match-backdrop" role="presentation" onMouseDown={props.onClose}>
      <section className="library-metadata-match-dialog" role="dialog" aria-modal="true" onMouseDown={(event) => event.stopPropagation()}>
        <header>
          <div>
            <span>TMDB</span>
            <h2>{zh ? '搜索并选择正确的元数据' : 'Search and select metadata'}</h2>
            <p>{props.item.title}</p>
          </div>
          <button type="button" aria-label={zh ? '关闭' : 'Close'} onClick={props.onClose}><X size={18} /></button>
        </header>
        <form onSubmit={submit}>
          <Search size={16} />
          <input value={query} onChange={(event) => setQuery(event.currentTarget.value)} autoFocus />
          <button type="submit" disabled={props.searching || !query.trim()}>{props.searching ? (zh ? '搜索中…' : 'Searching…') : (zh ? '搜索' : 'Search')}</button>
        </form>
        <div className="library-metadata-candidates">
          {props.candidates.map((candidate) => {
            const candidateKey = `${candidate.type}:${candidate.id}`
            return (
              <button
                type="button"
                className="library-metadata-candidate"
                key={candidateKey}
                disabled={Boolean(props.applyingId)}
                onClick={() => props.onApply(candidate)}
              >
                <div className="library-metadata-candidate-poster" style={{ background: candidate.poster }} />
                <div>
                  <strong>{candidate.title}</strong>
                  <span>{candidate.originalTitle}</span>
                  <small>{candidate.type === 'tv' ? (zh ? '剧集' : 'Series') : (zh ? '电影' : 'Movie')} · {candidate.year || '—'} · TMDB {candidate.id}</small>
                  <p>{candidate.overview || (zh ? '暂无简介' : 'No overview')}</p>
                </div>
                {props.applyingId === candidateKey ? <span>{zh ? '应用中…' : 'Applying…'}</span> : <Check size={18} />}
              </button>
            )
          })}
          {!props.searching && !props.candidates.length ? (
            <div className="library-metadata-candidates-empty">{zh ? '输入名称后搜索 TMDB 候选结果' : 'Enter a title to search TMDB candidates'}</div>
          ) : null}
        </div>
      </section>
    </div>
  )
}
