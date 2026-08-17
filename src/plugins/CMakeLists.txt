# 插件框架（主项目通用，无 Q_OBJECT 头不涉及 AUTOMOC 冲突）
set(PLUGIN_SOURCES
    plugin_manager.cpp
    plugin_tool_panel.cpp)
add_library(phone_plugins STATIC ${PLUGIN_SOURCES})
target_include_directories(phone_plugins PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(phone_plugins PUBLIC Qt6::Core Qt6::Widgets)
set_target_properties(phone_plugins PROPERTIES AUTOMOC ON)
