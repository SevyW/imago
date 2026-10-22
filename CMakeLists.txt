cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Imago C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Imago")
set(VITA_TITLEID  "IMAG00001")
set(VITA_VERSION  "01.20")

option(IMAGO_AUTOPILOT "Replay scripted input from ux0:data/Imago/autopilot.txt (testing only)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation")

# SQLite talks to the memory card through our own VFS (src/vita_vfs.c).
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_options(sqlite3 PRIVATE -w -Os)
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_OS_OTHER=1
  SQLITE_THREADSAFE=0
  SQLITE_TEMP_STORE=3
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_WAL
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_DEPRECATED
  SQLITE_DQS=0
)

add_executable(${PROJECT_NAME}
  src/main.c
  src/assets.c
  src/at9.c
  src/music.c
  src/screen_assets.c
  src/config.c
  src/dialog.c
  src/folders.c
  src/fs.c
  src/images.c
  src/ime.c
  src/input.c
  src/jobs.c
  src/library.c
  src/log.c
  src/net.c
  src/psvt.c
  src/screen_detail.c
  src/screen_discover.c
  src/screen_library.c
  src/screen_settings.c
  src/screen_studio.c
  src/screen_apps.c
  src/screen_appdetail.c
  src/screen_editor.c
  src/store.c
  src/appdb.c
  src/apps.c
  src/livearea.c
  src/edit.c
  src/quant.c
  src/sfo.c
  src/tasks.c
  src/theme.c
  src/themedoc.c
  src/xmltree.c
  src/imageio.c
  src/jpegio.c
  src/photos.c
  src/pictures.c
  src/picture_picker.c
  src/themedb.c
  src/dbtext.c
  src/ui.c
  src/unzip.c
  src/vita_vfs.c
)

# vita-elf-create appends the module's import tables (about 4 KB) right after
# the code segment; if the data segment starts closer than that, the tool
# corrupts its own memory and crashes, depending on the code size. Reserve
# headroom with the hook the toolchain's linker script provides.
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--defsym,__sce_headroom=0x4000)

target_link_libraries(${PROJECT_NAME}
  sqlite3
  vita2d
  curl
  ssl
  crypto
  freetype
  png16
  jpeg
  zstd
  bz2
  z
  m
  pthread
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudiodec_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceIme_stub
  SceIofilemgr_stub
  SceLibKernel_stub
  SceNet_stub
  SceNetCtl_stub
  ScePhotoExport_stub
  ScePgf_stub
  ScePower_stub
  SceProcessmgr_stub
  SceRtc_stub
  SceShellSvc_stub
  SceSysmodule_stub
  SceTouch_stub
)

if(IMAGO_AUTOPILOT)
  target_compile_definitions(${PROJECT_NAME} PRIVATE IMAGO_AUTOPILOT)
endif()

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
# Packaged by hand: vita_create_vpk() does not quote its file arguments, which
# breaks when the project lives in a path containing spaces.
set(VPK_FILES
  sce_sys/icon0.png
  sce_sys/livearea/contents/bg.png
  sce_sys/livearea/contents/startup.png
  sce_sys/livearea/contents/template.xml
  assets/Inter-Regular.ttf
  assets/Inter-SemiBold.ttf
  assets/Inter-LICENSE.txt
  assets/logo.png
  assets/splash.jpg
)
set(VPK_ARGS)
set(VPK_DEPENDS)
foreach(file ${VPK_FILES})
  list(APPEND VPK_ARGS -a "${CMAKE_CURRENT_SOURCE_DIR}/${file}=${file}")
  list(APPEND VPK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
endforeach()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/param.sfo
  COMMAND vita-mksfoex -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID}
          ${VITA_APP_NAME} ${CMAKE_CURRENT_BINARY_DIR}/param.sfo
  VERBATIM
)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
  COMMAND vita-pack-vpk -s ${CMAKE_CURRENT_BINARY_DIR}/param.sfo -b ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin
          ${VPK_ARGS} ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin ${CMAKE_CURRENT_BINARY_DIR}/param.sfo ${VPK_DEPENDS}
  VERBATIM
)
add_custom_target(vpk ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk)
add_dependencies(vpk eboot.bin-self)
