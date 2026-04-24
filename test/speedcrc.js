import { createRequire } from 'node:module'

import { parseArgs, sleep, bufAvg, rounds, run } from './_speedbase'

const require = createRequire(import.meta.url)
const y = require('../build/Release/yencode')

parseArgs('Syntax: node test/speedcrc [{-s|--sleep}=msecs(0)]')

// warmup
if (!sleep) {
  bufAvg.forEach(function (buf, i) {
    const p = process.hrtime()
    for (var i = 0; i < rounds; i += 2) y.crc32(buf)
    const t = process.hrtime(p)
  })
}

setTimeout(function () {
  bufAvg.forEach(function (buf, i) {
    run('Random (' + i + ')', y.crc32.bind(null, buf))
  })
}, sleep)
