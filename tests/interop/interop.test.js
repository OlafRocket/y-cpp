// Conformity tests: exchange real binary updates between the reference yjs
// implementation and the ycpp CLI. Usage: node interop.test.js <path-to-ycpp>
'use strict'
const Y = require('yjs')
const { execFileSync } = require('child_process')
const fs = require('fs')
const os = require('os')
const path = require('path')

const YCPP = process.argv[2]
if (!YCPP) {
  console.error('usage: node interop.test.js <path-to-ycpp-cli>')
  process.exit(2)
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ycpp-interop-'))
let failures = 0
let testName = ''

const check = (cond, msg) => {
  if (!cond) {
    failures++
    console.error(`FAIL [${testName}] ${msg}`)
  }
}
const checkEq = (a, b, msg) => check(a === b, `${msg}: ${JSON.stringify(a)} !== ${JSON.stringify(b)}`)

let docCounter = 0
const cli = (args, opts = {}) =>
  execFileSync(YCPP, args, { encoding: 'utf8', ...opts }).replace(/\n$/, '')

// A tiny wrapper around a ycpp doc file.
class CppDoc {
  constructor (clientId) {
    this.file = path.join(tmp, `doc${docCounter++}.ydoc`)
    this.env = { ...process.env, YCPP_CLIENT_ID: String(clientId) }
    cli(['new', this.file], { env: this.env })
  }
  insert (index, text) { cli(['insert', this.file, String(index), text], { env: this.env }) }
  remove (index, len) { cli(['delete', this.file, String(index), String(len)], { env: this.env }) }
  show () { return cli(['show', this.file], { env: this.env }) }
  export () { return Buffer.from(cli(['export', this.file], { env: this.env }), 'base64') }
  sv () { return Buffer.from(cli(['sv', this.file], { env: this.env }), 'base64') }
  diff (svBuf) {
    return Buffer.from(cli(['diff', this.file, Buffer.from(svBuf).toString('base64')], { env: this.env }), 'base64')
  }
  apply (updateBuf) { cli(['apply', this.file, Buffer.from(updateBuf).toString('base64')], { env: this.env }) }
  json () { return JSON.parse(cli(['json', this.file], { env: this.env })) }
  sample () { return JSON.parse(cli(['sample', this.file], { env: this.env })) }
}

const syncBoth = (ydoc, cppDoc) => {
  // yjs -> cpp and cpp -> yjs using state-vector diffs, like a real provider.
  cppDoc.apply(Y.encodeStateAsUpdate(ydoc, cppDoc.sv()))
  Y.applyUpdate(ydoc, cppDoc.diff(Y.encodeStateVector(ydoc)))
}

const test = (name, fn) => {
  testName = name
  const before = failures
  try {
    fn()
  } catch (e) {
    failures++
    console.error(`FAIL [${name}] exception: ${e.message}`)
  }
  console.log(`${failures === before ? 'ok  ' : 'FAIL'} ${name}`)
}

// --------------------------------------------------------------------------

test('yjs update applied by ycpp', () => {
  const ydoc = new Y.Doc()
  ydoc.getText('default').insert(0, 'hello from yjs')
  const cpp = new CppDoc(1000)
  cpp.apply(Buffer.from(Y.encodeStateAsUpdate(ydoc)))
  checkEq(cpp.show(), 'hello from yjs', 'text transferred to cpp')
})

test('ycpp update applied by yjs', () => {
  const cpp = new CppDoc(1001)
  cpp.insert(0, 'hello from cpp')
  cpp.insert(5, ',')
  cpp.remove(0, 1)
  const ydoc = new Y.Doc()
  Y.applyUpdate(ydoc, cpp.export())
  checkEq(ydoc.getText('default').toString(), 'ello, from cpp', 'text transferred to yjs')
})

test('yjs deletes applied by ycpp', () => {
  const ydoc = new Y.Doc()
  const t = ydoc.getText('default')
  t.insert(0, 'abcdefghij')
  t.delete(2, 3)
  const cpp = new CppDoc(1002)
  cpp.apply(Buffer.from(Y.encodeStateAsUpdate(ydoc)))
  checkEq(cpp.show(), t.toString(), 'tombstones transferred')
})

test('concurrent edits converge (yjs vs ycpp)', () => {
  const ydoc = new Y.Doc()
  const cpp = new CppDoc(2000)
  ydoc.getText('default').insert(0, 'The quick fox')
  syncBoth(ydoc, cpp)
  // Concurrent edits on both sides.
  ydoc.getText('default').insert(9, ' brown')
  cpp.insert(13, ' jumps')
  cpp.remove(0, 3)
  syncBoth(ydoc, cpp)
  syncBoth(ydoc, cpp)
  checkEq(cpp.show(), ydoc.getText('default').toString(), 'converged text')
  check(cpp.show().includes('brown') && cpp.show().includes('jumps'), 'both edits present')
})

test('concurrent same-position inserts have identical order', () => {
  // yjs client id is random; run against smaller and larger cpp client ids.
  for (const cppId of [1, 0x7ffffff0]) {
    const ydoc = new Y.Doc()
    const cpp = new CppDoc(cppId)
    ydoc.getText('default').insert(0, '[]')
    syncBoth(ydoc, cpp)
    ydoc.getText('default').insert(1, 'YYY')
    cpp.insert(1, 'CCC')
    syncBoth(ydoc, cpp)
    syncBoth(ydoc, cpp)
    checkEq(cpp.show(), ydoc.getText('default').toString(), `same order (cpp id ${cppId})`)
  }
})

test('ycpp splits items created by yjs', () => {
  const ydoc = new Y.Doc()
  ydoc.getText('default').insert(0, '0123456789')
  const cpp = new CppDoc(2001)
  syncBoth(ydoc, cpp)
  cpp.insert(5, 'X')
  cpp.remove(0, 2)
  syncBoth(ydoc, cpp)
  checkEq(ydoc.getText('default').toString(), '234X56789', 'split + delete visible in yjs')
  checkEq(cpp.show(), ydoc.getText('default').toString(), 'still converged')
})

test('yjs splits items created by ycpp', () => {
  const cpp = new CppDoc(2002)
  cpp.insert(0, '0123456789')
  const ydoc = new Y.Doc()
  syncBoth(ydoc, cpp)
  ydoc.getText('default').insert(5, 'X')
  ydoc.getText('default').delete(0, 2)
  syncBoth(ydoc, cpp)
  checkEq(cpp.show(), '234X56789', 'split + delete visible in cpp')
  checkEq(cpp.show(), ydoc.getText('default').toString(), 'still converged')
})

test('unicode text (multi-byte + surrogate pairs)', () => {
  const ydoc = new Y.Doc()
  const t = ydoc.getText('default')
  t.insert(0, 'héllo 中文 😀🎉 done')
  const cpp = new CppDoc(2003)
  syncBoth(ydoc, cpp)
  checkEq(cpp.show(), t.toString(), 'unicode transferred')
  cpp.insert(6, '👍 ')  // insert an astral-plane char at a UTF-16 index
  t.delete(0, 1)
  syncBoth(ydoc, cpp)
  syncBoth(ydoc, cpp)
  checkEq(cpp.show(), t.toString(), 'unicode edits converged')
})

test('out-of-order delivery to ycpp', () => {
  const ydoc = new Y.Doc()
  ydoc.getText('default').insert(0, 'one')
  const sv1 = Y.encodeStateVector(ydoc)
  const u1 = Y.encodeStateAsUpdate(ydoc)
  ydoc.getText('default').insert(3, ' two')
  const u2 = Y.encodeStateAsUpdate(ydoc, sv1)
  const cpp = new CppDoc(2004)
  cpp.apply(Buffer.from(u2))  // newer diff first
  checkEq(cpp.show(), '', 'diff alone shows nothing')
  cpp.apply(Buffer.from(u1))
  checkEq(cpp.show(), 'one two', 'complete after base arrives')
})

test('random fuzz converges with yjs', () => {
  // Deterministic PRNG so failures are reproducible.
  let seed = 42
  const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n }
  const words = ['a', 'bb', 'ccc', 'é', '😀', 'word ']
  for (let round = 0; round < 5; round++) {
    const ydoc = new Y.Doc()
    const t = ydoc.getText('default')
    const cpp = new CppDoc(3000 + round)
    for (let step = 0; step < 30; step++) {
      if (rnd(2) === 0) {
        if (rnd(3) && t.length > 0) {
          const idx = rnd(t.length)
          t.delete(idx, 1 + rnd(Math.min(t.length - idx, 3)))
        } else {
          t.insert(rnd(t.length + 1), words[rnd(words.length)])
        }
      } else {
        const len = Number(cli(['show', cpp.file], { env: cpp.env }).length)  // UTF-8 length != UTF-16; use insert at 0 for safety
        if (rnd(3) === 0) cpp.insert(0, words[rnd(words.length)])
        else cpp.insert(0, 'z')
        void len
      }
      if (rnd(4) === 0) syncBoth(ydoc, cpp)
    }
    syncBoth(ydoc, cpp)
    syncBoth(ydoc, cpp)
    checkEq(cpp.show(), t.toString(), `round ${round} converged`)
  }
})

// Deep equality with object keys normalized (cpp emits keys sorted, yjs in
// insertion order — both are valid JSON object representations).
const normalize = v => {
  if (Array.isArray(v)) return v.map(normalize)
  if (v && typeof v === 'object') {
    const out = {}
    for (const k of Object.keys(v).sort()) out[k] = normalize(v[k])
    return out
  }
  return v
}
const deepEq = (a, b) => JSON.stringify(normalize(a)) === JSON.stringify(normalize(b))

test('yjs map/array applied by ycpp', () => {
  const ydoc = new Y.Doc()
  const list = ydoc.getArray('list')
  list.insert(0, [1, 'two', 3.5, true, null, { obj: [1, 2] }])
  const meta = ydoc.getMap('meta')
  meta.set('title', 'from yjs')
  meta.set('gone', 'x')
  meta.delete('gone')
  const nested = new Y.Map()
  meta.set('nested', nested)
  nested.set('deep', -5)
  const items = new Y.Array()
  nested.set('items', items)
  items.push(['a', 'b'])
  const ytext = new Y.Text()
  meta.set('note', ytext)
  ytext.insert(0, 'nested text')

  const cpp = new CppDoc(5000)
  cpp.apply(Buffer.from(Y.encodeStateAsUpdate(ydoc)))
  const j = cpp.json()
  check(deepEq(j.list, list.toJSON()), 'array transferred')
  check(deepEq(j.meta, meta.toJSON()), 'map with nesting transferred')
})

test('ycpp map/array applied by yjs', () => {
  const cpp = new CppDoc(5001)
  const expected = cpp.sample()
  const ydoc = new Y.Doc()
  Y.applyUpdate(ydoc, cpp.export())
  check(deepEq(ydoc.getArray('list').toJSON(), expected.list), 'array read by yjs')
  check(deepEq(ydoc.getMap('meta').toJSON(), expected.meta), 'nested map read by yjs')
  check(ydoc.getText('prose').toString() === expected.prose, 'text read by yjs')
})

test('concurrent map sets converge across implementations', () => {
  for (const cppId of [1, 0x7ffffff0]) {  // below and above yjs's random id
    const ydoc = new Y.Doc()
    const cpp = new CppDoc(cppId)
    ydoc.getMap('m').set('k', 'init')
    syncBoth(ydoc, cpp)
    ydoc.getMap('m').set('k', 'from-yjs')
    // cpp side sets the same key concurrently via a second cpp doc edit:
    // build the edit through a sample-free path: apply a small update built here
    const helper = new Y.Doc()
    Y.applyUpdate(helper, cpp.export())
    // Instead: cpp has no map-set CLI; simulate the concurrent edit from a
    // separate yjs doc applied only to cpp, so cpp integrates it first.
    helper.getMap('m').set('k', 'from-helper')
    cpp.apply(Buffer.from(Y.encodeStateAsUpdate(helper, cpp.sv())))
    syncBoth(ydoc, cpp)
    syncBoth(ydoc, cpp)
    check(deepEq(cpp.json().m, ydoc.getMap('m').toJSON()),
          `map conflict converged (cpp id ${cppId})`)
  }
})

test('yjs deletes nested type created by ycpp', () => {
  const cpp = new CppDoc(5002)
  cpp.sample()
  const ydoc = new Y.Doc()
  Y.applyUpdate(ydoc, cpp.export())
  ydoc.getMap('meta').delete('nested')  // cascade-deletes cpp-created subtree
  cpp.apply(Buffer.from(Y.encodeStateAsUpdate(ydoc, cpp.sv())))
  check(cpp.json().meta.nested === undefined, 'cascade delete applied by cpp')
  check(deepEq(cpp.json().meta, ydoc.getMap('meta').toJSON()), 'still converged')
})

test('binary values survive the cpp round-trip', () => {
  const ydoc = new Y.Doc()
  ydoc.getMap('m').set('bytes', new Uint8Array([0, 1, 2, 250, 255]))
  const cpp = new CppDoc(5003)
  cpp.apply(Buffer.from(Y.encodeStateAsUpdate(ydoc)))
  const ydoc2 = new Y.Doc()
  Y.applyUpdate(ydoc2, cpp.export())  // re-encoded by cpp
  const back = ydoc2.getMap('m').get('bytes')
  check(back instanceof Uint8Array && Buffer.from(back).equals(Buffer.from([0, 1, 2, 250, 255])),
        'bytes identical after cpp re-encode')
})

test('ycpp doc file is a valid yjs update', () => {
  const cpp = new CppDoc(4000)
  cpp.insert(0, 'file = update')
  const ydoc = new Y.Doc()
  Y.applyUpdate(ydoc, fs.readFileSync(cpp.file))
  checkEq(ydoc.getText('default').toString(), 'file = update', 'doc file readable by yjs')
})

fs.rmSync(tmp, { recursive: true, force: true })
if (failures > 0) {
  console.error(`${failures} failure(s)`)
  process.exit(1)
}
console.log('all interop tests passed')
