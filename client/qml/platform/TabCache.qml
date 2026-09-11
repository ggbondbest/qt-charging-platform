pragma Singleton
import QtQuick

// Cross-instance cache for tab pages (stale-while-revisit).
// Tab switches replace StackView items, so a page would otherwise re-boot
// empty and flash its 空态 until the service round-trip lands (2026-09-07
// user report: "先出现无订单再突然变"). Pages render from this cache first,
// then update silently when fresh data arrives — approximating the widgets
// QStackedWidget feel where tab pages were resident.
QtObject {
    // { chargingCount, waitingCount, activeOrder } — shape owned by its page.
    property var charging: null
}
