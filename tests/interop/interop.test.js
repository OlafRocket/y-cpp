// Conformity tests: exchange real binary updates between the reference yjs
// implementation and the ycpp CLI. Usage: node interop.test.js <path-to-ycpp>
'use strict'
const { Y, name: yjsName, version: yjsVersion, isV14 } = require('./yjs-compat')
const { execFileSync } = require('child_process')
const fs = require('fs')
const os = require('os')
const path = require('path')

const YCPP = process.argv[2]
if (!YCPP) {
  console.error('usage: node interop.test.js <path-to-ycpp-cli>')
  process.exit(2)
}

console.log(`yjs under test: ${yjsName}@${yjsVersion}${isV14 ? ' (v14 shim)' : ''}`)
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

// opts.skipV14: a reason string; the test is reported as skipped under a
// yjs 14 package because of a documented, intended upstream difference.
const test = (name, fn, opts = {}) => {
  testName = name
  const before = failures
  if (isV14 && opts.skipV14) {
    console.log(`skip ${name} (v14: ${opts.skipV14})`)
    return
  }
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
}, { skipV14: 'nested nodes created by v14 are encoded with the XmlFragment type ref' })

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

// Uniform peer interface over a yjs doc, matching CppDoc, for multi-peer
// scenarios where the roles are swapped between implementations.
class YPeer {
  constructor () { this.doc = new Y.Doc(); this.text = this.doc.getText('default') }
  insert (index, text) { this.text.insert(index, text) }
  remove (index, len) { this.text.delete(index, len) }
  show () { return this.text.toString() }
  sv () { return Y.encodeStateVector(this.doc) }
  diff (sv) { return Y.encodeStateAsUpdate(this.doc, sv) }
  apply (update) { Y.applyUpdate(this.doc, update) }
}
const push = (from, to) => to.apply(from.diff(to.sv()))

test('insert before a leading tombstone converges (y-octo #53)', () => {
  // c0 writes "a"; c1 appends "b" after it; c0 inserts "c" and deletes "a";
  // c2, knowing only c0's state, inserts "d" at index 0 in front of the
  // tombstone. origin and rightOrigin of "d" must be adjacent or c1 and c2
  // diverge. Both role assignments put ycpp in the position that matters.
  for (const cppRole of ['c2', 'c1']) {
    const mk = (role, id) => (role === cppRole ? new CppDoc(id) : new YPeer())
    const c0 = new YPeer()
    const c1 = mk('c1', 7001)
    const c2 = mk('c2', 7002)
    c0.insert(0, 'a')
    push(c0, c1)
    c1.insert(1, 'b')
    c0.insert(1, 'c')
    c0.remove(0, 1)
    push(c0, c2)
    c2.insert(0, 'd')
    push(c1, c2)
    push(c2, c1)
    checkEq(c1.show(), c2.show(), `c1 and c2 converge (cpp as ${cppRole})`)
    push(c1, c0)
    push(c2, c0)
    checkEq(c0.show(), c1.show(), `c0 converges (cpp as ${cppRole})`)
    checkEq(c0.show().length, 3, `d, c and b present (cpp as ${cppRole})`)
  }
})

test('update with a mid-stream Skip struct (yjs 14 store shape)', () => {
  // A yjs 14 store tolerates clock gaps and emits Skip structs between runs
  // of one client in ordinary updates. yjs 13 produces the same shape with
  // mergeUpdates over non-adjacent updates.
  const ydoc = new Y.Doc()
  const t = ydoc.getText('default')
  t.insert(0, 'one')
  const u1 = Y.encodeStateAsUpdate(ydoc)
  const sv1 = Y.encodeStateVector(ydoc)
  t.insert(3, ' two')
  const u2 = Y.encodeStateAsUpdate(ydoc, sv1)
  const sv2 = Y.encodeStateVector(ydoc)
  t.insert(7, ' three')
  const u3 = Y.encodeStateAsUpdate(ydoc, sv2)
  const merged = Y.mergeUpdates([u1, u3])  // clocks [0,3) and [7,13): Skip in between
  const cpp = new CppDoc(2005)
  cpp.apply(Buffer.from(merged))
  checkEq(cpp.show(), 'one', 'structs behind the Skip stay pending')
  cpp.apply(Buffer.from(u2))
  checkEq(cpp.show(), 'one two three', 'gap filled, pending structs integrated')
  const back = new Y.Doc()
  Y.applyUpdate(back, cpp.export())
  checkEq(back.getText('default').toString(), 'one two three', 'cpp re-encode readable by yjs')
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
