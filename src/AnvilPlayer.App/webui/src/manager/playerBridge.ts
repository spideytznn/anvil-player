import type { MediaItem, PlayerLaunchRequest } from './types'

export function createPlayerLaunchRequest(item: MediaItem): PlayerLaunchRequest {
  return {
    mediaId: item.id,
    title: item.title,
    sourceId: item.sourceId,
    mediaKind: item.type,
    startPositionRatio: item.progress > 0 && item.progress < 1 ? item.progress : 0,
    target: 'external-player-window'
  }
}

export function requestPlayerLaunch(item: MediaItem): string {
  const request = createPlayerLaunchRequest(item)
  window.dispatchEvent(new CustomEvent<PlayerLaunchRequest>('anvil-manager:playback-request', { detail: request }))
  return `${item.title} 已加入播放器调用队列`
}
