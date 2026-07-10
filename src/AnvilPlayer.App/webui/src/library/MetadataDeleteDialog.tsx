import { Trash2, Unlink, X } from 'lucide-react'
import type { MediaItem } from '../manager/types'
import type { UiLanguage } from '../uiSettings'

interface MetadataDeleteDialogProps {
  item: MediaItem
  language: UiLanguage
  onDeleteOnly: () => void
  onDeleteAndDissolve: () => void
  onClose: () => void
}

export function MetadataDeleteDialog(props: MetadataDeleteDialogProps): JSX.Element {
  const zh = props.language === 'zh'
  const canDissolve = props.item.type === 'series' || Boolean(props.item.versions?.length || props.item.seasons?.length || props.item.episodes?.length)
  return (
    <div className="library-metadata-delete-backdrop" role="presentation" onMouseDown={props.onClose}>
      <section className="library-metadata-delete-dialog" role="dialog" aria-modal="true" onMouseDown={(event) => event.stopPropagation()}>
        <header>
          <div><span>{zh ? '删除元数据' : 'Remove metadata'}</span><h2>{props.item.title}</h2></div>
          <button type="button" aria-label={zh ? '关闭' : 'Close'} onClick={props.onClose}><X size={17} /></button>
        </header>
        <p>{canDissolve
          ? (zh ? '这个条目包含剧集或多个视频版本。请选择是否同时解散合集。' : 'This item contains episodes or multiple video versions. Choose whether to dissolve the collection too.')
          : (zh ? '这会删除当前条目的海报、简介和匹配信息。' : 'This removes artwork, overview and matching information from this item.')}</p>
        <div>
          <button type="button" onClick={props.onDeleteOnly}><Trash2 size={16} /><span><strong>{zh ? '仅删除元数据' : 'Remove metadata only'}</strong><small>{zh ? '保留当前合集和版本结构' : 'Keep the current collection and version structure'}</small></span></button>
          {canDissolve ? (
            <button className="is-danger" type="button" onClick={props.onDeleteAndDissolve}><Unlink size={16} /><span><strong>{zh ? '删除并解散合集' : 'Remove and dissolve'}</strong><small>{zh ? '把剧集或多版本拆回独立视频' : 'Split episodes or versions back into individual videos'}</small></span></button>
          ) : null}
        </div>
      </section>
    </div>
  )
}
