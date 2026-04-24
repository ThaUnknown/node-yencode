module.exports = (() => {
  try {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    require.addon = require('require-addon')

    return require.addon('.', __filename)
  } catch (err) {
    console.warn('yencode not supported in this environment', err)
    return {}
  }
})()

