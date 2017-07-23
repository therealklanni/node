'use strict';
const ModuleJob = require('internal/loader/ModuleJob');
const { SafeMap } = require('internal/safe_globals');
const { TypeError } = global;

// Tracks the state of the loader-level module cache
class ModuleMap extends SafeMap {
  get(url) {
    if (typeof url !== 'string') {
      throw new TypeError(`expected string, got ${typeof url}`);
    }
    return super.get(url);
  }
  set(url, job) {
    if (typeof url !== 'string') {
      throw new TypeError(`expected string, got ${typeof url}`);
    }
    if (job instanceof ModuleJob !== true) {
      throw new TypeError('expected ModuleJob');
    }
    return super.set(url, job);
  }
  has(url) {
    if (typeof url !== 'string') {
      throw new TypeError(`expected string, got ${typeof url}`);
    }
    return super.has(url);
  }
}
module.exports = ModuleMap;
