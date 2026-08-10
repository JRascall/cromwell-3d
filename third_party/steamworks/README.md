# Steamworks SDK

The Steamworks SDK, in Valve's own layout:

    sdk/public/steam/*.h                          headers
    sdk/redistributable_bin/win64/steam_api64.lib import library
    sdk/redistributable_bin/win64/steam_api64.dll runtime, staged beside xcom.exe

NOT CHECKED IN — it is Valve's, distributed under the Steamworks SDK Access
Agreement, and not ours to redistribute. `.gitignore` excludes everything here
except this file.

## Where this copy came from

Version **1.61**, copied out of Unreal Engine 5.7, which ships the SDK for its
own `OnlineSubsystemSteam`:

    <UE>/Engine/Source/ThirdParty/Steamworks/Steamv161/sdk/public/steam/
    <UE>/Engine/Source/ThirdParty/Steamworks/Steamv161/sdk/redistributable_bin/win64/steam_api64.lib
    <UE>/Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/steam_api64.dll

(the .dll lives under Binaries, apart from the .lib — hence the two source
paths). A COPY, not a reference: headers, an import library and a DLL are not
generated from anything, so nothing here has to be regenerated and the build
does not need Unreal installed. UE 5.6 ships the older 1.57 in the same layout
if this one is ever gone.

## Replacing it with the official download

Sign in at partner.steamgames.com, download `steamworks_sdk_<version>.zip`,
and unzip so that its `sdk/` directory lands here — i.e. this file ends up
beside `sdk/`. That is the same shape as above, so nothing in the build changes.
`cmake/steamworks.cmake` also accepts the SDK unzipped one level flatter
(`public/` and `redistributable_bin/` directly here), because that is what you
get if you unzip the inner folder by mistake.

## App id

Set in the root CMakeLists as `XC_STEAM_APPID`, currently **480** — Spacewar,
Valve's public test app. Every Steam account owns it, so identity, the overlay
and the friends list all work against it without an app of our own. Swap the
number for the real app id when there is one; nothing else moves.
