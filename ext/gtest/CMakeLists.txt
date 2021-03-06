set(project_name googletest)

set(target_url ${GTEST_GIT_SOURCE})
set(git_tag ${GTEST_GIT_TAG})

set(my_cmake_args "-Dgtest_force_shared_crt=OFF;")

if(WIN32)
    set(target_binaries gtest.lib gtest_main.lib)
else()
    set(target_binaries libgtest.a libgtest_main.a)
endif()

include(${PROJECT_SOURCE_DIR}/bin/dsn.ext.cmake)

install(DIRECTORY ${my_source_dir}/include/gtest
    DESTINATION include/ext
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.tcc" PATTERN "*.cpp"
)

set(GTEST_INCLUDE_DIR ${my_source_dir}/include PARENT_SCOPE)
set(GTEST_LIB_DIR ${my_binary_dir} PARENT_SCOPE)
