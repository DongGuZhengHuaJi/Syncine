cmake_minimum_required(VERSION 3.28)

project(Syncine)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS
        Core
        Gui
        Qml
        Quick
        QuickControls2
        Multimedia
)

qt_standard_project_setup()

qt_add_executable(Syncine
        src/main.cpp
        src/PlaybackController.cpp
        src/PlaybackController.h
)

set_source_files_properties(ui/Style.qml PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(Syncine
        URI SyncineApp
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        QML_FILES
        ui/Main.qml
        ui/Style.qml
        ui/components/Card.qml
        ui/components/PrimaryButton.qml
        ui/components/GhostButton.qml
        ui/components/AppTextField.qml
        ui/components/VideoPlayer.qml
        ui/components/PlaybackControl.qml
        ui/components/PlaybackSeekControl.qml
        ui/pages/HomePage.qml
        ui/pages/CreateRoomPage.qml
        ui/pages/JoinRoomPage.qml
        ui/pages/RoomPage.qml
)

target_link_libraries(Syncine
        PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Multimedia
)