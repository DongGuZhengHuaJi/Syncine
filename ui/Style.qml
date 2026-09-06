pragma Singleton

import QtQuick

QtObject {
    readonly property color bg: "#F4F6FB"
    readonly property color card: "#FFFFFF"
    readonly property color border: "#E5EAF2"
    readonly property color fieldBg: "#F7F9FC"
    readonly property color fieldHover: "#F0F4F9"

    readonly property color textPrimary: "#1B2434"
    readonly property color textSecondary: "#6B7890"
    readonly property color textOnAccent: "#FFFFFF"

    readonly property color accent: "#0FA3B1"
    readonly property color accentPressed: "#0C8B97"
    readonly property color accentSoft: "#E4F5F6"

    readonly property color danger: "#E5484D"
    readonly property color success: "#2ECF8A"

    readonly property int radius: 18
    readonly property int radiusSmall: 12
    readonly property int cardPadding: 28
    readonly property int pageWidth: 420
    readonly property int inputHeight: 46
}
