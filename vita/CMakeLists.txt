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
# The test kit (src/test.c): a separate app beside this one, "Lewd Vault Test" (LWDT00001), the same
# vault and data folder, a menu of tests first, everything logged, reports in ux0:data/LWDV00001/reports.
#   cmake -S vita -B vita/build-test -DLV_TESTKIT=ON ... && cmake --build vita/build-test   (LewdVaultTest.vpk)
option(LV_TESTKIT "Build the test kit (LewdVaultTest.vpk) instead of the app" OFF)
set(LV_VPK_NAME ${PROJECT_NAME})
if(LV_TESTKIT)
  set(VITA_APP_NAME "Lewd Vault Test")
  set(VITA_TITLEID "LWDT00001")
  set(LV_VPK_NAME "${PROJECT_NAME}Test")
endif()
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_C_STANDARD 11)
# Tuned for the Vita's Cortex-A9 (docs: vita/README.md, "Speed"): -O3 for the work, NEON, and the
# linker drops every function and variable nothing uses (smaller code, more of it in the I-cache).
# (room after the code for the module data vita-elf-create adds: without it, a build whose code
#  happens to end just short of a 64 KiB line fails with "segment 1 overlaps")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=__sce_headroom=0x2000")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -ffunction-sections -fdata-sections -Wall -Wno-unused-parameter -D_GNU_SOURCE")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# (src/web.c, lib.c, show.c and drive_ui.c are compiled as part of main.c)
add_executable(${PROJECT_NAME} src/main.c src/vault.c src/lvcrypt.c src/image.c src/video.c src/booru.c src/json.c src/net.c src/sfx.c
  src/drive.c src/favmerge.c src/manga.c src/discover.c src/conv.c src/third_party/qrcodegen.c)
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
  SceAvPlayer_stub SceVideodec_stub SceAudio_stub SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCtrl_stub
  SceTouch_stub ScePower_stub SceAppUtil_stub SceCommonDialog_stub SceIme_stub ScePgf_stub
  SceLibKernel_stub SceKernelThreadMgr_stub SceRtc_stub SceAppMgr_stub SceLiveArea_stub
  SceShellSvc_stub SceNet_stub SceNetCtl_stub SceKernelModulemgr_stub SceIofilemgr_stub SceSysmem_stub SceProcessmgr_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
set(LV_EXTRA_FILES "")
if(LV_TESTKIT)
  target_compile_definitions(${PROJECT_NAME} PRIVATE LV_DEBUG LV_TESTKIT)
  # (the test kit's sample videos: one of each kind the player reads, for its Video formats test)
  foreach(clip h264.mkv h264.avi mpeg4.avi vp9.webm)
    list(APPEND LV_EXTRA_FILES FILE testclips/${clip} testclips/${clip})
  endforeach()
endif()
# (the LiveArea page, configured as the build's own: kept so a future frame can name its Title ID)
configure_file(sce_sys/livearea/contents/template.xml ${CMAKE_CURRENT_BINARY_DIR}/template.xml @ONLY)
vita_create_vpk(${LV_VPK_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  ${LV_EXTRA_FILES}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_CURRENT_BINARY_DIR}/template.xml sce_sys/livearea/contents/template.xml
  FILE font.ttf font.ttf
  FILE certs.bin certs.bin
  FILE decor/bikini.webp decor/bikini.webp
  FILE decor/corset.webp decor/corset.webp
  FILE decor/croptop.webp decor/croptop.webp
  FILE decor/sonico.webp decor/sonico.webp
  FILE decor/suit.webp decor/suit.webp
  FILE decor/tearju.webp decor/tearju.webp
  FILE decor/yoruichi.webp decor/yoruichi.webp
  FILE decor/peace.webp decor/peace.webp
  FILE splash.png splash.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sounds/tick.wav sounds/tick.wav
  FILE sounds/soft/back.wav sounds/soft/back.wav
  FILE sounds/soft/close.wav sounds/soft/close.wav
  FILE sounds/soft/delete.wav sounds/soft/delete.wav
  FILE sounds/soft/done.wav sounds/soft/done.wav
  FILE sounds/soft/error.wav sounds/soft/error.wav
  FILE sounds/soft/more.wav sounds/soft/more.wav
  FILE sounds/soft/open.wav sounds/soft/open.wav
  FILE sounds/soft/saved.wav sounds/soft/saved.wav
  FILE sounds/soft/select.wav sounds/soft/select.wav
  FILE sounds/soft/slideshow.wav sounds/soft/slideshow.wav
  FILE sounds/soft/start.wav sounds/soft/start.wav
  FILE sounds/soft/tab.wav sounds/soft/tab.wav
  FILE sounds/soft/tick.wav sounds/soft/tick.wav
  FILE sounds/soft/toggle.wav sounds/soft/toggle.wav
  FILE sounds/retro/back.wav sounds/retro/back.wav
  FILE sounds/retro/close.wav sounds/retro/close.wav
  FILE sounds/retro/delete.wav sounds/retro/delete.wav
  FILE sounds/retro/done.wav sounds/retro/done.wav
  FILE sounds/retro/error.wav sounds/retro/error.wav
  FILE sounds/retro/more.wav sounds/retro/more.wav
  FILE sounds/retro/open.wav sounds/retro/open.wav
  FILE sounds/retro/saved.wav sounds/retro/saved.wav
  FILE sounds/retro/select.wav sounds/retro/select.wav
  FILE sounds/retro/slideshow.wav sounds/retro/slideshow.wav
  FILE sounds/retro/start.wav sounds/retro/start.wav
  FILE sounds/retro/tab.wav sounds/retro/tab.wav
  FILE sounds/retro/tick.wav sounds/retro/tick.wav
  FILE sounds/retro/toggle.wav sounds/retro/toggle.wav
  FILE sounds/bubbly/back.wav sounds/bubbly/back.wav
  FILE sounds/bubbly/close.wav sounds/bubbly/close.wav
  FILE sounds/bubbly/delete.wav sounds/bubbly/delete.wav
  FILE sounds/bubbly/done.wav sounds/bubbly/done.wav
  FILE sounds/bubbly/error.wav sounds/bubbly/error.wav
  FILE sounds/bubbly/more.wav sounds/bubbly/more.wav
  FILE sounds/bubbly/open.wav sounds/bubbly/open.wav
  FILE sounds/bubbly/saved.wav sounds/bubbly/saved.wav
  FILE sounds/bubbly/select.wav sounds/bubbly/select.wav
  FILE sounds/bubbly/slideshow.wav sounds/bubbly/slideshow.wav
  FILE sounds/bubbly/start.wav sounds/bubbly/start.wav
  FILE sounds/bubbly/tab.wav sounds/bubbly/tab.wav
  FILE sounds/bubbly/tick.wav sounds/bubbly/tick.wav
  FILE sounds/bubbly/toggle.wav sounds/bubbly/toggle.wav
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
  FILE ui/chev_l.png ui/chev_l.png
  FILE ui/chev_r.png ui/chev_r.png
  FILE ui/refresh.png ui/refresh.png
  FILE ui/all.png ui/all.png
  FILE ui/album.png ui/album.png
  FILE ui/bookmark.png ui/bookmark.png
  FILE ui/direction.png ui/direction.png
  FILE ui/eraser.png ui/eraser.png
  FILE ui/eye.png ui/eye.png
  FILE ui/eye_off.png ui/eye_off.png
  FILE ui/flame.png ui/flame.png
  FILE ui/grid.png ui/grid.png
  FILE ui/history.png ui/history.png
  FILE ui/hourglass.png ui/hourglass.png
  FILE ui/newest.png ui/newest.png
  FILE ui/oldest.png ui/oldest.png
  FILE ui/pages.png ui/pages.png
  FILE ui/rotate.png ui/rotate.png
  FILE ui/save.png ui/save.png
  FILE ui/scroll.png ui/scroll.png
  FILE ui/shuffle.png ui/shuffle.png
  FILE ui/sparkle.png ui/sparkle.png
  FILE ui/stack.png ui/stack.png
  FILE ui/star.png ui/star.png
  FILE ui/swipe.png ui/swipe.png
  FILE ui/tap.png ui/tap.png
  FILE ui/touchpad.png ui/touchpad.png
  FILE ui/zoom.png ui/zoom.png
  FILE ui/tag.png ui/tag.png
  FILE ui/wifi.png ui/wifi.png
  FILE ui/back.png ui/back.png
  FILE ui/book.png ui/book.png
  FILE ui/compass.png ui/compass.png
  FILE ui/compass_fill.png ui/compass_fill.png
  FILE ui/film.png ui/film.png
  FILE ui/gear.png ui/gear.png
  FILE ui/gear_fill.png ui/gear_fill.png
  FILE ui/heart.png ui/heart.png
  FILE ui/heart_fill.png ui/heart_fill.png
  FILE ui/heart_big.png ui/heart_big.png
  FILE ui/image.png ui/image.png
  FILE ui/info.png ui/info.png
  FILE ui/more.png ui/more.png
  FILE ui/photos.png ui/photos.png
  FILE ui/photos_fill.png ui/photos_fill.png
  FILE ui/library.png ui/library.png
  FILE ui/library_fill.png ui/library_fill.png
  FILE ui/stats.png ui/stats.png
  FILE ui/play.png ui/play.png
  FILE ui/play_fill.png ui/play_fill.png
  FILE ui/plus.png ui/plus.png
  FILE ui/search.png ui/search.png
  FILE ui/sliders.png ui/sliders.png
  FILE ui/sort.png ui/sort.png
  FILE ui/xmark.png ui/xmark.png
  FILE ui/check.png ui/check.png
  FILE sites/danbooru.png sites/danbooru.png
  FILE sites/yandere.png sites/yandere.png
  FILE sites/konachan.png sites/konachan.png
  FILE sites/xcomics.png sites/xcomics.png
  FILE sites/xbooru.png sites/xbooru.png
  FILE sites/tbib.png sites/tbib.png
  FILE sites/hypnohub.png sites/hypnohub.png
  FILE sites/rule34video.png sites/rule34video.png
  FILE sites/eshuushuu.png sites/eshuushuu.png
  FILE sites/xvideos.png sites/xvideos.png
  FILE sites/hentaiworld.png sites/hentaiworld.png
  FILE sites/hentaicloud.png sites/hentaicloud.png
  FILE sites/hentaicity.png sites/hentaicity.png
  FILE sites/hentaifox.png sites/hentaifox.png
  FILE sites/asmhentai.png sites/asmhentai.png
  FILE sites/hentaiera.png sites/hentaiera.png
  FILE sites/nhentaixxx.png sites/nhentaixxx.png
  FILE sites/hentaizap.png sites/hentaizap.png
  FILE sites/hentaienvy.png sites/hentaienvy.png
  FILE sites/3hentai.png sites/3hentai.png
  FILE sites/hentaihand.png sites/hentaihand.png
  FILE sites/manhwa18.png sites/manhwa18.png
  FILE sites/hentaiwebtoon.png sites/hentaiwebtoon.png
  FILE sites/mangadistrict.png sites/mangadistrict.png
  FILE sites/multporn.png sites/multporn.png
  FILE sites/freeadultcomix.png sites/freeadultcomix.png
  FILE sites/hentairox.png sites/hentairox.png
  FILE sites/justmanga.png sites/justmanga.png
  FILE sites/pururin.png sites/pururin.png
  FILE sites/hentaimanga.png sites/hentaimanga.png
  FILE sites/manga18fx.png sites/manga18fx.png
  FILE sites/manhwaden.png sites/manhwaden.png
  FILE sites/booruio.png sites/booruio.png
  FILE sites/r34anime.png sites/r34anime.png
  FILE sites/safebooru.png sites/safebooru.png
  FILE sites/dancomics.png sites/dancomics.png
  FILE sites/mangadex.png sites/mangadex.png
  FILE sites/ehentai.png sites/ehentai.png
  FILE sites/gelbooru.png sites/gelbooru.png
  FILE sites/rule34.png sites/rule34.png
  FILE sites/sakugabooru.png sites/sakugabooru.png
  FILE sites/hanime.png sites/hanime.png
  FILE sites/missav.png sites/missav.png
  FILE sites/add.png sites/add.png
)
