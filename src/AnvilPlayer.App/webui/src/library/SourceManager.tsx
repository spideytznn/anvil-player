import { ArrowLeft, Check, ChevronRight, Database, FolderOpen, HardDrive, Search, Server, X } from 'lucide-react'

export interface FileServiceDirectory {
  name: string
  path: string
}

function locationKey(value: string): string {
  return value.trim().replace(/[\\/]+$/g, '').toLocaleLowerCase()
}

function displayName(value: string): string {
  const normalized = value.replace(/[\\/]+$/g, '')
  const name = normalized.split(/[\\/]/).filter(Boolean).at(-1) ?? normalized
  try {
    return decodeURIComponent(name)
  } catch {
    return name
  }
}

export function FileServiceDirectoryBrowser(props: {
  currentPath: string
  directories: FileServiceDirectory[]
  selectedPaths: string[]
  loading: boolean
  emptyLabel: string
  parentPath: string
  onBrowse: (path?: string) => void
  onToggle: (path: string) => void
}): JSX.Element {
  const selected = (path: string): boolean => props.selectedPaths.some((row) => locationKey(row) === locationKey(path))
  return (
    <div className="library-folder-browser">
      <div className="library-folder-browser-toolbar">
        <button className="library-secondary-action" type="button" disabled={props.loading || !props.parentPath} onClick={() => props.onBrowse(props.parentPath)}><ArrowLeft size={15} /><span>上级</span></button>
        <button className="library-secondary-action" type="button" disabled={props.loading} onClick={() => props.onBrowse(props.currentPath || undefined)}><Search size={15} /><span>{props.loading ? '读取中…' : '刷新'}</span></button>
        <button className="library-secondary-action" type="button" disabled={props.loading || !props.currentPath} onClick={() => props.onToggle(props.currentPath)}><Check size={15} /><span>{selected(props.currentPath) ? '取消当前' : '选择当前'}</span></button>
        <span className="library-folder-browser-path">{props.currentPath || '尚未连接'}</span>
      </div>
      <div className="library-folder-browser-list">
        {props.directories.length ? props.directories.map((directory) => (
          <div className="library-folder-browser-row" key={directory.path}>
            <label><input type="checkbox" checked={selected(directory.path)} onChange={() => props.onToggle(directory.path)} /><FolderOpen size={16} /><span>{directory.name}</span></label>
            <button className="library-icon-action" type="button" onClick={() => props.onBrowse(directory.path)}><ChevronRight size={16} /></button>
          </div>
        )) : <div className="library-folder-browser-empty">{props.loading ? '正在读取…' : props.emptyLabel}</div>}
      </div>
      {props.selectedPaths.length ? (
        <div className="library-folder-selected-list">
          {props.selectedPaths.map((path) => <button type="button" key={path} onClick={() => props.onToggle(path)}><Check size={13} /><span>{displayName(path)}</span><X size={13} /></button>)}
        </div>
      ) : null}
    </div>
  )
}

function EmbyIcon(): JSX.Element {
  return (
    <svg className="library-emby-icon" width="22" height="22" viewBox="0 0 512 512" aria-hidden="true">
      <path className="library-emby-icon-mark" d="m97.1 229.4 26.5 26.5L0 379.5l132.4 132.4 26.5-26.5L282.5 609l141.2-141.2-26.5-26.5L512 326.5 379.6 194.1l-26.5 26.5L229.5 97z" transform="translate(0 -97)" />
      <path className="library-emby-icon-play" d="M196.8 351.2v-193L366 254.7 281.4 303z" />
    </svg>
  )
}

export function SourceManager(props: {
  onSelectEmby: () => void
  onSelectLocalFolder: () => void
  onSelectSmb: () => void
  onSelectWebDav: () => void
}): JSX.Element {
  const choices = [
    { key: 'local', icon: <HardDrive size={22} />, title: '本地文件夹', caption: '扫描本机目录，后续可自行刮削', select: props.onSelectLocalFolder },
    { key: 'smb', icon: <Server size={22} />, title: 'SMB', caption: '扫描 Windows 共享或映射盘路径', select: props.onSelectSmb },
    { key: 'webdav', icon: <Database size={22} />, title: 'WebDAV', caption: '连接常见 WebDAV 文件服务', select: props.onSelectWebDav }
  ]
  return (
    <section className="library-source-setup">
      <div className="library-source-setup-heading"><span>媒体源</span><h2>添加媒体源</h2></div>
      <div className="library-source-type-sections">
        <section className="library-source-type-section"><span>文件系统</span><div className="library-source-type-grid">
          {choices.map((choice) => <button key={choice.key} className="library-source-type-button" type="button" onClick={choice.select}>{choice.icon}<strong>{choice.title}</strong><span>{choice.caption}</span></button>)}
        </div></section>
        <section className="library-source-type-section"><span>媒体库服务</span><div className="library-source-type-grid">
          <button className="library-source-type-button" type="button" onClick={props.onSelectEmby}><EmbyIcon /><strong>Emby</strong><span>服务器媒体库</span></button>
        </div></section>
      </div>
    </section>
  )
}
