module.exports = (() => {
  try {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    return require('node-gyp-build')(__dirname)
  } catch (err) {
    console.warn('yencode not supported in this environment', err)
    return {}
  }
})()

