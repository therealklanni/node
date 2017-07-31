'use strict';

const { URL } = require('url');
const internalCJSModule = require('internal/module');
const internalURLModule = require('internal/url');
const NativeModule = require('native_module');
const { extname } = require('path');

const { ModuleWrap } = process.binding('module_wrap');

const search = require('internal/loader/search');
const asyncReadFile = require('util').promisify(require('fs').readFile);
const debug = require('util').debuglog('esm');

const ArrayJoin = Function.call.bind(Array.prototype.join);
const ArrayMap = Function.call.bind(Array.prototype.map);

const { Error } = global;

const createDynamicModule = (exports, url = '', evaluate) => {
  debug(
    `creating ESM facade for ${url} with exports: ${ArrayJoin(exports, ', ')}`
  );
  const names = ArrayMap(exports, (name) => `${name}`);
  // sanitized ESM for reflection purposes
  const src = `export let executor;
  ${ArrayJoin(ArrayMap(names, (name) => `export let $${name}`), ';\n')}
  ;(() => [
    fn => executor = fn,
    { exports: { ${
      ArrayJoin(ArrayMap(names, (name) => `${name}: {
        get: () => $${name},
        set: v => $${name} = v
      }`), ',\n')
    } } }
  ]);
  `;
  const reflectiveModule = new ModuleWrap(src, `cjs-facade:${url}`);
  reflectiveModule.instantiate();
  const [setExecutor, reflect] = reflectiveModule.evaluate()();
  // public exposed ESM
  const reexports = `import { executor,
    ${ArrayMap(names, (name) => `$${name}`)}
  } from "";
  export {
    ${ArrayJoin(ArrayMap(names, (name) => `$${name} as ${name}`), ', ')}
  }
  // add await to this later if top level await comes along
  typeof executor === "function" ? executor() : void 0;`;
  if (typeof evaluate === 'function') {
    setExecutor(() => evaluate(reflect));
  }
  const runner = new ModuleWrap(reexports, `${url}`);
  runner.link(async () => reflectiveModule);
  runner.instantiate();
  return {
    module: runner,
    reflect
  };
};

class ModuleRequest {
  constructor(url) {
    this.url = url;
  }
}
Object.setPrototypeOf(ModuleRequest.prototype, null);

// Strategy for loading a standard JavaScript module
class StandardModuleRequest extends ModuleRequest {
  async createModule() {
    const source = `${await asyncReadFile(this.url)}`;
    debug(`Loading StandardModule ${this.url}`);
    return new ModuleWrap(internalCJSModule.stripShebang(source),
                          `${this.url}`);
  }
}

// Strategy for loading a node-style CommonJS module
class CJSModuleRequest extends ModuleRequest {
  async createModule() {
    const ctx = createDynamicModule(['default'], this.url, (reflect) => {
      const CJSModule = require('module');
      const pathname = internalURLModule.getPathFromURL(this.url);
      debug(`Loading CJSModule ${pathname}`);
      const cjs = new CJSModule(pathname, process.mainModule);
      cjs.load(pathname);
      reflect.exports.default.set(cjs.exports);
    });
    return ctx.module;
  }
}

// Strategy for loading a node builtin CommonJS module that isn't
// through normal resolution
class NativeModuleRequest extends CJSModuleRequest {
  async createModule() {
    const ctx = createDynamicModule(['default'], this.url, (reflect) => {
      debug(`Loading NativeModule ${this.url.pathname}`);
      const exports = require(this.url.pathname);
      reflect.exports.default.set(exports);
    });
    return ctx.module;
  }
}

const normalizeBaseURL = (baseURLOrString) => {
  if (baseURLOrString instanceof URL) return baseURLOrString;
  if (typeof baseURLOrString === 'string') return new URL(baseURLOrString);
  return undefined;
};

const resolveRequestUrl = (baseURLOrString, specifier) => {
  if (NativeModule.nonInternalExists(specifier)) {
    return new NativeModuleRequest(new URL(`node:${specifier}`));
  }

  const baseURL = normalizeBaseURL(baseURLOrString);
  let url = search(specifier, baseURL);

  if (url.protocol !== 'file:') {
    throw new Error('Module must be on file: protocol');
  }

  const ext = extname(url.pathname);
  if (ext === '.mjs') {
    return new StandardModuleRequest(url);
  }

  return new CJSModuleRequest(url);
};
module.exports = resolveRequestUrl;
