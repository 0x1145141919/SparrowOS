find_package(nlohmann_json REQUIRED)

add_executable(initramfs-builder
    main.cpp
    builder.cpp
)

target_include_directories(initramfs-builder PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
)

target_link_libraries(initramfs-builder PRIVATE
    nlohmann_json::nlohmann_json
)
