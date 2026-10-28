// Loads the yjs package under test and presents the yjs 13.x API subset the
// interop suite uses.
//
// Selection (set by run_interop.sh): YCPP_INTEROP_YJS_DIR is a directory with
// its own node_modules, YCPP_INTEROP_YJS_NAME the package name inside it
// ("yjs" or "@y/y"). Unset: the yjs in tests/interop/node_modules.
//
// For @y/y 14 (single YNode class, attrs instead of map keys, no getText /
// getArray / getMap) a thin shim maps the 13.x calls onto YNode. The shim
// does not hide wire-level differences: a nested node created by v14 is
// still encoded with v14's type ref.
'use strict'
const path = require('path')
const { createRequire } = require('module')

const dir = process.env.YCPP_INTEROP_YJS_DIR || ''
const name = process.env.YCPP_INTEROP_YJS_NAME || 'yjs'
const req = dir ? createRequire(path.join(dir, 'package.json')) : require
const raw = req(name)
const version = req(`${name}/package.json`).version
const isV14 = raw.Map === undefined && typeof raw.Node === 'function'

let Y = raw
if (isV14) {
  const N = raw.Node
  class Shim { constructor (node) { this.node = node } }
  const unwrap = v => (v instanceof Shim ? v.node : v)
  const toPlain = v => (v instanceof N ? nodeToPlain(v) : v)
  // 13.x toJSON shape: a node with attributes is an object, text is a string,
  // anything else is an array.
  // Nodes decoded from the wire keep the 13.x type ref (0 array, 1 map,
  // 2 text); nodes created by v14 carry 4 (XmlFragment) and are guessed.
  const nodeToPlain = node => {
    const ref = node._legacyTypeRef
    const attrs = node.getAttrs()
    const keys = Object.keys(attrs)
    if (ref === 1 || (ref !== 0 && ref !== 2 && keys.length > 0)) {
      const o = {}
      for (const k of keys) o[k] = toPlain(attrs[k])
      return o
    }
    const children = node.toArray()
    if (ref === 2 || (ref !== 0 && children.length > 0 && children.every(c => typeof c === 'string'))) {
      return children.join('')
    }
    return children.map(toPlain)
  }
  const wrapValue = v => (v instanceof N ? new MapShim(v) : v)

  class TextShim extends Shim {
    insert (index, text) { this.node.insert(index, text) }
    delete (index, len) { this.node.delete(index, len) }
    toString () { return this.node.toString() }
    get length () { return this.node.length }
  }
  class ArrayShim extends Shim {
    insert (index, values) { this.node.insert(index, values.map(unwrap)) }
    push (values) { this.node.push(values.map(unwrap)) }
    delete (index, len) { this.node.delete(index, len) }
    get (index) { return wrapValue(this.node.get(index)) }
    toJSON () { return this.node.toArray().map(toPlain) }
    get length () { return this.node.length }
  }
  class MapShim extends Shim {
    set (key, value) { this.node.setAttr(key, unwrap(value)) }
    get (key) { return wrapValue(this.node.getAttr(key)) }
    delete (key) { this.node.deleteAttr(key) }
    has (key) { return this.node.hasAttr(key) }
    toJSON () {
      const attrs = this.node.getAttrs()
      const o = {}
      for (const k of Object.keys(attrs)) o[k] = toPlain(attrs[k])
      return o
    }
  }
  class Doc extends raw.Doc {
    getText (name = '') { return new TextShim(this.get(name)) }
    getArray (name = '') { return new ArrayShim(this.get(name)) }
    getMap (name = '') { return new MapShim(this.get(name)) }
  }
  Y = {
    ...raw,
    Doc,
    Text: class extends TextShim { constructor () { super(new N()) } },
    Array: class extends ArrayShim { constructor () { super(new N()) } },
    Map: class extends MapShim { constructor () { super(new N()) } }
  }
}

module.exports = { Y, name, version, isV14 }
