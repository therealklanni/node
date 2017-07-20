'use strict';

const URL = require('url').URL;

const ModuleWrap = process.binding('module_wrap').ModuleWrap;

const ModuleMap = require('internal/loader/ModuleMap');
const ModuleJob = require('internal/loader/ModuleJob');
const resolveRequestUrl = require('internal/loader/resolveRequestUrl');
const {Error} = global;

const pathToFileURL = (pathname) => `file://${pathname}/`;

const getNamespaceOf = (m) => {
  const tmp = new ModuleWrap('import * as _ from "";_;', '');
  tmp.link(async () => m);
  tmp.instantiate();
  return tmp.evaluate();
};

class Loader {
  constructor(
    base = pathToFileURL(process.cwd())
  ) {
    this.moduleMap = new ModuleMap();
    this.base = new URL(`${base}`);
  }

  async resolve(specifier) {
    const request = resolveRequestUrl(this.base, specifier);
    if (request.url.protocol !== 'file:') {
      throw new Error('Expected to resolve a file: url');
    }
    return request.url;
  }

  async getModuleJob(dependentJob, specifier) {
    if (!this.moduleMap.has(dependentJob.url)) {
      throw new Error(`unknown dependent ModuleJob ${dependentJob.url}`);
    }
    const request = await resolveRequestUrl(dependentJob.url, specifier);
    const url = `${request.url}`;
    if (this.moduleMap.has(url)) {
      return this.moduleMap.get(url);
    }
    const dependencyJob = new ModuleJob(this, request, url);
    this.moduleMap.set(url, dependencyJob);
    return dependencyJob;
  }

  async import(specifier) {
    const request = await resolveRequestUrl(this.base, specifier);
    const url = `${request.url}`;
    let job;
    if (this.moduleMap.has(url)) {
      job = this.moduleMap.get(url);
    } else {
      job = new ModuleJob(this, request, url);
      this.moduleMap.set(url, job);
    }
    const module = await job.run();
    return getNamespaceOf(module);
  }
}
Object.setPrototypeOf(Loader.prototype, Object.create(null));
module.exports = Loader;
