/* GENERATED FILE - DO NOT EDIT.
 *
 * Regenerate via:
 *   python3 pastime/tools/sync_external_presets.py
 *
 * Source:    https://github.com/TapiocaFox/Daijishou
 * Commit:    2b4554360922297184914bf7cd84483fdad117b6
 * Filtered:  0 RetroArch players,
 *            773 raw-path/bool-extra entries,
 *            0 malformed,
 *            44 package duplicates,
 *            0 without a derivable shortname.
 * Kept:      94 entries.
 *
 * Daijishou is MIT-licensed (Copyright (c) 2022 TapiocaFox / Yves Chen).
 */

#ifndef PASTIME_EXTERNAL_PRESETS_H
#define PASTIME_EXTERNAL_PRESETS_H

#include "pastime_external.h"

static const pastime_external_spec_t pastime_external_presets[] = {
   {  /* SonyPlayStation3.json#ps3.aenu.aps3e */
      "aenu.aps3e",
      "aps3e",
      "aenu.aps3e.EmulatorActivity",
      "aenu.intent.action.APS3E",
      NULL,
      "iso_uri",
      NULL,
      true
   },
   {  /* MicrosoftXbox360.json#xbox360.aenu.ax360e */
      "aenu.ax360e",
      "ax360e",
      "aenu.ax360e.EmulatorActivity",
      "aenu.intent.action.AX360E",
      NULL,
      "game_uri",
      NULL,
      false
   },
   {  /* MicrosoftXbox360.json#xbox360.aenu.ax360e.free */
      "aenu.ax360e.free",
      "ax360efree",
      "aenu.ax360e.EmulatorActivity",
      "aenu.intent.action.AX360E",
      NULL,
      "game_uri",
      NULL,
      false
   },
   {  /* SuperGrafx.json#supergrafx.com.PceEmu */
      "com.PceEmu",
      "pceemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Arcadia2001.json#arcadia.com.amigan.droidarcadia */
      "com.amigan.droidarcadia",
      "droidarcadia",
      ".MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      "application/zip",
      true
   },
   {  /* Nintendo3DS.json#3ds.com.antutu.ABenchMark */
      "com.antutu.ABenchMark",
      "storageaccessantutu",
      "org.citra.emu.ui.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      "GamePath",
      NULL,
      false
   },
   {  /* NintendoDS.json#nds.com.dsemu.drastic */
      "com.dsemu.drastic",
      "drastic",
      ".DraSticActivity",
      NULL,
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoGameBoyAdvance.json#gba.com.explusalpha.GbaEmu */
      "com.explusalpha.GbaEmu",
      "gbaemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      "application/zip",
      false
   },
   {  /* NintendoGameBoy.json#gb.com.explusalpha.GbcEmu */
      "com.explusalpha.GbcEmu",
      "gbcemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      "application/zip",
      false
   },
   {  /* AtariLynx.json#lynx.com.explusalpha.LynxEmu */
      "com.explusalpha.LynxEmu",
      "lynxemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      "application/zip",
      false
   },
   {  /* SegaCD.json#segacd.com.explusalpha.MdEmu */
      "com.explusalpha.MdEmu",
      "mdemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NeoGeo.json#neogeo.com.explusalpha.NeoEmu */
      "com.explusalpha.NeoEmu",
      "neoemu",
      "com.imagine.BaseActivity",
      NULL,
      NULL,
      NULL,
      "application/zip",
      false
   },
   {  /* FamicomDiskSystem.json#fds.com.explusalpha.NesEmu */
      "com.explusalpha.NesEmu",
      "nesemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSatellaview.json#satellaview.com.explusalpha.Snes9xPlus */
      "com.explusalpha.Snes9xPlus",
      "snes9xplus",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      "application/zip",
      false
   },
   {  /* WonderSwan.json#ws.com.explusalpha.SwanEmu */
      "com.explusalpha.SwanEmu",
      "swanemu",
      "com.imagine.BaseActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      "application/zip",
      false
   },
   {  /* NintendoGameBoyAdvance.json#gba.com.fastemulator.gba */
      "com.fastemulator.gba",
      "myboy",
      ".EmulatorActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoGameBoy.json#gb.com.fastemulator.gbc */
      "com.fastemulator.gbc",
      "mygbc",
      ".EmulatorActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Atomiswave.json#atomiswave.com.flycast.emulator */
      "com.flycast.emulator",
      "flycast",
      "com.flycast.emulator.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* SegaGameGear.json#gamegear.com.fms.mg */
      "com.fms.mg",
      "mastergear",
      "com.fms.emulib.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* BookReader.json#ebook.com.github.axet.bookreader */
      "com.github.axet.bookreader",
      "bookreader",
      ".activities.MainActivity",
      "android.intent.action.VIEW",
      "android.intent.category.DEFAULT",
      NULL,
      "{file.mime}",
      true
   },
   {  /* Ngage.json#ngage.com.github.eka2l1 */
      "com.github.eka2l1",
      "eka2l1",
      ".emu.EmulatorActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* SonyPlayStation.json#psx.com.github.stenzek.duckstation */
      "com.github.stenzek.duckstation",
      "duckstation",
      ".EmulationActivity",
      NULL,
      NULL,
      "bootPath",
      NULL,
      false
   },
   {  /* SegaModel3.json#model3.com.izzy2lost.super3 */
      "com.izzy2lost.super3",
      "super3",
      ".MainActivity",
      NULL,
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* MicrosoftXbox.json#xbox.com.izzy2lost.x1box */
      "com.izzy2lost.x1box",
      "x1box",
      ".LauncherActivity",
      NULL,
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Moonlight.json#moonlight.com.limelight.noir */
      "com.limelight.noir",
      "artemis",
      "com.limelight.ShortcutTrampoline",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* NintendoSwitch.json#switch.com.miHoYo.Yuanshen.eden */
      "com.miHoYo.Yuanshen",
      "edenoptimized",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoGameBoy.json#gb.com.pixelrespawn.linkboy */
      "com.pixelrespawn.linkboy",
      "linkboy",
      ".EmulatorActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* MicrosoftXbox.json#xbox.com.rfandango.haku_x */
      "com.rfandango.haku_x",
      "hakux",
      ".LauncherActivity",
      NULL,
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* PICO8.json#pico8.com.sa_moo_rai.picpic */
      "com.sa_moo_rai.picpic",
      "picpic",
      ".MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* SonyPlayStation2.json#ps2.com.sbro.emucorex */
      "com.sbro.emucorex",
      "emucorex",
      ".MainActivity",
      "com.sbro.emucorex.action.LAUNCH_GAME",
      NULL,
      "com.sbro.emucorex.extra.GAME_PATH",
      NULL,
      true
   },
   {  /* ArcadeMAME.json#mame.com.seleuco.mame4d2024 */
      "com.seleuco.mame4d2024",
      "comseleucomame4droid2024",
      "com.seleuco.mame4droid.MAME4droid",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* ArcadeMAME.json#mame.com.seleuco.mame4droid */
      "com.seleuco.mame4droid",
      "mame4droid",
      ".MAME4droid",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* VirtualBoy.json#virtualboy.com.simongellis.vvb */
      "com.simongellis.vvb",
      "vvs",
      "com.simongellis.vvb.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoDS.json#nds.com.sky.SkyEmu */
      "com.sky.SkyEmu",
      "skyemu",
      ".EnhancedNativeActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* SonyPlayStation2.json#ps2.come.nanodata.armsx2 */
      "come.nanodata.armsx2",
      "armsx2",
      "kr.co.iefriends.pcsx2.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* SonyPlayStation2.json#ps2.com.nanodata.armsx2.debug */
      "come.nanodata.armsx2.debug",
      "armsx2nightly",
      "kr.co.iefriends.pcsx2.activities.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* NintendoSwitch.json#switch.dev.eden.eden_emulator */
      "dev.eden.eden_emulator",
      "eden",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.dev.eden.eden_emulator.nightly */
      "dev.eden.eden_emulator.nightly",
      "edennightlynew",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.dev.eden.eden_nightly */
      "dev.eden.eden_nightly",
      "edennightly",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.dev.legacy.eden_emulator */
      "dev.legacy.eden_emulator",
      "edenlegacy",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.dev.suyu.suyu_emu.relWithDebInfo */
      "dev.suyu.suyu_emu.relWithDebInfo",
      "suyu",
      "dev.suyu.suyu_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoWiiU.json#wiiu.info.cemu.Cemu */
      "info.cemu.cemu",
      "cemu",
      ".emulation.EmulationActivity",
      NULL,
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.io.github.azaharplus.android */
      "io.github.azaharplus.android",
      "azaharplus",
      "org.citra.citra_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.io.github.borked3ds.android */
      "io.github.borked3ds.android",
      "borked3ds",
      ".activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.azahar */
      "io.github.lime3ds.android",
      "azahar",
      "org.citra.citra_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.io.github.mandarine3ds.mandarine */
      "io.github.mandarine3ds.mandarine",
      "mandarine",
      ".activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Atomiswave.json#atomiswave.io.recompiled.redream */
      "io.recompiled.redream",
      "redream",
      ".MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* PICO8.json#pico8.io.wip.pico8 */
      "io.wip.pico8",
      "pico8android",
      "com.godot.game.GodotAppLauncher",
      NULL,
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* VideoPlayer.json#videos.is.xyz.mpv */
      "is.xyz.mpv",
      "mpvandroid",
      "is.xyz.mpv.MPVActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* NintendoGameBoy.json#gb.it.dbtecno.pizzaboy */
      "it.dbtecno.pizzaboy",
      "pizzaboycbasic",
      "it.dbtecno.pizzaboy.MainActivity",
      NULL,
      NULL,
      "rom_uri",
      NULL,
      false
   },
   {  /* NintendoGameBoyAdvance.json#gba.it.dbtecno.pizzaboygba */
      "it.dbtecno.pizzaboygba",
      "pizzaboygbabasic",
      "it.dbtecno.pizzaboygba.MainActivity",
      NULL,
      NULL,
      "rom_uri",
      NULL,
      false
   },
   {  /* NintendoGameBoyAdvance.json#gba.it.dbtecno.pizzaboygbapro */
      "it.dbtecno.pizzaboygbapro",
      "pizzaboyapro",
      "it.dbtecno.pizzaboygbapro.MainActivity",
      NULL,
      NULL,
      "rom_uri",
      NULL,
      false
   },
   {  /* NintendoGameBoy.json#gb.it.dbtecno.pizzaboypro */
      "it.dbtecno.pizzaboypro",
      "pizzaboycpro",
      "it.dbtecno.pizzaboypro.MainActivity",
      NULL,
      NULL,
      "rom_uri",
      NULL,
      false
   },
   {  /* SegaGameGear.json#gamegear.it.dbtecno.pizzaboyscpro */
      "it.dbtecno.pizzaboyscpro",
      "pizzaboyscpro",
      ".MainActivity",
      NULL,
      NULL,
      "rom_uri",
      NULL,
      false
   },
   {  /* PICO8.json#pico8.me.dt2dev.infinity */
      "me.dt2dev.infinity",
      "infinityp8player",
      "me.dt2dev.infinity.SchemeActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* NintendoDS.json#nds.me.magnum.melonds */
      "me.magnum.melonds",
      "melonds",
      ".ui.emulator.EmulatorActivity",
      "me.magnum.melonds.LAUNCH_ROM",
      NULL,
      "uri",
      NULL,
      false
   },
   {  /* NintendoDS.json#nds.me.magnum.melonds.dev */
      "me.magnum.melonds.dev",
      "melondsdevdualscreenfork",
      "me.magnum.melonds.ui.emulator.EmulatorActivity",
      "me.magnum.melonds.dev.LAUNCH_ROM",
      NULL,
      "uri",
      NULL,
      true
   },
   {  /* NintendoDS.json#nds.me.magnum.melonds.nightly */
      "me.magnum.melonds.nightly",
      "melondsnightly",
      "me.magnum.melonds.ui.emulator.EmulatorActivity",
      "me.magnum.melonds.nightly.LAUNCH_ROM",
      NULL,
      "uri",
      NULL,
      true
   },
   {  /* NintendoDS.json#nds.me.magnum.melondualds */
      "me.magnum.melondualds",
      "melondualds",
      "me.magnum.melonds.ui.emulator.EmulatorActivity",
      "me.magnum.melonds.dev.LAUNCH_ROM",
      NULL,
      "uri",
      NULL,
      true
   },
   {  /* Nintendo3DS.json#3ds.azahar.vanilla */
      "org.azahar_emu.azahar",
      "azaharvanilla",
      "org.citra.citra_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.benjisc.android */
      "org.benjisc.android",
      "benjisc",
      "org.kenjinx.android.MainActivity",
      "org.kenjinx.android.LAUNCH_GAME",
      NULL,
      "bootPath",
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.org.citra.citra_emu */
      "org.citra.citra_emu",
      "citranightly",
      "org.citra.citra_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.org.citra.citra_emu.canary */
      "org.citra.citra_emu.canary",
      "citracanary",
      "org.citra.citra_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.citron.citron_emu */
      "org.citron.citron_emu",
      "citron",
      "org.citron.citron_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* SegaSaturn.json#saturn.org.devmiyax.yabasanshioro2.pro */
      "org.devmiyax.yabasanshioro2.pro",
      "yabasanshiro2pro",
      "org.uoyabause.android.Yabause",
      "android.intent.action.MAIN",
      NULL,
      "org.uoyabause.android.FileNameUri",
      NULL,
      false
   },
   {  /* Triforce.json#triforce.org.dolphinemu.dolphinemu */
      "org.dolphinemu.dolphinemu",
      "dolphin",
      ".ui.main.MainActivity",
      "android.intent.action.MAIN",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* NintendoGameCube.json#gc.org.dolphinemu.dolphinemu.debug */
      "org.dolphinemu.dolphinemu.debug",
      "dolphindebugbuild",
      "org.dolphinemu.dolphinemu.ui.main.MainActivity",
      "android.intent.action.MAIN",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* NintendoGameCube.json#gc.org.dolphinemu.handheld */
      "org.dolphinemu.handheld",
      "dolphinhandheld",
      "org.dolphinemu.dolphinemu.ui.main.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* NintendoGameCube.json#gc.org.dolphinemu.mmjr */
      "org.dolphinemu.mmjr",
      "dolphinmmjr",
      "org.dolphinemu.dolphinemu.ui.main.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* NintendoGameCube.json#gc.org.dolphinemu.mmjr3 */
      "org.dolphinemu.mmjr3",
      "dolphinmmjr3",
      "org.dolphinemu.dolphinemu.ui.main.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* NintendoWii.json#wii.org.dolphinemu.primehack */
      "org.dolphinemu.primehack",
      "dolphinprimehack",
      "org.dolphinemu.dolphinemu.ui.main.MainActivity",
      "android.intent.action.MAIN",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* Nintendo3DS.json#3ds.org.gamerytb.lemonade.canary */
      "org.gamerytb.lemonade.canary",
      "lemonadealpha",
      "org.citra.citra_emu.activities.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.kenjinx.android */
      "org.kenjinx.android",
      "kenjinx",
      ".MainActivity",
      "org.kenjinx.android.LAUNCH_GAME",
      NULL,
      "bootPath",
      NULL,
      false
   },
   {  /* BookReader.json#ebook.org.koreader.launcher */
      "org.koreader.launcher",
      "koreader",
      ".MainActivity",
      "android.intent.action.VIEW",
      "android.intent.category.DEFAULT",
      NULL,
      "{file.mime}",
      true
   },
   {  /* Nintendo64.json#n64.org.mupen64plusae.v3.alpha */
      "org.mupen64plusae.v3.alpha",
      "mupen64plus",
      "paulscode.android.mupen64plusae.SplashActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo64.json#n64.org.mupen64plusae.v3.fzurita */
      "org.mupen64plusae.v3.fzurita",
      "mupen64plusfz",
      "paulscode.android.mupen64plusae.SplashActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* Nintendo64.json#n64.org.mupen64plusae.v3.fzurita.pro */
      "org.mupen64plusae.v3.fzurita.pro",
      "orgmupen64plusaev3fzuritapro",
      "paulscode.android.mupen64plusae.SplashActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* PlayStationPortable.json#psp.org.ppsspp.ppsspp */
      "org.ppsspp.ppsspp",
      "ppsspp",
      ".PpssppActivity",
      "android.intent.action.VIEW",
      "android.intent.category.DEFAULT",
      NULL,
      "application/octet-stream",
      false
   },
   {  /* PlayStationPortable.json#psp.org.ppsspp.ppssppgold */
      "org.ppsspp.ppssppgold",
      "ppssppgold",
      "org.ppsspp.ppsspp.PpssppActivity",
      "android.intent.action.VIEW",
      "android.intent.category.DEFAULT",
      NULL,
      "application/octet-stream",
      false
   },
   {  /* PlayStationPortable.json#psp.org.ppsspp.ppsspplegacy */
      "org.ppsspp.ppsspplegacy",
      "ppsspplegacy",
      "org.ppsspp.ppsspp.PpssppActivity",
      "android.intent.action.VIEW",
      "android.intent.category.DEFAULT",
      NULL,
      "application/octet-stream",
      false
   },
   {  /* BookReader.json#ebook.org.readera */
      "org.readera",
      "readera",
      "org.readera.read.ReadActivity",
      NULL,
      NULL,
      NULL,
      NULL,
      true
   },
   {  /* NintendoWii.json#wii.org.shiiion.primehack */
      "org.shiiion.primehack",
      "shiiion",
      "org.dolphinemu.dolphinemu.ui.main.MainActivity",
      "android.intent.action.MAIN",
      NULL,
      "AutoStartFile",
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.stratoemu.strato */
      "org.stratoemu.strato",
      "stratoofficial",
      "org.stratoemu.strato.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.sudachi.sudachi_emu */
      "org.sudachi.sudachi_emu",
      "sudachimainline",
      "org.sudachi.sudachi_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.sudachi.sudachi_emu.ea */
      "org.sudachi.sudachi_emu.ea",
      "sudachiearlyaccess",
      "org.sudachi.sudachi_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.yuzu.yuzu_emu */
      "org.yuzu.yuzu_emu",
      "yuzu",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.org.yuzu.yuzu_emu.ea */
      "org.yuzu.yuzu_emu.ea",
      "yuzuearlyaccess",
      "org.yuzu.yuzu_emu.activities.EmulationActivity",
      "android.nfc.action.TECH_DISCOVERED",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* JavaMe.json#j2me.ru.playsoftware.j2meloader */
      "ru.playsoftware.j2meloader",
      "ruplaysoftwarej2meloader",
      "ru.playsoftware.j2meloader.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* JavaMe.json#j2me.ru.woesss.j2meloader */
      "ru.woesss.j2meloader",
      "jlmod",
      "ru.playsoftware.j2meloader.MainActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* NintendoSwitch.json#switch.skyline.emu */
      "skyline.emu",
      "skyline",
      "emu.skyline.EmulationActivity",
      "android.intent.action.VIEW",
      NULL,
      NULL,
      NULL,
      false
   },
   {  /* SonyPlayStation2.json#ps2.xyz.aethersx2.android */
      "xyz.aethersx2.android",
      "aethersx2",
      ".EmulationActivity",
      "android.intent.action.MAIN",
      NULL,
      "bootPath",
      NULL,
      true
   },
   {  /* SonyPlayStation2.json#ps2.xyz.nethersx2.custom.classic */
      "xyz.aethersx2.cturnip",
      "nethersx2turnipclassic",
      "xyz.aethersx2.android.EmulationActivity",
      "android.intent.action.MAIN",
      NULL,
      "bootPath",
      NULL,
      true
   },
   {  /* SonyPlayStation2.json#ps2.xyz.aethersx2.custom */
      "xyz.aethersx2.custom",
      "aethersx2turnip",
      ".EmulationActivity",
      "android.intent.action.MAIN",
      NULL,
      "bootPath",
      NULL,
      true
   },
   {  /* SonyPlayStation2.json#ps2.xyz.nethersx2.custom */
      "xyz.aethersx2.tturnip",
      "nethersx2turnip",
      "xyz.aethersx2.android.EmulationActivity",
      "android.intent.action.MAIN",
      NULL,
      "bootPath",
      NULL,
      true
   },
};

static const size_t pastime_external_presets_count =
   sizeof(pastime_external_presets) / sizeof(pastime_external_presets[0]);

#endif /* PASTIME_EXTERNAL_PRESETS_H */
