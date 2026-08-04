// End-to-end test of the ycpp-sync live server: two real yjs peers connect
// over WebSocket, edit concurrently, and must converge through the C++ relay.
// Usage: node websocket.test.js <path-to-ycpp-sync> <web-dir>
'use strict'
const Y = require('yjs')
const WebSocket = require('ws')
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
const withTimeout = (p, ms, what) =>
  Promise.race([p, sleep(ms).then(() => { throw new Error(`timeout: ${what}`) })])

// A yjs peer connected to the ycpp-sync server, mirroring web/live.html.
class Peer {
  constructor () {
    this.doc = new Y.Doc()
    this.text = this.doc.getText('default')
    this.ws = new WebSocket(`ws://127.0.0.1:${PORT}`)
    this.opened = new Promise(res => this.ws.on('open', res))
    this.firstMessage = new Promise(res => this.ws.once('message', res))
    this.ws.on('message', data => Y.applyUpdate(this.doc, new Uint8Array(data), 'ws'))
    this.doc.on('update', (update, origin) => {
      if (origin !== 'ws' && this.ws.readyState === WebSocket.OPEN) this.ws.send(update)
    })
  }
  async connect () {
    await withTimeout(this.opened, 5000, 'ws open')
    this.ws.send(Y.encodeStateAsUpdate(this.doc))
    await withTimeout(this.firstMessage, 5000, 'initial state')
  }
  // wait until a condition on the text holds (updates arrive asynchronously)
  async until (fn, what) {
    const deadline = Date.now() + 5000
    while (!fn()) {
      if (Date.now() > deadline) throw new Error(`timeout waiting for ${what}`)
      await sleep(25)
    }
  }
  close () { this.ws.close() }
}

const httpGet = path => new Promise((resolve, reject) => {
  http.get({ host: '127.0.0.1', port: PORT, path }, res => {
    let body = ''
    res.on('data', c => { body += c })
    res.on('end', () => resolve({ status: res.statusCode, body }))
  }).on('error', reject)
})

const main = async () => {
  const server = spawn(BIN, ['serve', '--headless', '--port', String(PORT), '--web', WEB],
                       { stdio: ['ignore', 'pipe', 'inherit'] })
  const listening = new Promise((resolve, reject) => {
    server.stdout.on('data', d => { if (String(d).includes('listening')) resolve() })
    server.on('exit', c => reject(new Error(`server exited early (${c})`)))
  })
  await withTimeout(listening, 5000, 'server start')

  try {
    // HTTP serves the live page
    const page = await withTimeout(httpGet('/'), 5000, 'GET /')
    check(page.status === 200 && page.body.includes('y-cpp live sync'), 'GET / serves live.html')
    const missing = await withTimeout(httpGet('/nope.html'), 5000, 'GET 404')
    check(missing.status === 404, 'GET /nope.html is 404')
    const evil = await withTimeout(httpGet('/../CMakeLists.txt'), 5000, 'GET traversal')
    check(evil.status !== 200, 'path traversal rejected')

    // Peer A connects and edits
    const a = new Peer()
    await a.connect()
    check(a.text.toString() === '', 'A starts empty')
    a.text.insert(0, 'hello from A')

    // Peer B connects later and must receive A's text from the server doc
    const b = new Peer()
    await b.connect()
    await b.until(() => b.text.toString() === 'hello from A', 'B initial state')
    check(true, 'B received state on connect')

    // B edits; the server must relay to A
    b.text.insert(b.text.length, ' + B')
    await a.until(() => a.text.toString() === 'hello from A + B', 'relay B→A')
    check(true, 'update relayed to A')

    // Concurrent edits at the same position converge identically
    a.text.insert(0, '[a]')
    b.text.insert(0, '[b]')
    await a.until(() => a.text.toString() === b.text.toString(), 'convergence')
    check(a.text.toString() === b.text.toString(), 'concurrent edits converged')
    check(a.text.toString().includes('[a]') && a.text.toString().includes('[b]'),
          'both concurrent edits present')

    // A third peer sees the final converged state
    const c = new Peer()
    await c.connect()
    await c.until(() => c.text.toString() === a.text.toString(), 'C initial state')
    check(true, 'late joiner gets converged state')

    a.close(); b.close(); c.close()
  } finally {
    server.kill('SIGTERM')
    await new Promise(res => server.on('exit', res))
  }

  if (failures > 0) {
    console.error(`${failures} failure(s)`)
    process.exit(1)
  }
  console.log('all websocket tests passed')
}

main().catch(e => {
  console.error(`FAIL ${e.message}`)
  process.exit(1)
})
