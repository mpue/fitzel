add_executable(sandbox src/main.cpp)

target_link_libraries(sandbox PRIVATE fitzel)

# Copy shader assets next to the executable so relative paths resolve at runtime.
add_custom_command(TARGET sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:sandbox>/assets
    COMMENT "Copying sandbox assets")
