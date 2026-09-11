// station 域跨页会话状态库（.pragma library：同引擎内所有 import 共享一份实例，
// 页面 pop/push 不丢）。桥缺位期的本地通道：二级密码（哈希存储、绑定手机号，
// 校验发生在登录环节——用户二轮指定口径）、保护开关、车辆 CRUD。
// 桥落地后各页优先读服务真值，本库自动退位。
// 注意：Qt 引擎销毁（进程退出）后清空——真持久化归 SettingsService（QSettings/SQLite）。
// 消费方：LoginPage（二验口令校验）、SettingsPage（口令/保护/车辆编辑）、
// ScanPage（桥缺位期读车辆数兜底）。
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
    // 高位链只取 21 位再乘 2^32：保证结果 ≤2^53——JS Number 超过该值即丢精度，
    // 散列会变"不稳定"（同口令两次算出不同串），校验就过不了。
    return String(4294967296 * (2097151 & h2) + (h1 >>> 0))
}

// ---- 二级口令 / 保护开关（状态量本身即"账户级"单例，不分用户） ----
var passHash = ""
var passPhone = ""       // 设置密码时绑定的手机号；登录页仅对该号码要求验证
var protectionOn = false
var lastLoginPhone = ""  // mock 通道最近一次登录的手机号（设置页绑定时取用）

// 登录页 mock 通道成功时回写：设置页绑定口令要取"账户手机号"，但登录态本身
// 不落库（归桥），这里只留最近一次号做跨页传值的桥。
function noteLoginPhone(phone) { if (phone) lastLoginPhone = phone }
function accountPhone() {
    // 优先当前会话登录号（App 未暴露时退最近登录号）。
    return lastLoginPhone
}
function setSecondPassword(plain, phone) { passHash = hash(plain); passPhone = phone || "" }
function hasSecondPassword() { return passHash.length > 0 }
function verifySecondPassword(plain) { return hasSecondPassword() && hash(plain) === passHash }
// 开关生效态与"已设口令"取与：只开开关没口令时，登录二验门会永远空转
// （needsSecondPassword 无从比对），故保护必须成对成立。
function setProtectionEnabled(on) { protectionOn = !!on }
function protectionEnabled() { return protectionOn && passHash.length > 0 }
// 登录环节判定：保护开启 + 已设密码 + 输入手机号 === 设密码时绑定的号码。
function needsSecondPassword(phone) {
    return protectionEnabled() && passPhone.length > 0 && phone === passPhone
}

// ---- 车辆（桥缺位本地通道；服务 vehicles() 可读时以服务为准） ----
var vehicles = []
// 本地自增号从 1000 起：与服务端真实小整数车辆 id 段位错开，双通道并存时好排查。
var nextVehicleId = 1000

// 先拷贝再补 id：页面表单对象与库内对象不共享引用，避免"表单还在改、列表已生效"。
function addVehicle(v) {
    const nv = Object.assign({}, v, { id: nextVehicleId++ })
    if (vehicles.length === 0) nv.isDefault = true
    vehicles.push(nv)
    return nv
}
// 整体覆盖式合并（只并传入字段）：编辑表单可不带 isDefault，不冲掉默认标记。
function updateVehicle(v) {
    for (let i = 0; i < vehicles.length; ++i) {
        if (vehicles[i].id === v.id) {
            vehicles[i] = Object.assign({}, vehicles[i], v)
            return
        }
    }
}
// 删掉默认车时把队首车辆顶上：维持"列表非空则 defaultVehicle() 必有所指"的
// 不变式，消费方（预约/扫码）删车后不会拿到空值。
function removeVehicle(id) {
    const wasDefault = defaultVehicle() !== null && defaultVehicle().id === id
    vehicles = vehicles.filter(function (v) { return v.id !== id })
    if (wasDefault && vehicles.length > 0) vehicles[0].isDefault = true
}
// 整表刷布尔而非"先清后设"：一遍循环即保证互斥，库里永不会同时存在两辆默认车。
function setDefaultVehicle(id) {
    for (const v of vehicles) v.isDefault = (v.id === id)
}
function defaultVehicle() {
    for (const v of vehicles) if (v.isDefault) return v
    return null
}
