cmake_minimum_required(VERSION 3.14)
project(gpujob)
enable_testing()
include(GNUInstallDirs)

find_package(fmt 8 REQUIRED)
find_package(ftxui REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers)

get_property(ImportedTargets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY IMPORTED_TARGETS)
message("Imported targets: ${ImportedTargets}")

add_executable(job src/job.cpp)
target_link_libraries(job PRIVATE fmt::fmt ftxui::screen ftxui::dom ftxui::component Boost::headers)
target_compile_features(job INTERFACE cxx_std_23)
add_executable(jobd src/jobd.cpp)
target_link_libraries(jobd PRIVATE fmt::fmt)
target_compile_features(job INTERFACE cxx_std_23)
