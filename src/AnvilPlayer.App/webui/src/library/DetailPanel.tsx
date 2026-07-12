import type { ReactNode } from 'react'

export function DetailPanel(props: { children: ReactNode }): JSX.Element {
  return <aside className="library-detail">{props.children}</aside>
}
