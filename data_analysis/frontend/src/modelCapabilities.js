export const modelCapabilities = [
  { key:'load', name:'负荷功率预测', endpoint:'/api/v1/predict/load', state:'integrating', output:'kW · 1/6/24h', model:'hgb-q50-history24-v1', description:'逐小时负荷点预测，区间带为独立分析工件。' },
  { key:'availability', name:'空闲桩数预测', endpoint:'/api/v1/predict/availability', state:'pendingMerge', output:'chargers · 1/6/24h', model:'AvailabilityForecaster', description:'整数空闲桩预测，并提供无桩概率与区间风险侧信道。' },
  { key:'anomaly', name:'充电异常筛查', endpoint:null, proposed:'/api/v1/models/anomaly/screen', state:'contract', output:'session risk batch', model:'context-baseline-rankfuse-v4', description:'会话级离线批筛；当前不能描述为实时告警。' },
  { key:'churn', name:'用户流失风险', endpoint:null, proposed:'/api/v1/models/churn/score', state:'contract', output:'risk score · 14d', model:'gbdt-churn-user-v2', description:'预测未来 14 天未再次发起充电的风险。' },
  { key:'recommend', name:'站点智能推荐', endpoint:null, proposed:'/api/v1/models/stations/recommend', state:'contract', output:'in-city Top 3', model:'gbdt-rank-incity-v1', description:'同城 5 站个性化排序；需新增 Top-3 与理由字段契约。' }
]
export async function invokeContractedModel(key, payload) {
  const capability = modelCapabilities.find(item => item.key === key)
  if (!capability?.endpoint) throw new Error('CONTRACT_NOT_PUBLISHED')
  const response = await fetch(capability.endpoint, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload) })
  const body = await response.json()
  if (!response.ok || body.code !== 'OK') throw new Error(body.code || body.message)
  return body
}
