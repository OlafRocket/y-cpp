// End-to-end test of the ycpp-sync server speaking the y-websocket protocol:
// two STOCK y-websocket providers (unmodified npm package) connect, sync
// text through the C++ relay, and see each other through awareness.
// Usage: node websocket.test.js <path-to-ycpp-sync> <web-dir>
'use strict'
const http = require('http')
const { spawn } = require('child_process')

const BIN = process.argv[2]
const WEB = process.argv[3]
if (!BIN || !WEB) {
  console.error('usage: node websocket.test.js <path-to-ycpp-sync> <web-dir>')
  process.exit(2)
}
const PORT = 20000 + (process.pid % 10000)

let failures = 0
const check = (cond, msg) => {
  if (!cond) {
    failures++
    console.error(`FAIL ${msg}`)
  } else {
    console.log(`ok   ${msg}`)
  }
}
const sleep = ms => new Promise(r => setTimeout(r, ms))
const until = async (fn, what, ms = 8000) => {
  const deadline = Date.now() + ms
  while (!fn()) {
    if (Date.now() > deadline) throw new Error(`timeout waiting for ${what}`)
    await sleep(25)
  }
}

const httpGet = path => new Promise((resolve, reject) => {
  http.get({ host: '127.0.0.1', port: PORT, path }, res => {
    let body = ''
    res.on('data', c => { body += c })
    res.on('end', () => resolve({ status: res.statusCode, body }))
  }).on('error', reject)
})

const main = async () => {
  // Import yjs through the same ESM resolution as y-websocket so both share
  // one module instance (mixing the CJS and ESM builds breaks instanceof).
  const Y = await import('yjs')
  const { WebsocketProvider } = await import('y-websocket')
  const WebSocketPolyfill = require('ws')

  // Watchdog: never let a wedged run hang the suite.
  const watchdog = setTimeout(() => {
    console.error('FAIL global test timeout')
    process.exit(1)
  }, 90000)
  watchdog.unref()

  const server = spawn(BIN, ['serve', '--headless', '--port', String(PORT), '--web', WEB],
                       { stdio: ['ignore', 'pipe', 'inherit'] })
  const serverExit = new Promise(resolve => server.on('exit', resolve))
  const listening = new Promise((resolve, reject) => {
    server.stdout.on('data', d => { if (String(d).includes('listening')) resolve() })
    serverExit.then(c => reject(new Error(`server exited early (${c})`)))
  })
  await Promise.race([listening,
                      sleep(10000).then(() => { throw new Error('server did not start') })])

  const providers = []
  const peer = () => {
    const doc = new Y.Doc()
    const provider = new WebsocketProvider(`ws://127.0.0.1:${PORT}`, 'room', doc,
                                           { WebSocketPolyfill })
    providers.push(provider)
    return { doc, provider, text: doc.getText('default') }
  }

  try {
    // HTTP still serves the live page (any path -> single-room page).
    const page = await httpGet('/')
    check(page.status === 200 && page.body.includes('y-cpp live sync'), 'GET / serves live.html')

    // Stock provider A connects and syncs.
    const a = peer()
    await until(() => a.provider.synced, 'A synced')
    check(true, 'stock y-websocket provider completes the sync handshake')
    a.text.insert(0, 'hello from A')

    // B joins later and receives A's text through the C++ server.
    const b = peer()
    await until(() => b.provider.synced, 'B synced')
    await until(() => b.text.toString() === 'hello from A', 'B received state')
    check(true, 'late joiner receives document through cpp server')

    // Live relay in both directions.
    b.text.insert(b.text.length, ' + B')
    await until(() => a.text.toString() === 'hello from A + B', 'relay B->A')
    check(true, 'updates relayed live')
    a.text.insert(0, '[a]')
    b.text.insert(0, '[b]')
    await until(() => a.text.toString() === b.text.toString(), 'convergence')
    check(a.text.toString().includes('[a]') && a.text.toString().includes('[b]'),
          'concurrent edits converge')

    // Awareness: peers see each other's states through the server,
    // including the timebar pattern (last writer wins).
    a.provider.awareness.setLocalStateField('user', { name: 'A' })
    b.provider.awareness.setLocalStateField('user', { name: 'B' })
    a.provider.awareness.setLocalStateField('playhead', { t: 100, at: 1 })
    await until(() => {
      const s = [...b.provider.awareness.getStates().values()]
      return s.some(st => st.user && st.user.name === 'A' && st.playhead && st.playhead.t === 100)
    }, 'awareness A->B')
    check(true, 'awareness states relayed (timebar value visible)')
    b.provider.awareness.setLocalStateField('playhead', { t: 250, at: 2 })
    await until(() => {
      const s = [...a.provider.awareness.getStates().values()]
      return s.some(st => st.playhead && st.playhead.t === 250)
    }, 'awareness B->A')
    check(true, 'timebar last-write visible on the other side')

    // Disconnect removes the peer's awareness state.
    const bClient = b.doc.clientID
    b.provider.destroy()
    await until(() => ![...a.provider.awareness.getStates().keys()].includes(bClient),
                'awareness removal on disconnect')
    check(true, 'server removes awareness of disconnected peers')

    // A third peer still gets the full converged document.
    const c = peer()
    await until(() => c.provider.synced && c.text.toString() === a.text.toString(), 'C synced')
    check(true, 'fresh peer syncs converged state')
  } finally {
    for (const p of providers) { try { p.destroy() } catch (e) {} }
    server.kill('SIGTERM')
    await Promise.race([serverExit, sleep(3000).then(() => server.kill('SIGKILL'))])
  }

  if (failures > 0) {
    console.error(`${failures} failure(s)`)
    process.exit(1)
  }
  console.log('all websocket tests passed')
  process.exit(0)  // lingering provider handles keep the loop alive
}

main().catch(e => {
  console.error(`FAIL ${e.message}`)
  process.exit(1)
})
