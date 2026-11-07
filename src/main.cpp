{
    "geode": "5.11.2",
    "gd": {
        "win": "2.2081",
        "android": "2.2081",
        "mac": "2.2081",
        "ios": "2.2081"
    },
    "id": "roper.afc",
    "name": "Auto Frame Counter",
    "version": "v1.0.0-alpha.1",
    "developer": "Roper",
    "description": "Auto counter frame window with NanGD style visual & sound effects.",
    "settings": {
        "enable-mod": {
            "name": "Enable Mod",
            "description": "Bat hoac tat toan bo chuc nang cua mod.",
            "type": "bool",
            "default": true
        },
        "toggle-key": {
            "name": "PC Toggle Keybind",
            "description": "Phim tat tren PC de Bat/Tat nhanh mod khi dang choi.",
            "type": "string",
            "default": "C",
            "one-of": ["C", "F", "V", "X", "Z", "Tab", "F1", "F2"]
        },
        "reset-key": {
            "name": "PC Reset Counter Keybind",
            "description": "Phim tat tren PC de reset bo dem Frame.",
            "type": "string",
            "default": "R",
            "one-of": ["R", "T", "G", "B", "F3", "F4"]
        },
        "target-fps": {
            "name": "Reference FPS",
            "description": "Moc FPS dung de tinh do hep cua Frame Window (VD: 60, 120, 240).",
            "type": "int",
            "default": 240,
            "min": 60,
            "max": 1000
        },
        "max-frame-display": {
            "name": "Max Frame Threshold",
            "description": "Chi hien thi cac pha nhay co cua so hep hon hoac bang so frame nay.",
            "type": "int",
            "default": 6,
            "min": 1,
            "max": 15
        },
        "show-world-text": {
            "name": "NanGD In-Level Popup",
            "description": "Hien chu so Frame co mau ngay tai vi tri nhan vat nhay tren man choi.",
            "type": "bool",
            "default": true
        },
        "enable-sound": {
            "name": "NanGD Hit Sound",
            "description": "Phat am thanh khi qua mot Frame Window.",
            "type": "bool",
            "default": true
        },
        "sound-volume": {
            "name": "Sound Volume",
            "description": "Am luong hieu ung am thanh (0.1 - 1.0).",
            "type": "float",
            "default": 0.7,
            "min": 0.0,
            "max": 1.0
        }
    }
}
