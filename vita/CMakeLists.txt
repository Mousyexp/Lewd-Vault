# Lewd Vault for the PS Vita (see README.md). Build with VitaSDK:
#   cmake -S vita -B vita/build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && cmake --build vita/build
# which makes vita/build/LewdVault.vpk.
cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(LewdVault C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Lewd Vault")
# (the Title ID is where the Vita keeps the app and its data: never change it, or a new install
# loses its settings and thumbnail cache)
set(VITA_TITLEID "LWDV00001")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_C_STANDARD 11)
# Tuned for the Vita's Cortex-A9 (docs: vita/README.md, "Speed"): -O3 for the work, NEON, and the
# linker drops every function and variable nothing uses (smaller code, more of it in the I-cache).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -ffunction-sections -fdata-sections -Wall -Wno-unused-parameter -D_GNU_SOURCE")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# (src/web.c, lib.c, show.c and drive_ui.c are compiled as part of main.c)
add_executable(${PROJECT_NAME} src/main.c src/vault.c src/lvcrypt.c src/image.c src/video.c src/booru.c src/json.c src/net.c src/sfx.c
  src/drive.c src/third_party/qrcodegen.c)
# Google Drive: a build can carry a "TVs and Limited Input devices" key of the PC's Google Cloud project
# (CI secrets; never in the repository). Without one, it's typed in once on the Vita.
if(NOT "$ENV{LEWD_VAULT_GOOGLE_TV_CLIENT_ID}" STREQUAL "" AND NOT "$ENV{LEWD_VAULT_GOOGLE_TV_CLIENT_SECRET}" STREQUAL "")
  target_compile_definitions(${PROJECT_NAME} PRIVATE
    LV_GOOGLE_TV_ID="$ENV{LEWD_VAULT_GOOGLE_TV_CLIENT_ID}" LV_GOOGLE_TV_SECRET="$ENV{LEWD_VAULT_GOOGLE_TV_CLIENT_SECRET}")
endif()
target_link_libraries(${PROJECT_NAME}
  vita2d freetype bz2 png turbojpeg webp sharpyuv
  avformat avcodec swresample swscale avutil vpx mp3lame mpg123 opus vorbis ogg
  curl ssl crypto zstd pthread z m
  SceAvPlayer_stub SceAudio_stub SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCtrl_stub
  SceTouch_stub ScePower_stub SceAppUtil_stub SceCommonDialog_stub SceIme_stub ScePgf_stub
  SceLibKernel_stub SceKernelThreadMgr_stub SceRtc_stub SceAppMgr_stub SceLiveArea_stub
  SceNet_stub SceNetCtl_stub SceKernelModulemgr_stub SceIofilemgr_stub SceSysmem_stub SceProcessmgr_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE sce_sys/livearea/contents/drive.png sce_sys/livearea/contents/drive.png
  FILE sce_sys/livearea/contents/local.png sce_sys/livearea/contents/local.png
  FILE sce_sys/livearea/contents/strip.png sce_sys/livearea/contents/strip.png
  FILE font.ttf font.ttf
  FILE cacert.pem cacert.pem
  FILE splash.png splash.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sounds/tick.wav sounds/tick.wav
  FILE sounds/select.wav sounds/select.wav
  FILE sounds/back.wav sounds/back.wav
  FILE sounds/toggle.wav sounds/toggle.wav
  FILE sounds/open.wav sounds/open.wav
  FILE sounds/close.wav sounds/close.wav
  FILE sounds/tab.wav sounds/tab.wav
  FILE sounds/error.wav sounds/error.wav
  FILE sounds/saved.wav sounds/saved.wav
  FILE sounds/done.wav sounds/done.wav
  FILE sounds/delete.wav sounds/delete.wav
  FILE sounds/slideshow.wav sounds/slideshow.wav
  FILE sounds/more.wav sounds/more.wav
  FILE sounds/start.wav sounds/start.wav
  FILE sites/danbooru.png sites/danbooru.png
  FILE sites/yandere.png sites/yandere.png
  FILE sites/konachan.png sites/konachan.png
  FILE sites/e621.png sites/e621.png
  FILE sites/xbooru.png sites/xbooru.png
  FILE sites/tbib.png sites/tbib.png
  FILE sites/hypnohub.png sites/hypnohub.png
  FILE sites/aibooru.png sites/aibooru.png
  FILE sites/safebooru.png sites/safebooru.png
  FILE sites/e926.png sites/e926.png
  FILE sites/gelbooru.png sites/gelbooru.png
  FILE sites/rule34.png sites/rule34.png
  FILE sites/sakugabooru.png sites/sakugabooru.png
)
