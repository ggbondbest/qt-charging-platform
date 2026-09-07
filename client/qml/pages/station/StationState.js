// station 域跨页会话状态库（.pragma library：同引擎内所有 import 共享一份实例，
// 页面 pop/push 不丢）。桥缺位期的本地通道：二级密码（哈希存储，明文即散即用）、
// 保护开关、车辆 CRUD。桥落地后各页优先读服务真值，本库自动退位。
// 注意：Qt 引擎销毁（进程退出）后清空——真持久化归 SettingsService（QSettings/SQLite）。
.pragma library

// cyrb53 双 32 位混合散列（演示级：仅保证 UI 不落明文，真实散列归服务层 SHA-256）。
function hash(s) {
    let h1 = 0xdeadbeef, h2 = 0x41c6ce57
    for (let i = 0; i < s.length; ++i) {
        const ch = s.charCodeAt(i)
        h1 = Math.imul(h1 ^ ch, 2654435761)
        h2 = Math.imul(h2 ^ ch, 1597334677)
    }
    h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909)
    h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909)
    return String(4294967296 * (2097151 & h2) + (h1 >>> 0))
}

var passHash = ""
var protectionOn = false

function setSecondPassword(plain) { passHash = hash(plain) }
function hasSecondPassword() { return passHash.length > 0 }
function verifySecondPassword(plain) { return hasSecondPassword() && hash(plain) === passHash }
function setProtectionEnabled(on) { protectionOn = !!on }
function protectionEnabled() { return protectionOn && passHash.length > 0 }

// ---- 车辆（桥缺位本地通道；服务 vehicles() 可读时以服务为准） ----
var vehicles = []
var nextVehicleId = 1000

function addVehicle(v) {
    const nv = Object.assign({}, v, { id: nextVehicleId++ })
    if (vehicles.length === 0) nv.isDefault = true
    vehicles.push(nv)
    return nv
}
function updateVehicle(v) {
    for (let i = 0; i < vehicles.length; ++i) {
        if (vehicles[i].id === v.id) {
            vehicles[i] = Object.assign({}, vehicles[i], v)
            return
        }
    }
}
function removeVehicle(id) {
    const wasDefault = defaultVehicle() !== null && defaultVehicle().id === id
    vehicles = vehicles.filter(function (v) { return v.id !== id })
    if (wasDefault && vehicles.length > 0) vehicles[0].isDefault = true
}
function setDefaultVehicle(id) {
    for (const v of vehicles) v.isDefault = (v.id === id)
}
function defaultVehicle() {
    for (const v of vehicles) if (v.isDefault) return v
    return null
}
