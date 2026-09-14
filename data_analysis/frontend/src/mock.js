export const cities = [
  { id: 'ALL', name: '全国总览' }, { id: 'BJ', name: '北京市' }, { id: 'DL', name: '大连市' },
  { id: 'SH', name: '上海市' }, { id: 'SY', name: '沈阳市' }, { id: 'SZ', name: '深圳市' }
]
export const cityStats = [
  { name: '北京', value: 12680, growth: 8.2 }, { name: '大连', value: 8940, growth: 5.7 },
  { name: '上海', value: 14860, growth: 11.4 }, { name: '沈阳', value: 9760, growth: 6.3 },
  { name: '深圳', value: 15720, growth: 13.1 }
]
export const hours = Array.from({ length: 24 }, (_, i) => `${String(i).padStart(2, '0')}:00`)
export const load = [18,14,12,10,11,17,31,56,82,94,86,74,69,72,78,83,98,121,137,129,105,78,51,30]
export const energy = [16,12,10,9,10,15,28,48,71,82,75,65,61,64,69,73,86,104,118,111,91,68,44,26]
export const week = ['12/01','12/02','12/03','12/04','12/05','12/06','12/07']
export const revenue = [12.8, 14.1, 13.5, 16.2, 17.6, 19.4, 18.7]
export const stationRank = [
  ['深圳·南山科技园站', 96, 4286], ['上海·陆家嘴中心站', 92, 3950], ['北京·中关村站', 89, 3680],
  ['沈阳·浑南新城站', 84, 3190], ['大连·高新园区站', 81, 2975]
]
export const quality = { raw: 203973, clean: 203887, rejected: 86, normalized: 21 }
