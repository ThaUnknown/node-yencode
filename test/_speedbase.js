// test config (defaults)
import { createCipheriv as cipher } from 'crypto'

import maxSize from './_maxsize'

let sz = 768000
let rounds = 80
let trials = 8
const asyncWait = 1000
const allocBuffer = (Buffer.allocUnsafe || Buffer)
const decimal = ('' + 1.1).substring(1, 2)
const fmtSpeed = function (size, time) {
  const rate = ('' + (Math.round(100 * (size / 1048576) / time) / 100)).split(decimal)

  return ('        ' + rate[0]).slice(-8) + decimal + ((rate[1] | 0) + '00').substring(0, 2) + ' MiB/s'
}
const initBuffers = function () {
  module.exports.bufWorst = allocBuffer(sz)
  module.exports.bufBest = allocBuffer(sz)
  module.exports.bufAvg = []
  module.exports.bufAvg2x = []
  module.exports.bufTarget = allocBuffer(maxSize(sz))

  module.exports.bufWorst.fill(224)
  module.exports.bufBest.fill(0)

  // use cipher as a fast, consistent RNG
  ;[['aes-128-cbc', 'my_incredible_pw', '                '],
    ['aes-128-cbc', 'nfa807g2lablzxk1', '                '],
    ['aes-128-cbc', '3h89sdg923jnkbas', '                ']
  ].forEach(function (cargs) {
    const rand = cipher.apply(null, cargs)
    const data = Buffer.concat([rand.update(module.exports.bufBest), rand.final()]).slice(0, sz)
    module.exports.bufAvg.push(Buffer.concat([data]))

    // all yEnc special characters exist in range 0-61 (post shift) or 214-19 (pre-shift)
    // to generate biased data, we'll pack the range down (64-191 will get packed to 192-63)
    for (let i = 0; i < data.length; i++) {
      if (data[i] >= 64 && data[i] < 192) { data[i] = (data[i] + 128) & 0xff }
    }
    module.exports.bufAvg2x.push(data)
  })
}
export default {
  size: sz,
  rounds,
  sleep: 0,
  avgOnly: false,
  decMethods: { clean: true, raw: true, incr: true, rawincr: true },

  bufWorst: null,
  bufBest: null,
  bufAvg: null,
  bufAvg2x: null,
  bufTarget: null,

  bench: function (fn) {
    const times = Array(trials)
    for (let trial = 0; trial < trials; trial++) {
      const p = process.hrtime()
      for (let i = 0; i < rounds; i++) fn()
      const t = process.hrtime(p)

      times[trial] = t[0] + t[1] / 1000000000
    }
    // pick fastest time to try to avoid issues with clockspeed throttling
    return Math.min.apply(null, times)
  },
  _benchAsync: function (fn, cb, trials, results) {
    const p = process.hrtime()
    for (let i = 0; i < rounds; i++) fn()
    results.push(process.hrtime(p))

    if (--trials) { setTimeout(module.exports._benchAsync.bind(null, fn, cb, trials, results), asyncWait) } else { cb(Math.min.apply(null, results)) }
  },
  benchAsync: function (fn, cb) {
    setTimeout(function () {
      module.exports._benchAsync(fn, cb, trials, [])
    }, asyncWait)
  },
  run: function (name, fn, sz2, fn2) {
    const time = module.exports.bench(fn)
    const time2 = fn2 ? module.exports.bench(fn2) : null
    console.log(
      (name + '                         ').substring(0, 25) + ':' +
      fmtSpeed(sz * rounds, time) +
      ' ' + (sz2 ? fmtSpeed(sz2 * rounds, time) : '                 ') +
      ' ' + (fn2 ? fmtSpeed(sz * rounds, time2) : '                 ')
    )
  },

  parseArgs: function (helpText) {
    process.argv.forEach(function (arg) {
      arg = arg.toLowerCase()
      if (arg == '-h' || arg == '--help' || arg == '-?') {
        console.log(helpText + ' [{-z|--size}=bytes(' + sz + ')] [{-r|--rounds}=num(' + rounds + ')] [{-t|--trials}=num(' + trials + ')]')
        process.exit(0)
      }
      if (arg == '-a' || arg == '--average-only') {
        module.exports.avgOnly = true
      }

      let m = arg.match(/^(-s=?|--sleep=)(\d+)$/)
      if (m) { module.exports.sleep = m[2] | 0 }

      m = arg.match(/^(-m=?|--methods=)([a-z,]+)$/)
      if (m) {
        const methods = module.exports.decMethods
        for (const k in methods) { methods[k] = false }
        let setAMethod = false
        m[2].split(',').forEach(function (meth) {
          if (meth in methods) {
            setAMethod = true
            methods[meth] = true
          }
        })
        if (!setAMethod) {
          console.log('No valid method specified')
          process.exit(1)
        }
      }

      m = arg.match(/^(-t=?|--trials=)(\d+)$/)
      if (m) { trials = m[2] | 0 }

      m = arg.match(/^(-r=?|--rounds=)(\d+)$/)
      if (m) {
        rounds = m[2] | 0
        module.exports.rounds = rounds
      }

      m = arg.match(/^(-z=?|--size=)(\d+)$/)
      if (m) {
        sz = m[2] | 0
        module.exports.size = sz
        initBuffers()
      }
    })
  }

}

initBuffers()
