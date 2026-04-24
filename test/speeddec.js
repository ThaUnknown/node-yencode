import { createRequire } from 'node:module'

import _ from './_speedbase'

const require = createRequire(import.meta.url)
const y = require('../build/Release/yencode')
const allocBuffer = (Buffer.allocUnsafe || Buffer)

const bufSize = _.bufTarget.length

const mWorst = allocBuffer(bufSize)
const mAvg = _.bufAvg.map(function () {
  return allocBuffer(bufSize)
})
const mAvg2x = _.bufAvg2x.map(function () {
  return allocBuffer(bufSize)
})
const mBest = allocBuffer(bufSize)
const mBest2 = allocBuffer(_.size)
mBest2.fill(32)

const lenWorst = y.encodeTo(_.bufWorst, mWorst)
const lenBest = y.encodeTo(_.bufBest, mBest)
const lenAvg = Array(_.bufAvg.length)
const lenAvg2x = Array(_.bufAvg2x.length)
_.bufAvg.forEach(function (buf, i) {
  lenAvg[i] = y.encodeTo(buf, mAvg[i])
})
_.bufAvg2x.forEach(function (buf, i) {
  lenAvg2x[i] = y.encodeTo(buf, mAvg2x[i])
})

_.parseArgs('Syntax: node test/speeddec [-a|--average-only] [{-s|--sleep}=msecs(0)] [{-m|--methods}=clean,raw,rawincr]')

console.log('    Test                     Output rate         Read rate      Output + CRC   ')

// warmup
if (!_.sleep) {
  mAvg.forEach(function (buf) {
    const p = process.hrtime()
    for (var j = 0; j < _.rounds; j += 2) y.decodeTo(buf, _.bufTarget)
    for (var j = 0; j < _.rounds; j += 2) y.decodeTo(buf, _.bufTarget, true)
    for (var j = 0; j < _.rounds; j += 2) y.decodeIncr(buf, 0, _.bufTarget)
    const t = process.hrtime(p)
  })
}

const decCrc = function (src, raw) {
  const len = y.decodeTo(src, _.bufTarget, raw)
  y.crc32(_.bufTarget.slice(0, len))
}
const decIncrCrc = function (src) {
  const out = y.decodeIncr(src, 0, _.bufTarget)
  y.crc32(_.bufTarget.slice(0, out.written))
}

setTimeout(function () {
  if (!_.avgOnly) {
    if (_.decMethods.clean) {
      _.run('Clean worst (all escaping)', y.decodeTo.bind(null, mWorst, _.bufTarget), lenWorst, decCrc.bind(null, mWorst))
      _.run('Clean best (min escaping)', y.decodeTo.bind(null, mBest, _.bufTarget), lenBest, decCrc.bind(null, mBest))
      _.run('Clean pass (no escaping)', y.decodeTo.bind(null, mBest2, _.bufTarget), null, decCrc.bind(null, mBest2))
    }
    if (_.decMethods.raw) {
      _.run('Raw worst', y.decodeTo.bind(null, mWorst, _.bufTarget, true), lenWorst, decCrc.bind(null, mWorst, true))
      _.run('Raw best', y.decodeTo.bind(null, mBest, _.bufTarget, true), lenBest, decCrc.bind(null, mBest, true))
      _.run('Raw pass', y.decodeTo.bind(null, mBest2, _.bufTarget, true), null, decCrc.bind(null, mBest2, true))
    }
    if (_.decMethods.rawincr) {
      _.run('Raw-incr worst', y.decodeIncr.bind(null, mWorst, 0, _.bufTarget), lenWorst, decIncrCrc.bind(null, mWorst))
      _.run('Raw-incr best', y.decodeIncr.bind(null, mBest, 0, _.bufTarget), lenBest, decIncrCrc.bind(null, mBest))
      _.run('Raw-incr pass', y.decodeIncr.bind(null, mBest2, 0, _.bufTarget), null, decIncrCrc.bind(null, mBest2))
    }
  }

  if (_.decMethods.clean) {
    mAvg.forEach(function (buf, i) {
      _.run('Clean random (' + i + ')', y.decodeTo.bind(null, buf, _.bufTarget), lenAvg[i], decCrc.bind(null, buf))
    })
  }
  if (_.decMethods.raw) {
    mAvg.forEach(function (buf, i) {
      _.run('Raw random (' + i + ')', y.decodeTo.bind(null, buf, _.bufTarget, true), lenAvg[i], decCrc.bind(null, buf, true))
    })
  }
  if (_.decMethods.rawincr) {
    mAvg.forEach(function (buf, i) {
      _.run('Raw-incr random (' + i + ')', y.decodeIncr.bind(null, buf, 0, _.bufTarget), lenAvg[i], decIncrCrc.bind(null, buf))
    })
  }

  if (!_.avgOnly) {
    if (_.decMethods.clean) {
      mAvg2x.forEach(function (buf, i) {
        _.run('Clean random 2xEsc (' + i + ')', y.decodeTo.bind(null, buf, _.bufTarget), lenAvg2x[i], decCrc.bind(null, buf))
      })
    }
    if (_.decMethods.raw) {
      mAvg2x.forEach(function (buf, i) {
        _.run('Raw random 2xEsc (' + i + ')', y.decodeTo.bind(null, buf, _.bufTarget, true), lenAvg2x[i], decCrc.bind(null, buf, true))
      })
    }
    if (_.decMethods.rawincr) {
      mAvg2x.forEach(function (buf, i) {
        _.run('Raw-incr random 2xEsc (' + i + ')', y.decodeIncr.bind(null, buf, 0, _.bufTarget), lenAvg2x[i], decIncrCrc.bind(null, buf))
      })
    }
  }
}, _.sleep)
