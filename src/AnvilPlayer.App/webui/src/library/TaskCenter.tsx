import { CheckCircle2, CircleAlert, Pause, Play, RotateCcw, X, XCircle } from 'lucide-react'
import type { UiLanguage } from '../uiSettings'

export type BackgroundTaskKind = 'scan' | 'probe' | 'metadata' | 'artwork'
export type BackgroundTaskStatus = 'queued' | 'running' | 'paused' | 'completed' | 'failed' | 'cancelled'

export interface BackgroundTask {
  id: string
  kind: BackgroundTaskKind
  title: string
  detail: string
  status: BackgroundTaskStatus
  completed: number
  total: number
  startedAt: number
  updatedAt: number
  error?: string
}

interface TaskCenterProps {
  open: boolean
  language: UiLanguage
  tasks: BackgroundTask[]
  onClose: () => void
  onPause: (task: BackgroundTask) => void
  onResume: (task: BackgroundTask) => void
  onRetry: (task: BackgroundTask) => void
  onCancel: (task: BackgroundTask) => void
  onClearFinished: () => void
}

function taskProgress(task: BackgroundTask): number {
  if (task.status === 'completed') return 100
  if (task.total <= 0) return task.status === 'running' ? 12 : 0
  return Math.max(0, Math.min(100, Math.round(task.completed / task.total * 100)))
}

function taskRate(task: BackgroundTask, zh: boolean): string {
  if (task.status !== 'running' || task.completed <= 0) return ''
  const seconds = Math.max(0.25, (task.updatedAt - task.startedAt) / 1000)
  const rate = task.completed / seconds
  return `${rate.toFixed(rate >= 10 ? 0 : 1)} ${zh ? '项/秒' : 'items/s'}`
}

export function TaskCenter(props: TaskCenterProps): JSX.Element | null {
  if (!props.open) return null
  const zh = props.language === 'zh'
  const activeCount = props.tasks.filter((task) => task.status === 'running' || task.status === 'queued' || task.status === 'paused').length
  return (
    <aside className="library-task-center" aria-label={zh ? '后台任务中心' : 'Background tasks'}>
      <header>
        <div>
          <span>{zh ? '后台任务' : 'Background tasks'}</span>
          <small>{activeCount ? `${activeCount} ${zh ? '项进行中' : 'active'}` : (zh ? '当前没有运行中的任务' : 'No active tasks')}</small>
        </div>
        <button type="button" aria-label={zh ? '关闭' : 'Close'} onClick={props.onClose}><X size={17} /></button>
      </header>
      <div className="library-task-list">
        {props.tasks.map((task) => {
          const progress = taskProgress(task)
          const finished = task.status === 'completed' || task.status === 'failed' || task.status === 'cancelled'
          return (
            <article className={`library-task-card is-${task.status}`} key={task.id}>
              <div className="library-task-card-heading">
                {task.status === 'completed' ? <CheckCircle2 size={16} /> : task.status === 'failed' ? <CircleAlert size={16} /> : task.status === 'cancelled' ? <XCircle size={16} /> : <span className="library-task-spinner" />}
                <div><strong>{task.title}</strong><small>{task.detail}</small></div>
                <span>{progress}%</span>
              </div>
              <div className="library-task-progress"><i style={{ width: `${progress}%` }} /></div>
              <div className="library-task-footer">
                <span>{task.error || taskRate(task, zh) || `${task.completed}/${task.total || '—'}`}</span>
                <div>
                  {task.status === 'running' || task.status === 'queued' ? <button type="button" title={zh ? '暂停' : 'Pause'} onClick={() => props.onPause(task)}><Pause size={14} /></button> : null}
                  {task.status === 'paused' ? <button type="button" title={zh ? '继续' : 'Resume'} onClick={() => props.onResume(task)}><Play size={14} /></button> : null}
                  {finished ? <button type="button" title={zh ? '重试' : 'Retry'} onClick={() => props.onRetry(task)}><RotateCcw size={14} /></button> : null}
                  {!finished ? <button type="button" title={zh ? '取消' : 'Cancel'} onClick={() => props.onCancel(task)}><X size={14} /></button> : null}
                </div>
              </div>
            </article>
          )
        })}
        {!props.tasks.length ? <div className="library-task-empty">{zh ? '扫描、媒体探测、刮削和图片下载会显示在这里。' : 'Scans, media probes, metadata and artwork downloads appear here.'}</div> : null}
      </div>
      {props.tasks.some((task) => ['completed', 'failed', 'cancelled'].includes(task.status)) ? (
        <button className="library-task-clear" type="button" onClick={props.onClearFinished}>{zh ? '清除已结束任务' : 'Clear finished tasks'}</button>
      ) : null}
    </aside>
  )
}
