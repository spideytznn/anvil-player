import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App'
import LibraryApp from './LibraryApp'
import { postNativeCommand } from './nativeBridge'
import './styles.css'

function Root(): JSX.Element {
  const [hash, setHash] = React.useState(() => window.location.hash)

  React.useEffect(() => {
    const onHashChange = (): void => setHash(window.location.hash)
    window.addEventListener('hashchange', onHashChange)
    return () => window.removeEventListener('hashchange', onHashChange)
  }, [])

  React.useEffect(() => {
    postNativeCommand({
      type: 'command',
      command: 'setWebUiRoute',
      route: hash.startsWith('#/player') ? 'player' : 'library'
    })
  }, [hash])

  if (hash.startsWith('#/player')) {
    return <App />
  }

  return <LibraryApp />
}

ReactDOM.createRoot(document.getElementById('root') as HTMLElement).render(
  <React.StrictMode>
    <Root />
  </React.StrictMode>
)
