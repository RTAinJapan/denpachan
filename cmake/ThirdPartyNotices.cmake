# 同梱するライブラリのライセンス文を出力フォルダの licenses/ にコピーする (docs/dependencies.md)。
# FFmpeg は同梱しないので含めない。

function(denpachan_copy_licenses)
    set(root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    set(dest "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/licenses")
    set(commands COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}")
    foreach(pkg IN LISTS ARGN)
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${root}/${pkg}/copyright" "${dest}/${pkg}.txt")
    endforeach()
    list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE" "${dest}/denpachan.txt")
    list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/docs/third_party_notices.md"
         "${dest}/README.md")
    add_custom_target(denpachan_licenses ALL ${commands} COMMENT "Copying third-party licenses" VERBATIM)
endfunction()
