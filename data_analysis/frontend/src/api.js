const base = import.meta.env.VITE_API_BASE || ''
export async function getJson(path, params = {}) {
  const query = new URLSearchParams(Object.entries(params).filter(([, v]) => v !== '' && v != null))
  const response = await fetch(`${base}${path}?${query}`, { signal: AbortSignal.timeout(2500) })
  const body = await response.json()
  if (!response.ok || body.code !== 'OK') throw new Error(body.message || `HTTP ${response.status}`)
  return body
}
